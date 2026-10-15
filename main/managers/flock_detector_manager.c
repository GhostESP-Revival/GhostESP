// flock_detector_manager.c
//
// Flock Safety camera detector for GhostESP
// Based on bennjordan/flock-you (https://github.com/bennjordan/flock-you)
//

#include "managers/flock_detector_manager.h"
#include "managers/wifi_manager.h"
#include "managers/rgb_manager.h"
#include "scans/wifi/hop_profile.h"
#include "scans/wifi/wifi_channels.h"
#include "core/glog.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "FlockDetector";

// Pre-packed 24-bit OUI values from bennjordan/flock-you device databases
static const uint32_t flock_oui_packed[] = {
    // FS Ext Battery devices
    0x588E81, 0xCCCCCC, 0xEC1BBD, 0x9035EA, 0x040D84,
    0xF082C0, 0x1C34F1, 0x385B44, 0x943469, 0xB4E3F9,
    // Flock WiFi devices
    0x70C94E, 0x3C9180, 0xD8F3BC, 0x803049, 0x145AFC,
    0x744CA1, 0x083A88, 0x9C2F9D, 0xB41E52,
    // Penguin surveillance devices
    0xCC0924, 0xEDC763, 0xE8CE56, 0xEA0CEA, 0xD88F14,
    0xF9D9C0, 0xF132F9, 0xF6A076, 0xE41C9E, 0xE7F243,
    0xE27133, 0xDA91A9, 0xE10E15, 0xC8AE87, 0xF4EDB2,
    0xD8BFB5, 0xEE8F3C, 0xD72B21, 0xEA5A98,
};
#define FLOCK_OUI_COUNT (sizeof(flock_oui_packed) / sizeof(flock_oui_packed[0]))

// Additional WiFi surveillance signatures, each tagged with its device class
// in the top byte (see FLOCK_CLS). Camera-specific makers (Wyze, Verkada,
// Avigilon, Axis), ALPR (Motorola Solutions, Genetec), Ring LLC doorbells,
// Axon body cams and the Hak5 Pineapple default MAC (00:13:37, same signature
// the PineAP detector uses). Broad vendor blocks (Amazon, Hikvision) are left
// out to keep the hit rate meaningful.
#define FLOCK_CLS(cls, oui) (((uint32_t)(cls) << 24) | (uint32_t)(oui))
static const uint32_t camera_oui_packed[] = {
    FLOCK_CLS(FLOCK_CLASS_CAMERA, 0x2CAA8E), FLOCK_CLS(FLOCK_CLASS_CAMERA, 0xD03F27),
    FLOCK_CLS(FLOCK_CLASS_CAMERA, 0x7C78B2),  // Wyze Labs
    FLOCK_CLS(FLOCK_CLASS_CAMERA, 0xE0A700),  // Verkada
    FLOCK_CLS(FLOCK_CLASS_CAMERA, 0x701AD5),  // Avigilon
    FLOCK_CLS(FLOCK_CLASS_CAMERA, 0x00408C), FLOCK_CLS(FLOCK_CLASS_CAMERA, 0xB8A44F),  // Axis
    FLOCK_CLS(FLOCK_CLASS_ALPR, 0x00047D), FLOCK_CLS(FLOCK_CLASS_ALPR, 0x001885),
    FLOCK_CLS(FLOCK_CLASS_ALPR, 0x001F92), FLOCK_CLS(FLOCK_CLASS_ALPR, 0x4CCC34),
    FLOCK_CLS(FLOCK_CLASS_ALPR, 0xB8E28C),   // Motorola Solutions
    FLOCK_CLS(FLOCK_CLASS_ALPR, 0x00BF15), FLOCK_CLS(FLOCK_CLASS_ALPR, 0x0CBF15),  // Genetec
    FLOCK_CLS(FLOCK_CLASS_RING, 0xAC9FC3), FLOCK_CLS(FLOCK_CLASS_RING, 0x187F88),
    FLOCK_CLS(FLOCK_CLASS_RING, 0x343EA4), FLOCK_CLS(FLOCK_CLASS_RING, 0x54E019),
    FLOCK_CLS(FLOCK_CLASS_RING, 0x5C475E), FLOCK_CLS(FLOCK_CLASS_RING, 0x649A63),
    FLOCK_CLS(FLOCK_CLASS_RING, 0x90486C), FLOCK_CLS(FLOCK_CLASS_RING, 0x9C7613),
    FLOCK_CLS(FLOCK_CLASS_RING, 0xCC3BFB), FLOCK_CLS(FLOCK_CLASS_RING, 0xC4DBAD),
    FLOCK_CLS(FLOCK_CLASS_RING, 0x242BD6), FLOCK_CLS(FLOCK_CLASS_RING, 0x00B463),
    FLOCK_CLS(FLOCK_CLASS_RING, 0x50E467),   // Ring LLC
    FLOCK_CLS(FLOCK_CLASS_AXON, 0x0025DF),   // Axon Enterprise
    FLOCK_CLS(FLOCK_CLASS_HAK5, 0x001337),   // Hak5 Pineapple default MAC (mirrors PineAP detect)
};
#define CAMERA_OUI_COUNT (sizeof(camera_oui_packed) / sizeof(camera_oui_packed[0]))

