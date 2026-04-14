#include "managers/ghostchi_manager.h"

#include "core/callbacks.h"
#include "core/utils.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "managers/sd_card_manager.h"
#include "managers/status_display_manager.h"
#include "managers/wifi_manager.h"
#include "scans/wifi/ap_scan.h"
#include "vendor/pcap.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <time.h>

#define GHOSTCHI_LEARN_MAX 32
#define GHOSTCHI_DIR "/mnt/ghostesp/ghostchi"
#define GHOSTCHI_PCAP_DIR "/mnt/ghostesp/ghostchi/pcaps"
#define GHOSTCHI_LOG_DIR "/mnt/ghostesp/ghostchi/sessions"
#define GHOSTCHI_LEARN_FILE "/mnt/ghostesp/ghostchi/learn.bin"
#define GHOSTCHI_LEARN_FILE_LEGACY "/mnt/ghostesp/ghostchi/learn.csv"
#define GHOSTCHI_STATE_FILE "/mnt/ghostesp/ghostchi/state.bin"
#define GHOSTCHI_LEARN_MAGIC 0x314C4847u
#define GHOSTCHI_STATE_MAGIC 0x31435447u
#define GHOSTCHI_LEARN_VERSION 1u
#define GHOSTCHI_STATE_VERSION 1u
#define GHOSTCHI_COOLDOWN_IDLE_MS 1500u
#define GHOSTCHI_COOLDOWN_SUCCESS_MS 1800u

#if CONFIG_SPIRAM
#define GHOSTCHI_SESSION_LOG_BUFFER_SIZE 2048u
#else
#define GHOSTCHI_SESSION_LOG_BUFFER_SIZE 256u
#endif

typedef struct __attribute__((packed)) {
    uint8_t bssid[6];
    uint8_t channel;
    int8_t best_rssi;
    uint16_t successes;
    uint16_t failures;
    uint32_t last_seen_s;
} ghostchi_learn_entry_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t entry_count;
    uint16_t reserved;
} ghostchi_learn_file_header_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version;
    uint8_t reserved[3];
    uint32_t total_sessions;
    uint32_t total_handshakes;
    uint32_t total_attempts;
    uint32_t total_failures;
    uint32_t last_session_end_s;
    uint8_t last_session_end_valid;
    uint8_t reserved2[3];
} ghostchi_state_file_t;

typedef struct {
    wifi_ap_record_t ap;
    uint16_t score;
    uint8_t confidence;
    uint16_t cooldown_s;
    int learn_idx;
    char reason[32];
} ghostchi_target_t;

typedef struct {
    uint16_t passive_ms;
    uint16_t settle_ms;
    uint16_t deauth_ms;
    bool allow_deauth;
} ghostchi_strategy_t;

static SemaphoreHandle_t s_lock;
static volatile bool s_stop_requested = false;
static bool s_running = false;
static bool s_scan_active = false;
static bool s_monitor_active = false;
static bool s_deauth_active = false;
static bool s_deauth_used = false;
static ghostchi_snapshot_t s_snapshot;
static ghostchi_learn_entry_t s_learn[GHOSTCHI_LEARN_MAX];
static size_t s_learn_count = 0;
static bool s_storage_ready = false;
static char s_session_log_path[128];
static char s_session_log_buffer[GHOSTCHI_SESSION_LOG_BUFFER_SIZE];
static size_t s_session_log_buffer_len = 0;
static uint32_t s_phase_deadline_ms = 0;
static uint32_t s_handshakes_before = 0;
static uint32_t s_total_handshakes = 0;
static uint32_t s_total_attempts = 0;
static uint32_t s_total_failures = 0;
static uint32_t s_last_session_end_s = 0;
static uint32_t s_total_sessions = 0;
static bool s_last_session_end_valid = false;
static ghostchi_strategy_t s_active_strategy;
static ghostchi_target_t s_current_target;
static bool s_pcap_capture_enabled = false;

static void save_learning(void);
static void save_state(void);
static void flush_session_log(void);
esp_err_t pcap_file_open_in_dir(const char *base_file_name,
                                const char *dir_path,
                                pcap_capture_type_t capture_type);

static uint32_t now_s(void) {
    return (uint32_t)(esp_timer_get_time() / 1000000ULL);
}

static uint32_t now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static bool rtc_now_s(uint32_t *out) {
    time_t t = time(NULL);
    if (t <= 1700000000) {
        return false;
    }
    if (out) {
        *out = (uint32_t)t;
    }
    return true;
}

static void mac_to_text(const uint8_t *mac, char *out, size_t out_len) {
    format_mac_address(mac, out, out_len, false);
}

