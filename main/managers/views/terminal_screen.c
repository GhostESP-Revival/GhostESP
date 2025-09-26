#include "managers/views/terminal_screen.h"
#include "core/serial_manager.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "managers/views/main_menu_screen.h"
#include "managers/wifi_manager.h"
#include "managers/display_manager.h"
#include "esp_timer.h"
#include <stdlib.h>
#include <string.h>

extern View keyboard_view;
extern void keyboard_view_set_return_view(View *view);

static View *terminal_return_view = NULL;

/*
 * MEMORY-EFFICIENT TERMINAL IMPLEMENTATIONS
 * =========================================
 *
 * This file provides 4 different approaches for terminal text display:
 *
 * 1. LVGL TEXTAREA (TERMINAL_IMPLEMENTATION_TEXTAREA)
 *    - Uses single LVGL TextArea widget with built-in scrolling
 *    - Memory: ~4KB + text buffer (grows with content)
 *    - Pros: Simple, automatic scrolling, built-in text handling
 *    - Cons: Single large text buffer, memory grows with content
 *
 * 2. SINGLE LABEL (TERMINAL_IMPLEMENTATION_SINGLELABEL)
 *    - Uses single LVGL label with manual text management
 *    - Memory: ~2KB + line buffer (fixed size)
 *    - Pros: Fixed memory footprint, line-based management
 *    - Cons: Rebuilds entire text on each update, complex text handling
 *
 * 3. RING BUFFER (TERMINAL_IMPLEMENTATION_RINGBUFFER)
 *    - Ring buffer with recycled LVGL label objects
 *    - Memory: ~1KB + (50 * line objects) ~3-5KB fixed
 *    - Pros: Constant memory usage, object recycling, good performance
 *    - Cons: Limited to fixed number of lines, some LVGL objects
 *
 * 4. CANVAS RENDERING (TERMINAL_IMPLEMENTATION_CANVAS)
 *    - Canvas-based rendering with manual drawing
 *    - Memory: ~1KB + canvas buffer (~15KB for 320x500@16bit)
 *    - Pros: Minimal LVGL objects, maximum memory control, custom rendering
 *    - Cons: Complex implementation, manual text layout, no built-in scrolling
 *
 * RECOMMENDATIONS:
 * - Use CANVAS (4) for maximum memory efficiency and control
 * - Use RING BUFFER (3) for good balance of memory/performance
 * - Use TEXTAREA (1) for simplicity if memory isn't critical
 * - Avoid SINGLE LABEL (2) for frequent updates due to rebuild overhead
 *
 * Change TERMINAL_IMPLEMENTATION define above to switch between methods.
 */
#define TERMINAL_IMPLEMENTATION_TEXTAREA    1  // Uses LVGL TextArea with scrolling
#define TERMINAL_IMPLEMENTATION_SINGLELABEL 2  // Single label with manual management
#define TERMINAL_IMPLEMENTATION_RINGBUFFER  3  // Ring buffer with line recycling
#define TERMINAL_IMPLEMENTATION_CANVAS      4  // Canvas-based rendering (most memory efficient)

// Choose implementation (change this to switch methods)
// 1: LVGL TextArea, 2: Single Label, 3: Ring Buffer, 4: Canvas
#define TERMINAL_IMPLEMENTATION TERMINAL_IMPLEMENTATION_CANVAS

#include "lvgl.h"
#include "managers/settings_manager.h"

static const char *TAG = "Terminal";
static lv_obj_t *terminal_page = NULL;
static SemaphoreHandle_t terminal_mutex = NULL;
static bool retry_cleanup_flag = false;
static lv_timer_t *terminal_cleanup_retry_timer = NULL;
static bool terminal_active = false;
static bool is_stopping = false;
static bool terminal_initialized = false; // Flag to track if terminal has been fully initialized
#define MAX_TEXT_LENGTH 4096
#define CLEANUP_THRESHOLD (MAX_TEXT_LENGTH * 3 / 4)
#define CLEANUP_AMOUNT (MAX_TEXT_LENGTH / 2)
#define MAX_QUEUE_SIZE 15
#define MAX_MESSAGE_SIZE 256
#define MIN_SCREEN_SIZE 239
#define BUTTON_SIZE 40
#define BUTTON_PADDING 5
#define MAX_MESSAGES_PER_BATCH 5  // Process max 5 messages per timer tick
#define PROCESSING_INTERVAL_MS 50 // Process messages every 50ms during bursts

static lv_obj_t *back_btn = NULL;
static lv_obj_t *input_label = NULL;
static size_t current_text_length = 0; // track total characters to manage memory
lv_timer_t *terminal_update_timer = NULL;
static unsigned long createdTimeInMs = 0;
#define ENCODER_DEBOUNCE_TIME_MS 500

static char input_buffer[128] = {0}; // keyboard input buffer
static int input_len = 0; // input length counter

static void scroll_terminal_up(void);
static void scroll_terminal_down(void);
static void stop_all_operations(void);

// keyboard function predefs
static void submit_text();
static void add_char_to_buffer(char c);
static void remove_char_from_buffer();
static void update_input_label();

typedef struct {
  char messages[MAX_QUEUE_SIZE][MAX_MESSAGE_SIZE];
  int head;
  int tail;
  int count;
} MessageQueue;

static MessageQueue message_queue = {.head = 0, .tail = 0, .count = 0};

// Add a pre-initialization queue for messages that arrive before terminal is ready
static MessageQueue pre_init_message_queue = {.head = 0, .tail = 0, .count = 0};

static void submit_text() {
    if (input_len > 0) {
      char prompt_buf[sizeof(input_buffer) + 4]; // +4 for "> " and null terminator
      snprintf(prompt_buf, sizeof(prompt_buf), "> %s", input_buffer); // format the prompt
      terminal_view_add_text(prompt_buf); // add prompt before the command when printing to screen
      simulateCommand(input_buffer); // execute the command
      memset(input_buffer, 0, sizeof(input_buffer)); // clear the input buffer
      input_len = 0; // reset input length
      update_input_label(); // update the input label to show empty state
    }
}

static void add_char_to_buffer(char c) {
  if (input_len < sizeof(input_buffer) - 1) {
    input_buffer[input_len++] = c;
    input_buffer[input_len] = '\0';
    update_input_label();
  }
}