typedef struct __attribute__((packed)) {
    uint16_t frame_ctrl, duration;
    uint8_t addr1[6], addr2[6], addr3[6];
    uint16_t seq_ctrl;
} flock_mac_hdr_t;

// ---- Alert ring buffer (WiFi callback → drain task) ----

#define FLOCK_ALERT_QUEUE_SIZE 32

#define FLOCK_ALERT_SSID_LEN 33

typedef struct {
    FlockDetectMethod method;
    uint8_t mac[6];
    int8_t rssi;
    uint8_t channel;
    uint8_t cls;                      // FlockDeviceClass
    char ssid[FLOCK_ALERT_SSID_LEN];  // empty string if no SSID
} FlockAlertEntry;

static FlockAlertEntry *s_alert_queue = NULL;
static volatile size_t s_alert_head = 0;
static volatile size_t s_alert_tail = 0;
static portMUX_TYPE s_alert_mux = portMUX_INITIALIZER_UNLOCKED;

static void IRAM_ATTR enqueue_alert(FlockDetectMethod method, const uint8_t *mac,
                                     int8_t rssi, uint8_t ch, const char *ssid,
                                     uint8_t cls) {
    portENTER_CRITICAL_ISR(&s_alert_mux);
    size_t next = (s_alert_head + 1) % FLOCK_ALERT_QUEUE_SIZE;
    if (next == s_alert_tail) { portEXIT_CRITICAL_ISR(&s_alert_mux); return; }
    FlockAlertEntry *e = &s_alert_queue[s_alert_head];
    e->method = method;
    e->rssi = rssi;
    e->channel = ch;
    e->cls = cls;
    memcpy((void *)e->mac, mac, 6);
    if (ssid && ssid[0]) strlcpy((char *)e->ssid, ssid, FLOCK_ALERT_SSID_LEN);
    else e->ssid[0] = '\0';
    s_alert_head = next;
    portEXIT_CRITICAL_ISR(&s_alert_mux);
}

// ---- Hash-based detection table ----

#define FLOCK_HASH_BUCKETS 16
#define FLOCK_HASH_MASK   (FLOCK_HASH_BUCKETS - 1)

static FlockDetection *s_dets = NULL;
static int *s_det_bucket = NULL;  // heap-allocated, FLOCK_HASH_BUCKETS entries
static int *s_det_next = NULL;    // heap-allocated, FLOCK_MAX_DETECTIONS entries
static int s_count = 0;
static bool s_running = false;
static SemaphoreHandle_t s_mutex = NULL;
static FlockDetectorCallback s_cb = NULL;
static void *s_cb_data = NULL;

static inline int mac_hash(const char *mac) {
    // hash on the 6 hex chars of the OUI portion ("xx:xx:xx")
    int h = 0;
    for (int i = 0; i < 8; i++) h = h * 31 + mac[i];
    return h & FLOCK_HASH_MASK;
}

static void det_chain_init(void) {
    for (int i = 0; i < FLOCK_HASH_BUCKETS; i++) s_det_bucket[i] = -1;
    for (int i = 0; i < FLOCK_MAX_DETECTIONS; i++) s_det_next[i] = -1;
}

static void det_chain_alloc(void) {
    s_det_bucket = malloc(FLOCK_HASH_BUCKETS * sizeof(int));
    s_det_next = malloc(FLOCK_MAX_DETECTIONS * sizeof(int));
}

static void det_chain_free(void) {
    free(s_det_bucket); s_det_bucket = NULL;
    free(s_det_next); s_det_next = NULL;
}

