#include "managers/espnow_manager.h"

#include "core/ghostchi_identity.h"
#include "managers/wifi_manager.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

#define ESPNOW_MANAGER_MAX_PEERS 16
#define ESPNOW_MANAGER_MAX_MESSAGES 16
#define ESPNOW_MANAGER_PROTOCOL_MAGIC 0x47455350u
#define ESPNOW_MANAGER_PROTOCOL_VERSION 1u
#define ESPNOW_MANAGER_TYPE_HELLO 1u
#define ESPNOW_MANAGER_TYPE_MESSAGE 2u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t type;
    uint8_t channel;
    uint8_t name_len;
    uint8_t text_len;
    char name[ESPNOW_MANAGER_NAME_MAX];
    char text[ESPNOW_MANAGER_MESSAGE_MAX];
} espnow_manager_packet_t;

static const char *TAG = "EspNowManager";
static const uint8_t s_broadcast_mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static espnow_manager_peer_t *s_peers;
static espnow_manager_message_t *s_messages;
static uint8_t s_message_head;
static uint8_t s_message_count;
static uint8_t s_channel;
static bool s_active;
static bool s_reconnect_sta_on_stop;
static char s_name[ESPNOW_MANAGER_NAME_MAX];
static char s_last_error[96];
static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool allocate_buffers(void) {
    if (!s_peers) {
        s_peers = heap_caps_malloc(sizeof(espnow_manager_peer_t) * ESPNOW_MANAGER_MAX_PEERS,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_peers) s_peers = malloc(sizeof(espnow_manager_peer_t) * ESPNOW_MANAGER_MAX_PEERS);
        if (!s_peers) return false;
    }
    if (!s_messages) {
        s_messages = heap_caps_malloc(sizeof(espnow_manager_message_t) * ESPNOW_MANAGER_MAX_MESSAGES,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_messages) s_messages = malloc(sizeof(espnow_manager_message_t) * ESPNOW_MANAGER_MAX_MESSAGES);
        if (!s_messages) {
            free(s_peers);
            s_peers = NULL;
            return false;
        }
    }
    return true;
}

static void free_buffers(void) {
    free(s_peers);
    free(s_messages);
    s_peers = NULL;
    s_messages = NULL;
}

static void set_error(const char *message) {
    snprintf(s_last_error, sizeof(s_last_error), "%s", message ? message : "Unknown ESP-NOW error");
    ESP_LOGW(TAG, "%s", s_last_error);
}

static bool mac_equal(const uint8_t left[6], const uint8_t right[6]) {
    return memcmp(left, right, 6) == 0;
}

static void upsert_peer(const uint8_t mac[6], const char *name, int8_t rssi) {
    int slot = -1;
    int oldest = 0;
    uint32_t oldest_seen = UINT32_MAX;
    uint32_t seen = now_ms();
    bool existing = false;

    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < ESPNOW_MANAGER_MAX_PEERS; ++i) {
        if (s_peers[i].name[0] && mac_equal(s_peers[i].mac, mac)) {
            slot = i;
            existing = true;
            break;
        }
        if (!s_peers[i].name[0] && slot < 0) slot = i;
        if (s_peers[i].last_seen_ms < oldest_seen) {
            oldest_seen = s_peers[i].last_seen_ms;
            oldest = i;
        }
    }
    if (slot < 0) slot = oldest;
    memcpy(s_peers[slot].mac, mac, sizeof(s_peers[slot].mac));
    s_peers[slot].rssi = rssi;
    s_peers[slot].last_seen_ms = seen;
    snprintf(s_peers[slot].name, sizeof(s_peers[slot].name), "%s", name && name[0] ? name : "GhostESP");
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "%s peer %02X:%02X:%02X:%02X:%02X:%02X name='%s' rssi=%d",
             existing ? "Updated" : "Discovered",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             name && name[0] ? name : "GhostESP", rssi);
}

static void queue_message(const uint8_t sender_mac[6], const char *sender_name, const char *text) {
    portENTER_CRITICAL(&s_lock);
    uint8_t index = (uint8_t)((s_message_head + s_message_count) % ESPNOW_MANAGER_MAX_MESSAGES);
    if (s_message_count == ESPNOW_MANAGER_MAX_MESSAGES) {
        s_message_head = (uint8_t)((s_message_head + 1) % ESPNOW_MANAGER_MAX_MESSAGES);
        index = (uint8_t)((s_message_head + s_message_count - 1) % ESPNOW_MANAGER_MAX_MESSAGES);
    } else {
        s_message_count++;
    }
    espnow_manager_message_t *message = &s_messages[index];
    memset(message, 0, sizeof(*message));
    memcpy(message->sender_mac, sender_mac, sizeof(message->sender_mac));
    message->received_at_ms = now_ms();
    snprintf(message->sender_name, sizeof(message->sender_name), "%s", sender_name);
    snprintf(message->text, sizeof(message->text), "%s", text);
    portEXIT_CRITICAL(&s_lock);
}

