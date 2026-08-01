#ifndef ESPNOW_MANAGER_H
#define ESPNOW_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESPNOW_MANAGER_NAME_MAX 24
#define ESPNOW_MANAGER_MESSAGE_MAX 160

typedef struct {
    uint8_t mac[6];
    int8_t rssi;
    uint32_t last_seen_ms;
    char name[ESPNOW_MANAGER_NAME_MAX];
} espnow_manager_peer_t;

typedef struct {
    uint8_t sender_mac[6];
    uint32_t received_at_ms;
    char sender_name[ESPNOW_MANAGER_NAME_MAX];
    char text[ESPNOW_MANAGER_MESSAGE_MAX];
} espnow_manager_message_t;

bool espnow_manager_start(uint8_t channel);
void espnow_manager_stop(void);
bool espnow_manager_is_active(void);
uint8_t espnow_manager_channel(void);
const char *espnow_manager_name(void);
const char *espnow_manager_last_error(void);
bool espnow_manager_announce(void);
int espnow_manager_peer_count(void);
bool espnow_manager_get_peer(int index, espnow_manager_peer_t *out);
bool espnow_manager_send(const uint8_t mac[6], const char *text);
int espnow_manager_message_count(void);
bool espnow_manager_receive(espnow_manager_message_t *out);

#ifdef __cplusplus
}
#endif

#endif