static int det_find(const char *mac) {
    int bi = mac_hash(mac);
    for (int i = s_det_bucket[bi]; i >= 0; i = s_det_next[i]) {
        if (strcasecmp(s_dets[i].mac, mac) == 0) return i;
    }
    return -1;
}

static int det_insert(const char *mac, FlockDetectMethod method, int8_t rssi, uint8_t ch, const char *ssid, uint8_t cls) {
    if (s_count >= FLOCK_MAX_DETECTIONS) return -1;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    int idx = s_count;
    FlockDetection *d = &s_dets[idx];
    strlcpy(d->mac, mac, FLOCK_MAC_STR_LEN);
    strlcpy(d->method, flock_detector_method_str(method), FLOCK_METHOD_STR_LEN);
    d->rssi = rssi; d->channel = ch;
    d->device_class = cls;
    d->first_seen_ms = now; d->last_seen_ms = now; d->count = 1;
    d->confidence = (method == FLOCK_DETECT_WILDCARD_PROBE || method == FLOCK_DETECT_SSID)
                    ? FLOCK_CONF_HIGH : FLOCK_CONF_LOW;
    if (ssid && ssid[0]) strlcpy(d->ssid, ssid, FLOCK_SSID_STR_LEN); else d->ssid[0] = '\0';
    int bi = mac_hash(mac);
    s_det_next[idx] = s_det_bucket[bi];
    s_det_bucket[bi] = idx;
    s_count++;
    return idx;
}

// ---- Dedup (drain task only, no lock needed) ----

#define FLOCK_DEDUP_SLOTS 8
static struct { char mac[FLOCK_MAC_STR_LEN]; uint32_t ts; } s_dedup[FLOCK_DEDUP_SLOTS];
static size_t s_dedup_idx = 0;
#define FLOCK_COOLDOWN_MS 5000

static bool should_suppress(const char *mac) {
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    for (size_t i = 0; i < FLOCK_DEDUP_SLOTS; i++) {
        if (strcmp(s_dedup[i].mac, mac) == 0) {
            if ((now - s_dedup[i].ts) < FLOCK_COOLDOWN_MS) return true;
            s_dedup[i].ts = now;
            return false;
        }
    }
    strlcpy(s_dedup[s_dedup_idx].mac, mac, FLOCK_MAC_STR_LEN);
    s_dedup[s_dedup_idx].ts = now;
    s_dedup_idx = (s_dedup_idx + 1) % FLOCK_DEDUP_SLOTS;
    return false;
}

// ---- Drain task ----

static TaskHandle_t s_drain_task = NULL;

