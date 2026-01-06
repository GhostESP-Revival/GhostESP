#include "core/serial_manager.h"
#include "core/system_manager.h"
#include "driver/uart.h"
#include "core/glog.h"
#include "driver/usb_serial_jtag.h"
#include "esp_task_wdt.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "managers/gps_manager.h"
#include "managers/wifi_manager.h"
#include "managers/infrared_manager.h"
#include "managers/views/terminal_screen.h"
#include <core/commandline.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(CONFIG_IDF_TARGET_ESP32S3) ||                                      \
    defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
#define JTAG_SUPPORTED 1
#else
#define JTAG_SUPPORTED 0
#endif
#ifndef CONFIG_USE_TDECK
#define UART_NUM UART_NUM_0
#else
#define UART_NUM UART_NUM_1
#endif
#define BUF_SIZE (512)
#define SERIAL_BUFFER_SIZE 512

char serial_buffer[SERIAL_BUFFER_SIZE];
static TaskHandle_t s_serial_task_handle = NULL;
static bool s_serial_initialized = false;
static bool s_uart_disabled = false; // disable main serial UART for certain templates

int serial_manager_write_bytes(const void *data, size_t len) {
  if (data == NULL || len == 0) {
    return 0;
  }

  int written = 0;

  if (!s_uart_disabled) {
    written = uart_write_bytes(UART_NUM, (const char *)data, (size_t)len);
  }

#if JTAG_SUPPORTED
  usb_serial_jtag_write_bytes((const uint8_t *)data, (uint32_t)len, 0);
#endif

  return written;
}

// Cursor position tracking
static int cursor_position = 0;

// Command history instance
static CommandHistory command_history;

// Prompt display tracking
static bool prompt_displayed = false;
static bool prompt_pending = false;
static uint32_t prompt_delay_ticks = 0;

// Output deferral tracking for typing interruption prevention
static bool output_deferred = false;
static uint32_t last_typing_activity_tick = 0;
static const uint32_t TYPING_IDLE_TIMEOUT_MS = 200;

// Arrow key state machine
typedef enum {
    ARROW_STATE_NONE,
    ARROW_STATE_ESC,
    ARROW_STATE_BRACKET
} arrow_state_t;

static arrow_state_t arrow_state = ARROW_STATE_NONE;

// HTML capture state
typedef enum {
    HTML_STATE_IDLE,
    HTML_STATE_CAPTURING,
    HTML_STATE_COMPLETE
} html_capture_state_t;

static html_capture_state_t html_capture_state = HTML_STATE_IDLE;
static char html_capture_buffer[2048];
static size_t html_capture_pos = 0;

// IR capture state
typedef enum {
    IR_STATE_IDLE,
    IR_STATE_CAPTURING
} ir_capture_state_t;

static ir_capture_state_t ir_capture_state = IR_STATE_IDLE;
static char ir_capture_buffer[2048];
static size_t ir_capture_pos = 0;

// Forward declaration of command handler
int handle_serial_command(const char *command);

// Command history management functions
void command_history_init(void) {
    command_history.current_index = 0;
    command_history.history_count = 0;
    command_history.display_index = -1;
    memset(command_history.commands, 0, sizeof(command_history.commands));
}

void command_history_add(const char* command) {
    if (command == NULL || strlen(command) == 0) {
        return;
    }
    
    // Don't add duplicate consecutive commands
    if (command_history.history_count > 0) {
        int last_index = (command_history.current_index - 1 + MAX_HISTORY_SIZE) % MAX_HISTORY_SIZE;
        if (strcmp(command_history.commands[last_index], command) == 0) {
            return;
        }
    }
    
    // Add command to current position
    strncpy(command_history.commands[command_history.current_index], command, MAX_COMMAND_LENGTH - 1);
    command_history.commands[command_history.current_index][MAX_COMMAND_LENGTH - 1] = '\0';
    
    // Move to next position
    command_history.current_index = (command_history.current_index + 1) % MAX_HISTORY_SIZE;
    
    // Update history count
    if (command_history.history_count < MAX_HISTORY_SIZE) {
        command_history.history_count++;
    }
    
    // Reset display index when new command is added
    command_history.display_index = -1;
}

