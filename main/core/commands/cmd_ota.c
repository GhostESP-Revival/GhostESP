// cmd_ota.c
// GhostLink peer-flashing remote commands (see managers/peer_ota_manager.c).
// These are only ever meaningfully invoked remotely, over GhostLink, from a
// peer's own primary (see peer_ota_manager.c's "peer_ota_send_and_wait"),
// but are registered as normal CLI commands like any other -- GhostLink's
// remote-command relay dispatches through the same "peer:<cmd>" ->
// handle_serial_command() path as local commands.

#include "core/commands.h"
#include "managers/peer_ota_manager.h"

void handle_otarecv_cmd(int argc, char **argv) {
    peer_ota_manager_handle_otarecv_cmd(argc, argv);
}

void handle_otastatus_cmd(int argc, char **argv) {
    peer_ota_manager_handle_otastatus_cmd(argc, argv);
}

void handle_otaabort_cmd(int argc, char **argv) {
    peer_ota_manager_handle_otaabort_cmd(argc, argv);
}