static void ssid_to_text(const uint8_t *ssid, char *out, size_t out_len) {
    size_t n = 0;
    if (!out || out_len == 0) return;
    for (size_t i = 0; i < 32 && n + 1 < out_len; ++i) {
        uint8_t c = ssid[i];
        if (c == 0) break;
        out[n++] = (c >= 32 && c <= 126) ? (char)c : '.';
    }
    out[n] = '\0';
    if (n == 0) {
        strncpy(out, "<hidden>", out_len - 1);
        out[out_len - 1] = '\0';
    }
}

static bool ghostchi_needs_jit_mount(void) {
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    return strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0;
#else
    return false;
#endif
}

static bool ghostchi_sd_begin(bool *display_was_suspended, bool *mounted_here) {
    bool needs_jit = ghostchi_needs_jit_mount();
    if (display_was_suspended) *display_was_suspended = false;
    if (mounted_here) *mounted_here = false;

    if (needs_jit) {
        if (!sd_card_manager.is_initialized) {
            if (sd_card_mount_for_flush(display_was_suspended) != ESP_OK) {
                return false;
            }
            if (mounted_here) *mounted_here = true;
        }
        return true;
    }

    if (!sd_card_manager.is_initialized) {
        if (sd_card_init() != ESP_OK) {
            return false;
        }
    }
    return sd_card_manager.is_initialized;
}

static void ghostchi_sd_end(bool display_was_suspended, bool mounted_here) {
    if (ghostchi_needs_jit_mount() && mounted_here) {
        sd_card_unmount_after_flush(display_was_suspended);
    } else {
        (void)display_was_suspended;
    }
}

static bool storage_probe_if_needed(bool force) {
    bool display_was_suspended = false;
    bool mounted_here = false;
    bool ready = false;
    if (sd_card_manager.is_initialized && sd_card_exists("/mnt/ghostesp")) {
        s_storage_ready = true;
        return true;
    }
    if (!force) {
        return s_storage_ready;
    }
    if (!ghostchi_sd_begin(&display_was_suspended, &mounted_here)) {
        s_storage_ready = false;
        return false;
    }
    ready = sd_card_exists("/mnt/ghostesp");
    s_storage_ready = ready;
    ghostchi_sd_end(display_was_suspended, mounted_here);
    return ready;
}

static bool sd_ready(void) {
    return s_storage_ready || (sd_card_manager.is_initialized && sd_card_exists("/mnt/ghostesp"));
}

static uint32_t idle_age_s(bool *valid_out) {
    uint32_t now = 0;
    bool valid = s_last_session_end_valid && rtc_now_s(&now) && now >= s_last_session_end_s;
    if (valid_out) {
        *valid_out = valid;
    }
    if (valid) {
        return now - s_last_session_end_s;
    }
    return s_running ? 0u : now_s();
}

static void snapshot_refresh_pet_locked(void) {
    s_snapshot.idle_clock_valid = false;
    s_snapshot.idle_for_sec = idle_age_s(&s_snapshot.idle_clock_valid);
    s_snapshot.total_sessions = s_total_sessions;
}

static void snapshot_write_locked(ghostchi_state_t state, const char *status_line, const char *reason) {
    s_snapshot.sd_ready = sd_ready();
    s_snapshot.running = s_running;
    s_snapshot.state = state;
    s_snapshot.uptime_sec = now_s();
    snapshot_refresh_pet_locked();
    if (status_line) {
        strncpy(s_snapshot.status_line, status_line, sizeof(s_snapshot.status_line) - 1);
        s_snapshot.status_line[sizeof(s_snapshot.status_line) - 1] = '\0';
    }
    if (reason) {
        strncpy(s_snapshot.reason, reason, sizeof(s_snapshot.reason) - 1);
        s_snapshot.reason[sizeof(s_snapshot.reason) - 1] = '\0';
    }
}

static void snapshot_set_state(ghostchi_state_t state, const char *status_line, const char *reason) {
    if (!s_lock) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        snapshot_write_locked(state, status_line, reason);
        xSemaphoreGive(s_lock);
    }
}

static void snapshot_set_target(const wifi_ap_record_t *ap, uint16_t score, uint8_t confidence, const char *reason) {
    char ssid[33];
    char bssid[18];
    if (!ap || !s_lock) return;
    ssid_to_text(ap->ssid, ssid, sizeof(ssid));
    mac_to_text(ap->bssid, bssid, sizeof(bssid));
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        strncpy(s_snapshot.target_ssid, ssid, sizeof(s_snapshot.target_ssid) - 1);
        s_snapshot.target_ssid[sizeof(s_snapshot.target_ssid) - 1] = '\0';
        strncpy(s_snapshot.target_bssid, bssid, sizeof(s_snapshot.target_bssid) - 1);
        s_snapshot.target_bssid[sizeof(s_snapshot.target_bssid) - 1] = '\0';
        s_snapshot.current_channel = ap->primary;
        s_snapshot.confidence = confidence;
        if (reason) {
            strncpy(s_snapshot.reason, reason, sizeof(s_snapshot.reason) - 1);
            s_snapshot.reason[sizeof(s_snapshot.reason) - 1] = '\0';
        }
        (void)score;
        xSemaphoreGive(s_lock);
    }
}