const char* command_history_get_previous(void) {
    if (command_history.history_count == 0) {
        return NULL;
    }
    
    if (command_history.display_index == -1) {
        // First time navigating, start from the most recent command
        command_history.display_index = (command_history.current_index - 1 + MAX_HISTORY_SIZE) % MAX_HISTORY_SIZE;
    } else {
        // Move to previous command
        command_history.display_index = (command_history.display_index - 1 + MAX_HISTORY_SIZE) % MAX_HISTORY_SIZE;
    }
    
    // Additional safety check to ensure we have a valid command
    if (command_history.display_index >= 0 && command_history.display_index < MAX_HISTORY_SIZE) {
        const char* cmd = command_history.commands[command_history.display_index];
        if (cmd != NULL && strlen(cmd) > 0) {
            return cmd;
        }
    }
    
    return NULL;
}

const char* command_history_get_next(void) {
    if (command_history.history_count == 0) {
        return NULL;
    }
    
    if (command_history.display_index == -1) {
        return NULL; // No navigation started yet
    }
    
    // Move to next command
    command_history.display_index = (command_history.display_index + 1) % MAX_HISTORY_SIZE;
    
    // If we've reached the current position, we're at the end
    if (command_history.display_index == command_history.current_index) {
        command_history.display_index = -1;
        return NULL;
    }
    
    // Additional safety check to ensure we have a valid command
    if (command_history.display_index >= 0 && command_history.display_index < MAX_HISTORY_SIZE) {
        const char* cmd = command_history.commands[command_history.display_index];
        if (cmd != NULL && strlen(cmd) > 0) {
            return cmd;
        }
    }
    
    return NULL;
}

void command_history_reset_display_index(void) {
    command_history.display_index = -1;
}

// Cursor management functions
static void move_cursor_to_position(int new_pos) {
    // Ensure cursor position is within bounds
    if (new_pos < 0) new_pos = 0;
    if (new_pos > strlen(serial_buffer)) new_pos = strlen(serial_buffer);
    
    int current_pos = cursor_position;
    cursor_position = new_pos;
    
    // Calculate how many characters to move
    int move_count = new_pos - current_pos;
    
    if (move_count > 0) {
        // Move right - send right arrow sequences
        for (int i = 0; i < move_count; i++) {
            const char right_arrow[] = "\033[C";
            uart_write_bytes(UART_NUM, right_arrow, 3);
#if JTAG_SUPPORTED
            usb_serial_jtag_write_bytes((const uint8_t*)right_arrow, 3, 0);
#endif
        }
    } else if (move_count < 0) {
        // Move left - send left arrow sequences
        for (int i = 0; i < -move_count; i++) {
            const char left_arrow[] = "\033[D";
            uart_write_bytes(UART_NUM, left_arrow, 3);
#if JTAG_SUPPORTED
            usb_serial_jtag_write_bytes((const uint8_t*)left_arrow, 3, 0);
#endif
        }
    }
}

static void insert_character_at_cursor(char c) {
    int len = strlen(serial_buffer);
    if (len >= SERIAL_BUFFER_SIZE - 1) return; // Buffer full
    
    // Shift characters to the right
    for (int i = len; i > cursor_position; i--) {
        serial_buffer[i] = serial_buffer[i - 1];
    }
    
    // Insert character at cursor position
    serial_buffer[cursor_position] = c;
    serial_buffer[len + 1] = '\0';
    
    // Display the character and move cursor right
    uart_write_bytes(UART_NUM, &c, 1);
#if JTAG_SUPPORTED
    usb_serial_jtag_write_bytes((const uint8_t*)&c, 1, 0);
#endif
    
    // Display remaining characters
    for (int i = cursor_position + 1; i <= len; i++) {
        uart_write_bytes(UART_NUM, &serial_buffer[i], 1);
#if JTAG_SUPPORTED
        usb_serial_jtag_write_bytes((const uint8_t*)&serial_buffer[i], 1, 0);
#endif
    }
    
    // Move cursor back to correct position
    for (int i = len; i > cursor_position; i--) {
        const char left_arrow[] = "\033[D";
        uart_write_bytes(UART_NUM, left_arrow, 3);
#if JTAG_SUPPORTED
        usb_serial_jtag_write_bytes((const uint8_t*)left_arrow, 3, 0);
#endif
    }
    
    cursor_position++;
}