static bool ensure_peer(const uint8_t mac[6]) {
    if (esp_now_is_peer_exist(mac)) return true;

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, mac, sizeof(peer.peer_addr));
    peer.channel = 0;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    esp_err_t err = esp_now_add_peer(&peer);
    if (err == ESP_OK || err == ESP_ERR_ESPNOW_EXIST) return true;
    set_error(esp_err_to_name(err));
    return false;
}

static bool send_packet(const uint8_t mac[6], uint8_t type, const char *text) {
    if (!s_active || !ensure_peer(mac)) return false;

    espnow_manager_packet_t packet = {
        .magic = ESPNOW_MANAGER_PROTOCOL_MAGIC,
        .version = ESPNOW_MANAGER_PROTOCOL_VERSION,
        .type = type,
        .channel = s_channel,
    };
    snprintf(packet.name, sizeof(packet.name), "%s", s_name);
    packet.name_len = (uint8_t)strnlen(packet.name, sizeof(packet.name));
    if (text) snprintf(packet.text, sizeof(packet.text), "%s", text);
    packet.text_len = (uint8_t)strnlen(packet.text, sizeof(packet.text));

    esp_err_t err = esp_now_send(mac, (const uint8_t *)&packet, sizeof(packet));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to queue %s for %02X:%02X:%02X:%02X:%02X:%02X: %s",
                 type == ESPNOW_MANAGER_TYPE_MESSAGE ? "message" : "announcement",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], esp_err_to_name(err));
        set_error(esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "Queued %s for %02X:%02X:%02X:%02X:%02X:%02X%s",
             type == ESPNOW_MANAGER_TYPE_MESSAGE ? "message" : "announcement",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             type == ESPNOW_MANAGER_TYPE_MESSAGE ? "" : " (broadcast)");
    return true;
}

static void espnow_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (!info || !info->src_addr || !data || len != sizeof(espnow_manager_packet_t)) return;

    const espnow_manager_packet_t *packet = (const espnow_manager_packet_t *)data;
    if (packet->magic != ESPNOW_MANAGER_PROTOCOL_MAGIC ||
        packet->version != ESPNOW_MANAGER_PROTOCOL_VERSION ||
        (packet->type != ESPNOW_MANAGER_TYPE_HELLO && packet->type != ESPNOW_MANAGER_TYPE_MESSAGE) ||
        packet->name_len >= sizeof(packet->name) || packet->text_len >= sizeof(packet->text)) {
        return;
    }

    char name[ESPNOW_MANAGER_NAME_MAX];
    char text[ESPNOW_MANAGER_MESSAGE_MAX];
    memcpy(name, packet->name, packet->name_len);
    name[packet->name_len] = '\0';
    memcpy(text, packet->text, packet->text_len);
    text[packet->text_len] = '\0';
    int8_t rssi = info->rx_ctrl ? info->rx_ctrl->rssi : 0;
    ESP_LOGI(TAG, "Received %s from %02X:%02X:%02X:%02X:%02X:%02X name='%s' rssi=%d text_len=%u",
             packet->type == ESPNOW_MANAGER_TYPE_MESSAGE ? "message" : "announcement",
             info->src_addr[0], info->src_addr[1], info->src_addr[2],
             info->src_addr[3], info->src_addr[4], info->src_addr[5],
             name, rssi, packet->text_len);
    upsert_peer(info->src_addr, name, rssi);
    if (packet->type == ESPNOW_MANAGER_TYPE_MESSAGE && text[0]) {
        queue_message(info->src_addr, name, text);
    }
}

