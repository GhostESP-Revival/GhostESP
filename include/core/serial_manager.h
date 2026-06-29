// serial_manager.h

#ifndef SERIAL_MANAGER_H
#define SERIAL_MANAGER_H

#include <esp_types.h>
#include <stdbool.h>
#include <stddef.h>
#include <managers/display_manager.h>

void serial_manager_init();

void serial_manager_deinit();

// Re-installs the native USB-Serial-JTAG console driver if it was previously
// overtaken by TinyUSB (e.g. during BadUSB/HID sessions on ESP32-S3/C3/C5/C6).
// No-op on targets without the native USB-Serial-JTAG peripheral and when the
// driver is already installed.
void serial_manager_restore_console();

int serial_manager_get_uart_num();

// Temporarily hand the main serial UART driver to another subsystem (e.g. GPS
// on boards where they share a UART port). Returns true if the driver was
// released and must be restored later with serial_manager_reacquire_uart().
// No-op (returns false) when the port doesn't match, the UART is disabled, or
// it is already released. USB-JTAG console I/O is unaffected.
bool serial_manager_release_uart(int uart_num);

// Reinstalls the main serial UART driver previously released with
// serial_manager_release_uart(). No-op if it was not released.
void serial_manager_reacquire_uart(void);

int serial_manager_write_bytes(const void *data, size_t len);

void serial_task(void *pvParameter);

int handle_serial_command(const char *input);

void simulateCommand(const char *commandString);

QueueHandle_tt commandQueue;

typedef struct {
  char command[256];
} SerialCommand;

// Command history structures
#define MAX_HISTORY_SIZE 10
#define MAX_COMMAND_LENGTH 256

typedef struct {
  char commands[MAX_HISTORY_SIZE][MAX_COMMAND_LENGTH];
  int current_index;
  int history_count;
  int display_index;
} CommandHistory;

// Command history functions
void command_history_init(void);
void command_history_add(const char* command);
const char* command_history_get_previous(void);
const char* command_history_get_next(void);
void command_history_reset_display_index(void);

#endif // SERIAL_MANAGER_H