static void delete_character_at_cursor(void) {
    int len = strlen(serial_buffer);
    if (cursor_position >= len) return; // Nothing to delete
    
    // Shift characters to the left
    for (int i = cursor_position; i < len; i++) {
        serial_buffer[i] = serial_buffer[i + 1];
    }

    // New length after deletion
    int new_len = len - 1;

    // Display remaining characters (overwrite current position)
    for (int i = cursor_position; i < new_len; i++) {
        uart_write_bytes(UART_NUM, &serial_buffer[i], 1);
#if JTAG_SUPPORTED
        usb_serial_jtag_write_bytes((const uint8_t*)&serial_buffer[i], 1, 0);
#endif
    }
    
    // Clear the last character
    const char space[] = " ";
    uart_write_bytes(UART_NUM, space, 1);
#if JTAG_SUPPORTED
    usb_serial_jtag_write_bytes((const uint8_t*)space, 1, 0);
#endif
    
    // Move cursor back to correct position
    for (int i = len - cursor_position; i > 0; i--) {
        const char left_arrow[] = "\033[D";
        uart_write_bytes(UART_NUM, left_arrow, 3);
#if JTAG_SUPPORTED
        usb_serial_jtag_write_bytes((const uint8_t*)left_arrow, 3, 0);
#endif
    }
}

static void backspace_at_cursor(void) {
    int len = strlen(serial_buffer);
    if (cursor_position <= 0) return; // Nothing to delete
    
    // Move cursor left first
    const char left_arrow[] = "\033[D";
    uart_write_bytes(UART_NUM, left_arrow, 3);
#if JTAG_SUPPORTED
    usb_serial_jtag_write_bytes((const uint8_t*)left_arrow, 3, 0);
#endif
    
    // Shift characters to the left (delete character at cursor_position - 1)
    for (int i = cursor_position - 1; i < len; i++) {
        serial_buffer[i] = serial_buffer[i + 1];
    }

    // New length after deletion
    int new_len = len - 1;

    // Display remaining characters from cursor position
    for (int i = cursor_position - 1; i < new_len; i++) {
        uart_write_bytes(UART_NUM, &serial_buffer[i], 1);
#if JTAG_SUPPORTED
        usb_serial_jtag_write_bytes((const uint8_t*)&serial_buffer[i], 1, 0);
#endif
    }
    
    // Clear the last character
    const char space[] = " ";
    uart_write_bytes(UART_NUM, space, 1);
#if JTAG_SUPPORTED
    usb_serial_jtag_write_bytes((const uint8_t*)space, 1, 0);
#endif
    
    // Move cursor back to correct position
    for (int i = len - cursor_position + 1; i > 0; i--) {
        uart_write_bytes(UART_NUM, left_arrow, 3);
#if JTAG_SUPPORTED
        usb_serial_jtag_write_bytes((const uint8_t*)left_arrow, 3, 0);
#endif
    }
    
    cursor_position--;
}

static void clear_line_from_cursor(void) {
    // Use ANSI escape to clear from cursor to end of line to avoid
    // backspace-based visual corruption when replacing the line.
    const char clear_to_eol[] = "\033[K";
    uart_write_bytes(UART_NUM, clear_to_eol, 3);
#if JTAG_SUPPORTED
    usb_serial_jtag_write_bytes((const uint8_t*)clear_to_eol, 3, 0);
#endif
    // Truncate buffer at cursor position
    serial_buffer[cursor_position] = '\0';
}

static void clear_entire_line(void) {
    // Move cursor to beginning of line
    const char cr[] = "\r";
    uart_write_bytes(UART_NUM, cr, 1);
#if JTAG_SUPPORTED
    usb_serial_jtag_write_bytes((const uint8_t*)cr, 1, 0);
#endif
    // Clear entire line using ANSI escape sequence
    const char clear_line[] = "\033[2K";
    uart_write_bytes(UART_NUM, clear_line, 4);
#if JTAG_SUPPORTED
    usb_serial_jtag_write_bytes((const uint8_t*)clear_line, 4, 0);
#endif
}

static void display_prompt(void) {
    const char prompt[] = "ghost-cli> ";
    uart_write_bytes(UART_NUM, prompt, sizeof(prompt) - 1);
#if JTAG_SUPPORTED
    usb_serial_jtag_write_bytes((const uint8_t*)prompt, sizeof(prompt) - 1, 0);
#endif
    prompt_displayed = true;
}

