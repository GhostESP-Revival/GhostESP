// serial_manager.h

#ifndef SERIAL_MANAGER_H
#define SERIAL_MANAGER_H

#include <esp_types.h>
#include <stddef.h>
#include <managers/display_manager.h>

void serial_manager_init();

void serial_manager_deinit();

int serial_manager_get_uart_num();

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