static void drain_task_fn(void *arg) {
    (void)arg;
    while (s_running) {
        while (true) {
            portENTER_CRITICAL(&s_alert_mux);
            if (s_alert_tail == s_alert_head) { portEXIT_CRITICAL(&s_alert_mux); break; }
            FlockAlertEntry e;
            memcpy(&e, &s_alert_queue[s_alert_tail], sizeof(e));
            s_alert_tail = (s_alert_tail + 1) % FLOCK_ALERT_QUEUE_SIZE;
            portEXIT_CRITICAL(&s_alert_mux);

            char ms[FLOCK_MAC_STR_LEN];
            snprintf(ms, sizeof(ms), "%02x:%02x:%02x:%02x:%02x:%02x",
                     e.mac[0], e.mac[1], e.mac[2], e.mac[3], e.mac[4], e.mac[5]);

            if (!should_suppress(ms)) {
                int idx = det_find(ms);
                FlockConfidence conf = (e.method == FLOCK_DETECT_WILDCARD_PROBE || e.method == FLOCK_DETECT_SSID)
                                       ? FLOCK_CONF_HIGH : FLOCK_CONF_LOW;
                if (idx >= 0) {
                    if (s_dets[idx].count < 0xFFFF) s_dets[idx].count++;
                    s_dets[idx].last_seen_ms = (uint32_t)(esp_timer_get_time() / 1000);
                    s_dets[idx].rssi = e.rssi;
                    s_dets[idx].channel = e.channel;
                    if (conf == FLOCK_CONF_HIGH) s_dets[idx].confidence = FLOCK_CONF_HIGH;
                    if (s_dets[idx].device_class == FLOCK_CLASS_NONE) s_dets[idx].device_class = e.cls;
                    if (e.ssid[0] && !s_dets[idx].ssid[0])
                        strlcpy(s_dets[idx].ssid, e.ssid, FLOCK_SSID_STR_LEN);
                } else {
                    idx = det_insert(ms, e.method, e.rssi, e.channel, e.ssid, e.cls);
                }
                if (conf == FLOCK_CONF_HIGH) {
                    const char *sig = (e.rssi > -50) ? "Strong" : (e.rssi > -70) ? "Medium" : "Weak";
                    glog("[FLOCK] Surveillance device detected! %s | MAC: %s | Signal: %s (%ddBm) | Ch: %u%s%s | Hits: %d\n",
                         flock_detector_method_str(e.method), ms, sig, e.rssi, e.channel,
                         e.ssid[0] ? " | SSID: " : "", e.ssid[0] ? e.ssid : "",
                         (idx >= 0) ? s_dets[idx].count : 0);
                    pulse_once(&rgb_manager, 255, 255, 255);
                }
                if (idx >= 0 && s_cb) s_cb(&s_dets[idx], s_cb_data);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    vTaskDelete(NULL);
}

// ---- Channel hopping ----

static uint8_t s_ch_idx = 0;
static uint8_t s_ch_count = 0;
static uint8_t s_channels[WIFI_CHANNELS_MAX] = {0};
static esp_timer_handle_t s_hop_timer = NULL;
#define FLOCK_HOP_MS 250

static void hop_cb(void *arg) {
    if (!s_running) return;
    if (s_ch_count == 0) return;
    s_ch_idx = (s_ch_idx + 1) % s_ch_count;
    esp_wifi_set_channel(s_channels[s_ch_idx], WIFI_SECOND_CHAN_NONE);
}

// ---- WiFi sniffer callback (ISR context — no blocking, no glog, no mutex) ----

static inline uint8_t IRAM_ATTR oui_class(const uint8_t *mac) {
    uint32_t oui = ((uint32_t)mac[0] << 16) | ((uint32_t)mac[1] << 8) | mac[2];
    for (size_t i = 0; i < FLOCK_OUI_COUNT; i++) {
        if (oui == flock_oui_packed[i]) return FLOCK_CLASS_FLOCK;
    }
    for (size_t i = 0; i < CAMERA_OUI_COUNT; i++) {
        if ((camera_oui_packed[i] & 0xFFFFFF) == oui) return (uint8_t)(camera_oui_packed[i] >> 24);
    }
    return FLOCK_CLASS_NONE;
}

static int IRAM_ATTR is_wildcard_probe(const uint8_t *body, int len) {
    if (!body || len < 2) return -1;
    while (len >= 2) {
        uint8_t id = body[0], elen = body[1];
        if ((int)elen + 2 > len) break;
        if (id == 0) return (elen == 0) ? 1 : 0;
        body += elen + 2; len -= elen + 2;
    }
    return -1;
}

static bool IRAM_ATTR extract_ssid(const uint8_t *body, int len, char *out, size_t out_sz) {
    if (!body || len < 2 || !out || out_sz == 0) return false;
    while (len >= 2) {
        uint8_t id = body[0], elen = body[1];
        if ((int)elen + 2 > len) break;
        if (id == 0 && elen > 0) {
            size_t n = (elen < (out_sz - 1)) ? elen : (out_sz - 1);
            memcpy(out, body + 2, n);
            out[n] = '\0';
            return true;
        }
        body += elen + 2; len -= elen + 2;
    }
    return false;
}

static const char * const flock_ssid_keywords[] = {
    "flock", "FS Ext Battery", "Penguin", "Pigvision"
};
#define FLOCK_SSID_KW_COUNT (sizeof(flock_ssid_keywords) / sizeof(flock_ssid_keywords[0]))

// Vendors whose cameras leak an SSID prefix (Axon body-cam SSIDs are
// agency-set; low confidence, kept as a hint).
static const struct { const char *kw; uint8_t cls; } flock_ssid_extra[] = {
    {"AXON-", FLOCK_CLASS_AXON}, {"AB2-", FLOCK_CLASS_AXON},
    {"AB3-", FLOCK_CLASS_AXON}, {"AB4-", FLOCK_CLASS_AXON},
    {"Pineapple", FLOCK_CLASS_HAK5},
};
#define FLOCK_SSID_EXTRA_COUNT (sizeof(flock_ssid_extra) / sizeof(flock_ssid_extra[0]))

static uint8_t IRAM_ATTR match_ssid_class(const char *ssid) {
    if (!ssid || !ssid[0]) return FLOCK_CLASS_NONE;
    for (size_t i = 0; i < FLOCK_SSID_KW_COUNT; i++) {
        if (strstr(ssid, flock_ssid_keywords[i])) return FLOCK_CLASS_FLOCK;
    }
    for (size_t i = 0; i < FLOCK_SSID_EXTRA_COUNT; i++) {
        if (strstr(ssid, flock_ssid_extra[i].kw)) return flock_ssid_extra[i].cls;
    }
    return FLOCK_CLASS_NONE;
}

static void IRAM_ATTR flock_sniffer_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (!buf || !s_running) return;
    if (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA) return;
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    int len = pkt->rx_ctrl.sig_len;
    if (len < (int)sizeof(flock_mac_hdr_t)) return;
    flock_mac_hdr_t *hdr = (flock_mac_hdr_t *)pkt->payload;
    int8_t rssi = pkt->rx_ctrl.rssi;
    if (rssi < -95) return;
    uint8_t ch = (uint8_t)pkt->rx_ctrl.channel;

    uint8_t c2 = oui_class(hdr->addr2);
    if (c2) {
        bool emitted = false;
        if (type == WIFI_PKT_MGMT) {
            uint8_t fc0 = hdr->frame_ctrl & 0xFF;
            uint8_t ftype = (fc0 >> 2) & 0x03, sub = (fc0 >> 4) & 0x0F;
            if (ftype == 0 && sub == 4) {
                int bl = len - (int)sizeof(flock_mac_hdr_t);
                const uint8_t *body = pkt->payload + sizeof(flock_mac_hdr_t);
                int r = (bl > 0) ? is_wildcard_probe(body, bl) : -1;
                if (r == -1 && bl > 4) r = is_wildcard_probe(body, bl - 4);
                if (r == 1) { enqueue_alert(FLOCK_DETECT_WILDCARD_PROBE, hdr->addr2, rssi, ch, NULL, c2); emitted = true; }
            }
        }
        if (!emitted) enqueue_alert(FLOCK_DETECT_OUI_ADDR2, hdr->addr2, rssi, ch, NULL, c2);
    }

    uint8_t c1 = oui_class(hdr->addr1);
    if (!(hdr->addr1[0] & 0x01) && c1)
        enqueue_alert(FLOCK_DETECT_OUI_ADDR1, hdr->addr1, rssi, ch, NULL, c1);

    uint8_t c3 = oui_class(hdr->addr3);
    if (type == WIFI_PKT_MGMT && c3)
        enqueue_alert(FLOCK_DETECT_OUI_ADDR3, hdr->addr3, rssi, ch, NULL, c3);

    // SSID keyword matching on management frames (beacon, probe resp, probe req)
    if (type == WIFI_PKT_MGMT) {
        uint8_t fc0 = hdr->frame_ctrl & 0xFF;
        uint8_t ftype = (fc0 >> 2) & 0x03, sub = (fc0 >> 4) & 0x0F;
        const uint8_t *body = NULL;
        int blen = 0;
        if (ftype == 0 && (sub == 8 || sub == 5)) {
            int off = (int)sizeof(flock_mac_hdr_t) + 12;  // fixed params after beacon/probe resp
            if (len > off) { body = pkt->payload + off; blen = len - off; }
        } else if (ftype == 0 && sub == 4) {
            int off = (int)sizeof(flock_mac_hdr_t);  // IEs follow directly after probe req hdr
            if (len > off) { body = pkt->payload + off; blen = len - off; }
        }
        if (body && blen > 0) {
            char ssid[FLOCK_ALERT_SSID_LEN];
            uint8_t sc = FLOCK_CLASS_NONE;
            if (extract_ssid(body, blen, ssid, sizeof(ssid)) &&
                (sc = match_ssid_class(ssid)) != FLOCK_CLASS_NONE) {
                enqueue_alert(FLOCK_DETECT_SSID, hdr->addr2, rssi, ch, ssid, sc);
            }
        }
    }
}

// ---- Public API ----

void flock_detector_init(void) {
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    s_count = 0;
    s_running = false;
    memset(s_dedup, 0, sizeof(s_dedup));
    ESP_LOGI(TAG, "flock detector initialized (%d OUIs)", (int)FLOCK_OUI_COUNT);
}

void flock_detector_deinit(void) {
    flock_detector_stop();
    if (s_mutex) { vSemaphoreDelete(s_mutex); s_mutex = NULL; }
}

esp_err_t flock_detector_start(void) {
    if (s_running) return ESP_ERR_INVALID_STATE;

    s_dets = calloc(FLOCK_MAX_DETECTIONS, sizeof(FlockDetection));
    s_alert_queue = calloc(FLOCK_ALERT_QUEUE_SIZE, sizeof(FlockAlertEntry));
    det_chain_alloc();
    if (!s_dets || !s_alert_queue || !s_det_bucket || !s_det_next) {
        ESP_LOGE(TAG, "failed to allocate detection buffers");
        free(s_dets); s_dets = NULL;
        free(s_alert_queue); s_alert_queue = NULL;
        det_chain_free();
        return ESP_ERR_NO_MEM;
    }

    s_count = 0;
    det_chain_init();
    memset(s_dedup, 0, sizeof(s_dedup));
    s_ch_idx = 0;
    size_t profile_count = 0;
    hop_profile_resolve_monitor(s_channels, sizeof(s_channels), &profile_count);
    s_ch_count = (uint8_t)profile_count;
    if (s_ch_count == 0) {
        s_ch_count = wifi_channels_build_country_list(
            s_channels, sizeof(s_channels));
    }
    if (s_ch_count == 0) {
        s_channels[0] = 1;
        s_ch_count = 1;
    }
    s_alert_head = 0;
    s_alert_tail = 0;
    s_running = true;

    wifi_manager_start_monitor_mode(flock_sniffer_cb);
    esp_wifi_set_channel(s_channels[0], WIFI_SECOND_CHAN_NONE);

    esp_timer_create_args_t targs = { .callback = hop_cb, .name = "flock_hop" };
    esp_timer_create(&targs, &s_hop_timer);
    esp_timer_start_periodic(s_hop_timer, FLOCK_HOP_MS * 1000);

    xTaskCreate(drain_task_fn, "flock_drain", 3072, NULL, 2, &s_drain_task);

    glog("[FLOCK] Scanning for surveillance cameras on %u country/profile channels...\n",
         (unsigned)s_ch_count);
    ESP_LOGI(TAG, "detection started");
    return ESP_OK;
}

esp_err_t flock_detector_stop(void) {
    if (!s_running) return ESP_OK;
    s_running = false;

    // wait for drain task to see s_running=false and self-delete
    vTaskDelay(pdMS_TO_TICKS(200));
    s_drain_task = NULL;

    if (s_hop_timer) { esp_timer_stop(s_hop_timer); esp_timer_delete(s_hop_timer); s_hop_timer = NULL; }
    wifi_manager_stop_monitor_mode();

    int found = s_count;
    free(s_dets); s_dets = NULL;
    free(s_alert_queue); s_alert_queue = NULL;
    det_chain_free();
    s_count = 0;

    glog("[FLOCK] Scan stopped. %d surveillance device%s found.\n",
         found, found == 1 ? "" : "s");
    ESP_LOGI(TAG, "detection stopped");
    return ESP_OK;
}

bool flock_detector_is_running(void) { return s_running; }

int flock_detector_get_count(void) { return s_count; }

const FlockDetection* flock_detector_get_detection(int index) {
    if (!s_dets || index < 0 || index >= s_count) return NULL;
    return &s_dets[index];
}

void flock_detector_clear_detections(void) {
    if (!s_dets) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memset(s_dets, 0, FLOCK_MAX_DETECTIONS * sizeof(FlockDetection));
        det_chain_init();
        s_count = 0;
        xSemaphoreGive(s_mutex);
    }
}

void flock_detector_set_callback(FlockDetectorCallback cb, void *user_data) {
    s_cb = cb;
    s_cb_data = user_data;
}

const char* flock_detector_method_str(FlockDetectMethod method) {
    switch (method) {
        case FLOCK_DETECT_OUI_ADDR2:      return "OUI (transmitter)";
        case FLOCK_DETECT_OUI_ADDR1:      return "OUI (receiver)";
        case FLOCK_DETECT_OUI_ADDR3:      return "OUI (BSSID)";
        case FLOCK_DETECT_SSID:           return "SSID keyword";
        case FLOCK_DETECT_WILDCARD_PROBE: return "Wildcard probe";
        default:                          return "unknown";
    }
}
