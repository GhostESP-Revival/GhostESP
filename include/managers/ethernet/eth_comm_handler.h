#pragma once

#include "sdkconfig.h"
#include <stdbool.h>

#ifdef CONFIG_WITH_ETHERNET

#ifdef __cplusplus
extern "C" {
#endif

// Initialize ethernet remote-command support.
// Handles incoming "ethernet" commands from a peer display device and
// streams results back via COMM_STREAM_CHANNEL_ETHERNET.
void eth_comm_handler_init(void);

// Unregister and stop any running remote scan task.
void eth_comm_handler_deinit(void);

// Handle a GhostLink remote command destined for the ethernet subsystem.
// Returns true when the command was recognized and consumed.
bool eth_comm_handler_handle_command(const char *command, const char *data);

#ifdef __cplusplus
}
#endif

#endif // CONFIG_WITH_ETHERNET
