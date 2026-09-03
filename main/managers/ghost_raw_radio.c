#include "managers/ghost_raw_radio.h"

#if !defined(CONFIG_IDF_TARGET_ESP32P4)
#include "esp_wifi.h"
esp_err_t ghost_raw_radio_init(void) { return ESP_OK; }
bool ghost_raw_radio_is_supported(void) { return true; }
esp_err_t ghost_wifi_request_sta_diag(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ghost_wifi_80211_tx(wifi_interface_t ifx, const void *buffer, int len, bool en_sys_seq) { return esp_wifi_80211_tx(ifx, buffer, len, en_sys_seq); }
esp_err_t ghost_wifi_set_promiscuous_rx_cb(wifi_promiscuous_cb_t cb) { return esp_wifi_set_promiscuous_rx_cb(cb); }
esp_err_t ghost_wifi_set_promiscuous(bool en) { return esp_wifi_set_promiscuous(en); }
esp_err_t ghost_wifi_get_promiscuous(bool *en) { return esp_wifi_get_promiscuous(en); }
esp_err_t ghost_wifi_set_promiscuous_filter(const wifi_promiscuous_filter_t *f) { return esp_wifi_set_promiscuous_filter(f); }
esp_err_t ghost_wifi_get_promiscuous_filter(wifi_promiscuous_filter_t *f) { return esp_wifi_get_promiscuous_filter(f); }
esp_err_t ghost_wifi_set_promiscuous_ctrl_filter(const wifi_promiscuous_filter_t *f) { return esp_wifi_set_promiscuous_ctrl_filter(f); }
esp_err_t ghost_wifi_get_promiscuous_ctrl_filter(wifi_promiscuous_filter_t *f) { return esp_wifi_get_promiscuous_ctrl_filter(f); }

#else // CONFIG_IDF_TARGET_ESP32P4

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_hosted.h"
#include "managers/ghost_sta_diag.h"
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "GhostRaw";
static wifi_promiscuous_cb_t s_user_cb = NULL;
static bool s_promisc_enabled = false;
static wifi_promiscuous_filter_t s_prom_filter = { .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL };
static wifi_promiscuous_filter_t s_prom_ctrl_filter = { .filter_mask = WIFI_PROMIS_CTRL_FILTER_MASK_ALL };
typedef struct { uint32_t type; wifi_promiscuous_pkt_t *pkt; } ghost_raw_qitem_t;
static QueueHandle_t s_rx_queue = NULL;
static TaskHandle_t s_rx_task = NULL;
static bool s_registered = false;
static bool s_host_ready = false;

static void ghost_sta_diag_custom_cb(uint32_t id, const uint8_t *data, size_t len, void *ctx) {
    (void)id; (void)ctx;
    if (!data || len != sizeof(ghost_sta_diag_t)) return;
    ghost_sta_diag_t d;
    memcpy(&d, data, sizeof(d));
    if (d.version != GHOST_STA_DIAG_VERSION) return;
    ESP_LOGI("P4_STA", "C6 MAC=%02x:%02x:%02x:%02x:%02x:%02x (%s)",
             d.radio_mac[0], d.radio_mac[1], d.radio_mac[2], d.radio_mac[3],
             d.radio_mac[4], d.radio_mac[5], esp_err_to_name(d.mac_result));
    ESP_LOGI("P4_STA", "C6 host_frames=%" PRIu32 " wifi_tx_ok=%" PRIu32
             " wifi_tx_fail=%" PRIu32 " disconnected_drops=%" PRIu32
             " last_tx=%s wifi_rx=%" PRIu32,
             d.host_frames, d.wifi_tx_ok, d.wifi_tx_fail, d.disconnected_drops,
             esp_err_to_name(d.last_tx_result), d.wifi_rx_frames);
    ESP_LOGI("P4_STA", "C6 DHCP tx=%" PRIu32 " rx=%" PRIu32
             " offers=%" PRIu32 " acks=%" PRIu32 " naks=%" PRIu32
             " (counts include other clients' broadcasts)",
             d.dhcp_tx_frames, d.dhcp_rx_frames, d.offers, d.acks, d.naks);
    const ghost_sta_dhcp_summary_t *samples[] = { &d.last_dhcp_tx, &d.last_dhcp_rx };
    for (unsigned i = 0; i < 2; i++) {
        const ghost_sta_dhcp_summary_t *s = samples[i];
        if (!s->type) continue;
        ESP_LOGI("P4_STA", "C6 last DHCP %s type=%u xid=%02x%02x%02x%02x"
                 " src=%02x:%02x:%02x:%02x:%02x:%02x"
                 " client=%02x:%02x:%02x:%02x:%02x:%02x ip_csum=%s udp_csum=%s",
                 i == 0 ? "TX" : "RX", s->type, s->xid[0], s->xid[1], s->xid[2], s->xid[3],
                 s->source_mac[0], s->source_mac[1], s->source_mac[2],
                 s->source_mac[3], s->source_mac[4], s->source_mac[5],
                 s->client_mac[0], s->client_mac[1], s->client_mac[2],
                 s->client_mac[3], s->client_mac[4], s->client_mac[5],
                 s->ip_checksum_ok ? "ok" : "BAD", s->udp_checksum_ok ? "ok" : "BAD");
    }
    /* Wi-Fi accepting a frame is not an over-the-air delivery acknowledgment. */
}

esp_err_t ghost_wifi_request_sta_diag(void) {
    if (!s_host_ready) return ESP_ERR_INVALID_STATE;
    /* Registration updates an existing entry under the hosted callback mutex. */
    esp_err_t err = esp_hosted_register_custom_callback(GHOST_RAW_MSG_STA_DIAG_RESP,
                                                       ghost_sta_diag_custom_cb, NULL);
    if (err != ESP_OK) return err;
    uint8_t version = GHOST_STA_DIAG_VERSION;
    return esp_hosted_send_custom_data(GHOST_RAW_MSG_STA_DIAG_REQ, &version, sizeof(version));
}

static void ghost_raw_rx_custom_cb(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx) {
    (void)msg_id; (void)ctx;
    if (!data || len < sizeof(ghost_raw_rx_hdr_t)) return;
    const ghost_raw_rx_hdr_t *hdr = (const ghost_raw_rx_hdr_t *)data;
    size_t frame_len = hdr->sig_len;
    size_t total = sizeof(wifi_promiscuous_pkt_t) + frame_len;
    if (frame_len > GHOST_RAW_MAX_FRAME_LEN || total > 4096) return;
    if (len < sizeof(ghost_raw_rx_hdr_t) + frame_len) return;
    if (!s_rx_queue) return;
    uint8_t *buf = (uint8_t *)heap_caps_malloc(total, MALLOC_CAP_8BIT);
    if (!buf) return;
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    memset(&pkt->rx_ctrl, 0, sizeof(pkt->rx_ctrl));
    pkt->rx_ctrl.rssi = hdr->rssi;
#if __has_include("esp_wifi_he_types.h")
    // HE-capable P4 target uses esp_wifi_rxctrl_t; map portable header onto it
    pkt->rx_ctrl.channel = hdr->channel;
    pkt->rx_ctrl.second = hdr->secondary_channel;
    pkt->rx_ctrl.sig_len = hdr->sig_len;
    pkt->rx_ctrl.rate = hdr->rate;
    pkt->rx_ctrl.timestamp = hdr->timestamp;
    pkt->rx_ctrl.rx_state = hdr->rx_state;
#else
    pkt->rx_ctrl.channel = hdr->channel;
    pkt->rx_ctrl.secondary_channel = hdr->secondary_channel;
    pkt->rx_ctrl.sig_len = hdr->sig_len;
    pkt->rx_ctrl.rate = hdr->rate;
    pkt->rx_ctrl.timestamp = hdr->timestamp;
    pkt->rx_ctrl.rx_state = hdr->rx_state;
#endif
    memcpy(pkt->payload, data + sizeof(ghost_raw_rx_hdr_t), frame_len);
    ghost_raw_qitem_t item = { .type = hdr->pkt_type, .pkt = pkt };
    if (xQueueSend(s_rx_queue, &item, 0) != pdTRUE) heap_caps_free(buf);
}

static void ghost_raw_dispatch_task(void *arg) {
    (void)arg;
    ghost_raw_qitem_t item;
    while (1) {
        if (xQueueReceive(s_rx_queue, &item, portMAX_DELAY) == pdTRUE) {
            wifi_promiscuous_cb_t cb = s_user_cb;
            /* The C6 owns the real promiscuous-mode state. Do not gate the
             * RX path on the P4-side mirror: esp_wifi_remote can enable the
             * slave radio without updating s_promisc_enabled in every call
             * path, which otherwise turns a working capture into 0 packets. */
            if (cb && item.pkt) cb(item.pkt, (wifi_promiscuous_pkt_type_t)item.type);
            if (item.pkt) heap_caps_free(item.pkt);
        }
    }
}

esp_err_t ghost_raw_radio_init(void) {
    if (s_registered) return ESP_OK;
    esp_err_t err = esp_hosted_register_custom_callback(GHOST_RAW_MSG_RX, ghost_raw_rx_custom_cb, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "custom RX callback not ready: %s (will retry after hosted link up)", esp_err_to_name(err));
        return err;
    }
    s_rx_queue = xQueueCreate(32, sizeof(ghost_raw_qitem_t));
    if (!s_rx_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(ghost_raw_dispatch_task, "ghost_raw_rx", 4096, NULL, 10, &s_rx_task) != pdPASS) {
        vQueueDelete(s_rx_queue); s_rx_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_registered = true;
    s_host_ready = true;
    ESP_LOGI(TAG, "raw radio ready (host peer-data)");
    return ESP_OK;
}
bool ghost_raw_radio_is_supported(void) { return s_registered && s_host_ready; }

esp_err_t ghost_wifi_80211_tx(wifi_interface_t ifx, const void *buffer, int len, bool en_sys_seq) {
    if (!buffer || len <= 0 || len > GHOST_RAW_MAX_FRAME_LEN) return ESP_ERR_INVALID_ARG;
    if (!s_registered) return ESP_ERR_NOT_SUPPORTED;
    size_t tot = sizeof(ghost_raw_tx_hdr_t) + (size_t)len;
    uint8_t *tmp = (uint8_t *)heap_caps_malloc(tot, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!tmp) return ESP_ERR_NO_MEM;
    ghost_raw_tx_hdr_t *hdr = (ghost_raw_tx_hdr_t *)tmp;
    hdr->ifx = (uint8_t)ifx;
    hdr->en_sys_seq = en_sys_seq ? 1 : 0;
    hdr->len = (uint16_t)len;
    memcpy(tmp + sizeof(*hdr), buffer, len);
    esp_err_t err = esp_hosted_send_custom_data(GHOST_RAW_MSG_TX, tmp, tot);
    heap_caps_free(tmp);
    return err;
}
esp_err_t ghost_wifi_set_promiscuous_rx_cb(wifi_promiscuous_cb_t cb) { s_user_cb = cb; return ESP_OK; }
esp_err_t ghost_wifi_set_promiscuous(bool en) {
    if (!s_registered) return ESP_ERR_NOT_SUPPORTED;
    ghost_raw_promisc_req_t req = { .enable = en ? 1 : 0 };
    esp_err_t err = esp_hosted_send_custom_data(GHOST_RAW_MSG_SET_PROMISC, (uint8_t *)&req, sizeof(req));
    if (err == ESP_OK) s_promisc_enabled = en;
    return err;
}
esp_err_t ghost_wifi_get_promiscuous(bool *en) { if (!en) return ESP_ERR_INVALID_ARG; *en = s_promisc_enabled; return ESP_OK; }
esp_err_t ghost_wifi_set_promiscuous_filter(const wifi_promiscuous_filter_t *f) {
    if (!f) return ESP_ERR_INVALID_ARG;
    if (!s_registered) return ESP_ERR_NOT_SUPPORTED;
    ghost_raw_filter_req_t req = { .filter_mask = f->filter_mask };
    esp_err_t err = esp_hosted_send_custom_data(GHOST_RAW_MSG_SET_PROM_FILTER, (uint8_t *)&req, sizeof(req));
    if (err == ESP_OK) s_prom_filter = *f;
    return err;
}
esp_err_t ghost_wifi_get_promiscuous_filter(wifi_promiscuous_filter_t *f) { if (!f) return ESP_ERR_INVALID_ARG; *f = s_prom_filter; return ESP_OK; }
esp_err_t ghost_wifi_set_promiscuous_ctrl_filter(const wifi_promiscuous_filter_t *f) {
    if (!f) return ESP_ERR_INVALID_ARG;
    if (!s_registered) return ESP_ERR_NOT_SUPPORTED;
    ghost_raw_filter_req_t req = { .filter_mask = f->filter_mask };
    esp_err_t err = esp_hosted_send_custom_data(GHOST_RAW_MSG_SET_PROM_CTRL_FILTER, (uint8_t *)&req, sizeof(req));
    if (err == ESP_OK) s_prom_ctrl_filter = *f;
    return err;
}
esp_err_t ghost_wifi_get_promiscuous_ctrl_filter(wifi_promiscuous_filter_t *f) { if (!f) return ESP_ERR_INVALID_ARG; *f = s_prom_ctrl_filter; return ESP_OK; }

// Strong overrides for esp_wifi_remote weak symbols so existing call sites transparently use hosted raw radio
#include "esp_wifi_remote.h"
esp_err_t esp_wifi_remote_80211_tx(wifi_interface_t ifx, const void *buffer, int len, bool en_sys_seq) { return ghost_wifi_80211_tx(ifx, buffer, len, en_sys_seq); }
esp_err_t esp_wifi_remote_set_promiscuous_rx_cb(wifi_promiscuous_cb_t cb) { return ghost_wifi_set_promiscuous_rx_cb(cb); }
esp_err_t esp_wifi_remote_set_promiscuous(bool en) { return ghost_wifi_set_promiscuous(en); }
esp_err_t esp_wifi_remote_get_promiscuous(bool *en) { return ghost_wifi_get_promiscuous(en); }
esp_err_t esp_wifi_remote_set_promiscuous_filter(const wifi_promiscuous_filter_t *f) { return ghost_wifi_set_promiscuous_filter(f); }
esp_err_t esp_wifi_remote_get_promiscuous_filter(wifi_promiscuous_filter_t *f) { return ghost_wifi_get_promiscuous_filter(f); }
esp_err_t esp_wifi_remote_set_promiscuous_ctrl_filter(const wifi_promiscuous_filter_t *f) { return ghost_wifi_set_promiscuous_ctrl_filter(f); }
esp_err_t esp_wifi_remote_get_promiscuous_ctrl_filter(wifi_promiscuous_filter_t *f) { return ghost_wifi_get_promiscuous_ctrl_filter(f); }

#endif