static void session_log(const char *fmt, ...) {
    char line[192];
    bool display_was_suspended = false;
    bool mounted_here = false;
    FILE *f = NULL;
    size_t line_len;
    va_list args;
    if (s_session_log_path[0] == '\0') {
        return;
    }
    va_start(args, fmt);
    line_len = (size_t)vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    if (line_len >= sizeof(line)) {
        line_len = sizeof(line) - 1;
    }

    if (line_len == 0) {
        return;
    }

    if (line_len > sizeof(s_session_log_buffer)) {
        if (!ghostchi_sd_begin(&display_was_suspended, &mounted_here)) {
            return;
        }
        f = fopen(s_session_log_path, "a");
        if (f) {
            (void)fwrite(line, 1, line_len, f);
            fflush(f);
            fclose(f);
        }
        ghostchi_sd_end(display_was_suspended, mounted_here);
        return;
    }

    if (s_session_log_buffer_len + line_len > sizeof(s_session_log_buffer)) {
        flush_session_log();
    }

    if (s_session_log_buffer_len + line_len > sizeof(s_session_log_buffer)) {
        return;
    }

    memcpy(s_session_log_buffer + s_session_log_buffer_len, line, line_len);
    s_session_log_buffer_len += line_len;
}

static void flush_session_log(void) {
    bool display_was_suspended = false;
    bool mounted_here = false;
    FILE *f = NULL;

    if (s_session_log_path[0] == '\0' || s_session_log_buffer_len == 0) {
        return;
    }

    if (!ghostchi_sd_begin(&display_was_suspended, &mounted_here)) {
        return;
    }

    f = fopen(s_session_log_path, "a");
    if (f) {
        size_t written = fwrite(s_session_log_buffer, 1, s_session_log_buffer_len, f);
        if (written == s_session_log_buffer_len) {
            fflush(f);
            s_session_log_buffer_len = 0;
        }
        fclose(f);
    }

    ghostchi_sd_end(display_was_suspended, mounted_here);
}

static int find_learn_entry(const uint8_t *bssid) {
    for (size_t i = 0; i < s_learn_count; ++i) {
        if (memcmp(s_learn[i].bssid, bssid, 6) == 0) return (int)i;
    }
    return -1;
}

static int upsert_learn_entry(const uint8_t *bssid) {
    int idx = find_learn_entry(bssid);
    if (idx >= 0) return idx;
    if (s_learn_count < GHOSTCHI_LEARN_MAX) {
        idx = (int)s_learn_count++;
    } else {
        idx = 0;
        for (size_t i = 1; i < s_learn_count; ++i) {
            if (s_learn[i].last_seen_s < s_learn[idx].last_seen_s) idx = (int)i;
        }
    }
    memset(&s_learn[idx], 0, sizeof(s_learn[idx]));
    memcpy(s_learn[idx].bssid, bssid, 6);
    return idx;
}

static bool load_learning_legacy_csv(FILE *f) {
    bool loaded_any = false;
    while (!feof(f) && s_learn_count < GHOSTCHI_LEARN_MAX) {
        ghostchi_learn_entry_t entry;
        unsigned mac[6];
        memset(&entry, 0, sizeof(entry));
        if (fscanf(f,
                   "%02x:%02x:%02x:%02x:%02x:%02x,%hhu,%hhd,%hu,%hu,%" SCNu32 "\n",
                   &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5],
                   &entry.channel, &entry.best_rssi, &entry.successes,
                   &entry.failures, &entry.last_seen_s) == 11) {
            for (int i = 0; i < 6; ++i) entry.bssid[i] = (uint8_t)mac[i];
            s_learn[s_learn_count++] = entry;
            loaded_any = true;
        } else {
            char discard[128];
            if (!fgets(discard, sizeof(discard), f)) break;
        }
    }
    return loaded_any;
}