// HTML marker processing and IR inline handling
static void process_html_line(const char* line) {
    if (strstr(line, "[IR/BEGIN]") != NULL) {
        ir_capture_state = IR_STATE_CAPTURING;
        ir_capture_pos = 0;
        return;
    }

    if (strstr(line, "[IR/CLOSE]") != NULL) {
        if (ir_capture_state == IR_STATE_CAPTURING) {
            ir_capture_state = IR_STATE_IDLE;
            if (ir_capture_pos >= sizeof(ir_capture_buffer)) {
                ir_capture_pos = sizeof(ir_capture_buffer) - 1;
            }
            ir_capture_buffer[ir_capture_pos] = '\0';
            infrared_signal_t sig;
            memset(&sig, 0, sizeof(sig));
            if (infrared_manager_parse_buffer_single(ir_capture_buffer, &sig)) {
                bool ok = infrared_manager_transmit(&sig);
                glog("IR: send %s\n", ok ? "OK" : "FAIL");
                if (sig.is_raw) {
                    if (sig.payload.raw.timings && sig.payload.raw.timings_size > 0) {
                        glog("IR: signal raw len=%u freq=%luHz duty=%.2f\n",
                             (unsigned)sig.payload.raw.timings_size,
                             (unsigned long)sig.payload.raw.frequency,
                             (double)sig.payload.raw.duty_cycle);
                    }
                } else {
                    const char *proto = sig.payload.message.protocol;
                    if (proto && proto[0] != '\0') {
                        uint32_t addr = sig.payload.message.address;
                        uint32_t cmd = sig.payload.message.command;
                        if (sig.name[0] != '\0') {
                            glog("IR: signal [%s] protocol=%s addr=0x%08lX cmd=0x%08lX\n",
                                 sig.name, proto, (unsigned long)addr, (unsigned long)cmd);
                        } else {
                            glog("IR: signal protocol=%s addr=0x%08lX cmd=0x%08lX\n",
                                 proto, (unsigned long)addr, (unsigned long)cmd);
                        }
                    }
                }
                infrared_manager_free_signal(&sig);
            } else {
                glog("IR inline parse failed\n");
            }
        }
        return;
    }

    if (ir_capture_state == IR_STATE_CAPTURING) {
        size_t line_len = strlen(line);
        if (ir_capture_pos + line_len + 1 < sizeof(ir_capture_buffer)) {
            memcpy(ir_capture_buffer + ir_capture_pos, line, line_len);
            ir_capture_pos += line_len;
            ir_capture_buffer[ir_capture_pos++] = '\n';
        }
        return;
    }

    if (strstr(line, "[HTML/BEGIN]") != NULL) {
        html_capture_state = HTML_STATE_CAPTURING;
        html_capture_pos = 0;
        glog("HTML capture started\n");
        return;
    }
    
    if (strstr(line, "[HTML/CLOSE]") != NULL) {
        if (html_capture_state == HTML_STATE_CAPTURING) {
            html_capture_state = HTML_STATE_COMPLETE;
            wifi_manager_store_html_chunk(html_capture_buffer, html_capture_pos, true);
            glog("HTML capture completed (%zu bytes)\n", html_capture_pos);
        }
        return;
    }
    
    if (html_capture_state == HTML_STATE_CAPTURING) {
        size_t line_len = strlen(line);
        if (html_capture_pos + line_len + 1 < sizeof(html_capture_buffer)) {
            memcpy(html_capture_buffer + html_capture_pos, line, line_len);
            html_capture_pos += line_len;
            html_capture_buffer[html_capture_pos++] = '\n';
        }
        return;
    }
    
    handle_serial_command(line);
}

