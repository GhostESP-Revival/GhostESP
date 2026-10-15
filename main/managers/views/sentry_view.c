// sentry_view.c
//
// Sentry: one live view that fuses the surveillance-device detectors GhostESP
// already ships -- the Flock/Safety camera detector (WiFi OUI + SSID) and the
// BLE device-detect scan (trackers, skimmers, Meta glasses, Flipper gear) --
// into one selectable recent-devices list, with a per-device detail screen and
// a full-screen alert on a fresh hit.
//
// RAM: nothing here lives in .bss. The record cache, row handles and alert
// dedup table are heap-allocated on view entry (PSRAM first, then internal)
// and freed on exit, so the footprint only exists while the screen is open.
//
// The list is a persistent cache, not a live read: on boards without PSRAM the
// radios alternate and every stop frees the engine's records, so reading the
// engines directly would wipe the list each phase. Devices merge in by MAC and
// stay until ten newer unique MACs push them out -- a re-scan that misses a
// device must not drop it.
//
// Radio rules: PSRAM boards run Flock + BLE together in a coexistence session
// (ble_set_suspend_allowed(false)); PSRAM-less boards alternate wifi and ble
// like the environment sweep. Touch is routed by hand -- GhostESP views send
// LV_EVENT_CLICKED themselves; the LVGL indev does not deliver these taps.

#include "sdkconfig.h"

#include "managers/views/sentry_view.h"
#include "core/ouis.h"
#include "core/utils.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "gui/accessibility_fonts.h"
#include "gui/design_tokens.h"
#include "gui/detail_view.h"
#include "gui/lvgl_safe.h"
#include "gui/rssi_meter.h"
#include "gui/screen_layout.h"
#include "gui/theme_palette_api.h"
#include "gui/toast.h"
#include "gui/touch_bar.h"
#include "gui/view_input.h"
#include "managers/display_manager.h"
#include "managers/flock_detector_manager.h"
#include "managers/settings_manager.h"
#include "managers/wifi_manager.h"
#include "scans/ble/device_detect_scan.h"
#if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(GHOSTESP_NO_NATIVE_BLE)
#include "managers/ble_manager.h"
#endif
#include "lvgl.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// Ghostchi faces, reused as the alert mascot so the view shares the app's art
// instead of raw neon. They are linked only in display builds: screenless
// boards exclude the sprite sources to stay inside their app partition (see
// the CONFIG_WITH_SCREEN block in main/CMakeLists.txt), so the references must
// compile out there too. Same guard the lockscreen and plugin canvas use.
#ifdef CONFIG_WITH_SCREEN
extern const lv_img_dsc_t evil_50x50;
extern const lv_img_dsc_t angry_50x50;
extern const lv_img_dsc_t surpised_50x50;
extern const lv_img_dsc_t what2_50x50;
#endif

#define SENTRY_RECENT 10
#define SENTRY_REFRESH_MS 400
#define SENTRY_ALERT_MS 8000
#define SENTRY_ALERT_SCORE 80
#define SENTRY_ALERT_DEDUP 8
#define SENTRY_ALERT_COOLDOWN_MS 60000
#define SENTRY_PHASE_MS 8000

// Horizontal-swipe-to-cancel-tap threshold, matching badble/nfc: resistive
// XPT2046 panels use ratio 1, everything else 10.
#if CONFIG_LV_TOUCH_CONTROLLER_XPT2046
#define SENTRY_SWIPE_RATIO 1
#else
#define SENTRY_SWIPE_RATIO 10
#endif

// hit categories
enum { SENTRY_KIND_FLOCK = 0, SENTRY_KIND_TRACKER, SENTRY_KIND_SKIMMER,
       SENTRY_KIND_GEAR, SENTRY_KIND_OTHER };

typedef struct {
    bool used;
    bool ble;
    uint8_t kind;         // SENTRY_KIND_*
    uint8_t flock_class;  // FlockDeviceClass (flock hits only)
    bool alert;
    uint8_t score;
    int16_t rssi;
    uint8_t channel;
    uint32_t first_seen;
    uint32_t last_seen;
    uint32_t hits;
    uint32_t color;
    char mac[18];
    char tag[10];
    char name[32];
    char sub[20];
} sentry_rec_t;

typedef struct {
    lv_obj_t *card;
    lv_obj_t *head;
    lv_obj_t *detail;
} sentry_row_t;

typedef struct {
    char mac[18];
    uint32_t ts;
} sentry_seen_t;

typedef enum { SENTRY_LIST, SENTRY_DETAIL } sentry_mode_t;

static lv_obj_t *root_container = NULL;
static lv_obj_t *content_scroller = NULL;
static lv_timer_t *update_timer = NULL;
static lv_obj_t *lbl_state = NULL;
static lv_obj_t *lbl_summary = NULL;

static sentry_rec_t *s_rec = NULL;             // [SENTRY_RECENT]
static sentry_row_t *s_rows = NULL;            // [SENTRY_RECENT]
static sentry_seen_t *s_seen = NULL;           // [SENTRY_ALERT_DEDUP]
static sentry_rec_t *s_detail = NULL;          // only while the detail is open
static int s_rec_count = 0;
static int s_selected = 0;
static int s_rows_created = 0;
static size_t s_seen_idx = 0;

static gui_touch_bar_t touch_bar;
static detail_view_t *detail = NULL;
static rssi_meter_t *s_meter = NULL;   // live Track overlay, only while tracking
static int s_track_src = 0;            // 0 none, 1 BLE, 2 WiFi
static lv_obj_t *s_detail_scroll = NULL;  // detail region the current drag scrolls
static sentry_mode_t s_mode = SENTRY_LIST;

static lv_obj_t *alert_overlay = NULL;
static lv_timer_t *alert_timer = NULL;

static uint32_t c_bg = 0x0A0A0A;
static uint32_t c_card = 0x1A1A1A;
static uint32_t c_surface = 0x242424;
static uint32_t c_accent = 0x00FFFF;
static uint32_t c_on_accent = 0xFFFFFF;
static uint32_t c_text = 0xFFFFFF;
static uint32_t c_dim = 0x888888;
static uint32_t c_warn = 0xFFAA00;
static uint32_t c_error = 0xFF4444;
static uint32_t c_good = 0x00FF00;
static bool s_rounded = false;

static bool s_flock_started = false;
static bool s_ble_started = false;
static bool s_coexist = false;
static bool s_dual_phase = false;
static bool s_phase_ble = false;
static uint32_t s_phase_since = 0;

static bool s_alert_primed = false;
static bool s_touch_moved = false;
static touch_drag_t s_touch_drag = {0};