static void load_learning(void) {
    bool display_was_suspended = false;
    bool mounted_here = false;
    FILE *f;
    ghostchi_learn_file_header_t header;
    s_learn_count = 0;
    if (!ghostchi_sd_begin(&display_was_suspended, &mounted_here)) return;
    f = fopen(GHOSTCHI_LEARN_FILE, "rb");
    if (!f) {
        f = fopen(GHOSTCHI_LEARN_FILE_LEGACY, "r");
        if (f) {
            bool migrated = load_learning_legacy_csv(f);
            fclose(f);
            if (migrated) {
                save_learning();
            }
        }
        ghostchi_sd_end(display_was_suspended, mounted_here);
        return;
    }

    memset(&header, 0, sizeof(header));
    if (fread(&header, 1, sizeof(header), f) == sizeof(header) &&
        header.magic == GHOSTCHI_LEARN_MAGIC &&
        header.version == GHOSTCHI_LEARN_VERSION) {
        size_t to_read = header.entry_count;
        if (to_read > GHOSTCHI_LEARN_MAX) to_read = GHOSTCHI_LEARN_MAX;
        s_learn_count = fread(s_learn, sizeof(ghostchi_learn_entry_t), to_read, f);
    }
    fclose(f);
    ghostchi_sd_end(display_was_suspended, mounted_here);
}

static void save_learning(void) {
    bool display_was_suspended = false;
    bool mounted_here = false;
    FILE *f;
    ghostchi_learn_file_header_t header;
    if (!ghostchi_sd_begin(&display_was_suspended, &mounted_here)) return;
    f = fopen(GHOSTCHI_LEARN_FILE, "wb");
    if (!f) {
        ghostchi_sd_end(display_was_suspended, mounted_here);
        return;
    }
    header.magic = GHOSTCHI_LEARN_MAGIC;
    header.version = GHOSTCHI_LEARN_VERSION;
    header.entry_count = (uint8_t)s_learn_count;
    header.reserved = 0;

    if (fwrite(&header, 1, sizeof(header), f) == sizeof(header) && s_learn_count > 0) {
        (void)fwrite(s_learn, sizeof(ghostchi_learn_entry_t), s_learn_count, f);
    }
    fclose(f);
    remove(GHOSTCHI_LEARN_FILE_LEGACY);
    ghostchi_sd_end(display_was_suspended, mounted_here);
}

static void load_state(void) {
    bool display_was_suspended = false;
    bool mounted_here = false;
    FILE *f;
    ghostchi_state_file_t state;

    s_total_sessions = 0;
    s_total_handshakes = 0;
    s_total_attempts = 0;
    s_total_failures = 0;
    s_last_session_end_s = 0;
    s_last_session_end_valid = false;
    if (!ghostchi_sd_begin(&display_was_suspended, &mounted_here)) return;

    f = fopen(GHOSTCHI_STATE_FILE, "rb");
    if (!f) {
        ghostchi_sd_end(display_was_suspended, mounted_here);
        return;
    }

    memset(&state, 0, sizeof(state));
    if (fread(&state, 1, sizeof(state), f) == sizeof(state) &&
        state.magic == GHOSTCHI_STATE_MAGIC &&
        state.version == GHOSTCHI_STATE_VERSION) {
        s_total_sessions = state.total_sessions;
        s_total_handshakes = state.total_handshakes;
        s_total_attempts = state.total_attempts;
        s_total_failures = state.total_failures;
        s_last_session_end_s = state.last_session_end_s;
        s_last_session_end_valid = state.last_session_end_valid != 0;
    }
    fclose(f);
    ghostchi_sd_end(display_was_suspended, mounted_here);
}

static void save_state(void) {
    bool display_was_suspended = false;
    bool mounted_here = false;
    FILE *f;
    ghostchi_state_file_t state;

    if (!ghostchi_sd_begin(&display_was_suspended, &mounted_here)) return;
    f = fopen(GHOSTCHI_STATE_FILE, "wb");
    if (!f) {
        ghostchi_sd_end(display_was_suspended, mounted_here);
        return;
    }

    memset(&state, 0, sizeof(state));
    state.magic = GHOSTCHI_STATE_MAGIC;
    state.version = GHOSTCHI_STATE_VERSION;
    state.total_sessions = s_total_sessions;
    state.total_handshakes = s_total_handshakes;
    state.total_attempts = s_total_attempts;
    state.total_failures = s_total_failures;
    state.last_session_end_s = s_last_session_end_s;
    state.last_session_end_valid = s_last_session_end_valid ? 1u : 0u;
    (void)fwrite(&state, 1, sizeof(state), f);
    fclose(f);
    ghostchi_sd_end(display_was_suspended, mounted_here);
}

static bool ensure_storage(void) {
    bool display_was_suspended = false;
    bool mounted_here = false;
    bool ok = false;
    if (!ghostchi_sd_begin(&display_was_suspended, &mounted_here)) return false;
    ok = (sd_card_create_directory(GHOSTCHI_DIR) == ESP_OK || sd_card_exists(GHOSTCHI_DIR)) &&
         (sd_card_create_directory(GHOSTCHI_PCAP_DIR) == ESP_OK || sd_card_exists(GHOSTCHI_PCAP_DIR)) &&
         (sd_card_create_directory(GHOSTCHI_LOG_DIR) == ESP_OK || sd_card_exists(GHOSTCHI_LOG_DIR));
    ghostchi_sd_end(display_was_suspended, mounted_here);
    return ok;
}