void serial_task(void *pvParameter) {
  uint8_t *data = (uint8_t *)malloc(BUF_SIZE);
  int index = 0;
  static uint32_t hwm_log_counter = 0;

  // Display initial prompt after startup messages have time to print
  if (!s_uart_disabled) {
    vTaskDelay(500 / portTICK_PERIOD_MS); // Wait for UART to be ready and startup messages to print
    fflush(stdout); // Ensure any buffered output is flushed
    display_prompt();
  }

  bool first_iteration = true;
  while (1) {
    // Ensure prompt is displayed on first iteration if it wasn't shown during init
    if (first_iteration && !s_uart_disabled && !prompt_displayed) {
      fflush(stdout);
      display_prompt();
    }
    first_iteration = false;
    if (++hwm_log_counter >= 6000) {
      UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
      ESP_LOGI("SerialTask", "Stack HWM: %u words (%u bytes free)", hwm, hwm * 4);
      UBaseType_t queue_avail = uxQueueSpacesAvailable(commandQueue);
      ESP_LOGI("SerialTask", "Command queue available: %u/%u", queue_avail, 6);
      hwm_log_counter = 0;
    }
    int length = 0;

    // Read data from the main UART (if not disabled)
    if (!s_uart_disabled) {
      length = uart_read_bytes(UART_NUM, data, BUF_SIZE, 10 / portTICK_PERIOD_MS);
    }

#if JTAG_SUPPORTED
    if (length <= 0) {
      length =
          usb_serial_jtag_read_bytes(data, BUF_SIZE, 10 / portTICK_PERIOD_MS);
    }
#endif

    if (length > 0) {
      for (int i = 0; i < length; i++) {
        char incoming_char = (char)data[i];

        if (incoming_char == '\b' || (unsigned char)incoming_char == 0x7F) {
          // Reset arrow key state when backspace is pressed
          arrow_state = ARROW_STATE_NONE;
          
          // Enable output deferral when user is editing
          if (!output_deferred) {
            glog_set_defer(1);
            output_deferred = true;
          }
          last_typing_activity_tick = xTaskGetTickCount();
          
          if (cursor_position > 0) {
            // Delete character to the left of cursor
            backspace_at_cursor();
            index = strlen(serial_buffer);
          }
          continue;
        }

        // Handle arrow keys using state machine
        if (arrow_state == ARROW_STATE_NONE && incoming_char == 0x1B) {
          // Start of potential arrow key sequence
          arrow_state = ARROW_STATE_ESC;
          continue;
        } else if (arrow_state == ARROW_STATE_ESC && incoming_char == '[') {
          // Second character of arrow key sequence
          arrow_state = ARROW_STATE_BRACKET;
          continue;
        } else if (arrow_state == ARROW_STATE_BRACKET) {
          // Third character - determine if it's an arrow key
          if (incoming_char == 'A') { // Up arrow
            const char* history_cmd = command_history_get_previous();
            if (history_cmd != NULL && strlen(history_cmd) > 0) {
              // Enable output deferral when navigating history
              if (!output_deferred) {
                glog_set_defer(1);
                output_deferred = true;
              }
              last_typing_activity_tick = xTaskGetTickCount();
              
              // Clear entire line to remove any existing text
              clear_entire_line();
              // Display prompt
              display_prompt();
              // Copy history command to buffer
              strncpy(serial_buffer, history_cmd, SERIAL_BUFFER_SIZE - 1);
              serial_buffer[SERIAL_BUFFER_SIZE - 1] = '\0';
              index = strlen(serial_buffer);
              cursor_position = index; // Move cursor to end
              // Echo the history command
              if (index > 0) {
                uart_write_bytes(UART_NUM, history_cmd, index);
#if JTAG_SUPPORTED
                usb_serial_jtag_write_bytes((const uint8_t*)history_cmd, index, 0);
#endif
              }
            }
          } else if (incoming_char == 'B') { // Down arrow
            const char* history_cmd = command_history_get_next();
            
            // Enable output deferral when navigating history
            if (!output_deferred) {
              glog_set_defer(1);
              output_deferred = true;
            }
            last_typing_activity_tick = xTaskGetTickCount();
            
            // Clear entire line to remove any existing text
            clear_entire_line();
            // Display prompt
            display_prompt();
            if (history_cmd != NULL && strlen(history_cmd) > 0) {
              // Copy history command to buffer
              strncpy(serial_buffer, history_cmd, SERIAL_BUFFER_SIZE - 1);
              serial_buffer[SERIAL_BUFFER_SIZE - 1] = '\0';
              index = strlen(serial_buffer);
              cursor_position = index; // Move cursor to end
              // Echo the history command
              if (index > 0) {
                uart_write_bytes(UART_NUM, history_cmd, index);
#if JTAG_SUPPORTED
                usb_serial_jtag_write_bytes((const uint8_t*)history_cmd, index, 0);
#endif
              }
            } else {
              // No more history, clear buffer
              index = 0;
              serial_buffer[0] = '\0';
              cursor_position = 0;
            }
          } else if (incoming_char == 'C') { // Right arrow
            // Enable output deferral when navigating cursor
            if (!output_deferred) {
              glog_set_defer(1);
              output_deferred = true;
            }
            last_typing_activity_tick = xTaskGetTickCount();
            
            if (cursor_position < strlen(serial_buffer)) {
              move_cursor_to_position(cursor_position + 1);
            }
          } else if (incoming_char == 'D') { // Left arrow
            // Enable output deferral when navigating cursor
            if (!output_deferred) {
              glog_set_defer(1);
              output_deferred = true;
            }
            last_typing_activity_tick = xTaskGetTickCount();
            
            if (cursor_position > 0) {
              move_cursor_to_position(cursor_position - 1);
            }
          }
          // Reset state after processing (whether it was an arrow key or not)
          arrow_state = ARROW_STATE_NONE;
          continue;
        } else if (arrow_state != ARROW_STATE_NONE) {
          // We were in the middle of an arrow key sequence but got an unexpected character
          // Reset state and process the current character normally
          arrow_state = ARROW_STATE_NONE;
          // Fall through to normal character processing
        }

        if (incoming_char == '\n' || incoming_char == '\r') {
          // Flush any deferred output before processing command
          if (output_deferred) {
            glog_set_defer(0);
            glog_flush_deferred();
            output_deferred = false;
          }
          
          // Echo newline directly to UART
          const char newline[] = "\n";
          if (!s_uart_disabled) uart_write_bytes(UART_NUM, newline, 1);
#if JTAG_SUPPORTED
          usb_serial_jtag_write_bytes((const uint8_t*)newline, 1, 0);
#endif
          serial_buffer[index] = '\0';
          if (index > 0) {
            // Reset history display index when entering a new command
            command_history_reset_display_index();
            // Reset arrow key state
            arrow_state = ARROW_STATE_NONE;
            // Reset cursor position
            cursor_position = 0;
            process_html_line(serial_buffer);
            // Clear the buffer completely
            memset(serial_buffer, 0, sizeof(serial_buffer));
            index = 0;
          }
          // Schedule prompt to be displayed after command output completes
          // This allows time for command output to flush before showing the prompt
          prompt_displayed = false;
          prompt_pending = true;
          prompt_delay_ticks = xTaskGetTickCount() + pdMS_TO_TICKS(50); // 50ms delay
          continue;
        }

        if ((unsigned char)incoming_char >= 32 && (unsigned char)incoming_char != 127) {
          // Reset arrow key state when typing normal characters
          arrow_state = ARROW_STATE_NONE;
          // Display prompt if not already displayed
          if (!prompt_displayed) {
            display_prompt();
          }
          
          // Enable output deferral when user starts typing
          if (!output_deferred) {
            glog_set_defer(1);
            output_deferred = true;
          }
          last_typing_activity_tick = xTaskGetTickCount();
          
          if (strlen(serial_buffer) < SERIAL_BUFFER_SIZE - 1) {
            // Insert character at cursor position
            insert_character_at_cursor(incoming_char);
            index = strlen(serial_buffer);
          }
        }
      }
    }

    // Check command queue for simulated commands
    SerialCommand command;
    if (xQueueReceive(commandQueue, &command, 0) == pdTRUE) {
      handle_serial_command(command.command);
      // Schedule prompt after simulated command too
      prompt_displayed = false;
      prompt_pending = true;
      prompt_delay_ticks = xTaskGetTickCount() + pdMS_TO_TICKS(50);
    }

    // Check if we need to display a pending prompt
    if (prompt_pending && xTaskGetTickCount() >= prompt_delay_ticks) {
      // Flush any pending output before displaying prompt
      fflush(stdout);
      display_prompt();
      prompt_pending = false;
    }

    // Check if user has stopped typing and flush deferred output
    if (output_deferred && last_typing_activity_tick > 0) {
      uint32_t time_since_typing = xTaskGetTickCount() - last_typing_activity_tick;
      if (time_since_typing >= pdMS_TO_TICKS(TYPING_IDLE_TIMEOUT_MS)) {
        // User has been idle for 200ms, flush deferred output
        glog_set_defer(0);
        glog_flush_deferred();
        output_deferred = false;
        last_typing_activity_tick = 0;
      }
    }

    vTaskDelay(10 / portTICK_PERIOD_MS);
  }

  free(data);
}