static void *sentry_alloc(size_t n) {
    // PSRAM first so the working set stays off internal RAM; internal heap is
    // the fallback on boards without PSRAM.
    void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = calloc(1, n);
    return p;
}

static bool has_psram(void) {
    return heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) > 0;
}

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool tiny_layout(void) { return LV_VER_RES <= 160; }
static bool compact_layout(void) { return LV_VER_RES <= 320; }

static lv_coord_t card_pad(void) {
    return tiny_layout() ? 2 : (compact_layout() ? 3 : 6);
}

static void format_mac(const uint8_t mac[6], char *out, size_t out_size) {
    snprintf(out, out_size, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static bool parse_mac(const char *s, uint8_t out[6]) {
    unsigned v[6];
    if (!s || sscanf(s, "%x:%x:%x:%x:%x:%x",
                     &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) out[i] = (uint8_t)v[i];
    return true;
}

static const char *ble_tag(BLEDetectDeviceType type) {
    switch (type) {
        case BLE_DETECT_DEVICE_AIRTAG:      return "AIR TAG";
        case BLE_DETECT_DEVICE_FLIPPER:     return "FLIPPER";
        case BLE_DETECT_DEVICE_SKIMMER:     return "SKIMMER";
        case BLE_DETECT_DEVICE_TILE:        return "TILE";
        case BLE_DETECT_DEVICE_SMARTTAG:    return "SMRTTAG";
        case BLE_DETECT_DEVICE_CHIPOLO:     return "CHIPOLO";
        case BLE_DETECT_DEVICE_AIRPODS:     return "AIRPODS";
        case BLE_DETECT_DEVICE_APPLE_WATCH: return "WATCH";
        case BLE_DETECT_DEVICE_FINDMY:      return "FIND MY";
        case BLE_DETECT_DEVICE_FASTPAIR:    return "FSTPAIR";
        case BLE_DETECT_DEVICE_HEARING_AID: return "HEAR AID";
        case BLE_DETECT_DEVICE_GAEN:        return "CONTACT";
        case BLE_DETECT_DEVICE_CHAMELEON:   return "CHAMELN";
        case BLE_DETECT_DEVICE_META_GLASSES: return "META";
        case BLE_DETECT_DEVICE_GOOGLE_TAG:   return "GOOGLE";
        default:                            return "BLE";
    }
}

static uint8_t ble_kind(BLEDetectDeviceType type) {
    switch (type) {
        case BLE_DETECT_DEVICE_SKIMMER:  return SENTRY_KIND_SKIMMER;
        case BLE_DETECT_DEVICE_FLIPPER:
        case BLE_DETECT_DEVICE_CHAMELEON: return SENTRY_KIND_GEAR;
        case BLE_DETECT_DEVICE_AIRTAG:
        case BLE_DETECT_DEVICE_TILE:
        case BLE_DETECT_DEVICE_SMARTTAG:
        case BLE_DETECT_DEVICE_CHIPOLO:
        case BLE_DETECT_DEVICE_FINDMY:
        case BLE_DETECT_DEVICE_GOOGLE_TAG: return SENTRY_KIND_TRACKER;
        default:                         return SENTRY_KIND_OTHER;
    }
}

static uint8_t ble_score(BLEDetectDeviceType type) {
    switch (type) {
        case BLE_DETECT_DEVICE_SKIMMER:   return 100;
        case BLE_DETECT_DEVICE_FLIPPER:
        case BLE_DETECT_DEVICE_CHAMELEON: return 90;
        case BLE_DETECT_DEVICE_META_GLASSES: return 85;
        case BLE_DETECT_DEVICE_AIRTAG:    return 80;
        case BLE_DETECT_DEVICE_TILE:
        case BLE_DETECT_DEVICE_SMARTTAG:
        case BLE_DETECT_DEVICE_CHIPOLO:
        case BLE_DETECT_DEVICE_FINDMY:
        case BLE_DETECT_DEVICE_GOOGLE_TAG: return 70;
        default:                          return 30;
    }
}

static uint32_t kind_color(uint8_t kind, bool alert) {
    switch (kind) {
        case SENTRY_KIND_SKIMMER: return c_error;
        case SENTRY_KIND_FLOCK:   return alert ? c_error : c_warn;
        case SENTRY_KIND_GEAR:
        case SENTRY_KIND_TRACKER: return c_warn;
        default:                  return c_accent;
    }
}

static const char *flock_class_tag(uint8_t cls) {
    switch (cls) {
        case FLOCK_CLASS_CAMERA: return "CAMERA";
        case FLOCK_CLASS_ALPR:   return "ALPR";
        case FLOCK_CLASS_RING:   return "RING";
        case FLOCK_CLASS_AXON:   return "AXON";
        case FLOCK_CLASS_HAK5:   return "PINEAPPLE";
        default:                 return "FLOCK";
    }
}

// Spelled-out reading of the row's tag: the specific vendor/device class, not
// the coarse colour/face bucket.
static const char *rec_category(const sentry_rec_t *r) {
    if (!r->ble) {
        switch (r->flock_class) {
            case FLOCK_CLASS_CAMERA: return "IP camera";
            case FLOCK_CLASS_ALPR:   return "License plate reader";
            case FLOCK_CLASS_RING:   return "Doorbell camera";
            case FLOCK_CLASS_AXON:   return "Body camera";
            case FLOCK_CLASS_HAK5:   return "WiFi Pineapple (Hak5)";
            default:                 return "Flock Safety camera";
        }
    }
    switch (r->kind) {
        case SENTRY_KIND_SKIMMER: return "Card skimmer";
        case SENTRY_KIND_GEAR:    return "Pentest hardware";
        case SENTRY_KIND_TRACKER: return "Tracker";
        default:                  return "BLE device";
    }
}

static const lv_img_dsc_t *kind_face(uint8_t kind) {
#ifdef CONFIG_WITH_SCREEN
    switch (kind) {
        case SENTRY_KIND_FLOCK:
        case SENTRY_KIND_SKIMMER: return &evil_50x50;
        case SENTRY_KIND_GEAR:    return &angry_50x50;
        case SENTRY_KIND_TRACKER: return &surpised_50x50;
        default:                  return &what2_50x50;
    }
#else
    // Screenless build: no ghostchi sprites are linked, so the alert overlay
    // simply gets no mascot. lv_img_set_src() accepts NULL.
    (void)kind;
    return NULL;
#endif
}

// ---- recent-device cache --------------------------------------------------

static int rec_find(const char *mac) {
    if (!s_rec) return -1;
    for (int i = 0; i < SENTRY_RECENT; i++) {
        if (s_rec[i].used && strcasecmp(s_rec[i].mac, mac) == 0) return i;
    }
    return -1;
}

static int rec_slot(void) {
    for (int i = 0; i < SENTRY_RECENT; i++) {
        if (!s_rec[i].used) return i;
    }
    int oldest = 0;
    for (int i = 1; i < SENTRY_RECENT; i++) {
        if (s_rec[i].last_seen < s_rec[oldest].last_seen) oldest = i;
    }
    return oldest;
}

static void rec_upsert(const char *mac, bool ble, uint8_t kind, uint8_t flock_class,
                       uint8_t score, bool alert, int8_t rssi, uint8_t ch,
                       uint32_t color, const char *tag, const char *name, const char *sub) {
    if (!s_rec) return;
    int i = rec_find(mac);
    if (i < 0) {
        i = rec_slot();
        memset(&s_rec[i], 0, sizeof(s_rec[i]));
        s_rec[i].used = true;
        s_rec[i].first_seen = now_ms();
        snprintf(s_rec[i].mac, sizeof(s_rec[i].mac), "%s", mac);
    }
    s_rec[i].ble = ble;
    s_rec[i].kind = kind;
    s_rec[i].flock_class = flock_class;
    s_rec[i].score = score;
    s_rec[i].alert = alert;
    s_rec[i].rssi = rssi;
    s_rec[i].channel = ch;
    s_rec[i].color = color;
    s_rec[i].last_seen = now_ms();
    if (s_rec[i].hits < 0xFFFFFFFFu) s_rec[i].hits++;
    snprintf(s_rec[i].tag, sizeof(s_rec[i].tag), "%s", tag);
    if (name && name[0]) snprintf(s_rec[i].name, sizeof(s_rec[i].name), "%s", name);
    if (sub && sub[0]) snprintf(s_rec[i].sub, sizeof(s_rec[i].sub), "%s", sub);
}

// Pull both engines into the cache, age out what has gone, keep newest first.
static void sentry_merge(void) {
    if (!s_rec) return;

    int fc = flock_detector_get_count();
    for (int i = 0; i < fc; i++) {
        const FlockDetection *d = flock_detector_get_detection(i);
        if (d == NULL) continue;
        bool high = (d->confidence == FLOCK_CONF_HIGH);
        uint8_t cls = d->device_class;
        bool vendor = (cls != FLOCK_CLASS_NONE && cls != FLOCK_CLASS_FLOCK);
        bool alert = high || vendor;
        rec_upsert(d->mac, false, SENTRY_KIND_FLOCK, cls,
                   (uint8_t)(vendor ? 85 : (high ? 95 : 50)), alert,
                   d->rssi, d->channel, alert ? c_error : c_warn,
                   flock_class_tag(cls), d->ssid[0] ? d->ssid : d->mac, d->method);
    }

    int bc = ble_device_detect_get_count();
    for (int i = 0; i < bc; i++) {
        BLEDetectDeviceInfo info;
        if (ble_device_detect_get_device(i, &info) != 0) continue;
        uint8_t kind = ble_kind(info.type);
        uint8_t score = ble_score(info.type);
        bool alert = (score >= SENTRY_ALERT_SCORE);
        char mac[18];
        format_mac(info.mac, mac, sizeof(mac));
        const char *name = info.name[0] ? info.name
                         : (info.subtype[0] ? info.subtype : mac);
        rec_upsert(mac, true, kind, FLOCK_CLASS_NONE, score, alert, info.rssi, 0,
                   kind_color(kind, alert), ble_tag(info.type), name,
                   ble_device_detect_type_to_string(info.type));
    }

    // No staleness eviction: a device seen once stays in the cache until ten
    // newer unique MACs push it out (LRU). A re-scan that misses it must not
    // drop it, so only LRU pressure removes entries.
    int w = 0;
    for (int i = 0; i < SENTRY_RECENT; i++) {
        if (!s_rec[i].used) continue;
        if (w != i) s_rec[w] = s_rec[i];
        w++;
    }
    for (int i = w; i < SENTRY_RECENT; i++) {
        s_rec[i].used = false;
        s_rec[i].mac[0] = '\0';
    }

    for (int i = 1; i < w; i++) {
        sentry_rec_t key = s_rec[i];
        int j = i - 1;
        while (j >= 0 && s_rec[j].last_seen < key.last_seen) {
            s_rec[j + 1] = s_rec[j];
            j--;
        }
        s_rec[j + 1] = key;
    }
    s_rec_count = w;
}

static void format_rec(const sentry_rec_t *r, char *head, size_t hs, char *det, size_t ds) {
    /* r->name can be an SSID or a MAC; r->mac is always a formatted MAC. Redact
     * stack copies only -- s_rec[] stays raw for parse/compare/track logic. */
    char name[sizeof(r->name)];
    privacy_redact_ssid_copy(r->name, name, sizeof(name));
    char mac[sizeof(r->mac)];
    snprintf(mac, sizeof(mac), "%s", r->mac);
    privacy_redact_mac(mac);
    snprintf(head, hs, "%s  %s", r->tag, name);
    if (r->ble) snprintf(det, ds, "%s  %ddBm", mac, (int)r->rssi);
    else snprintf(det, ds, "%s  %ddBm ch%u", mac, (int)r->rssi, (unsigned)r->channel);
}

// ---- radios ---------------------------------------------------------------

static void radio_start_flock(void) {
    if (flock_detector_is_running()) return;
    flock_detector_init();
    if (flock_detector_start() == ESP_OK) s_flock_started = true;
}

static void radio_stop_flock(void) {
    if (s_flock_started) {
        flock_detector_stop();
        s_flock_started = false;
    }
}

static void radio_start_ble(void) {
    if (ble_device_detect_is_active()) return;
    ble_device_detect_start();
    s_ble_started = true;
}

static void radio_stop_ble(void) {
    if (s_ble_started) {
        ble_device_detect_stop();
        ble_device_detect_clear_results();
        s_ble_started = false;
    }
}

// non-PSRAM: alternate one radio at a time, wifi then ble (the sweep order).
static void manage_phase(void) {
    if (!s_dual_phase) return;
    if (s_meter && rssi_meter_is_active(s_meter)) return;  // keep the tracked radio up
    uint32_t now = now_ms();
    if (now - s_phase_since < SENTRY_PHASE_MS) return;
    s_phase_since = now;
    if (s_phase_ble) {
        radio_stop_ble();
        radio_start_flock();
        s_phase_ble = false;
    } else {
        radio_stop_flock();
        radio_start_ble();
        s_phase_ble = true;
    }
}

// ---- alert ----------------------------------------------------------------

static bool alert_seen_recently(const char *mac) {
    if (!s_seen) return false;
    uint32_t now = now_ms();
    for (size_t i = 0; i < SENTRY_ALERT_DEDUP; i++) {
        if (s_seen[i].mac[0] && strcasecmp(s_seen[i].mac, mac) == 0 &&
            (now - s_seen[i].ts) < SENTRY_ALERT_COOLDOWN_MS) {
            return true;
        }
    }
    return false;
}

static void alert_mark(const char *mac) {
    if (!s_seen) return;
    strlcpy(s_seen[s_seen_idx].mac, mac, sizeof(s_seen[0].mac));
    s_seen[s_seen_idx].ts = now_ms();
    s_seen_idx = (s_seen_idx + 1) % SENTRY_ALERT_DEDUP;
}

static void hide_alert(void) {
    lvgl_timer_del_safe(&alert_timer);
    lvgl_obj_del_safe(&alert_overlay);
}

static void alert_timeout_cb(lv_timer_t *timer) {
    (void)timer;
    hide_alert();
}

static void show_alert(const sentry_rec_t *r) {
    hide_alert();
    display_manager_flush_input_queue();

    alert_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(alert_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(alert_overlay, lv_color_hex(c_bg), 0);
    lv_obj_set_style_bg_opa(alert_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(alert_overlay, lv_color_hex(r->color), 0);
    lv_obj_set_style_border_width(alert_overlay, 3, 0);
    lv_obj_set_style_radius(alert_overlay, 0, 0);
    lv_obj_set_style_pad_all(alert_overlay, tiny_layout() ? 4 : 10, 0);
    lv_obj_set_style_pad_row(alert_overlay, tiny_layout() ? 2 : 6, 0);
    lv_obj_clear_flag(alert_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(alert_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(alert_overlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *face = lv_img_create(alert_overlay);
    lv_img_set_src(face, kind_face(r->kind));

    char badge[24];
    snprintf(badge, sizeof(badge), "%s DETECTED", r->tag);
    lv_obj_t *tag = lv_label_create(alert_overlay);
    lv_label_set_text(tag, badge);
    lv_obj_set_style_text_color(tag, lv_color_hex(r->color), 0);
    lv_obj_set_style_text_font(tag, accessibility_get_font_title(), 0);

    char name[sizeof(r->name)];
    privacy_redact_ssid_copy(r->name, name, sizeof(name));
    lv_obj_t *line = lv_label_create(alert_overlay);
    lv_label_set_text(line, name);
    lv_obj_set_style_text_font(line, accessibility_get_font_body(), 0);
    lv_obj_set_style_text_color(line, lv_color_hex(c_text), 0);
    lv_obj_set_width(line, LV_PCT(100));
    lv_obj_set_style_text_align(line, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(line, LV_LABEL_LONG_DOT);

    char mac[sizeof(r->mac)];
    snprintf(mac, sizeof(mac), "%s", r->mac);
    privacy_redact_mac(mac);
    lv_obj_t *sub = lv_label_create(alert_overlay);
    lv_label_set_text_fmt(sub, "%s  %ddBm", mac, (int)r->rssi);
    lv_obj_set_style_text_font(sub, accessibility_get_font_small(), 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(c_dim), 0);
    lv_obj_set_width(sub, LV_PCT(100));
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);

    lv_obj_t *hint = lv_label_create(alert_overlay);
    lv_label_set_text(hint, "Tap to dismiss");
    lv_obj_set_style_text_font(hint, accessibility_get_font_small(), 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(c_dim), 0);

    alert_timer = lv_timer_create(alert_timeout_cb, SENTRY_ALERT_MS, NULL);
}

// ---- rows -----------------------------------------------------------------

static void sentry_row_create(int i) {
    if (!s_rows) return;
    lv_obj_t *card = lv_obj_create(content_scroller);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_hex(c_card), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, s_rounded ? 6 : 0, 0);
    lv_obj_set_style_pad_hor(card, card_pad(), 0);
    lv_obj_set_style_pad_ver(card, card_pad() + (tiny_layout() ? 1 : (compact_layout() ? 2 : 3)), 0);
    lv_obj_set_style_pad_row(card, tiny_layout() ? 1 : 2, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *head = lv_label_create(card);
    lv_obj_set_width(head, LV_PCT(100));
    lv_obj_set_style_text_font(head, accessibility_get_font_body(), 0);
    lv_label_set_long_mode(head, LV_LABEL_LONG_DOT);

    lv_obj_t *det = lv_label_create(card);
    lv_obj_set_width(det, LV_PCT(100));
    lv_obj_set_style_text_font(det, accessibility_get_font_small(), 0);
    lv_obj_set_style_text_color(det, lv_color_hex(c_dim), 0);
    lv_label_set_long_mode(det, LV_LABEL_LONG_DOT);

    lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
    s_rows[i].card = card;
    s_rows[i].head = head;
    s_rows[i].detail = det;
}

static void rows_ensure(int wanted) {
    if (!s_rows) return;
    if (wanted > SENTRY_RECENT) wanted = SENTRY_RECENT;
    if (wanted <= s_rows_created) return;
    for (int i = s_rows_created; i < wanted; i++) sentry_row_create(i);
    s_rows_created = wanted;
}

static void style_row(int i, bool selected) {
    if (!s_rows) return;
    lv_obj_t *card = s_rows[i].card;
    lv_obj_set_style_bg_color(card, lv_color_hex(selected ? c_accent : c_card), 0);
    lv_obj_set_style_text_color(s_rows[i].head,
                                lv_color_hex(selected ? c_on_accent : s_rec[i].color), 0);
    lv_obj_set_style_text_color(s_rows[i].detail,
                                lv_color_hex(selected ? c_on_accent : c_dim), 0);
}

static void render_rows(void) {
    if (!s_rows) return;
    int n = s_rec_count;
    rows_ensure(n > 0 ? n : 1);
    for (int i = 0; i < s_rows_created; i++) {
        if (n == 0) {
            if (i == 0) {
                lv_obj_clear_flag(s_rows[0].card, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_bg_color(s_rows[0].card, lv_color_hex(c_card), 0);
                lv_obj_set_style_text_color(s_rows[0].head, lv_color_hex(c_good), 0);
                lv_obj_set_style_text_color(s_rows[0].detail, lv_color_hex(c_dim), 0);
                lv_label_set_text(s_rows[0].head, "Clear");
                lv_label_set_text(s_rows[0].detail, "Listening for cameras, trackers, gear");
            } else {
                lv_obj_add_flag(s_rows[i].card, LV_OBJ_FLAG_HIDDEN);
            }
            continue;
        }
        if (i >= n) {
            lv_obj_add_flag(s_rows[i].card, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        char head[52];
        char det[52];
        format_rec(&s_rec[i], head, sizeof(head), det, sizeof(det));
        lv_obj_clear_flag(s_rows[i].card, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_rows[i].head, head);
        lv_label_set_text(s_rows[i].detail, det);
        style_row(i, i == s_selected);
    }
}

// ---- detail ---------------------------------------------------------------

static void close_detail(void) {
    if (detail) {
        detail_view_destroy(detail);
        detail = NULL;
    }
    free(s_detail);
    s_detail = NULL;
    s_detail_scroll = NULL;
    if (content_scroller) lv_obj_clear_flag(content_scroller, LV_OBJ_FLAG_HIDDEN);
    s_mode = SENTRY_LIST;
}

static void detail_back_cb(lv_event_t *event) {
    (void)event;
    close_detail();
}

static int ble_index_for_mac(const char *mac) {
    int bc = ble_device_detect_get_count();
    for (int i = 0; i < bc; i++) {
        BLEDetectDeviceInfo info;
        if (ble_device_detect_get_device(i, &info) != 0) continue;
        char m[18];
        format_mac(info.mac, m, sizeof(m));
        if (strcasecmp(m, mac) == 0) return i;
    }
    return -1;
}

// Live signal samplers for the RSSI ring: BLE detect tracker or WiFi tracker.
static bool sentry_track_sample(void *user, int8_t *out_rssi) {
    (void)user;
    bool fresh = false;
    if (!ble_device_detect_get_track_status(out_rssi, &fresh)) return false;
    return fresh;
}

static bool sentry_track_sample_wifi(void *user, int8_t *out_rssi) {
    (void)user;
    bool fresh = false;
    if (!wifi_manager_get_track_status(out_rssi, &fresh)) return false;
    return fresh;
}

static void stop_meter(void) {
    if (s_track_src == 1) {
        ble_device_detect_stop_tracking();
    } else if (s_track_src == 2) {
        wifi_manager_stop_tracking();
        // WiFi tracking sat on one channel and replaced the promiscuous
        // callback; bring the detectors back now.
        radio_start_flock();
        if (s_coexist) radio_start_ble();
        s_phase_ble = false;
        s_phase_since = now_ms();
    }
    s_track_src = 0;
    if (s_meter) {
        rssi_meter_destroy(s_meter);
        s_meter = NULL;
    }
}

// Which inputs leave the tracking view (mirrors the detail view's back keys).
static bool track_exit_requested(const InputEvent *event) {
    switch (event->type) {
        case INPUT_TYPE_EXIT_BUTTON:
        case INPUT_TYPE_HOME_BUTTON:
            return true;
        case INPUT_TYPE_JOYSTICK:
            return event->data.joystick_index == 0 || event->data.joystick_index == 1;
        case INPUT_TYPE_ENCODER:
            return event->data.encoder.button;
        case INPUT_TYPE_KEYBOARD: {
            uint8_t k = event->data.key_value;
            return k == LV_KEY_LEFT || k == LV_KEY_ESC || k == LV_KEY_ENTER ||
                   k == 13 || k == 10 || k == 'h' || k == 'q' || k == 'Q' || k == '`';
        }
        default:
            return false;
    }
}

// Track ring is up: any back/select (or a tap) stops tracking; other inputs
// are swallowed so they never leak to the list underneath.
static void track_overlay_input(InputEvent *event) {
    if (event->type == INPUT_TYPE_TOUCH) {
        if (event->data.touch_data.state == LV_INDEV_STATE_REL) stop_meter();
        return;
    }
    if (track_exit_requested(event)) stop_meter();
}

// Mirrors the BLE device-detect detail's Track row, for BLE and WiFi rows.
static void sentry_track_cb(lv_event_t *event) {
    (void)event;
    if (!s_detail) return;

    bool is_ble = s_detail->ble;
    uint8_t channel = s_detail->channel;
    uint8_t macb[6];
    bool have_mac = parse_mac(s_detail->mac, macb);

    // Label shown under the ring. A hidden-SSID camera's name is just its MAC,
    // so prefix the tag to give the meter a readable title ("RING AA:BB:..").
    char label[48];
    if (is_ble) snprintf(label, sizeof(label), "%s", s_detail->name);
    else snprintf(label, sizeof(label), "%s %s", s_detail->tag, s_detail->name);
    privacy_redact_ssid(label);  /* title only; tracking uses the raw MAC below */

    if (is_ble) {
        int idx = ble_index_for_mac(s_detail->mac);
        if (idx < 0 || !ble_device_detect_start_tracking(idx)) {
            toast_show("Track failed", TOAST_ERROR);
            return;
        }
        s_track_src = 1;
        close_detail();
        s_meter = rssi_meter_create(lv_scr_act(), "Track", label, sentry_track_sample, NULL);
    } else {
        if (!have_mac) {
            toast_show("Track failed", TOAST_ERROR);
            return;
        }
        // The WiFi tracker owns the promiscuous callback and a fixed channel,
        // so release the detectors first and bring them back on stop.
        radio_stop_flock();
        radio_stop_ble();
        wifi_manager_track_bssid(macb, channel, label);
        s_track_src = 2;
        close_detail();
        s_meter = rssi_meter_create(lv_scr_act(), "Track", label, sentry_track_sample_wifi, NULL);
    }

    if (!s_meter) {
        stop_meter();
        toast_show("Track failed", TOAST_ERROR);
        return;
    }
    rssi_meter_set_bottom_reserved(s_meter, gui_touch_bar_height());
}

static void open_detail(void) {
    if (s_rec_count <= 0 || s_selected < 0 || s_selected >= s_rec_count) return;

    free(s_detail);
    s_detail = sentry_alloc(sizeof(*s_detail));
    if (!s_detail) return;
    *s_detail = s_rec[s_selected];

    detail = detail_view_create(root_container, s_detail->tag);
    if (!detail) return;
    detail_view_set_bottom_reserved(detail, gui_touch_bar_height());

    char vendor[64] = "Unknown";
    (void)ouis_lookup_vendor(s_detail->mac, vendor, sizeof(vendor));

    /* Display copies only: s_detail stays raw for parse/compare/track. */
    char name[sizeof(s_detail->name)];
    privacy_redact_ssid_copy(s_detail->name, name, sizeof(name));
    char mac[sizeof(s_detail->mac)];
    snprintf(mac, sizeof(mac), "%s", s_detail->mac);
    privacy_redact_mac(mac);

    detail_view_add_info(detail, "Category", rec_category(s_detail));
    detail_view_add_info(detail, "Name", name);
    detail_view_add_info(detail, "MAC", mac);
    detail_view_add_info(detail, "Vendor", vendor);
    if (s_detail->sub[0]) detail_view_add_info(detail, "Type", s_detail->sub);
    if (!s_detail->ble) {
        detail_view_add_infof(detail, "Channel", "%u", (unsigned)s_detail->channel);
        detail_view_add_info(detail, "Confidence", s_detail->alert ? "High" : "Low");
    }
    detail_view_add_infof(detail, "Signal", "%d dBm", (int)s_detail->rssi);
    detail_view_add_infof(detail, "Hits", "%u", (unsigned)s_detail->hits);
    detail_view_add_infof(detail, "Age", "%us", (unsigned)((now_ms() - s_detail->first_seen) / 1000));

    detail_view_add_action(detail, "Track", sentry_track_cb, NULL);
    detail_view_add_back(detail, detail_back_cb, NULL);
    detail_view_set_selected(detail, 0);

    if (content_scroller) lv_obj_add_flag(content_scroller, LV_OBJ_FLAG_HIDDEN);
    s_mode = SENTRY_DETAIL;
}

static void activate_detail_row(void) {
    if (!detail) return;
    lv_obj_t *obj = detail_view_get_selected_obj(detail);
    if (obj) lv_event_send(obj, LV_EVENT_CLICKED, NULL);
}

// ---- input ----------------------------------------------------------------

static lv_obj_t *active_scroller(void) {
    if (s_mode == SENTRY_DETAIL && detail) {
        lv_obj_t *panel = detail_view_get_info_panel(detail);
        if (panel && lv_obj_is_valid(panel)) {
            lv_obj_update_layout(panel);
            if (lv_obj_get_scroll_top(panel) > 0 || lv_obj_get_scroll_bottom(panel) > 0) return panel;
        }
        lv_obj_t *list = detail_view_get_list(detail);
        if (list) return list;
    }
    return content_scroller;
}

// The detail has two scroll regions; drag the one the touch landed in.
static lv_obj_t *detail_scroll_target_for(int x, int y) {
    lv_obj_t *panel = detail_view_get_info_panel(detail);
    if (panel && lv_obj_is_valid(panel)) {
        lv_area_t a;
        lv_obj_get_coords(panel, &a);
        if (x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2) return panel;
    }
    return detail_view_get_list(detail);
}

static void scroll_active(bool up) {
    view_input_scroll_page(active_scroller(), up);
}

static void sentry_scroll_up_cb(lv_event_t *event) { (void)event; scroll_active(true); }
static void sentry_scroll_down_cb(lv_event_t *event) { (void)event; scroll_active(false); }

static void sentry_back_cb(lv_event_t *event) {
    (void)event;
    if (s_mode == SENTRY_DETAIL) close_detail();
    else display_manager_go_back();
}

static int row_at(int x, int y) {
    if (!s_rows) return -1;
    for (int i = 0; i < s_rows_created && i < s_rec_count; i++) {
        lv_obj_t *card = s_rows[i].card;
        if (!card || !lv_obj_is_valid(card)) continue;
        lv_area_t a;
        lv_obj_get_coords(card, &a);
        if (x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2) return i;
    }
    return -1;
}

// GhostESP views send the click themselves; the LVGL indev does not deliver it.
static void send_tap(lv_obj_t *list, int x, int y) {
    if (!list || !lv_obj_is_valid(list)) return;
    uint32_t cnt = lv_obj_get_child_cnt(list);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *child = lv_obj_get_child(list, (int32_t)i);
        if (!child) continue;
        lv_area_t a;
        lv_obj_get_coords(child, &a);
        if (x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2) {
            lv_event_send(child, LV_EVENT_CLICKED, NULL);
            return;
        }
    }
}

static bool touch_bar_tap(const lv_indev_data_t *d) {
    if (!touch_bar.bar || d->state != LV_INDEV_STATE_PR) return false;
    bool hu = false, hb = false, hd = false;
    gui_touch_bar_hit_test(&touch_bar, d->point.x, d->point.y, &hu, &hb, &hd);
    if (hu) { scroll_active(true); touch_drag_reset(&s_touch_drag); return true; }
    if (hd) { scroll_active(false); touch_drag_reset(&s_touch_drag); return true; }
    if (hb) { sentry_back_cb(NULL); touch_drag_reset(&s_touch_drag); return true; }
    return false;
}

static void select_move(int delta) {
    if (s_rec_count <= 0) return;
    int s = s_selected + delta;
    if (s < 0) s = 0;
    if (s > s_rec_count - 1) s = s_rec_count - 1;
    s_selected = s;
    render_rows();
}

static void detail_input(InputEvent *event) {
    if (event->type == INPUT_TYPE_TOUCH) {
        lv_indev_data_t *d = &event->data.touch_data;
        if (alert_overlay) {
            if (d->state == LV_INDEV_STATE_REL) hide_alert();
            return;
        }
        if (touch_bar_tap(d)) return;

        lv_obj_t *list = detail_view_get_list(detail);
        if (d->state == LV_INDEV_STATE_PR) {
            if (!s_touch_drag.started) {
                s_detail_scroll = detail_scroll_target_for(d->point.x, d->point.y);
                touch_drag_begin(&s_touch_drag, d);
                s_touch_moved = false;
            } else if (s_detail_scroll) {
                touch_drag_update(&s_touch_drag, d, s_detail_scroll);
                s_touch_moved = true;
            }
            return;
        }
        if (d->state == LV_INDEV_STATE_REL) {
            if (!s_touch_drag.started) return;
            bool dragged = touch_drag_release(&s_touch_drag, d);
            if (dragged || s_touch_moved) {
                display_manager_flush_pending_scroll();
                return;
            }
            send_tap(list, d->point.x, d->point.y);
        }
        return;
    }

    if (alert_overlay) {
        hide_alert();
        return;
    }

    switch (event->type) {
        case INPUT_TYPE_JOYSTICK: {
            int b = event->data.joystick_index;
            if (b == 2) detail_view_step_up(detail);
            else if (b == 4) detail_view_step_down(detail);
            else if (b == 1) activate_detail_row();
            else if (b == 0) close_detail();
            break;
        }
        case INPUT_TYPE_KEYBOARD: {
            uint32_t k = event->data.key_value;
            if (k == LV_KEY_UP || k == 'k' || k == ';') detail_view_step_up(detail);
            else if (k == LV_KEY_DOWN || k == 'j' || k == '.') detail_view_step_down(detail);
            else if (k == 13 || k == 10 || k == LV_KEY_ENTER) activate_detail_row();
            else if (view_input_is_back_event(event) || k == 'q' || k == 'Q') close_detail();
            break;
        }
        case INPUT_TYPE_ENCODER:
            if (event->data.encoder.button) activate_detail_row();
            else if (event->data.encoder.direction > 0) detail_view_step_down(detail);
            else if (event->data.encoder.direction < 0) detail_view_step_up(detail);
            break;
        case INPUT_TYPE_EXIT_BUTTON:
        case INPUT_TYPE_HOME_BUTTON:
            close_detail();
            break;
        default:
            break;
    }
}

static void list_input(InputEvent *event) {
    if (event->type == INPUT_TYPE_TOUCH) {
        lv_indev_data_t *d = &event->data.touch_data;
        if (alert_overlay) {
            if (d->state == LV_INDEV_STATE_REL) hide_alert();
            return;
        }
        if (touch_bar_tap(d)) return;

        if (d->state == LV_INDEV_STATE_PR) {
            if (!s_touch_drag.started) {
                touch_drag_begin(&s_touch_drag, d);
                s_touch_moved = false;
            } else if (content_scroller) {
                touch_drag_update(&s_touch_drag, d, content_scroller);
                s_touch_moved = true;
            }
            return;
        }
        if (d->state == LV_INDEV_STATE_REL) {
            if (!s_touch_drag.started) return;
            int dx = (int)d->point.x - (int)s_touch_drag.start_x;
            bool dragged = touch_drag_release(&s_touch_drag, d);
            if (dragged || s_touch_moved) {
                display_manager_flush_pending_scroll();
                return;
            }
            if (abs(dx) > view_input_swipe_threshold_x(SENTRY_SWIPE_RATIO)) return;

            // Optional thirds control: tapping the top/bottom third of the list
            // steps the selection, like the other list views.
            if (settings_get_thirds_control_enabled(&G_Settings) &&
                content_scroller && lv_obj_is_valid(content_scroller)) {
                lv_area_t a;
                lv_obj_get_coords(content_scroller, &a);
                if (a.y2 > a.y1 && d->point.x >= a.x1 && d->point.x <= a.x2 &&
                    d->point.y >= a.y1 && d->point.y <= a.y2) {
                    int yr = (int)d->point.y - (int)a.y1;
                    int h = (int)(a.y2 - a.y1);
                    if (yr < h / 3) { select_move(-1); return; }
                    if (yr > (h * 2) / 3) { select_move(1); return; }
                }
            }

            // Tapping a row opens it; tapping the background does nothing.
            int hit = row_at(d->point.x, d->point.y);
            if (hit >= 0 && s_rec_count > 0) {
                s_selected = hit;
                open_detail();
            }
        }
        return;
    }

    if (alert_overlay) {
        hide_alert();
        return;
    }

    switch (event->type) {
        case INPUT_TYPE_JOYSTICK: {
            int b = event->data.joystick_index;
            if (b == 2) select_move(-1);
            else if (b == 4) select_move(1);
            else if (b == 1) open_detail();
            else if (b == 0) display_manager_go_back();
            break;
        }
        case INPUT_TYPE_KEYBOARD: {
            uint32_t k = event->data.key_value;
            if (k == LV_KEY_UP || k == 'k' || k == ';') select_move(-1);
            else if (k == LV_KEY_DOWN || k == 'j' || k == '.') select_move(1);
            else if (k == 13 || k == 10 || k == LV_KEY_ENTER) open_detail();
            else if (view_input_is_back_event(event) || k == 'q' || k == 'Q') display_manager_go_back();
            break;
        }
        case INPUT_TYPE_ENCODER:
            if (event->data.encoder.button) open_detail();
            else if (event->data.encoder.direction > 0) select_move(1);
            else if (event->data.encoder.direction < 0) select_move(-1);
            break;
        case INPUT_TYPE_EXIT_BUTTON:
        case INPUT_TYPE_HOME_BUTTON:
            display_manager_go_back();
            break;
        default:
            break;
    }
}

static void sentry_input_callback(InputEvent *event) {
    if (!event) return;
    if (s_meter && rssi_meter_is_active(s_meter)) { track_overlay_input(event); return; }
    if (s_mode == SENTRY_DETAIL) detail_input(event);
    else list_input(event);
}

// ---- update ---------------------------------------------------------------

static void update_counts(int counts[6]) {
    memset(counts, 0, sizeof(int) * 6);
    for (int i = 0; i < s_rec_count; i++) {
        if (!s_rec[i].ble) { counts[0]++; if (s_rec[i].alert) counts[1]++; continue; }
        counts[5]++;
        if (s_rec[i].kind == SENTRY_KIND_SKIMMER) counts[3]++;
        else if (s_rec[i].kind == SENTRY_KIND_GEAR) counts[4]++;
        else if (s_rec[i].kind == SENTRY_KIND_TRACKER) counts[2]++;
    }
}

static void update_display_cb(lv_timer_t *timer) {
    (void)timer;

    if (s_meter && rssi_meter_is_active(s_meter)) {
        // Tracking: the RSSI ring owns the screen and the scan is paused, so the
        // list, alerts and radio phase stay frozen until tracking stops.
        return;
    }

    manage_phase();
    sentry_merge();

    if (s_selected >= s_rec_count) s_selected = s_rec_count > 0 ? s_rec_count - 1 : 0;

    int counts[6];
    update_counts(counts);

    if (lbl_state) {
        bool running = flock_detector_is_running() || ble_device_detect_is_active();
        char state[40];
        if (s_dual_phase && running) {
            int remain = (SENTRY_PHASE_MS - (int)(now_ms() - s_phase_since)) / 1000;
            if (remain < 0) remain = 0;
            snprintf(state, sizeof(state), "Scanning %s (%ds)", s_phase_ble ? "BLE" : "WiFi", remain);
        } else {
            snprintf(state, sizeof(state), "%s", running ? "Scanning" : "Idle");
        }
        lv_label_set_text(lbl_state, state);
        lv_obj_set_style_text_color(lbl_state, lv_color_hex(running ? c_good : c_dim), 0);
    }
    if (lbl_summary) {
        lv_label_set_text_fmt(lbl_summary,
                              "Cam %d  Tracker %d  Skimmer %d  Gear %d  BLE %d",
                              counts[0], counts[2], counts[3], counts[4], counts[5]);
    }

    render_rows();

    // Alert on the first alert-worthy device not seen in the cooldown window;
    // the first tick only marks what is already in range.
    const sentry_rec_t *pick = NULL;
    for (int i = 0; i < s_rec_count && pick == NULL; i++) {
        if (!s_rec[i].alert) continue;
        if (!s_alert_primed) {
            alert_mark(s_rec[i].mac);
            continue;
        }
        if (alert_seen_recently(s_rec[i].mac)) continue;
        pick = &s_rec[i];
    }
    if (!s_alert_primed) {
        s_alert_primed = true;
    } else if (pick != NULL) {
        alert_mark(pick->mac);
        show_alert(pick);
    }

    if (touch_bar.bar) gui_touch_bar_update_visibility(&touch_bar, active_scroller());
}

// ---- lifecycle ------------------------------------------------------------

void sentry_view_create(void) {
    if (sentry_view.root != NULL) return;

    s_rec = sentry_alloc(sizeof(sentry_rec_t) * SENTRY_RECENT);
    s_rows = sentry_alloc(sizeof(sentry_row_t) * SENTRY_RECENT);
    s_seen = sentry_alloc(sizeof(sentry_seen_t) * SENTRY_ALERT_DEDUP);
    s_rec_count = 0;
    s_rows_created = 0;
    s_selected = 0;
    s_seen_idx = 0;

    uint8_t theme = settings_get_menu_theme(&G_Settings);
    c_accent = theme_palette_get_accent(theme);
    c_bg = theme_palette_get_background(theme);
    c_card = theme_palette_get_surface(theme);
    c_surface = theme_palette_get_surface_alt(theme);
    c_on_accent = theme_palette_get_on_accent(theme);
    c_text = theme_palette_get_text(theme);
    c_dim = theme_palette_get_text_muted(theme);
    c_warn = theme_palette_get_warning(theme);
    c_error = theme_palette_get_danger(theme);
    c_good = theme_palette_get_success(theme);
    s_rounded = settings_get_menu_rounded(&G_Settings);

    s_mode = SENTRY_LIST;
    detail = NULL;
    s_alert_primed = false;
    s_touch_moved = false;
    touch_drag_reset(&s_touch_drag);

    s_flock_started = false;
    s_ble_started = false;
    s_coexist = false;
    s_dual_phase = false;
    s_phase_ble = false;
    s_phase_since = now_ms();

#if defined(CONFIG_IDF_TARGET_ESP32S2) || defined(GHOSTESP_NO_NATIVE_BLE)
    radio_start_flock();             // no BLE radio: WiFi only
#else
    if (has_psram()) {
        ble_set_suspend_allowed(false);
        s_coexist = true;
        radio_start_flock();
        radio_start_ble();
    } else {
        s_dual_phase = true;         // sequence wifi then ble
        radio_start_flock();
    }
#endif

    display_manager_fill_screen(lv_color_hex(c_bg));
    root_container = gui_screen_create_root(NULL, "Sentry", lv_color_hex(c_bg), LV_OPA_COVER);
    sentry_view.root = root_container;

    lv_obj_t *content = gui_screen_create_content_with_bottom_reserved(
        root_container, GUI_STATUS_BAR_HEIGHT, gui_touch_bar_height() + GUI_HOME_SAFE_H);
    content_scroller = content;
    lv_obj_set_style_text_color(content, lv_color_hex(c_text), 0);
    lv_obj_set_style_pad_hor(content, GUI_OPTIONS_LIST_PAD_HOR, 0);
    lv_obj_set_style_pad_ver(content, GUI_SAFEAREA_VER, 0);
    lv_obj_set_style_pad_row(content, compact_layout() ? 2 : 4, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    view_input_make_list_scrollable(content);

    lv_obj_t *status = lv_obj_create(content);
    lv_obj_set_width(status, LV_PCT(100));
    lv_obj_set_height(status, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(status, lv_color_hex(c_surface), 0);
    lv_obj_set_style_bg_opa(status, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(status, 0, 0);
    lv_obj_set_style_radius(status, s_rounded ? GUI_RADIUS_SM : 0, 0);
    lv_obj_set_style_pad_all(status, card_pad(), 0);
    lv_obj_set_style_pad_row(status, tiny_layout() ? 0 : 1, 0);
    lv_obj_clear_flag(status, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(status, LV_FLEX_FLOW_COLUMN);

    lbl_state = lv_label_create(status);
    lv_label_set_text(lbl_state, "Starting");
    lv_obj_set_style_text_font(lbl_state, accessibility_get_font_title(), 0);

    lbl_summary = lv_label_create(status);
    lv_obj_set_width(lbl_summary, LV_PCT(100));
    lv_label_set_long_mode(lbl_summary, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(lbl_summary, accessibility_get_font_small(), 0);
    lv_obj_set_style_text_color(lbl_summary, lv_color_hex(c_dim), 0);

    touch_bar = gui_touch_bar_create(root_container);
    if (touch_bar.bar) {
        gui_touch_bar_set_callbacks(&touch_bar,
                                    sentry_scroll_up_cb, NULL,
                                    sentry_back_cb, NULL,
                                    sentry_scroll_down_cb, NULL);
    }

    display_manager_add_status_bar("Sentry");
    update_timer = lv_timer_create(update_display_cb, SENTRY_REFRESH_MS, NULL);
    update_display_cb(NULL);
}

void sentry_view_destroy(void) {
    lvgl_timer_del_safe(&update_timer);
    hide_alert();
    close_detail();
    stop_meter();
    gui_touch_bar_destroy(&touch_bar);

    radio_stop_ble();
    radio_stop_flock();
#if !defined(CONFIG_IDF_TARGET_ESP32S2) && !defined(GHOSTESP_NO_NATIVE_BLE)
    if (s_coexist) {
        ble_set_suspend_allowed(true);
        s_coexist = false;
    }
#endif

    lvgl_obj_del_safe(&root_container);
    sentry_view.root = NULL;

    lbl_state = NULL;
    lbl_summary = NULL;
    content_scroller = NULL;

    free(s_rec);
    s_rec = NULL;
    free(s_rows);
    s_rows = NULL;
    free(s_seen);
    s_seen = NULL;
    s_rec_count = 0;
    s_rows_created = 0;

    touch_drag_reset(&s_touch_drag);
    s_touch_moved = false;
    s_alert_primed = false;
    s_mode = SENTRY_LIST;
}

static void get_sentry_callback(void **callback) {
    if (callback) {
        *callback = (void *)sentry_input_callback;
    }
}

View sentry_view = {
    .root = NULL,
    .create = sentry_view_create,
    .destroy = sentry_view_destroy,
    .input_callback = sentry_input_callback,
    .name = "SentryView",
    .get_hardwareinput_callback = get_sentry_callback
};
