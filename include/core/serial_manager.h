// serial_manager.h

#ifndef SERIAL_MANAGER_H
#define SERIAL_MANAGER_H

#include <esp_types.h>
#include <managers/display_manager.h>

void serial_manager_init();

void serial_manager_deinit();

int serial_manager_get_uart_num();

void serial_task(void *pvParameter);

int handle_serial_command(const char *input);

void simulateCommand(const char *commandString);

QueueHandle_tt commandQueue;

typedef struct {
  char command[1024];
} SerialCommand;

#endif // SERIAL_MANAGER_H
