// command.h

#ifndef COMMAND_H
#define COMMAND_H

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stddef.h>

typedef void (*CommandFunction)(int argc, char **argv);

typedef struct Command {
  const char *name;
  CommandFunction function;
  struct Command *next;
} Command;

// Functions to manage commands
void command_init();
void register_command(const char *name, CommandFunction function);
void unregister_command(const char *name);
CommandFunction find_command(const char *name);
const char *command_name_at(size_t index);
void handle_unknown_command(const char *cmd);

extern TaskHandle_t VisualizerHandle;

void register_commands();

// Stop command handler
void handle_stop_flipper(int argc, char **argv);

#endif // COMMAND_H