bool espnow_manager_start(uint8_t channel) {
    if (s_active) return true;
    if (channel < 1 || channel > 14) {
        set_error("Choose a 2.4 GHz channel (1-14)");
        return false;
    }

    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) != ESP_OK || !(mode & WIFI_MODE_STA)) {
        set_error("WiFi STA mode is unavailable");
        return false;
    }
    bool promiscuous = false;
    if (esp_wifi_get_promiscuous(&promiscuous) == ESP_OK && promiscuous) {
        set_error("Stop WiFi monitoring before nearby chat");
        return false;
    }
    bool reconnect_sta_on_stop = false;
    wifi_ap_record_t connected_ap = {0};
    wifi_manager_set_reconnect_hold(true);
    if (esp_wifi_sta_get_ap_info(&connected_ap) == ESP_OK) {
        ESP_LOGI(TAG, "Disconnecting STA for nearby chat on channel %u", channel);
        esp_err_t disconnect_err = esp_wifi_disconnect();
        if (disconnect_err != ESP_OK && disconnect_err != ESP_ERR_WIFI_NOT_CONNECT) {
            set_error("Unable to disconnect WiFi for nearby chat");
            wifi_manager_set_reconnect_hold(false);
            return false;
        }
        for (int attempt = 0; attempt < 10; ++attempt) {
            if (esp_wifi_sta_get_ap_info(&connected_ap) != ESP_OK) break;
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (esp_wifi_sta_get_ap_info(&connected_ap) == ESP_OK) {
            set_error("WiFi did not disconnect for nearby chat");
            wifi_manager_set_reconnect_hold(false);
            return false;
        }
        reconnect_sta_on_stop = true;
    }
    if (esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
        set_error("Unable to select the nearby-chat channel");
        wifi_manager_set_reconnect_hold(false);
        if (reconnect_sta_on_stop) wifi_manager_configure_sta_from_settings();
        return false;
    }
    if (!ghostchi_identity_get_name(s_name, sizeof(s_name))) {
        set_error("Unable to read the WiFi MAC address");
        wifi_manager_set_reconnect_hold(false);
        if (reconnect_sta_on_stop) wifi_manager_configure_sta_from_settings();
        return false;
    }
    ESP_LOGI(TAG, "Starting ESP-NOW channel=%u identity='%s' reconnect_sta=%d",
             channel, s_name, reconnect_sta_on_stop);

    if (!allocate_buffers()) {
        set_error("Out of memory for nearby chat");
        wifi_manager_set_reconnect_hold(false);
        if (reconnect_sta_on_stop) wifi_manager_configure_sta_from_settings();
        return false;
    }
    esp_err_t err = esp_now_init();
    if (err != ESP_OK) {
        set_error(esp_err_to_name(err));
        free_buffers();
        wifi_manager_set_reconnect_hold(false);
        if (reconnect_sta_on_stop) wifi_manager_configure_sta_from_settings();
        return false;
    }
    err = esp_now_register_recv_cb(espnow_recv_cb);
    if (err != ESP_OK) {
        esp_now_deinit();
        set_error(esp_err_to_name(err));
        free_buffers();
        wifi_manager_set_reconnect_hold(false);
        if (reconnect_sta_on_stop) wifi_manager_configure_sta_from_settings();
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    memset(s_peers, 0, sizeof(espnow_manager_peer_t) * ESPNOW_MANAGER_MAX_PEERS);
    memset(s_messages, 0, sizeof(espnow_manager_message_t) * ESPNOW_MANAGER_MAX_MESSAGES);
    s_message_head = 0;
    s_message_count = 0;
    s_channel = channel;
    s_active = true;
    s_reconnect_sta_on_stop = reconnect_sta_on_stop;
    s_last_error[0] = '\0';
    portEXIT_CRITICAL(&s_lock);
    (void)espnow_manager_announce();
    return true;
}

void espnow_manager_stop(void) {
    if (!s_active) return;
    ESP_LOGI(TAG, "Stopping ESP-NOW channel=%u peers=%d queued_messages=%d",
             s_channel, espnow_manager_peer_count(), espnow_manager_message_count());
    esp_now_unregister_recv_cb();
    esp_now_deinit();
    portENTER_CRITICAL(&s_lock);
    bool reconnect_sta = s_reconnect_sta_on_stop;
    s_active = false;
    s_channel = 0;
    s_reconnect_sta_on_stop = false;
    portEXIT_CRITICAL(&s_lock);
    free_buffers();
    wifi_manager_set_reconnect_hold(false);
    if (reconnect_sta) wifi_manager_configure_sta_from_settings();
}

bool espnow_manager_is_active(void) {
    return s_active;
}

uint8_t espnow_manager_channel(void) {
    return s_channel;
}

const char *espnow_manager_name(void) {
    return s_name;
}

const char *espnow_manager_last_error(void) {
    return s_last_error;
}

bool espnow_manager_announce(void) {
    return send_packet(s_broadcast_mac, ESPNOW_MANAGER_TYPE_HELLO, NULL);
}

int espnow_manager_peer_count(void) {
    if (!s_peers) return 0;
    int count = 0;
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < ESPNOW_MANAGER_MAX_PEERS; ++i) {
        if (s_peers[i].name[0]) count++;
    }
    portEXIT_CRITICAL(&s_lock);
    return count;
}

bool espnow_manager_get_peer(int index, espnow_manager_peer_t *out) {
    if (!out || index < 0 || !s_peers) return false;
    int current = 0;
    bool found = false;
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < ESPNOW_MANAGER_MAX_PEERS; ++i) {
        if (!s_peers[i].name[0]) continue;
        if (current++ == index) {
            *out = s_peers[i];
            found = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return found;
}

bool espnow_manager_send(const uint8_t mac[6], const char *text) {
    if (!mac || !text || !text[0]) return false;
    return send_packet(mac, ESPNOW_MANAGER_TYPE_MESSAGE, text);
}

int espnow_manager_message_count(void) {
    int count;
    portENTER_CRITICAL(&s_lock);
    count = s_message_count;
    portEXIT_CRITICAL(&s_lock);
    return count;
}

bool espnow_manager_receive(espnow_manager_message_t *out) {
    if (!out || !s_messages) return false;
    bool received = false;
    portENTER_CRITICAL(&s_lock);
    if (s_message_count > 0) {
        *out = s_messages[s_message_head];
        s_message_head = (uint8_t)((s_message_head + 1) % ESPNOW_MANAGER_MAX_MESSAGES);
        s_message_count--;
        received = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return received;
}
