#pragma once
#include "esp_err.h"
#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

// Portable wire protocol shared between P4 host and C6 slave.
// Keep this header single-source for both sides (copy to slave build).

#define GHOST_RAW_MSG_TX                 0x47520001u // host -> slave: raw 802.11 tx
#define GHOST_RAW_MSG_SET_PROMISC        0x47520002u // host -> slave: set promiscuous on/off
#define GHOST_RAW_MSG_SET_PROM_FILTER    0x47520003u // host -> slave: set promisc filter
#define GHOST_RAW_MSG_SET_PROM_CTRL_FILTER 0x47520004u // host -> slave: set ctrl filter
#define GHOST_RAW_MSG_RX                 0x47520010u // slave -> host: promisc rx frame
#define GHOST_RAW_MSG_STA_DIAG_REQ       0x47520020u // host -> slave: STA counters
#define GHOST_RAW_MSG_STA_DIAG_RESP      0x47520021u // slave -> host: STA counters

#define GHOST_RAW_MAX_FRAME_LEN  2304
#define GHOST_RAW_MAX_PAYLOAD    8166

typedef struct __attribute__((packed)) {
    uint8_t ifx;
    uint8_t en_sys_seq;
    uint16_t len; // frame length, little-endian
    // uint8_t frame[len] follows
} ghost_raw_tx_hdr_t;

typedef struct __attribute__((packed)) {
    uint8_t enable;
} ghost_raw_promisc_req_t;

typedef struct __attribute__((packed)) {
    uint32_t filter_mask;
} ghost_raw_filter_req_t;

// Slave -> host RX header, packed, followed by raw 802.11 bytes
// Keep this minimal and portable across HE / non-HE rx_ctrl layouts.
// Only rssi/channel/sig_len are load-bearing for GhostESP callbacks.
typedef struct __attribute__((packed)) {
    uint32_t pkt_type; // wifi_promiscuous_pkt_type_t
    int8_t rssi;
    uint8_t channel;
    uint8_t secondary_channel; // maps to rx_ctrl.second on HE targets
    uint16_t sig_len;
    uint8_t rate;
    uint32_t timestamp;
    uint8_t rx_state;
    // uint8_t payload[sig_len] follows
} ghost_raw_rx_hdr_t;

// Host-side lifecycle, call after esp_hosted_connect_to_slave succeeds.
esp_err_t ghost_raw_radio_init(void);
bool ghost_raw_radio_is_supported(void);
esp_err_t ghost_wifi_request_sta_diag(void);

// Host wrappers (P4 uses peer data, other targets call esp_wifi directly)
esp_err_t ghost_wifi_80211_tx(wifi_interface_t ifx, const void *buffer, int len, bool en_sys_seq);
esp_err_t ghost_wifi_set_promiscuous_rx_cb(wifi_promiscuous_cb_t cb);
esp_err_t ghost_wifi_set_promiscuous(bool en);
esp_err_t ghost_wifi_get_promiscuous(bool *en);
esp_err_t ghost_wifi_set_promiscuous_filter(const wifi_promiscuous_filter_t *filter);
esp_err_t ghost_wifi_get_promiscuous_filter(wifi_promiscuous_filter_t *filter);
esp_err_t ghost_wifi_set_promiscuous_ctrl_filter(const wifi_promiscuous_filter_t *filter);
esp_err_t ghost_wifi_get_promiscuous_ctrl_filter(wifi_promiscuous_filter_t *filter);

#ifdef __cplusplus
}
#endif
