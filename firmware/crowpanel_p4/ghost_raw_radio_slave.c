#include "ghost_raw_radio_slave.h"
#include "ghost_raw_radio.h"
#include "ghost_sta_diag.h"
#include "esp_wifi.h"
#include "esp_hosted_peer_data.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static const char *TAG = "GhostRawSlave";
static wifi_promiscuous_filter_t s_slave_prom_filter = { .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL };
static wifi_promiscuous_filter_t s_slave_prom_ctrl_filter = { .filter_mask = WIFI_PROMIS_CTRL_FILTER_MASK_ALL };
static portMUX_TYPE s_sta_diag_lock = portMUX_INITIALIZER_UNLOCKED;
static ghost_sta_diag_t s_sta_diag = { .version = GHOST_STA_DIAG_VERSION };

void ghost_sta_diag_record_tx(const void *frame, size_t len, esp_err_t result,
                              bool connected) {
    ghost_sta_dhcp_summary_t dhcp;
    bool is_dhcp = ghost_sta_dhcp_summary(frame, len, &dhcp);
    portENTER_CRITICAL(&s_sta_diag_lock);
    s_sta_diag.host_frames++;
    s_sta_diag.last_tx_result = result;
    if (!connected) s_sta_diag.disconnected_drops++;
    else if (result == ESP_OK) s_sta_diag.wifi_tx_ok++;
    else s_sta_diag.wifi_tx_fail++;
    if (is_dhcp) {
        s_sta_diag.dhcp_tx_frames++;
        s_sta_diag.last_dhcp_tx = dhcp;
    }
    portEXIT_CRITICAL(&s_sta_diag_lock);
}

void ghost_sta_diag_record_rx(const void *frame, size_t len) {
    ghost_sta_dhcp_summary_t dhcp;
    bool is_dhcp = ghost_sta_dhcp_summary(frame, len, &dhcp);
    portENTER_CRITICAL(&s_sta_diag_lock);
    s_sta_diag.wifi_rx_frames++;
    if (is_dhcp) {
        s_sta_diag.dhcp_rx_frames++;
        s_sta_diag.last_dhcp_rx = dhcp;
        if (dhcp.type == 2) s_sta_diag.offers++;
        else if (dhcp.type == 5) s_sta_diag.acks++;
        else if (dhcp.type == 6) s_sta_diag.naks++;
    }
    portEXIT_CRITICAL(&s_sta_diag_lock);
}

static void ghost_handle_sta_diag(uint32_t id, const uint8_t *data, size_t len, void *ctx) {
    (void)id; (void)ctx;
    if (!data || len != 1 || data[0] != GHOST_STA_DIAG_VERSION) return;
    ghost_sta_diag_t snapshot;
    portENTER_CRITICAL(&s_sta_diag_lock);
    snapshot = s_sta_diag;
    portEXIT_CRITICAL(&s_sta_diag_lock);
    snapshot.mac_result = esp_wifi_get_mac(WIFI_IF_STA, snapshot.radio_mac);
    esp_hosted_send_custom_data(GHOST_RAW_MSG_STA_DIAG_RESP,
                               (uint8_t *)&snapshot, sizeof(snapshot));
}

static void IRAM_ATTR ghost_raw_slave_promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    if (!pkt) return;
    uint16_t len = pkt->rx_ctrl.sig_len;
    if (len == 0 || len > GHOST_RAW_MAX_FRAME_LEN) return;
    ghost_raw_rx_hdr_t hdr = {0};
    hdr.pkt_type = (uint32_t)type;
    hdr.rssi = pkt->rx_ctrl.rssi;
#if __has_include("esp_wifi_he_types.h")
    hdr.channel = pkt->rx_ctrl.channel;
    hdr.secondary_channel = pkt->rx_ctrl.second;
    hdr.sig_len = pkt->rx_ctrl.sig_len;
    hdr.rate = pkt->rx_ctrl.rate;
    hdr.timestamp = pkt->rx_ctrl.timestamp;
    hdr.rx_state = pkt->rx_ctrl.rx_state;
#else
    hdr.channel = pkt->rx_ctrl.channel;
    hdr.secondary_channel = pkt->rx_ctrl.secondary_channel;
    hdr.sig_len = pkt->rx_ctrl.sig_len;
    hdr.rate = pkt->rx_ctrl.rate;
    hdr.timestamp = pkt->rx_ctrl.timestamp;
    hdr.rx_state = pkt->rx_ctrl.rx_state;