// Initialize the SerialManager
void serial_manager_init() {
  // UART configuration for main UART
  const uart_config_t uart_config = {
      .baud_rate = CONFIG_CONSOLE_UART_BAUDRATE,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
  };

  if (!s_uart_disabled) {
    uart_param_config(UART_NUM, &uart_config);
    uart_driver_install(UART_NUM, BUF_SIZE * 2, 0, 0, NULL, 0);
    ESP_LOGI("SerialManager", "UART installed: RX buffer=%d bytes", BUF_SIZE * 2);
  }

#if JTAG_SUPPORTED
  usb_serial_jtag_driver_config_t usb_serial_jtag_config = {
      .rx_buffer_size = BUF_SIZE,
      .tx_buffer_size = BUF_SIZE,
  };
  usb_serial_jtag_driver_install(&usb_serial_jtag_config);
  ESP_LOGI("SerialManager", "USB-JTAG installed: RX=%d TX=%d bytes", BUF_SIZE, BUF_SIZE);
#endif

  commandQueue = xQueueCreate(6, sizeof(SerialCommand));
  ESP_LOGI("SerialManager", "Command queue created: depth=6, item_size=%u bytes", sizeof(SerialCommand));

  xTaskCreate(serial_task, "SerialTask",  5120, NULL, 2, &s_serial_task_handle);
  ESP_LOGI("SerialManager", "Serial task created");
  s_serial_initialized = true;
  if (!s_uart_disabled) {
    command_history_init();
    glog("Serial Started...\n");
  } else {
    glog("Serial Disabled (template: somethingsomething)\n");
  }
}