static void open_session_log(void) {
    char name[32];
    char path[96];
    bool display_was_suspended = false;
    bool mounted_here = false;
    FILE *f = NULL;
    time_t t = time(NULL);
    struct tm tm_info;
    bool have_rtc = false;
    if (t > 1700000000) {
        struct tm *tm_ptr = localtime(&t);
        if (tm_ptr != NULL) {
            tm_info = *tm_ptr;
            have_rtc = true;
        }
    }
    if (have_rtc) {
        strftime(name, sizeof(name), "ghostchi_%Y%m%d_%H%M%S", &tm_info);
    } else {
        snprintf(name, sizeof(name), "ghostchi_%lu", (unsigned long)now_s());
    }
    snprintf(path, sizeof(path), "%s/%s.log", GHOSTCHI_LOG_DIR, name);
    strncpy(s_session_log_path, path, sizeof(s_session_log_path) - 1);
    s_session_log_path[sizeof(s_session_log_path) - 1] = '\0';
    s_session_log_buffer_len = 0;
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        strncpy(s_snapshot.session_name, name, sizeof(s_snapshot.session_name) - 1);
        s_snapshot.session_name[sizeof(s_snapshot.session_name) - 1] = '\0';
        xSemaphoreGive(s_lock);
    }
    if (!ghostchi_sd_begin(&display_was_suspended, &mounted_here)) {
        s_session_log_path[0] = '\0';
        return;
    }
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "Ghostchi session %s\n", name);
        fflush(f);
        fclose(f);
    }
    ghostchi_sd_end(display_was_suspended, mounted_here);
}

static ghostchi_strategy_t choose_strategy(uint16_t ap_visible) {
    ghostchi_strategy_t cfg;
    if (ap_visible >= 18) {
        cfg.passive_ms = 2200;
        cfg.settle_ms = 850;
        cfg.deauth_ms = 450;
    } else if (ap_visible <= 5) {
        cfg.passive_ms = 5000;
        cfg.settle_ms = 1500;
        cfg.deauth_ms = 850;
    } else {
        cfg.passive_ms = 3400;
        cfg.settle_ms = 1200;
        cfg.deauth_ms = 650;
    }
    cfg.allow_deauth = true;
    return cfg;
}

static bool auth_is_capture_worthy(wifi_auth_mode_t authmode) {
    switch (authmode) {
        case WIFI_AUTH_WPA_PSK:
        case WIFI_AUTH_WPA2_PSK:
        case WIFI_AUTH_WPA_WPA2_PSK:
        case WIFI_AUTH_WPA3_PSK:
        case WIFI_AUTH_WPA2_WPA3_PSK:
        case WIFI_AUTH_WPA2_ENTERPRISE:
        case WIFI_AUTH_WPA3_ENTERPRISE:
            return true;
        default:
            return false;
    }
}

static uint16_t score_candidate(const wifi_ap_record_t *ap, int *learn_idx_out, uint8_t *confidence_out, char *reason, size_t reason_len) {
    int score = 0;
    int confidence = 30;
    int learn_idx = upsert_learn_entry(ap->bssid);
    ghostchi_learn_entry_t *learn = &s_learn[learn_idx];
    bool hidden = (ap->ssid[0] == 0);

    if (auth_is_capture_worthy(ap->authmode)) {
        score += 70;
        confidence += 18;
    } else {
        score -= 40;
        confidence -= 25;
    }
    if (ap->rssi > -55) {
        score += 34;
        confidence += 16;
    } else if (ap->rssi > -65) {
        score += 24;
        confidence += 10;
    } else if (ap->rssi > -75) {
        score += 12;
        confidence += 5;
    } else {
        score -= 8;
    }
    if (!hidden) score += 8;
    if (learn->successes > 0) {
        score += 12 + learn->successes * 8;
        confidence += 12;
    }
    if (learn->failures > 0) {
        score -= learn->failures * 7;
        confidence -= learn->failures * 4;
    }
    if (learn->channel == ap->primary && learn->successes > 0) {
        score += 8;
    }

    if (confidence < 5) confidence = 5;
    if (confidence > 99) confidence = 99;
    if (reason && reason_len > 0) {
        snprintf(reason, reason_len, "%s+rssi%s%s",
                 auth_is_capture_worthy(ap->authmode) ? "wpa" : "weaksec",
                 hidden ? "+hidden" : "",
                 learn->successes ? "+history" : "");
    }
    if (learn_idx_out) *learn_idx_out = learn_idx;
    if (confidence_out) *confidence_out = (uint8_t)confidence;
    return (uint16_t)((score < 0) ? 0 : score);
}