#endif
    size_t tot = sizeof(hdr) + len;
    uint8_t *tmp = (uint8_t *)malloc(tot);
    if (!tmp) return;
    memcpy(tmp, &hdr, sizeof(hdr));
    memcpy(tmp + sizeof(hdr), pkt->payload, len);
    esp_hosted_send_custom_data(GHOST_RAW_MSG_RX, tmp, tot);
    free(tmp);
}

static void ghost_handle_tx(uint32_t id, const uint8_t *data, size_t len, void *ctx) {
    (void)id; (void)ctx;
    if (!data || len < sizeof(ghost_raw_tx_hdr_t)) return;
    const ghost_raw_tx_hdr_t *hdr = (const ghost_raw_tx_hdr_t *)data;
    size_t frame_len = hdr->len;
    if (frame_len == 0 || frame_len > GHOST_RAW_MAX_FRAME_LEN) return;
    if (len < sizeof(*hdr) + frame_len) return;
    const uint8_t *frame = data + sizeof(*hdr);
    esp_wifi_80211_tx((wifi_interface_t)hdr->ifx, frame, frame_len, hdr->en_sys_seq ? true : false);
}
static void ghost_handle_set_promisc(uint32_t id, const uint8_t *data, size_t len, void *ctx) {
    (void)id; (void)ctx;
    if (!data || len < sizeof(ghost_raw_promisc_req_t)) return;
    const ghost_raw_promisc_req_t *req = (const ghost_raw_promisc_req_t *)data;
    bool en = req->enable ? true : false;
    if (en) {
        esp_wifi_set_promiscuous_filter(&s_slave_prom_filter);
        esp_wifi_set_promiscuous_ctrl_filter(&s_slave_prom_ctrl_filter);
        esp_wifi_set_promiscuous_rx_cb(ghost_raw_slave_promisc_cb);
    }
    esp_wifi_set_promiscuous(en);
    if (!en) esp_wifi_set_promiscuous_rx_cb(NULL);
    ESP_LOGI(TAG, "promisc %s", en ? "on" : "off");
}
static void ghost_handle_set_filter(uint32_t id, const uint8_t *data, size_t len, void *ctx) {
    (void)id; (void)ctx;
    if (!data || len < sizeof(ghost_raw_filter_req_t)) return;
    const ghost_raw_filter_req_t *req = (const ghost_raw_filter_req_t *)data;
    s_slave_prom_filter.filter_mask = req->filter_mask;
    esp_wifi_set_promiscuous_filter(&s_slave_prom_filter);
}
static void ghost_handle_set_ctrl_filter(uint32_t id, const uint8_t *data, size_t len, void *ctx) {
    (void)id; (void)ctx;
    if (!data || len < sizeof(ghost_raw_filter_req_t)) return;
    const ghost_raw_filter_req_t *req = (const ghost_raw_filter_req_t *)data;
    s_slave_prom_ctrl_filter.filter_mask = req->filter_mask;
    esp_wifi_set_promiscuous_ctrl_filter(&s_slave_prom_ctrl_filter);
}

esp_err_t ghost_raw_slave_init(void) {
    esp_err_t err;
    err = esp_hosted_register_custom_callback(GHOST_RAW_MSG_TX, ghost_handle_tx, NULL);
    if (err != ESP_OK) return err;
    err = esp_hosted_register_custom_callback(GHOST_RAW_MSG_SET_PROMISC, ghost_handle_set_promisc, NULL);
    if (err != ESP_OK) return err;
    err = esp_hosted_register_custom_callback(GHOST_RAW_MSG_SET_PROM_FILTER, ghost_handle_set_filter, NULL);
    if (err != ESP_OK) return err;
    err = esp_hosted_register_custom_callback(GHOST_RAW_MSG_SET_PROM_CTRL_FILTER, ghost_handle_set_ctrl_filter, NULL);
    if (err != ESP_OK) return err;
    err = esp_hosted_register_custom_callback(GHOST_RAW_MSG_STA_DIAG_REQ, ghost_handle_sta_diag, NULL);
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "ghost raw slave ready");
    return ESP_OK;
}