static void remove_char_from_buffer() {
  if (input_len > 0) {
    input_buffer[--input_len] = '\0';
    update_input_label();
  }
}
static void update_input_label() {
    if (input_label) {
        lv_label_set_text(input_label, input_buffer);
    }
}

static void queue_message_chunk(const char *chunk) {
  if (message_queue.count >= MAX_QUEUE_SIZE) {
    message_queue.head = (message_queue.head + 1) % MAX_QUEUE_SIZE;
    message_queue.count--;
  }
  strncpy(message_queue.messages[message_queue.tail], chunk, MAX_MESSAGE_SIZE - 1);
  message_queue.messages[message_queue.tail][MAX_MESSAGE_SIZE - 1] = '\0';
  message_queue.tail = (message_queue.tail + 1) % MAX_QUEUE_SIZE;
  message_queue.count++;
}

static void queue_message(const char *text) {
  if (!text) return;
  
  size_t text_len = strlen(text);
  if (text_len <= MAX_MESSAGE_SIZE - 1) {
    // Message fits in one chunk
    queue_message_chunk(text);
    return;
  }
  
  // Split large message into chunks
  const char *pos = text;
  while (*pos) {
    size_t chunk_size = MAX_MESSAGE_SIZE - 1;
    
    // Try to break at word boundary if possible
    if (text_len > chunk_size) {
      const char *space = pos + chunk_size - 1;
      while (space > pos && *space != ' ' && *space != '\n' && *space != '\t') {
        space--;
      }
      if (space > pos) {
        chunk_size = space - pos + 1; // Include the space
      }
    } else {
      chunk_size = text_len;
    }
    
    char chunk[MAX_MESSAGE_SIZE];
    strncpy(chunk, pos, chunk_size);
    chunk[chunk_size] = '\0';
    
    queue_message_chunk(chunk);
    
    pos += chunk_size;
    text_len -= chunk_size;
  }
}

static void clear_message_queue(void) {
  message_queue.head = 0;
  message_queue.tail = 0;
  message_queue.count = 0;
}

static void clear_pre_init_message_queue(void) {
  pre_init_message_queue.head = 0;
  pre_init_message_queue.tail = 0;
  pre_init_message_queue.count = 0;
}