static bool select_best_target(ghostchi_target_t *best_target, uint16_t *visible_count_out, uint8_t *cooldown_count_out) {
    uint16_t count = 0;
    wifi_ap_record_t *aps = NULL;
    bool found = false;
    uint8_t cooldown_count = 0;
    int best_rank = -1;
    ap_scan_get_results(&count, &aps);
    if (visible_count_out) *visible_count_out = count;
    if (cooldown_count_out) *cooldown_count_out = 0;
    if (!best_target || !aps) return false;

    memset(best_target, 0, sizeof(*best_target));
    for (uint16_t i = 0; i < count; ++i) {
        ghostchi_target_t candidate;
        if (!auth_is_capture_worthy(aps[i].authmode)) continue;
        memset(&candidate, 0, sizeof(candidate));
        candidate.ap = aps[i];
        candidate.score = score_candidate(&aps[i], &candidate.learn_idx,
                                          &candidate.confidence,
                                          candidate.reason,
                                          sizeof(candidate.reason));
        ghostchi_learn_entry_t *learn = &s_learn[candidate.learn_idx];
        uint32_t elapsed = now_s() - learn->last_seen_s;
        int rank;

        candidate.cooldown_s = (learn->failures >= 3 && elapsed < 90) ? (uint16_t)(90 - elapsed) : 0;
        if (candidate.cooldown_s > 0) {
            ++cooldown_count;
            continue;
        }

        rank = (int)candidate.score;
        if (!found || rank > best_rank) {
            *best_target = candidate;
            best_rank = rank;
            found = true;
        }
    }
    if (cooldown_count_out) *cooldown_count_out = cooldown_count;
    return found;
}

static void refresh_runtime_counts(uint16_t aps_visible, uint8_t cooldown_targets) {
    if (!s_lock) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_snapshot.aps_visible = aps_visible;
        s_snapshot.cooldown_targets = cooldown_targets;
        s_snapshot.handshakes = wifi_callbacks_get_handshake_count();
        snapshot_refresh_pet_locked();
        xSemaphoreGive(s_lock);
    }
}

static void target_result_update(const ghostchi_target_t *target, bool success) {
    if (!target || target->learn_idx < 0 || target->learn_idx >= (int)s_learn_count) return;
    ghostchi_learn_entry_t *learn = &s_learn[target->learn_idx];
    learn->channel = target->ap.primary;
    if (target->ap.rssi > learn->best_rssi || learn->best_rssi == 0) learn->best_rssi = target->ap.rssi;
    learn->last_seen_s = now_s();
    if (success) ++learn->successes;
    else ++learn->failures;
}

static void ghostchi_stop_radio(void) {
    if (s_deauth_active) {
        wifi_manager_stop_deauth();
        s_deauth_active = false;
    }
    if (s_monitor_active) {
        wifi_manager_stop_monitor_mode();
        s_monitor_active = false;
    }
    if (s_scan_active) {
        ap_scan_stop();
        s_scan_active = false;
    }
}

static void ghostchi_enter_cooldown(const char *status, const char *reason, uint32_t delay_ms, bool count_failure) {
    ghostchi_stop_radio();
    ap_scan_clear_results();
    if (count_failure && s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        ++s_snapshot.failures;
        xSemaphoreGive(s_lock);
    }
    snapshot_set_state(GHOSTCHI_STATE_COOLDOWN, status, reason);
    s_phase_deadline_ms = now_ms() + delay_ms;
}

static void ghostchi_finalize_stop(void) {
    uint32_t rtc_now = 0;
    snapshot_set_state(GHOSTCHI_STATE_STOPPING, "stopping", "flushing capture buffers");
    ghostchi_stop_radio();
    ap_scan_clear_results();
    if (s_pcap_capture_enabled) {
        pcap_file_close();
    } else {
        pcap_discard_buffer();
        cleanup_pcap_queue();
    }
    wifi_callbacks_set_pcap_enabled(true);
    s_pcap_capture_enabled = false;
    
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_total_handshakes += s_snapshot.handshakes;
        s_total_attempts += s_snapshot.attempts;
        s_total_failures += s_snapshot.failures;
        xSemaphoreGive(s_lock);
    }
    
    s_last_session_end_valid = rtc_now_s(&rtc_now);
    s_last_session_end_s = s_last_session_end_valid ? rtc_now : 0;
    flush_session_log();
    save_state();
    save_learning();
    s_session_log_path[0] = '\0';
    s_session_log_buffer_len = 0;
    status_display_show_status("Ghostchi Off");
    s_running = false;
    s_stop_requested = false;
    s_deauth_used = false;
    s_phase_deadline_ms = 0;
    memset(&s_current_target, 0, sizeof(s_current_target));
    snapshot_set_state(s_storage_ready ? GHOSTCHI_STATE_IDLE : GHOSTCHI_STATE_BLOCKED,
                       s_storage_ready ? "ready" : "sd required",
                       s_storage_ready ? "automated hunter" : "insert mounted sd");
}