void serial_manager_deinit() {
  if (!s_serial_initialized) {
    return;
  }
  if (s_serial_task_handle) {
    vTaskDelete(s_serial_task_handle);
    s_serial_task_handle = NULL;
  }
#if JTAG_SUPPORTED
  usb_serial_jtag_driver_uninstall();
#endif
  uart_driver_delete(UART_NUM);

  if (commandQueue) {
    vQueueDelete(commandQueue);
    commandQueue = NULL;
  }
  s_serial_initialized = false;
}

int serial_manager_get_uart_num() {
    return (int)UART_NUM;
}

int handle_serial_command(const char *input) {
  // Handle peer commands with logging and proper remote flag management
  if (strncmp(input, "peer:", 5) == 0) {
    const char* actual_command = input + 5;
    esp_comm_manager_set_remote_command_flag(true);
    glog("Received command from peer: %s\n", actual_command);
    glog("Executing received command: %s\n", actual_command);
    int result = handle_serial_command(actual_command);
    esp_comm_manager_set_remote_command_flag(false);
    return result;
  }
  
  char input_copy[SERIAL_BUFFER_SIZE];
  size_t input_len = strlen(input);
  if (input_len >= sizeof(input_copy)) {
    input_len = sizeof(input_copy) - 1;
  }
  memcpy(input_copy, input, input_len);
  input_copy[input_len] = '\0';
  char *argv[10];
  int argc = 0;
  char *p = input_copy;

  while (*p != '\0' && argc < 10) {
    while (isspace((unsigned char)*p)) {
      p++;
    }

    if (*p == '\0') {
      break;
    }

    if (*p == '"' || *p == '\'') {
      // Handle quoted arguments
      char quote = *p++;
      argv[argc++] = p; // Start of the argument

      while (*p != '\0' && *p != quote) {
        p++;
      }

      if (*p == quote) {
        *p = '\0'; // Null-terminate the argument
        p++;
      } else {
        // Handle missing closing quote
        printf("Error: Missing closing quote\n");
        return ESP_ERR_INVALID_ARG;
      }
    } else {
      // Handle unquoted arguments
      argv[argc++] = p; // Start of the argument

      while (*p != '\0' && !isspace((unsigned char)*p)) {
        p++;
      }

      if (*p != '\0') {
        *p = '\0'; // Null-terminate the argument
        p++;
      }
    }
  }

  if (argc == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  CommandFunction cmd_func = find_command(argv[0]);
  if (cmd_func != NULL) {
    // Add command to history before executing
    command_history_add(input);
    cmd_func(argc, argv);
    return ESP_OK;
  } else {
    // Add command to history even if unknown
    command_history_add(input);
    handle_unknown_command(argv[0]);
    return ESP_ERR_INVALID_ARG;
  }
}

void simulateCommand(const char *commandString) {
  if (commandQueue) {
    SerialCommand command;
    strncpy(command.command, commandString, sizeof(command.command) - 1);
    command.command[sizeof(command.command) - 1] = '\0';
    if (xQueueSend(commandQueue, &command, 0) == pdTRUE) {
      return;
    }
  }
  handle_serial_command(commandString);
}