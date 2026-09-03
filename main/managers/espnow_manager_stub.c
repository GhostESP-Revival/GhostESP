#include "managers/espnow_manager.h"

bool espnow_manager_start(uint8_t channel) {
    (void)channel;
    return false;
}

void espnow_manager_stop(void) {}

bool espnow_manager_is_active(void) {
    return false;
}

uint8_t espnow_manager_channel(void) {
    return 0;
}

const char *espnow_manager_name(void) {
    return "";
}

const char *espnow_manager_last_error(void) {
    return "ESP-NOW is not supported on ESP32-P4 hosted Wi-Fi";
}

bool espnow_manager_announce(void) {
    return false;
}

int espnow_manager_peer_count(void) {
    return 0;
}

bool espnow_manager_get_peer(int index, espnow_manager_peer_t *out) {
    (void)index;
    (void)out;
    return false;
}

bool espnow_manager_send(const uint8_t mac[6], const char *text) {
    (void)mac;
    (void)text;
    return false;
}

int espnow_manager_message_count(void) {
    return 0;
}

bool espnow_manager_receive(espnow_manager_message_t *out) {
    (void)out;
    return false;
}