void ghostchi_manager_tick(void) {
    uint32_t now = now_ms();
    uint32_t handshakes_now;

    if (!s_running) {
        return;
    }

    if (s_stop_requested) {
        ghostchi_finalize_stop();
        return;
    }

    handshakes_now = wifi_callbacks_get_handshake_count();
    if (s_monitor_active && handshakes_now > s_handshakes_before) {
        if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
            s_snapshot.handshakes = handshakes_now;
            strncpy(s_snapshot.status_line, "handshake captured", sizeof(s_snapshot.status_line) - 1);
            s_snapshot.status_line[sizeof(s_snapshot.status_line) - 1] = '\0';
            xSemaphoreGive(s_lock);
        }
        target_result_update(&s_current_target, true);
        session_log("result=success total=%lu\n", (unsigned long)handshakes_now);
        ghostchi_enter_cooldown("capture confirmed", s_current_target.reason, GHOSTCHI_COOLDOWN_SUCCESS_MS, false);
        return;
    }

    switch (s_snapshot.state) {
        case GHOSTCHI_STATE_SWEEP:
            if (!s_scan_active) {
                if (ap_scan_start_async() != ESP_OK) {
                    ghostchi_enter_cooldown("scan failed", "retrying", GHOSTCHI_COOLDOWN_IDLE_MS, false);
                } else {
                    s_scan_active = true;
                }
                return;
            }
            if (ap_scan_check_done()) {
                ghostchi_target_t best_target;
                uint8_t cooldown_count = 0;
                uint16_t visible_count = 0;

                ap_scan_finish_async();
                s_scan_active = false;
                if (select_best_target(&best_target, &visible_count, &cooldown_count)) {
                    s_current_target = best_target;
                    s_active_strategy = choose_strategy(visible_count);
                    s_deauth_used = false;
                    s_handshakes_before = handshakes_now;
                    selected_ap = s_current_target.ap;
                    refresh_runtime_counts(visible_count, cooldown_count);
                    snapshot_set_target(&s_current_target.ap, s_current_target.score, s_current_target.confidence, s_current_target.reason);
                    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
                        ++s_snapshot.attempts;
                        xSemaphoreGive(s_lock);
                    }
                    session_log("target=%02x:%02x:%02x:%02x:%02x:%02x ch=%u score=%u reason=%s\n",
                                s_current_target.ap.bssid[0], s_current_target.ap.bssid[1], s_current_target.ap.bssid[2],
                                s_current_target.ap.bssid[3], s_current_target.ap.bssid[4], s_current_target.ap.bssid[5],
                                s_current_target.ap.primary, s_current_target.score, s_current_target.reason);
                    wifi_manager_start_monitor_mode(wifi_eapol_scan_callback);
                    s_monitor_active = true;
                    s_phase_deadline_ms = now + s_active_strategy.passive_ms;
                    snapshot_set_state(GHOSTCHI_STATE_LOCK, "listening", s_current_target.reason);
                } else {
                    refresh_runtime_counts(visible_count, cooldown_count);
                    ap_scan_clear_results();
                    session_log("no_target aps=%u cooldown=%u\n", (unsigned)visible_count, (unsigned)cooldown_count);
                    ghostchi_enter_cooldown("no viable target", "waiting for stronger wpa aps", GHOSTCHI_COOLDOWN_IDLE_MS, true);
                }
            }
            return;

        case GHOSTCHI_STATE_LOCK:
            if (now < s_phase_deadline_ms) {
                return;
            }
            if (s_active_strategy.allow_deauth && !s_deauth_used) {
                s_monitor_active = false;
                wifi_manager_start_deauth();
                s_deauth_active = true;
                s_deauth_used = true;
                s_phase_deadline_ms = now + s_active_strategy.deauth_ms;
                snapshot_set_state(GHOSTCHI_STATE_STIM, "deauth burst", s_current_target.reason);
            } else {
                target_result_update(&s_current_target, false);
                session_log("result=miss\n");
                ghostchi_enter_cooldown("missed target", s_current_target.reason, 1200u, true);
            }
            return;

        case GHOSTCHI_STATE_STIM:
            if (now < s_phase_deadline_ms) {
                return;
            }
            if (s_deauth_active) {
                wifi_manager_stop_deauth();
                s_deauth_active = false;
            }
            wifi_manager_start_monitor_mode(wifi_eapol_scan_callback);
            s_monitor_active = true;
            s_phase_deadline_ms = now + s_active_strategy.settle_ms;
            snapshot_set_state(GHOSTCHI_STATE_LOCK, "relocking", s_current_target.reason);
            return;

        case GHOSTCHI_STATE_COOLDOWN:
            if (now >= s_phase_deadline_ms) {
                snapshot_set_state(GHOSTCHI_STATE_SWEEP, "sweeping", "ranking nearby aps");
            }
            return;

        default:
            return;
    }
}