static void process_queued_messages(void) {
  if (!terminal_active || !terminal_page || is_stopping) {
    return;
  }

  // This function runs in LVGL task context - no mutex needed for LVGL operations!
  // Only protect message queue access with minimal critical sections
  
  lv_obj_t *last_item = NULL;
  int processed_count = 0;
  bool need_cleanup = false;
  
  // Process messages in batches to prevent UI overload
  while (processed_count < MAX_MESSAGES_PER_BATCH) {
    // Critical section: only for message queue access
    char msg_buffer[MAX_MESSAGE_SIZE];
    bool has_message = false;
    
    if (terminal_mutex && xSemaphoreTake(terminal_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
      if (message_queue.count > 0) {
        // Copy message out of queue quickly
        strncpy(msg_buffer, message_queue.messages[message_queue.head], MAX_MESSAGE_SIZE - 1);
        msg_buffer[MAX_MESSAGE_SIZE - 1] = '\0';
        
        // Dequeue immediately
        message_queue.head = (message_queue.head + 1) % MAX_QUEUE_SIZE;
        message_queue.count--;
        has_message = true;
      }
      xSemaphoreGive(terminal_mutex);
    }
    
    if (!has_message) {
      break; // No more messages to process
    }
    
    // LVGL operations - use memory-efficient terminal implementation
    terminal_add_text(msg_buffer);
    // For implementations that need last_item reference for scrolling
    #if TERMINAL_IMPLEMENTATION == TERMINAL_IMPLEMENTATION_RINGBUFFER
    // Ring buffer implementation handles scrolling internally
    #else
    last_item = terminal_page; // Use terminal container for scrolling
    #endif

    current_text_length += strlen(msg_buffer);

    // Mark cleanup needed but don't do it during object creation
    if (current_text_length > CLEANUP_THRESHOLD) {
        need_cleanup = true;
    }
    
    processed_count++;
  }
  
  // Only do cleanup AFTER all new objects are created to prevent race conditions
  if (need_cleanup) {
    size_t target_len = (current_text_length > CLEANUP_AMOUNT) ? current_text_length - CLEANUP_AMOUNT : 0;
    while (current_text_length > target_len && lv_obj_get_child_cnt(terminal_page) > 0) {
      lv_obj_t *oldest = lv_obj_get_child(terminal_page, 0);
      
      // Get text length before deletion
      size_t old_len = 0;
      const char *old_text = lv_label_get_text(oldest);
      if (old_text) {
        old_len = strlen(old_text);
      }
      
      // Update counter
      if (current_text_length > old_len) {
        current_text_length -= old_len;
      } else {
        current_text_length = 0;
      }
      
      // Safe deletion after all object creation is complete
      lv_obj_del(oldest);
    }
  }
  
  // Check if more messages need processing (quick check)
  bool has_more_messages = false;
  if (terminal_mutex && xSemaphoreTake(terminal_mutex, pdMS_TO_TICKS(1)) == pdTRUE) {
    has_more_messages = (message_queue.count > 0);
    xSemaphoreGive(terminal_mutex);
  }
  
  // Schedule next processing cycle if needed
  if (has_more_messages && terminal_update_timer) {
    lv_timer_set_period(terminal_update_timer, PROCESSING_INTERVAL_MS);
  }

  // Scroll to the last item added in this batch
  if (last_item) {
    lv_obj_scroll_to_view(last_item, LV_ANIM_OFF);
  }
}

static void process_queued_messages_callback(lv_timer_t * timer) {
    // This now runs within the LVGL task context - no race conditions!
    process_queued_messages();
}


static int (*default_log_vprintf)(const char *, va_list) = NULL;

static void scroll_terminal_up(void) {
  terminal_scroll(1); // Scroll up
  ESP_LOGI(TAG, "Scroll up triggered");
}

static void scroll_terminal_down(void) {
  terminal_scroll(-1); // Scroll down
  ESP_LOGI(TAG, "Scroll down triggered");
}

static void stop_all_operations(void) {
    terminal_active = false;
    is_stopping = true;

    // Send all stop commands
    simulateCommand("stop");

    vTaskDelay(pdMS_TO_TICKS(20));

    // Now, switch the view
    if (terminal_return_view) {
        display_manager_switch_view(terminal_return_view);
        terminal_return_view = NULL; // Clear after use
    } else {
        display_manager_switch_view(&main_menu_view); // Fallback
    }
    ESP_LOGI(TAG, "Stop all operations triggered");
}
#if defined(CONFIG_USE_HW_KB) || defined(CONFIG_USE_TOUCHSCREEN)
void text_box_click_cb(lv_event_t *e){
  ESP_LOGI(TAG, "Text box clicked");
  printf("Text box clicked\n");

  keyboard_view_set_return_view(&terminal_view);
  display_manager_switch_view(&keyboard_view);

  // If using a hardware keyboard, we can ignore this click
}
#endif

// ===== MEMORY-EFFICIENT TERMINAL IMPLEMENTATIONS =====

// Implementation 1: LVGL TextArea with automatic scrolling
#if TERMINAL_IMPLEMENTATION == TERMINAL_IMPLEMENTATION_TEXTAREA
static lv_obj_t *terminal_textarea = NULL;

static void terminal_textarea_create(void) {
    terminal_page = lv_textarea_create(terminal_view.root);
    lv_obj_set_pos(terminal_page, 0, STATUS_BAR_HEIGHT);
    lv_obj_set_size(terminal_page, LV_HOR_RES, LV_VER_RES - STATUS_BAR_HEIGHT - input_area_height);
    lv_obj_set_style_bg_color(terminal_page, lv_color_black(), 0);
    lv_obj_set_style_text_color(terminal_page, lv_color_hex(settings_get_terminal_text_color(&G_Settings)), 0);
    lv_obj_set_style_text_font(terminal_page, &lv_font_montserrat_10, 0);
    lv_obj_set_style_border_width(terminal_page, 0, 0);
    lv_obj_set_style_radius(terminal_page, 0, 0);
    lv_textarea_set_text(terminal_page, "");
    lv_textarea_set_cursor_pos(terminal_page, 0);
    lv_textarea_set_cursor_click_pos(terminal_page, false);
    lv_obj_set_scrollbar_mode(terminal_page, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(terminal_page, LV_DIR_VER);

    terminal_textarea = terminal_page;
}

static void terminal_textarea_add_text(const char *text) {
    if (!terminal_textarea || !text || is_stopping) return;

    // Get current text and append new text
    const char *current_text = lv_textarea_get_text(terminal_textarea);
    size_t current_len = current_text ? strlen(current_text) : 0;
    size_t text_len = strlen(text);

    // Limit total text length to prevent memory issues
    if (current_len + text_len > MAX_TEXT_LENGTH) {
        // Remove oldest lines to make room
        char *new_text = (char *)malloc(MAX_TEXT_LENGTH + 1);
        if (!new_text) return;

        // Find the position to start from (keep last MAX_TEXT_LENGTH/2 characters)
        size_t keep_start = current_len + text_len - MAX_TEXT_LENGTH/2;
        if (keep_start < current_len) {
            strcpy(new_text, current_text + keep_start);
        } else {
            strcpy(new_text, current_text);
        }
        strcat(new_text, text);
        lv_textarea_set_text(terminal_textarea, new_text);
        free(new_text);
    } else {
        lv_textarea_add_text(terminal_textarea, text);
    }

    // Auto-scroll to bottom
    lv_textarea_set_cursor_pos(terminal_textarea, LV_TEXTAREA_CURSOR_LAST);
}

static void terminal_textarea_scroll(int direction) {
    if (!terminal_textarea) return;

    lv_coord_t y = lv_obj_get_scroll_y(terminal_textarea);
    lv_coord_t scroll_step = lv_obj_get_height(terminal_textarea) / 4;

    if (direction > 0) { // Scroll up
        lv_obj_scroll_to_y(terminal_textarea, y - scroll_step, LV_ANIM_OFF);
    } else { // Scroll down
        lv_obj_scroll_to_y(terminal_textarea, y + scroll_step, LV_ANIM_OFF);
    }
}
#endif

// Implementation 2: Single label with manual text management
#if TERMINAL_IMPLEMENTATION == TERMINAL_IMPLEMENTATION_SINGLELABEL
#define MAX_TERMINAL_LINES 100
#define MAX_LINE_LENGTH 256

typedef struct {
    char lines[MAX_TERMINAL_LINES][MAX_LINE_LENGTH];
    int head;
    int count;
    int total_chars;
} TerminalBuffer;

static TerminalBuffer term_buffer = {.head = 0, .count = 0, .total_chars = 0};
static lv_obj_t *terminal_label = NULL;

static void terminal_singlelabel_create(void) {
    terminal_page = lv_obj_create(terminal_view.root);
    lv_obj_set_pos(terminal_page, 0, STATUS_BAR_HEIGHT);
    lv_obj_set_size(terminal_page, LV_HOR_RES, LV_VER_RES - STATUS_BAR_HEIGHT - input_area_height);
    lv_obj_set_style_bg_color(terminal_page, lv_color_black(), 0);
    lv_obj_set_style_border_width(terminal_page, 0, 0);
    lv_obj_set_style_radius(terminal_page, 0, 0);
    lv_obj_set_scrollbar_mode(terminal_page, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(terminal_page, LV_DIR_VER);

    terminal_label = lv_label_create(terminal_page);
    lv_obj_set_size(terminal_label, LV_HOR_RES - 10, LV_VER_RES);
    lv_obj_set_style_text_color(terminal_label, lv_color_hex(settings_get_terminal_text_color(&G_Settings)), 0);
    lv_obj_set_style_text_font(terminal_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_align(terminal_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_text(terminal_label, "");
    lv_label_set_long_mode(terminal_label, LV_LABEL_LONG_WRAP);
}

static void terminal_singlelabel_add_text(const char *text) {
    if (!terminal_label || !text || is_stopping) return;

    // Split text into lines and add to buffer
    char temp[MAX_LINE_LENGTH];
    const char *pos = text;
    while (*pos) {
        // Copy next line
        int i = 0;
        while (*pos && *pos != '\n' && i < MAX_LINE_LENGTH - 1) {
            temp[i++] = *pos++;
        }
        temp[i] = '\0';

        if (*pos == '\n') pos++; // Skip newline

        if (i > 0) {
            // Add line to buffer
            if (term_buffer.count >= MAX_TERMINAL_LINES) {
                // Remove oldest line
                term_buffer.total_chars -= strlen(term_buffer.lines[term_buffer.head]);
                term_buffer.head = (term_buffer.head + 1) % MAX_TERMINAL_LINES;
            } else {
                term_buffer.count++;
            }

            strcpy(term_buffer.lines[(term_buffer.head + term_buffer.count - 1) % MAX_TERMINAL_LINES], temp);
            term_buffer.total_chars += strlen(temp);
        }
    }

    // Rebuild text from buffer
    char *display_text = (char *)malloc(term_buffer.total_chars + term_buffer.count + 1);
    if (!display_text) return;

    int pos_in_text = 0;
    for (int i = 0; i < term_buffer.count; i++) {
        int line_idx = (term_buffer.head + i) % MAX_TERMINAL_LINES;
        strcpy(display_text + pos_in_text, term_buffer.lines[line_idx]);
        pos_in_text += strlen(term_buffer.lines[line_idx]);
        display_text[pos_in_text++] = '\n';
    }
    display_text[pos_in_text] = '\0';

    lv_label_set_text(terminal_label, display_text);
    free(display_text);

    // Auto-scroll to bottom
    lv_obj_scroll_to_view(terminal_label, LV_ANIM_OFF);
}

static void terminal_singlelabel_scroll(int direction) {
    if (!terminal_label) return;

    lv_coord_t y = lv_obj_get_scroll_y(terminal_page);
    lv_coord_t scroll_step = lv_obj_get_height(terminal_page) / 4;

    if (direction > 0) { // Scroll up
        lv_obj_scroll_to_y(terminal_page, y - scroll_step, LV_ANIM_OFF);
    } else { // Scroll down
        lv_obj_scroll_to_y(terminal_page, y + scroll_step, LV_ANIM_OFF);
    }
}
#endif

// Implementation 3: Ring buffer with line recycling
#if TERMINAL_IMPLEMENTATION == TERMINAL_IMPLEMENTATION_RINGBUFFER
#define RING_BUFFER_SIZE 50
#define LINE_BUFFER_SIZE 128

typedef struct {
    char lines[RING_BUFFER_SIZE][LINE_BUFFER_SIZE];
    int head;
    int count;
    lv_obj_t *line_objects[RING_BUFFER_SIZE];
} RingTerminal;

static RingTerminal ring_term = {.head = 0, .count = 0};

static void terminal_ringbuffer_create(void) {
    terminal_page = lv_list_create(terminal_view.root);
    lv_obj_set_pos(terminal_page, 0, STATUS_BAR_HEIGHT);
    lv_obj_set_size(terminal_page, LV_HOR_RES, LV_VER_RES - STATUS_BAR_HEIGHT - input_area_height);
    lv_obj_set_style_bg_color(terminal_page, lv_color_black(), 0);
    lv_obj_set_style_pad_all(terminal_page, 0, 0);
    lv_obj_set_style_radius(terminal_page, 0, 0);
    lv_obj_set_scrollbar_mode(terminal_page, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(terminal_page, 0, 0);
    lv_obj_set_scroll_dir(terminal_page, LV_DIR_VER);
}

static void terminal_ringbuffer_add_text(const char *text) {
    if (!terminal_page || !text || is_stopping) return;

    // Split text into lines
    char temp[LINE_BUFFER_SIZE];
    const char *pos = text;
    while (*pos) {
        int i = 0;
        while (*pos && *pos != '\n' && i < LINE_BUFFER_SIZE - 1) {
            temp[i++] = *pos++;
        }
        temp[i] = '\0';

        if (*pos == '\n') pos++;

        if (i > 0) {
            // Add line to ring buffer
            if (ring_term.count >= RING_BUFFER_SIZE) {
                // Remove oldest line object
                int oldest_idx = ring_term.head;
                if (ring_term.line_objects[oldest_idx]) {
                    lv_obj_del(ring_term.line_objects[oldest_idx]);
                    ring_term.line_objects[oldest_idx] = NULL;
                }
                ring_term.head = (ring_term.head + 1) % RING_BUFFER_SIZE;
            } else {
                ring_term.count++;
            }

            int current_idx = (ring_term.head + ring_term.count - 1) % RING_BUFFER_SIZE;
            strcpy(ring_term.lines[current_idx], temp);

            // Create or update line object
            if (ring_term.line_objects[current_idx]) {
                lv_label_set_text(ring_term.line_objects[current_idx], temp);
            } else {
                lv_obj_t *line_obj = lv_list_add_text(terminal_page, temp);
                lv_obj_set_style_bg_opa(line_obj, LV_OPA_TRANSP, 0);
                lv_obj_set_style_text_color(line_obj, lv_color_hex(settings_get_terminal_text_color(&G_Settings)), 0);
                lv_obj_set_style_text_font(line_obj, &lv_font_montserrat_10, 0);
                ring_term.line_objects[current_idx] = line_obj;
            }
        }
    }

    // Scroll to bottom
    if (ring_term.count > 0) {
        int last_idx = (ring_term.head + ring_term.count - 1) % RING_BUFFER_SIZE;
        if (ring_term.line_objects[last_idx]) {
            lv_obj_scroll_to_view(ring_term.line_objects[last_idx], LV_ANIM_OFF);
        }
    }
}

static void terminal_ringbuffer_scroll(int direction) {
    if (!terminal_page) return;

    lv_coord_t y = lv_obj_get_scroll_y(terminal_page);
    lv_coord_t scroll_step = lv_obj_get_height(terminal_page) / 4;

    if (direction > 0) { // Scroll up
        lv_obj_scroll_to_y(terminal_page, y - scroll_step, LV_ANIM_OFF);
    } else { // Scroll down
        lv_obj_scroll_to_y(terminal_page, y + scroll_step, LV_ANIM_OFF);
    }
}
#endif

// Implementation 4: Canvas-based rendering (most memory efficient)
#if TERMINAL_IMPLEMENTATION == TERMINAL_IMPLEMENTATION_CANVAS
#define CANVAS_LINES 25  // Number of visible lines
#define CANVAS_LINE_HEIGHT 20  // Height of each line in pixels
#define CANVAS_BUFFER_SIZE 100  // Total lines in buffer

typedef struct {
    char lines[CANVAS_BUFFER_SIZE][MAX_MESSAGE_SIZE];
    int head;
    int count;
    int scroll_pos;  // Current scroll position (0 = newest at bottom)
    lv_obj_t *canvas;
    lv_draw_buf_t *draw_buf;
    lv_canvas_t *canvas_obj;
} CanvasTerminal;

static CanvasTerminal canvas_term = {.head = 0, .count = 0, .scroll_pos = 0};

static void terminal_canvas_create(void) {
    // Create container for canvas
    terminal_page = lv_obj_create(terminal_view.root);
    lv_obj_set_pos(terminal_page, 0, STATUS_BAR_HEIGHT);
    lv_obj_set_size(terminal_page, LV_HOR_RES, LV_VER_RES - STATUS_BAR_HEIGHT - input_area_height);
    lv_obj_set_style_bg_color(terminal_page, lv_color_black(), 0);
    lv_obj_set_style_border_width(terminal_page, 0, 0);
    lv_obj_set_style_radius(terminal_page, 0, 0);
    lv_obj_set_scrollbar_mode(terminal_page, LV_SCROLLBAR_MODE_OFF);

    // Create canvas
    canvas_term.canvas = lv_canvas_create(terminal_page);
    lv_obj_set_size(canvas_term.canvas, LV_HOR_RES, CANVAS_LINES * CANVAS_LINE_HEIGHT);
    lv_canvas_set_buffer(canvas_term.canvas, NULL, LV_HOR_RES, CANVAS_LINES * CANVAS_LINE_HEIGHT, LV_COLOR_FORMAT_RGB565);

    // Create draw buffer
    size_t buf_size = LV_HOR_RES * CANVAS_LINES * CANVAS_LINE_HEIGHT * sizeof(lv_color16_t);
    canvas_term.draw_buf = lv_draw_buf_create(LV_HOR_RES, CANVAS_LINES * CANVAS_LINE_HEIGHT, LV_COLOR_FORMAT_RGB565, buf_size);
    if (!canvas_term.draw_buf) {
        ESP_LOGE(TAG, "Failed to create canvas draw buffer");
        return;
    }

    lv_canvas_set_buffer(canvas_term.canvas, canvas_term.draw_buf->data, LV_HOR_RES, CANVAS_LINES * CANVAS_LINE_HEIGHT, LV_COLOR_FORMAT_RGB565);
    canvas_term.canvas_obj = (lv_canvas_t *)canvas_term.canvas;

    // Clear canvas with black background
    lv_canvas_fill_bg(canvas_term.canvas, lv_color_black(), LV_OPA_COVER);
}

static void terminal_canvas_add_text(const char *text) {
    if (!canvas_term.canvas || !text || is_stopping) return;

    // Add text to buffer
    char temp[MAX_MESSAGE_SIZE];
    const char *pos = text;
    while (*pos) {
        int i = 0;
        while (*pos && *pos != '\n' && i < MAX_MESSAGE_SIZE - 1) {
            temp[i++] = *pos++;
        }
        temp[i] = '\0';

        if (*pos == '\n') pos++;

        if (i > 0) {
            // Add line to buffer
            if (canvas_term.count >= CANVAS_BUFFER_SIZE) {
                canvas_term.head = (canvas_term.head + 1) % CANVAS_BUFFER_SIZE;
            } else {
                canvas_term.count++;
            }

            int current_idx = (canvas_term.head + canvas_term.count - 1) % CANVAS_BUFFER_SIZE;
            strcpy(canvas_term.lines[current_idx], temp);
        }
    }

    // Redraw canvas
    lv_canvas_fill_bg(canvas_term.canvas, lv_color_black(), LV_OPA_COVER);

    // Draw visible lines
    int start_line = canvas_term.count - CANVAS_LINES - canvas_term.scroll_pos;
    if (start_line < 0) start_line = 0;

    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.font = &lv_font_montserrat_10;
    label_dsc.color = lv_color_hex(settings_get_terminal_text_color(&G_Settings));
    label_dsc.align = LV_TEXT_ALIGN_LEFT;

    for (int i = 0; i < CANVAS_LINES && start_line + i < canvas_term.count; i++) {
        int line_idx = (canvas_term.head + start_line + i) % CANVAS_BUFFER_SIZE;
        lv_area_t area = {
            .x1 = 0,
            .y1 = i * CANVAS_LINE_HEIGHT,
            .x2 = LV_HOR_RES - 1,
            .y2 = (i + 1) * CANVAS_LINE_HEIGHT - 1
        };

        lv_canvas_draw_text(canvas_term.canvas, area.x1, area.y1, area.x2 - area.x1, &label_dsc, canvas_term.lines[line_idx]);
    }

    lv_obj_invalidate(canvas_term.canvas);
}

static void terminal_canvas_scroll(int direction) {
    if (direction > 0) { // Scroll up
        if (canvas_term.scroll_pos < canvas_term.count - CANVAS_LINES) {
            canvas_term.scroll_pos++;
        }
    } else { // Scroll down
        if (canvas_term.scroll_pos > 0) {
            canvas_term.scroll_pos--;
        }
    }

    // Redraw after scroll
    if (canvas_term.count > 0) {
        terminal_canvas_add_text(""); // Trigger redraw
    }
}

static void terminal_canvas_destroy(void) {
    if (canvas_term.draw_buf) {
        lv_draw_buf_free(canvas_term.draw_buf);
        canvas_term.draw_buf = NULL;
    }
    canvas_term.canvas = NULL;
    canvas_term.canvas_obj = NULL;
}
#endif

// Unified interface functions
static void terminal_create(void) {
    input_area_height = 0;
    if (show_back_btn && show_input_bar) {
        input_area_height = (back_button_height > (textbox_height + padding) ? back_button_height : (textbox_height + padding));
    } else if (show_back_btn) {
        input_area_height = back_button_height;
    } else if (show_input_bar) {
        input_area_height = textbox_height + padding;
    } else {
        input_area_height = 0;
    }

    int textarea_height = LV_VER_RES - STATUS_BAR_HEIGHT - input_area_height;

    #if TERMINAL_IMPLEMENTATION == TERMINAL_IMPLEMENTATION_TEXTAREA
    terminal_textarea_create();
    #elif TERMINAL_IMPLEMENTATION == TERMINAL_IMPLEMENTATION_SINGLELABEL
    terminal_singlelabel_create();
    #elif TERMINAL_IMPLEMENTATION == TERMINAL_IMPLEMENTATION_RINGBUFFER
    terminal_ringbuffer_create();
    #elif TERMINAL_IMPLEMENTATION == TERMINAL_IMPLEMENTATION_CANVAS
    terminal_canvas_create();
    #endif
}

static void terminal_add_text(const char *text) {
    #if TERMINAL_IMPLEMENTATION == TERMINAL_IMPLEMENTATION_TEXTAREA
    terminal_textarea_add_text(text);
    #elif TERMINAL_IMPLEMENTATION == TERMINAL_IMPLEMENTATION_SINGLELABEL
    terminal_singlelabel_add_text(text);
    #elif TERMINAL_IMPLEMENTATION == TERMINAL_IMPLEMENTATION_RINGBUFFER
    terminal_ringbuffer_add_text(text);
    #elif TERMINAL_IMPLEMENTATION == TERMINAL_IMPLEMENTATION_CANVAS
    terminal_canvas_add_text(text);
    #endif
}

static void terminal_scroll(int direction) {
    #if TERMINAL_IMPLEMENTATION == TERMINAL_IMPLEMENTATION_TEXTAREA
    terminal_textarea_scroll(direction);
    #elif TERMINAL_IMPLEMENTATION == TERMINAL_IMPLEMENTATION_SINGLELABEL
    terminal_singlelabel_scroll(direction);
    #elif TERMINAL_IMPLEMENTATION == TERMINAL_IMPLEMENTATION_RINGBUFFER
    terminal_ringbuffer_scroll(direction);
    #elif TERMINAL_IMPLEMENTATION == TERMINAL_IMPLEMENTATION_CANVAS
    terminal_canvas_scroll(direction);
    #endif
}

static void terminal_destroy(void) {
    #if TERMINAL_IMPLEMENTATION == TERMINAL_IMPLEMENTATION_CANVAS
    terminal_canvas_destroy();
    #endif
}

static void back_btn_event_cb(lv_event_t *e) {
    stop_all_operations();
}

void terminal_view_create(void) {
    is_stopping = false;
    if (terminal_view.root != NULL) {
        return;
    }

    if (!terminal_mutex) {
        terminal_mutex = xSemaphoreCreateMutex();
        if (!terminal_mutex) {
            ESP_LOGE(TAG, "Failed to create terminal mutex");
            return;
        }
    }

    terminal_active = true;

    terminal_view.root = lv_obj_create(lv_scr_act());
    lv_obj_set_size(terminal_view.root, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(terminal_view.root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(terminal_view.root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(terminal_view.root, 0, 0); // Remove border
    lv_obj_set_style_radius(terminal_view.root, 0, 0);
    lv_obj_set_scrollbar_mode(terminal_view.root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_all(terminal_view.root, 0, 0);

    const int STATUS_BAR_HEIGHT = 20;
    const int padding = 5;
    const int textbox_height = 40;

    int back_button_height = 0;
    bool show_back_btn = false;
    bool show_input_bar = false;

#ifdef CONFIG_USE_TOUCHSCREEN
    if (LV_HOR_RES > MIN_SCREEN_SIZE && LV_VER_RES > MIN_SCREEN_SIZE) {
        show_back_btn = true;
        back_button_height = BUTTON_SIZE + BUTTON_PADDING * 2;
    }
#endif

#if defined(CONFIG_USE_HW_KB) || defined(CONFIG_USE_TOUCHSCREEN)
    show_input_bar = true;
#endif

    // Calculate the height for the input area (input box + padding)
    int input_area_height = 0;
    if (show_back_btn && show_input_bar) {
        input_area_height = (back_button_height > (textbox_height + padding) ? back_button_height : (textbox_height + padding));
    } else if (show_back_btn) {
        input_area_height = back_button_height;
    } else if (show_input_bar) {
        input_area_height = textbox_height + padding;
    } else {
        input_area_height = 0;
    }

    // Calculate the height for the terminal readout area
    int textarea_height = LV_VER_RES - STATUS_BAR_HEIGHT - input_area_height;

    // Create the terminal using memory-efficient implementation
    terminal_create();

#ifdef CONFIG_USE_TOUCHSCREEN
    if (show_back_btn) {
        back_btn = lv_btn_create(terminal_view.root);
        lv_obj_set_size(back_btn, BUTTON_SIZE, BUTTON_SIZE);
        lv_obj_align(back_btn, LV_ALIGN_BOTTOM_LEFT, BUTTON_PADDING, -BUTTON_PADDING);
        lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x333333), LV_PART_MAIN);
        lv_obj_set_style_radius(back_btn, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_add_event_cb(back_btn, back_btn_event_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_set_style_border_width(back_btn, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(back_btn, 0, LV_PART_MAIN);
        lv_obj_t *back_label = lv_label_create(back_btn);
        lv_label_set_text(back_label, LV_SYMBOL_LEFT);
        lv_obj_center(back_label);

        lv_obj_update_layout(terminal_view.root);
        ESP_LOGW(TAG, "Back pos: x=%d, y=%d, w=%d, h=%d",
                 lv_obj_get_x(back_btn), lv_obj_get_y(back_btn),
                 lv_obj_get_width(back_btn), lv_obj_get_height(back_btn));
    }
#endif

#if defined(CONFIG_USE_HW_KB) || defined(CONFIG_USE_TOUCHSCREEN)
    if (show_input_bar) {
        int textbox_width = LV_HOR_RES - 2 * padding;
    #ifdef CONFIG_USE_TOUCHSCREEN
        if (show_back_btn) {
            textbox_width -= BUTTON_SIZE + 2 * BUTTON_PADDING;
        }
    #endif
        if (textbox_width < 40) textbox_width = 40;

        input_label = lv_label_create(terminal_view.root);
        lv_obj_set_size(input_label, textbox_width, textbox_height);
        lv_obj_set_style_bg_color(input_label, lv_color_hex(0x333333), 0);
        lv_obj_set_style_bg_opa(input_label, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(input_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_pad_all(input_label, padding, 0);
        lv_obj_set_style_radius(input_label, 0, 0);
        lv_obj_set_style_border_width(input_label, 0, 0);
        lv_obj_set_style_shadow_width(input_label, 0, 0);
        lv_obj_align(input_label, LV_ALIGN_BOTTOM_RIGHT, -padding, -padding);
        lv_obj_add_event_cb(input_label, text_box_click_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_flag(input_label, LV_OBJ_FLAG_CLICKABLE);
        lv_label_set_long_mode(input_label, LV_LABEL_LONG_CLIP);
        lv_label_set_text(input_label, "Type Command...");
        // Center vertically by adjusting vertical padding
        const lv_font_t *current_font = lv_obj_get_style_text_font(input_label, 0);
        int font_height = lv_font_get_line_height(current_font);
        int vertical_pad = (textbox_height - font_height) / 2;
        if (vertical_pad < 0) vertical_pad = 0; // Prevent negative padding
        lv_obj_set_style_pad_top(input_label, vertical_pad, 0);
        lv_obj_set_style_pad_bottom(input_label, vertical_pad, 0);
    }
#endif

    display_manager_add_status_bar("Terminal");

    if (!terminal_update_timer) {
        terminal_update_timer = lv_timer_create(process_queued_messages_callback, 50, NULL);
        if (!terminal_update_timer) {
            ESP_LOGE(TAG, "Failed to create terminal update timer");
        }
    }
    
    // Process any messages that were queued before terminal was initialized
    if (xSemaphoreTake(terminal_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        // Move all pre-initialization messages to the main message queue
        while (pre_init_message_queue.count > 0) {
            const char *msg = pre_init_message_queue.messages[pre_init_message_queue.head];
            queue_message(msg);
            // dequeue from pre-init queue
            pre_init_message_queue.head = (pre_init_message_queue.head + 1) % MAX_QUEUE_SIZE;
            pre_init_message_queue.count--;
        }
        xSemaphoreGive(terminal_mutex);
    }
    
    // Mark terminal as fully initialized
    terminal_initialized = true;
    
    createdTimeInMs = (unsigned long)(esp_timer_get_time() / 1000ULL);
}
static void terminal_retry_cleanup_cb(lv_timer_t *timer) {
    if (!retry_cleanup_flag) {
        lv_timer_del(timer);
        terminal_cleanup_retry_timer = NULL;
        return;
    }
    ESP_LOGI(TAG, "Retrying terminal cleanup...");
    // Try to destroy again
    retry_cleanup_flag = false;
    terminal_view_destroy();
    // If cleanup succeeds, the flag will stay false and timer will be deleted
    // If not, the flag will be set again and timer will keep running
}

void terminal_view_destroy(void) {
    // Signal all callbacks/timers to stop
    terminal_active = false;
    is_stopping = true;
    terminal_initialized = false; // Reset initialization flag

    // Clear message queue and reset state
    clear_message_queue();
    clear_pre_init_message_queue(); // Clear pre-initialization queue
    current_text_length = 0;
    input_len = 0;
    input_buffer[0] = '\0';

    // Delete timer first to prevent callbacks after objects are freed
    if (terminal_update_timer) {
        lv_timer_del(terminal_update_timer);
        terminal_update_timer = NULL;
    }

    // Safely delete LVGL objects
    if (terminal_mutex) {
        if (xSemaphoreTake(terminal_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            // Delete LVGL objects if they exist
            if (terminal_view.root) {
                lv_obj_del(terminal_view.root);
                terminal_view.root = NULL;
            }
            // Set all pointers to NULL to avoid dangling references
            terminal_page = NULL;
            back_btn = NULL;
            input_label = NULL;

            // Call implementation-specific cleanup
            terminal_destroy();

            vSemaphoreDelete(terminal_mutex); // Delete mutex directly after acquiring
            terminal_mutex = NULL;
        } else {
            ESP_LOGE(TAG, "Failed to acquire terminal mutex during destroy. A leak may occur.");
            retry_cleanup_flag = true; // Set flag to retry cleanup later
            if (!terminal_cleanup_retry_timer) {
                terminal_cleanup_retry_timer = lv_timer_create(terminal_retry_cleanup_cb, 250, NULL);
            }
        }
    } else {
        // If mutex is already NULL, still clear pointers
        terminal_view.root = NULL;
        terminal_page = NULL;
        back_btn = NULL;
        input_label = NULL;
    }

    // Final state reset
    is_stopping = false;
    if (terminal_cleanup_retry_timer) {
        lv_timer_del(terminal_cleanup_retry_timer);
        terminal_cleanup_retry_timer = NULL;
    }
}

void terminal_view_add_text(const char *text) {
  if (!text || is_stopping || text[0] == '\0') {
      return;
  }

  // If terminal is not yet initialized, queue messages in the pre-init queue
  if (!terminal_initialized) {
      // If mutex doesn't exist yet, just add directly to pre-init queue without semaphore
      if (!terminal_mutex) {
          // Direct access to pre-init queue without semaphore since it doesn't exist yet
          if (pre_init_message_queue.count >= MAX_QUEUE_SIZE) {
              pre_init_message_queue.head = (pre_init_message_queue.head + 1) % MAX_QUEUE_SIZE;
              pre_init_message_queue.count--;
          }
          strncpy(pre_init_message_queue.messages[pre_init_message_queue.tail], text, MAX_MESSAGE_SIZE - 1);
          pre_init_message_queue.messages[pre_init_message_queue.tail][MAX_MESSAGE_SIZE - 1] = '\0';
          pre_init_message_queue.tail = (pre_init_message_queue.tail + 1) % MAX_QUEUE_SIZE;
          pre_init_message_queue.count++;
      } else {
          // Mutex exists, use it to protect access
          if (xSemaphoreTake(terminal_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
              // Use the pre-initialization queue when terminal is not ready
              if (pre_init_message_queue.count >= MAX_QUEUE_SIZE) {
                  pre_init_message_queue.head = (pre_init_message_queue.head + 1) % MAX_QUEUE_SIZE;
                  pre_init_message_queue.count--;
              }
              strncpy(pre_init_message_queue.messages[pre_init_message_queue.tail], text, MAX_MESSAGE_SIZE - 1);
              pre_init_message_queue.messages[pre_init_message_queue.tail][MAX_MESSAGE_SIZE - 1] = '\0';
              pre_init_message_queue.tail = (pre_init_message_queue.tail + 1) % MAX_QUEUE_SIZE;
              pre_init_message_queue.count++;
              xSemaphoreGive(terminal_mutex);
          } else {
              ESP_LOGW(TAG, "Failed to acquire terminal mutex in add_text (pre-init)");
          }
      }
      return;
  }

  if (!terminal_mutex) {
      ESP_LOGW(TAG, "Attempted to add text while terminal is destroying. Ignoring.");
      return;
  }

  if (xSemaphoreTake(terminal_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      // Use new memory-efficient terminal implementation
      terminal_add_text(text);
      xSemaphoreGive(terminal_mutex);
  } else {
      ESP_LOGW(TAG, "Failed to acquire terminal mutex in add_text");
  }
}

void terminal_view_hardwareinput_callback(InputEvent *event) {
  if (event->type == INPUT_TYPE_TOUCH) {
    ESP_LOGW(TAG, "Touch event");
    if (event->data.touch_data.state != LV_INDEV_STATE_PR) {
      return;
    }
    int touch_x = event->data.touch_data.point.x;
    int touch_y = event->data.touch_data.point.y;
    ESP_LOGW(TAG, "Touch detected at x=%d, y=%d (screen: %dx%d)", touch_x, touch_y, LV_HOR_RES, LV_VER_RES);

    if (input_label){
      ESP_LOGI(TAG, "Input label exists, checking for click");
      // Check if the touch is within the input label area
      lv_obj_t *input_area = lv_obj_get_parent(input_label);
      int input_x_min = lv_obj_get_x(input_label);
      int input_x_max = input_x_min + lv_obj_get_width(input_label);
      int input_y_min = lv_obj_get_y(input_label);
      int input_y_max = input_y_min + lv_obj_get_height(input_label);

      if (touch_x >= input_x_min && touch_x <= input_x_max &&
          touch_y >= input_y_min && touch_y <= input_y_max) {
        ESP_LOGI(TAG, "Input label clicked at x=%d, y=%d", touch_x, touch_y);
        lv_event_send(input_label, LV_EVENT_CLICKED, NULL);
        return;
      }
    }

    if (LV_HOR_RES > MIN_SCREEN_SIZE && LV_VER_RES > MIN_SCREEN_SIZE) {
      int button_y_min = LV_VER_RES - (BUTTON_SIZE + BUTTON_PADDING * 2);
      int button_y_max = LV_VER_RES - BUTTON_PADDING;
      

      if (touch_y >= button_y_min && touch_y <= button_y_max) {
        int back_x_min = BUTTON_PADDING;
        int back_x_max = BUTTON_PADDING + BUTTON_SIZE + 25;
        if (touch_x >= back_x_min && touch_x <= back_x_max) {
          ESP_LOGW(TAG, "Back button triggered");
          lv_event_send(back_btn, LV_EVENT_CLICKED, NULL);
          return;
        }
      }
      

      int screen_half = LV_VER_RES / 2;
      if (touch_y < screen_half) {
        ESP_LOGW(TAG, "Top half tap - Scroll up");
        scroll_terminal_up();
      } else if (touch_y < button_y_min) {
        ESP_LOGW(TAG, "Bottom half tap - Scroll down");
        scroll_terminal_down();
      }
    } else {
      int screen_half = LV_VER_RES / 2;
      if (touch_y < screen_half) {
        ESP_LOGW(TAG, "Top half tap - Scroll up (small screen)");
        scroll_terminal_up();
      } else {
        ESP_LOGW(TAG, "Bottom half tap - Scroll down (small screen)");
        scroll_terminal_down();
      }
    }
  } else if (event->type == INPUT_TYPE_JOYSTICK) {
    ESP_LOGI(TAG, "Joystick event");
    int button = event->data.joystick_index;
    if (button == 1) {
      ESP_LOGW(TAG, "Joystick button 1: Stop all operations");
      stop_all_operations();
    } else if (button == 2) {
      ESP_LOGW(TAG, "Joystick button 2: Scroll up");
      scroll_terminal_up();
    } else if (button == 4) {
      ESP_LOGW(TAG, "Joystick button 4: Scroll down");
      scroll_terminal_down();
    }
  } else if (event->type == INPUT_TYPE_KEYBOARD) {
    ESP_LOGI(TAG, "keyboard event");
    uint8_t key = event->data.key_value;
    if (key == 29 || key == '`') {
      stop_all_operations();
    } else if (key == 59 || key == ';') {// up arrow
      scroll_terminal_up();
    } else if (key == 46 || key == '.') {      //down arrow
      scroll_terminal_down();
    } else if (key == 13){
      ESP_LOGW(TAG, "Enter key pressed, submitting text");
      submit_text();
    } else if (key == 8 || key == 127) { // backspace
      ESP_LOGW(TAG, "Backspace key pressed, removing last character");
      remove_char_from_buffer();
    } else if (key == 32) { // space
      ESP_LOGW(TAG, "Space key pressed, adding space to input buffer");
      add_char_to_buffer(' ');
    } else if (key >= 32 && key <= 126) { // printable ASCII characters
      ESP_LOGW(TAG, "Adding character '%c' to input buffer", (char)key);
      add_char_to_buffer((char)key);
    } else if (key == 0) {
      ESP_LOGW(TAG, "Null character received, ignoring"); 
    }
    else {
      ESP_LOGW(TAG, "Unhandled keyboard input: %d", key);
      // Optionally handle other keys or log them
      char key_str[2];
      key_str[0] = (char)key;
      key_str[1] = '\0';
      terminal_view_add_text(key_str); // Add unhandled keys to terminal
    }
  } else if (event->type == INPUT_TYPE_ENCODER) {
    unsigned long now_ms = (unsigned long)(esp_timer_get_time() / 1000ULL);
    if (event->data.encoder.button) {
      if (now_ms - createdTimeInMs <= ENCODER_DEBOUNCE_TIME_MS) {
        ESP_LOGD(TAG, "Encoder button press debounced");
        return;
      }
      ESP_LOGW(TAG, "Encoder button pressed, stopping all operations");
      stop_all_operations();
      createdTimeInMs = now_ms; // Update last press time
    } else {
      if (event->data.encoder.direction > 0) {
        ESP_LOGW(TAG, "Encoder CW, scrolling down");
        scroll_terminal_down();
      } else {
        ESP_LOGW(TAG, "Encoder CCW, scrolling up");
        scroll_terminal_up();
      }
    }
#ifdef CONFIG_USE_ENCODER
  } else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
    ESP_LOGI(TAG, "IO6 exit button pressed, returning to main menu");
    stop_all_operations();
    display_manager_switch_view(&main_menu_view);
#endif
  }
}



void terminal_view_get_hardwareinput_callback(void **callback) {
  if (callback != NULL) {
    *callback = (void *)terminal_view_hardwareinput_callback;
  }
}



View terminal_view = {
  .root = NULL,
  .create = terminal_view_create,
  .destroy = terminal_view_destroy,
  .input_callback = terminal_view_hardwareinput_callback,
  .name = "TerminalView",
  .get_hardwareinput_callback = terminal_view_get_hardwareinput_callback
};

void terminal_set_return_view(View *view) {
    terminal_return_view = view;
}