void ghostchi_manager_get_snapshot(ghostchi_snapshot_t *out) {
    bool idle_valid = false;
    if (!out) return;
    if (!s_lock) {
        memset(out, 0, sizeof(*out));
        out->state = s_storage_ready ? GHOSTCHI_STATE_IDLE : GHOSTCHI_STATE_BLOCKED;
        out->sd_ready = s_storage_ready;
        out->idle_for_sec = idle_age_s(&idle_valid);
        out->idle_clock_valid = idle_valid;
        out->total_sessions = s_total_sessions;
        out->handshakes = s_total_handshakes;
        out->attempts = s_total_attempts;
        out->failures = s_total_failures;
        strncpy(out->status_line, out->sd_ready ? "ready" : "sd required", sizeof(out->status_line) - 1);
        return;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        snapshot_refresh_pet_locked();
        *out = s_snapshot;
        out->handshakes = s_total_handshakes + s_snapshot.handshakes;
        out->attempts = s_total_attempts + s_snapshot.attempts;
        out->failures = s_total_failures + s_snapshot.failures;
        out->total_sessions = s_total_sessions;
        xSemaphoreGive(s_lock);
    } else {
        memset(out, 0, sizeof(*out));
    }
}

bool ghostchi_manager_start(void) {
    if (s_running) return true;
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        memset(&s_snapshot, 0, sizeof(s_snapshot));
    }
    if (!storage_probe_if_needed(true) || !ensure_storage()) {
        snapshot_set_state(GHOSTCHI_STATE_BLOCKED, "sd required", "insert mounted sd");
        return false;
    }

    load_learning();
    load_state();
    open_session_log();
    wifi_callbacks_reset_handshake_tracking();
    s_pcap_capture_enabled = true;
    wifi_callbacks_set_pcap_enabled(s_pcap_capture_enabled);
    status_display_show_status("Ghostchi On");
    if (pcap_file_open_in_dir("ghostchi", GHOSTCHI_PCAP_DIR, PCAP_CAPTURE_WIFI) != ESP_OK) {
        snapshot_set_state(GHOSTCHI_STATE_BLOCKED, "pcap open failed", "check sd write access");
        s_session_log_path[0] = '\0';
        wifi_callbacks_set_pcap_enabled(true);
        s_pcap_capture_enabled = false;
        return false;
    }

    s_running = true;
    ++s_total_sessions;
    s_last_session_end_s = 0;
    s_last_session_end_valid = false;
    save_state();
    s_stop_requested = false;
    s_scan_active = false;
    s_monitor_active = false;
    s_deauth_active = false;
    s_deauth_used = false;
    s_phase_deadline_ms = 0;
    memset(&s_current_target, 0, sizeof(s_current_target));
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_snapshot.running = true;
        s_snapshot.handshakes = 0;
        s_snapshot.attempts = 0;
        s_snapshot.failures = 0;
        s_snapshot.current_channel = 0;
        s_snapshot.confidence = 0;
        s_snapshot.aps_visible = 0;
        s_snapshot.cooldown_targets = 0;
        s_snapshot.target_ssid[0] = '\0';
        s_snapshot.target_bssid[0] = '\0';
        snapshot_refresh_pet_locked();
        xSemaphoreGive(s_lock);
    }
    session_log("mode=automated hunter\n");
    snapshot_set_state(GHOSTCHI_STATE_SWEEP, "sweeping", "ranking nearby aps");
    return true;
}

bool ghostchi_manager_probe_storage(void) {
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        memset(&s_snapshot, 0, sizeof(s_snapshot));
    }
    bool ready = storage_probe_if_needed(true);
    if (ready) {
        load_state();
    }
    snapshot_set_state(ready ? GHOSTCHI_STATE_IDLE : GHOSTCHI_STATE_BLOCKED,
                       ready ? "ready" : "sd required",
                       ready ? "automated hunter" : "insert mounted sd");
    return ready;
}

void ghostchi_manager_stop(void) {
    if (!s_running) return;
    s_stop_requested = true;
}
