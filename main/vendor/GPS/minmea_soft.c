#include "vendor/GPS/minmea_soft.h"

#include "core/callbacks.h"
#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "vendor/GPS/minmea.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SOFT_RX_LINE_MAX 160
#define SOFT_RX_MAX_SYMBOLS 256
#define SOFT_RX_EVENT_QUEUE_LEN 8
#define SOFT_RX_RESOLUTION_HZ 1000000
#define SOFT_RX_BIT_TOLERANCE_PCT 35
#define SOFT_RX_MEM_BLOCK_SYMBOLS 48
#define SOFT_RX_SIGNAL_MIN_DIV 64
#define SOFT_RX_SIGNAL_MAX_BITS 40
#define SOFT_RX_SIGNAL_RANGE_MIN_NS_FLOOR 1000
#define SOFT_RX_SIGNAL_RANGE_MIN_NS_CEIL 2500
#define SOFT_RX_SIGNAL_RANGE_MAX_NS_FLOOR 1000000
#define SOFT_RX_SIGNAL_RANGE_MAX_NS_CEIL 8000000
#define SOFT_RX_STATS_LOG_MS 20000
#define SOFT_RX_UNKNOWN_LOG_BUDGET 4
#define SOFT_RX_GGA_NOFIX_LOG_BUDGET 3
#define SOFT_RX_TASK_PRIORITY 9
#define SOFT_RX_REARM_RETRY_MS 100
#define SOFT_RX_REARM_RETRY_LOG_MS 2000
#define SOFT_RX_LOCAL_TURNOVER_STALL_MS 1000
#define SOFT_RX_LOCAL_TURNOVER_COOLDOWN_MS 1200

typedef struct {
    uint32_t start_us;
    uint32_t end_us;
    uint8_t level;
} soft_uart_segment_t;

#define SOFT_RX_NUM_BUFFERS 2

typedef struct {
    size_t num_symbols;
    uint8_t buffer_idx;
} soft_rx_event_copy_t;

typedef struct {
    esp_gps_t gps;
    gpio_num_t rx_pin;
    uint32_t baud_rate;
    uint32_t bit_time_us;
    TaskHandle_t task;
    QueueHandle_t event_queue;
    rmt_channel_handle_t rx_chan;
    rmt_symbol_word_t rx_symbols[SOFT_RX_NUM_BUFFERS][SOFT_RX_MAX_SYMBOLS];
    volatile uint8_t active_buffer;
    soft_uart_segment_t *segments;
    size_t segments_capacity;
    volatile bool running;
    volatile bool rearm_pending;
    bool edge_isr_registered;
    char line_buf[SOFT_RX_LINE_MAX];
    size_t line_len;
} minmea_soft_ctx_t;

static const char *TAG = "minmea_soft";
static esp_err_t s_minmea_soft_last_error = ESP_OK;
static minmea_soft_stats_t s_minmea_soft_stats = {0};
static uint8_t s_minmea_soft_unknown_log_budget = 0;
static uint8_t s_minmea_soft_gga_nofix_log_budget = 0;
static bool s_gpio_isr_service_ready = false;

static void IRAM_ATTR minmea_soft_gpio_edge_isr(void *arg) {
    (void)arg;
    s_minmea_soft_stats.raw_gpio_edges++;
}

static void minmea_soft_log_stats_rolling(void) {
    static minmea_soft_stats_t last = {0};

    minmea_soft_stats_t cur = s_minmea_soft_stats;
    if (cur.rx_events < last.rx_events ||
        cur.raw_gpio_edges < last.raw_gpio_edges ||
        cur.rx_queue_drops < last.rx_queue_drops ||
        cur.rx_rearm_failures < last.rx_rearm_failures ||
        cur.rx_rearm_recovers < last.rx_rearm_recovers ||
        cur.rx_local_turnovers < last.rx_local_turnovers ||
        cur.rx_local_turnover_failures < last.rx_local_turnover_failures ||
        cur.line_overflow < last.line_overflow ||
        cur.max_line_len < last.max_line_len ||
        cur.rx_symbols < last.rx_symbols ||
        cur.bytes_decoded < last.bytes_decoded ||
        cur.frame_attempts < last.frame_attempts ||
        cur.framing_errors < last.framing_errors ||
        cur.lines_seen < last.lines_seen ||
        cur.valid_sentences < last.valid_sentences ||
        cur.checksum_fail < last.checksum_fail ||
        cur.sentence_unknown < last.sentence_unknown ||
        cur.gga_count < last.gga_count ||
        cur.gsa_count < last.gsa_count ||
        cur.gsv_count < last.gsv_count ||
        cur.rmc_count < last.rmc_count ||
        cur.vtg_count < last.vtg_count) {
        last = (minmea_soft_stats_t){0};
    }

    ESP_LOGI(TAG,
             "5s stats: events=%lu edges=%lu edge_probe=%s qdrop=%lu rearm=%lu/%lu turn=%lu/%lu ovf=%lu maxlen=%lu symbols=%lu bytes=%lu frames=%lu frame_err=%lu lines=%lu valid=%lu csum_fail=%lu unk=%lu gga=%lu gsa=%lu gsv=%lu rmc=%lu vtg=%lu",
             (unsigned long)(cur.rx_events - last.rx_events),
             (unsigned long)(cur.raw_gpio_edges - last.raw_gpio_edges),
             cur.edge_probe_ok ? "ok" : "n/a",
             (unsigned long)(cur.rx_queue_drops - last.rx_queue_drops),
             (unsigned long)(cur.rx_rearm_failures - last.rx_rearm_failures),
             (unsigned long)(cur.rx_rearm_recovers - last.rx_rearm_recovers),
             (unsigned long)(cur.rx_local_turnovers - last.rx_local_turnovers),
             (unsigned long)(cur.rx_local_turnover_failures - last.rx_local_turnover_failures),
             (unsigned long)(cur.line_overflow - last.line_overflow),
             (unsigned long)cur.max_line_len,
             (unsigned long)(cur.rx_symbols - last.rx_symbols),
             (unsigned long)(cur.bytes_decoded - last.bytes_decoded),
             (unsigned long)(cur.frame_attempts - last.frame_attempts),
             (unsigned long)(cur.framing_errors - last.framing_errors),
             (unsigned long)(cur.lines_seen - last.lines_seen),
             (unsigned long)(cur.valid_sentences - last.valid_sentences),
             (unsigned long)(cur.checksum_fail - last.checksum_fail),
             (unsigned long)(cur.sentence_unknown - last.sentence_unknown),
             (unsigned long)(cur.gga_count - last.gga_count),
             (unsigned long)(cur.gsa_count - last.gsa_count),
             (unsigned long)(cur.gsv_count - last.gsv_count),
             (unsigned long)(cur.rmc_count - last.rmc_count),
             (unsigned long)(cur.vtg_count - last.vtg_count));
    last = cur;
}

esp_err_t minmea_soft_get_last_error(void) {
    return s_minmea_soft_last_error;
}

void minmea_soft_get_stats(minmea_soft_stats_t *out_stats) {
    if (!out_stats) {
        return;
    }
    *out_stats = s_minmea_soft_stats;
}

static uint32_t minmea_soft_clamp_u32(uint32_t value, uint32_t low, uint32_t high) {
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

static uint32_t minmea_soft_signal_min_ns(const minmea_soft_ctx_t *ctx) {
    if (!ctx || ctx->bit_time_us == 0) {
        return SOFT_RX_SIGNAL_RANGE_MIN_NS_FLOOR;
    }
    uint32_t bit_time_ns = ctx->bit_time_us * 1000U;
    uint32_t candidate = bit_time_ns / SOFT_RX_SIGNAL_MIN_DIV;
    return minmea_soft_clamp_u32(candidate,
                                 SOFT_RX_SIGNAL_RANGE_MIN_NS_FLOOR,
                                 SOFT_RX_SIGNAL_RANGE_MIN_NS_CEIL);
}

static uint32_t minmea_soft_signal_max_ns(const minmea_soft_ctx_t *ctx) {
    if (!ctx || ctx->bit_time_us == 0) {
        return SOFT_RX_SIGNAL_RANGE_MAX_NS_FLOOR;
    }
    uint32_t bit_time_ns = ctx->bit_time_us * 1000U;
    uint32_t candidate = bit_time_ns * SOFT_RX_SIGNAL_MAX_BITS;
    return minmea_soft_clamp_u32(candidate,
                                 SOFT_RX_SIGNAL_RANGE_MAX_NS_FLOOR,
                                 SOFT_RX_SIGNAL_RANGE_MAX_NS_CEIL);
}

static bool minmea_soft_arm_receive(minmea_soft_ctx_t *ctx, bool log_error) {
    if (!ctx || !ctx->rx_chan) {
        return false;
    }

    uint32_t signal_min_ns = minmea_soft_signal_min_ns(ctx);
    uint32_t signal_max_ns = minmea_soft_signal_max_ns(ctx);

    rmt_receive_config_t rx_cfg = {
        .signal_range_min_ns = signal_min_ns,
        .signal_range_max_ns = signal_max_ns,
    };

    esp_err_t err = rmt_receive(ctx->rx_chan,
                                ctx->rx_symbols[ctx->active_buffer],
                                sizeof(ctx->rx_symbols[ctx->active_buffer]),
                                &rx_cfg);
    if (err == ESP_ERR_INVALID_ARG && signal_min_ns != 1000U) {
        rx_cfg.signal_range_min_ns = 1000U;
        err = rmt_receive(ctx->rx_chan,
                          ctx->rx_symbols[ctx->active_buffer],
                          sizeof(ctx->rx_symbols[ctx->active_buffer]),
                          &rx_cfg);
    }
    if (err != ESP_OK) {
        s_minmea_soft_last_error = err;
        if (log_error) {
            ESP_LOGW(TAG, "rmt_receive failed: %s", esp_err_to_name(err));
        }
        return false;
    }
    return true;
}

static bool minmea_soft_force_turnover(minmea_soft_ctx_t *ctx) {
    if (!ctx || !ctx->rx_chan) {
        return false;
    }

    s_minmea_soft_stats.rx_local_turnovers++;

    esp_err_t err = rmt_disable(ctx->rx_chan);
    if (err != ESP_OK) {
        s_minmea_soft_last_error = err;
        s_minmea_soft_stats.rx_local_turnover_failures++;
        return false;
    }

    err = rmt_enable(ctx->rx_chan);
    if (err != ESP_OK) {
        s_minmea_soft_last_error = err;
        s_minmea_soft_stats.rx_local_turnover_failures++;
        return false;
    }

    if (!minmea_soft_arm_receive(ctx, false)) {
        s_minmea_soft_stats.rx_local_turnover_failures++;
        return false;
    }

    return true;
}

static bool minmea_soft_on_rx_done(rmt_channel_handle_t channel,
                                   const rmt_rx_done_event_data_t *edata,
                                   void *user_ctx) {
    (void)channel;
    minmea_soft_ctx_t *ctx = (minmea_soft_ctx_t *)user_ctx;
    if (!ctx || !edata || !ctx->event_queue || !ctx->running) {
        return false;
    }

    uint8_t completed_buf = ctx->active_buffer;
    size_t num_symbols = edata->num_symbols;
    if (num_symbols > SOFT_RX_MAX_SYMBOLS) {
        num_symbols = SOFT_RX_MAX_SYMBOLS;
    }

    ctx->active_buffer = (ctx->active_buffer + 1) % SOFT_RX_NUM_BUFFERS;
    if (!minmea_soft_arm_receive(ctx, true)) {
        s_minmea_soft_stats.rx_rearm_failures++;
        ctx->rearm_pending = true;
    }

    soft_rx_event_copy_t event = {
        .num_symbols = num_symbols,
        .buffer_idx = completed_buf
    };

    BaseType_t hp_task_woken = pdFALSE;
    BaseType_t sent = xQueueSendFromISR(ctx->event_queue, &event, &hp_task_woken);
    if (sent != pdTRUE) {
        s_minmea_soft_stats.rx_queue_drops++;
    }
    if (hp_task_woken) {
        portYIELD_FROM_ISR();
    }
    return hp_task_woken == pdTRUE;
}

static inline void minmea_soft_emit_update(minmea_soft_ctx_t *ctx) {
    gps_event_handler(NULL, ESP_NMEA_EVENT, GPS_UPDATE, &ctx->gps.parent);
}

static inline void minmea_soft_set_time(gps_t *out, const struct minmea_time *t) {
    if (!out || !t) {
        return;
    }
    out->tim.hour = (uint8_t)t->hours;
    out->tim.minute = (uint8_t)t->minutes;
    out->tim.second = (uint8_t)t->seconds;
    out->tim.thousand = (uint16_t)(t->microseconds / 1000);
}

static inline void minmea_soft_set_date(gps_t *out, const struct minmea_date *d) {
    if (!out || !d) {
        return;
    }
    out->date.day = (uint8_t)d->day;
    out->date.month = (uint8_t)d->month;
    uint16_t y = (uint16_t)d->year;
    if (y >= 2000) {
        y = (uint16_t)(y - 2000);
    } else if (y >= 1900) {
        y = (uint16_t)(y - 1900);
    }
    out->date.year = y;
}

static void minmea_soft_apply_sentence(minmea_soft_ctx_t *ctx, const char *line) {
    if (!ctx || !line) {
        return;
    }

    if (!minmea_check(line, false)) {
        s_minmea_soft_stats.checksum_fail++;
        return;
    }

    s_minmea_soft_stats.valid_sentences++;

    gps_t *gps = &ctx->gps.parent;
    enum minmea_sentence_id sid = minmea_sentence_id(line, false);

    switch (sid) {
        case MINMEA_SENTENCE_RMC: {
            s_minmea_soft_stats.rmc_count++;
            struct minmea_sentence_rmc frame;
            if (!minmea_parse_rmc(&frame, line)) {
                return;
            }

            float lat = minmea_tocoord(&frame.latitude);
            float lon = minmea_tocoord(&frame.longitude);
            if (!isnan(lat)) {
                gps->latitude = lat;
            }
            if (!isnan(lon)) {
                gps->longitude = lon;
            }

            float speed_kts = minmea_tofloat(&frame.speed);
            if (!isnan(speed_kts)) {
                gps->speed = speed_kts * 0.514444f;
            }

            float cog = minmea_tofloat(&frame.course);
            if (!isnan(cog)) {
                gps->cog = cog;
            }

            minmea_soft_set_time(gps, &frame.time);
            minmea_soft_set_date(gps, &frame.date);

            if (frame.valid) {
                if (gps->fix == GPS_FIX_INVALID) {
                    gps->fix = GPS_FIX_GPS;
                }
                if (gps->fix_mode == GPS_MODE_INVALID) {
                    gps->fix_mode = GPS_MODE_2D;
                }
            } else {
                gps->fix = GPS_FIX_INVALID;
                gps->fix_mode = GPS_MODE_INVALID;
            }

            gps->valid = frame.valid;
            minmea_soft_emit_update(ctx);
            break;
        }
        case MINMEA_SENTENCE_GGA: {
            s_minmea_soft_stats.gga_count++;
            struct minmea_sentence_gga frame;
            if (!minmea_parse_gga(&frame, line)) {
                return;
            }

            float lat = minmea_tocoord(&frame.latitude);
            float lon = minmea_tocoord(&frame.longitude);
            if (!isnan(lat)) {
                gps->latitude = lat;
            }
            if (!isnan(lon)) {
                gps->longitude = lon;
            }

            float alt = minmea_tofloat(&frame.altitude);
            if (!isnan(alt)) {
                gps->altitude = alt;
            }

            float hdop = minmea_tofloat(&frame.hdop);
            if (!isnan(hdop)) {
                gps->dop_h = hdop;
            }

            gps->sats_in_use = (uint8_t)frame.satellites_tracked;
            minmea_soft_set_time(gps, &frame.time);

            if ((frame.fix_quality <= 0 || frame.satellites_tracked <= 0) &&
                s_minmea_soft_gga_nofix_log_budget > 0) {
                float lat = minmea_tocoord(&frame.latitude);
                float lon = minmea_tocoord(&frame.longitude);
                ESP_LOGI(TAG,
                         "GGA no-fix: fixq=%d sats=%d hdop=%.1f lat=%.6f lon=%.6f",
                         frame.fix_quality,
                         frame.satellites_tracked,
                         (double)minmea_tofloat(&frame.hdop),
                         (double)(isnan(lat) ? 0.0f : lat),
                         (double)(isnan(lon) ? 0.0f : lon));
                s_minmea_soft_gga_nofix_log_budget--;
            }

            if (frame.fix_quality <= 0) {
                gps->fix = GPS_FIX_INVALID;
                gps->fix_mode = GPS_MODE_INVALID;
                gps->valid = false;
            } else {
                gps->fix = (frame.fix_quality > 1) ? GPS_FIX_DGPS : GPS_FIX_GPS;
                if (gps->fix_mode == GPS_MODE_INVALID) {
                    gps->fix_mode = GPS_MODE_2D;
                }
                gps->valid = true;
            }

            minmea_soft_emit_update(ctx);
            break;
        }
        case MINMEA_SENTENCE_GSA: {
            s_minmea_soft_stats.gsa_count++;
            struct minmea_sentence_gsa frame;
            if (!minmea_parse_gsa(&frame, line)) {
                return;
            }

            gps->fix_mode = (gps_fix_mode_t)frame.fix_type;
            gps->dop_p = minmea_tofloat(&frame.pdop);
            gps->dop_h = minmea_tofloat(&frame.hdop);
            gps->dop_v = minmea_tofloat(&frame.vdop);

            for (int i = 0; i < GPS_MAX_SATELLITES_IN_USE; i++) {
                gps->sats_id_in_use[i] = (uint8_t)((frame.sats[i] > 0) ? frame.sats[i] : 0);
            }
            break;
        }
        case MINMEA_SENTENCE_GSV: {
            s_minmea_soft_stats.gsv_count++;
            struct minmea_sentence_gsv frame;
            if (!minmea_parse_gsv(&frame, line)) {
                return;
            }

            gps->sats_in_view = (uint8_t)frame.total_sats;
            int base_idx = (frame.msg_nr - 1) * 4;
            for (int i = 0; i < 4; i++) {
                int out_idx = base_idx + i;
                if (out_idx >= GPS_MAX_SATELLITES_IN_VIEW) {
                    break;
                }

                gps->sats_desc_in_view[out_idx].num = (uint8_t)frame.sats[i].nr;
                gps->sats_desc_in_view[out_idx].elevation = (uint8_t)frame.sats[i].elevation;
                gps->sats_desc_in_view[out_idx].azimuth = (uint16_t)frame.sats[i].azimuth;
                float snr = minmea_tofloat(&frame.sats[i].snr);
                gps->sats_desc_in_view[out_idx].snr = (uint8_t)(isnan(snr) ? 0 : (snr < 0 ? 0 : snr));
            }
            break;
        }
        case MINMEA_SENTENCE_GLL: {
            struct minmea_sentence_gll frame;
            if (!minmea_parse_gll(&frame, line)) {
                return;
            }

            float lat = minmea_tocoord(&frame.latitude);
            float lon = minmea_tocoord(&frame.longitude);
            if (!isnan(lat)) {
                gps->latitude = lat;
            }
            if (!isnan(lon)) {
                gps->longitude = lon;
            }

            minmea_soft_set_time(gps, &frame.time);

            if (frame.status == MINMEA_GLL_STATUS_DATA_VALID) {
                gps->valid = true;
                if (gps->fix == GPS_FIX_INVALID) {
                    gps->fix = GPS_FIX_GPS;
                }
                if (gps->fix_mode == GPS_MODE_INVALID) {
                    gps->fix_mode = GPS_MODE_2D;
                }
            } else if (gps->fix == GPS_FIX_INVALID || gps->fix_mode == GPS_MODE_INVALID) {
                gps->valid = false;
            }
            break;
        }
        case MINMEA_SENTENCE_VTG: {
            s_minmea_soft_stats.vtg_count++;
            struct minmea_sentence_vtg frame;
            if (!minmea_parse_vtg(&frame, line)) {
                return;
            }

            float speed_kph = minmea_tofloat(&frame.speed_kph);
            if (!isnan(speed_kph)) {
                gps->speed = speed_kph / 3.6f;
            }

            float track = minmea_tofloat(&frame.true_track_degrees);
            if (!isnan(track)) {
                gps->cog = track;
            }
            break;
        }
        case MINMEA_SENTENCE_ZDA: {
            struct minmea_sentence_zda frame;
            if (!minmea_parse_zda(&frame, line)) {
                return;
            }

            minmea_soft_set_time(gps, &frame.time);
            minmea_soft_set_date(gps, &frame.date);
            break;
        }
        default:
            s_minmea_soft_stats.sentence_unknown++;
            if (s_minmea_soft_unknown_log_budget > 0) {
                union minmea_type type;
                if (minmea_scan(line, "t", &type)) {
                    ESP_LOGI(TAG,
                             "Unknown NMEA: talker=%c%c sentence=%c%c%c",
                             type.talker_id[0],
                             type.talker_id[1],
                             type.sentence_id[0],
                             type.sentence_id[1],
                             type.sentence_id[2]);
                } else {
                    ESP_LOGI(TAG, "Unknown NMEA: could not parse sentence header");
                }
                s_minmea_soft_unknown_log_budget--;
            }
            break;
    }
}

static size_t minmea_soft_build_segments(const rmt_symbol_word_t *symbols,
                                         size_t num_symbols,
                                         soft_uart_segment_t *segments,
                                         size_t max_segments) {
    if (!symbols || !segments || max_segments == 0) {
        return 0;
    }

    size_t seg_count = 0;
    uint32_t t_us = 0;
    for (size_t i = 0; i < num_symbols && seg_count + 1 < max_segments; i++) {
        uint32_t d0 = symbols[i].duration0;
        uint32_t d1 = symbols[i].duration1;
        uint8_t l0 = (uint8_t)symbols[i].level0;
        uint8_t l1 = (uint8_t)symbols[i].level1;

        if (d0 > 0 && seg_count < max_segments) {
            segments[seg_count].start_us = t_us;
            segments[seg_count].end_us = t_us + d0;
            segments[seg_count].level = l0;
            t_us += d0;
            seg_count++;
        }

        if (d1 > 0 && seg_count < max_segments) {
            segments[seg_count].start_us = t_us;
            segments[seg_count].end_us = t_us + d1;
            segments[seg_count].level = l1;
            t_us += d1;
            seg_count++;
        }
    }

    return seg_count;
}

static bool minmea_soft_level_at(const soft_uart_segment_t *segments,
                                 size_t seg_count,
                                 uint32_t t_us,
                                 uint8_t *level_out) {
    if (!segments || !level_out || seg_count == 0) {
        return false;
    }

    for (size_t i = 0; i < seg_count; i++) {
        if (t_us >= segments[i].start_us && t_us < segments[i].end_us) {
            *level_out = segments[i].level;
            return true;
        }
    }
    return false;
}

static bool minmea_soft_find_start_bit(const soft_uart_segment_t *segments,
                                       size_t seg_count,
                                       uint32_t cursor_us,
                                       uint32_t bit_time_us,
                                       uint32_t *start_us_out) {
    if (!segments || !start_us_out || seg_count == 0) {
        return false;
    }

    uint32_t half_bit = bit_time_us / 2;
    uint32_t min_low = bit_time_us - ((bit_time_us * SOFT_RX_BIT_TOLERANCE_PCT) / 100);

    for (size_t i = 0; i < seg_count; i++) {
        if (segments[i].end_us <= cursor_us) {
            continue;
        }
        if (segments[i].level != 0) {
            continue;
        }

        uint32_t start_us = segments[i].start_us;
        if (start_us < cursor_us) {
            start_us = cursor_us;
        }

        uint32_t center_us = start_us + half_bit;
        if (center_us >= segments[i].end_us) {
            continue;
        }

        uint32_t low_dur = segments[i].end_us - start_us;
        if (low_dur < min_low) {
            continue;
        }

        if (i > 0 && segments[i - 1].level == 0) {
            continue;
        }

        *start_us_out = start_us;
        return true;
    }

    return false;
}

static bool minmea_soft_decode_byte(const soft_uart_segment_t *segments,
                                    size_t seg_count,
                                    uint32_t start_us,
                                    uint32_t bit_time_us,
                                    uint8_t *byte_out,
                                    uint32_t *next_cursor_us) {
    if (!segments || !byte_out || !next_cursor_us || bit_time_us == 0) {
        return false;
    }

    uint8_t value = 0;
    uint32_t sample_us = start_us + bit_time_us + (bit_time_us / 2);

    for (int bit = 0; bit < 8; bit++) {
        uint8_t level = 1;
        if (!minmea_soft_level_at(segments, seg_count, sample_us, &level)) {
            return false;
        }
        value |= (uint8_t)((level & 0x1u) << bit);
        sample_us += bit_time_us;
    }

    uint8_t stop_level = 0;
    if (!minmea_soft_level_at(segments,
                              seg_count,
                              start_us + (bit_time_us * 9) + (bit_time_us / 2),
                              &stop_level)) {
        return false;
    }
    if (stop_level == 0) {
        return false;
    }

    *byte_out = value;
    *next_cursor_us = start_us + (bit_time_us * 10);
    return true;
}

static void minmea_soft_feed_byte(minmea_soft_ctx_t *ctx, uint8_t ch) {
    if (!ctx) {
        return;
    }

    if (ch == '$') {
        ctx->line_len = 0;
    }

    if (ctx->line_len < (SOFT_RX_LINE_MAX - 1)) {
        ctx->line_buf[ctx->line_len++] = (char)ch;
        if (ctx->line_len > s_minmea_soft_stats.max_line_len) {
            s_minmea_soft_stats.max_line_len = (uint32_t)ctx->line_len;
        }
    } else {
        s_minmea_soft_stats.line_overflow++;
        ctx->line_len = 0;
    }

    if (ch == '\n') {
        s_minmea_soft_stats.lines_seen++;
        ctx->line_buf[ctx->line_len] = '\0';
        minmea_soft_apply_sentence(ctx, ctx->line_buf);
        ctx->line_len = 0;
    }
}

static void minmea_soft_process_symbols(minmea_soft_ctx_t *ctx,
                                        const rmt_symbol_word_t *symbols,
                                        size_t num_symbols) {
    if (!ctx || !symbols || num_symbols == 0) {
        return;
    }

    if (!ctx->segments || ctx->segments_capacity == 0) {
        return;
    }

    size_t seg_count = minmea_soft_build_segments(symbols,
                                                  num_symbols,
                                                  ctx->segments,
                                                  ctx->segments_capacity);
    if (seg_count < 2) {
        return;
    }

    uint32_t cursor_us = 0;
    while (ctx->running) {
        uint32_t start_us = 0;
        if (!minmea_soft_find_start_bit(ctx->segments,
                                        seg_count,
                                        cursor_us,
                                        ctx->bit_time_us,
                                        &start_us)) {
            break;
        }

        s_minmea_soft_stats.frame_attempts++;

        uint8_t byte = 0;
        uint32_t next_cursor = 0;
        if (!minmea_soft_decode_byte(ctx->segments,
                                     seg_count,
                                     start_us,
                                     ctx->bit_time_us,
                                     &byte,
                                     &next_cursor)) {
            s_minmea_soft_stats.framing_errors++;
            cursor_us = start_us + (ctx->bit_time_us / 2);
            continue;
        }

        s_minmea_soft_stats.bytes_decoded++;
        minmea_soft_feed_byte(ctx, byte);
        cursor_us = next_cursor;
    }
}

static void minmea_soft_task(void *arg) {
    minmea_soft_ctx_t *ctx = (minmea_soft_ctx_t *)arg;
    if (!ctx) {
        vTaskDelete(NULL);
        return;
    }

    TickType_t last_stats_tick = xTaskGetTickCount();
    TickType_t last_rearm_try_tick = 0;
    TickType_t last_rearm_fail_log_tick = 0;
    TickType_t last_rx_event_tick = last_stats_tick;
    TickType_t last_local_turnover_tick = 0;
    uint32_t last_edge_count = 0;

    while (ctx->running) {
        soft_rx_event_copy_t event = {0};
        if (xQueueReceive(ctx->event_queue, &event, pdMS_TO_TICKS(500)) == pdTRUE) {
            s_minmea_soft_stats.rx_events++;
            last_rx_event_tick = xTaskGetTickCount();
            s_minmea_soft_stats.rx_symbols += (uint32_t)event.num_symbols;
            if (event.num_symbols > 0) {
                minmea_soft_process_symbols(ctx, ctx->rx_symbols[event.buffer_idx], event.num_symbols);
            }
        }

        TickType_t now = xTaskGetTickCount();
        if (ctx->rearm_pending && (now - last_rearm_try_tick) >= pdMS_TO_TICKS(SOFT_RX_REARM_RETRY_MS)) {
            if (minmea_soft_arm_receive(ctx, false)) {
                ctx->rearm_pending = false;
                s_minmea_soft_stats.rx_rearm_recovers++;
            } else if ((last_rearm_fail_log_tick == 0) ||
                       ((now - last_rearm_fail_log_tick) >= pdMS_TO_TICKS(SOFT_RX_REARM_RETRY_LOG_MS))) {
                ESP_LOGW(TAG,
                         "RMT rearm retry failed: %s",
                         esp_err_to_name(s_minmea_soft_last_error));
                last_rearm_fail_log_tick = now;
            }
            last_rearm_try_tick = now;
        }

        if (!ctx->rearm_pending &&
            s_minmea_soft_stats.edge_probe_ok &&
            (now - last_rx_event_tick) >= pdMS_TO_TICKS(SOFT_RX_LOCAL_TURNOVER_STALL_MS)) {
            uint32_t edge_count = s_minmea_soft_stats.raw_gpio_edges;
            bool edges_moving = edge_count != last_edge_count;
            bool cooldown_done =
                (last_local_turnover_tick == 0) ||
                ((now - last_local_turnover_tick) >= pdMS_TO_TICKS(SOFT_RX_LOCAL_TURNOVER_COOLDOWN_MS));
            if (edges_moving && cooldown_done) {
                if (minmea_soft_force_turnover(ctx)) {
                    ESP_LOGW(TAG, "Local RMT turnover after stall (edges moving, no rx events)");
                    last_rx_event_tick = now;
                    ctx->rearm_pending = false;
                } else {
                    ESP_LOGW(TAG, "Local RMT turnover failed: %s", esp_err_to_name(s_minmea_soft_last_error));
                    ctx->rearm_pending = true;
                }
                last_local_turnover_tick = now;
            }
            last_edge_count = edge_count;
        } else {
            last_edge_count = s_minmea_soft_stats.raw_gpio_edges;
        }

        if ((now - last_stats_tick) >= pdMS_TO_TICKS(SOFT_RX_STATS_LOG_MS)) {
            minmea_soft_log_stats_rolling();
            last_stats_tick = now;
        }
    }

    vTaskDelete(NULL);
}

nmea_parser_handle_t minmea_soft_start(gpio_num_t rx_pin, uint32_t baud_rate) {
    s_minmea_soft_last_error = ESP_OK;
    memset(&s_minmea_soft_stats, 0, sizeof(s_minmea_soft_stats));
    s_minmea_soft_stats.edge_probe_ok = false;
    s_minmea_soft_unknown_log_budget = SOFT_RX_UNKNOWN_LOG_BUDGET;
    s_minmea_soft_gga_nofix_log_budget = SOFT_RX_GGA_NOFIX_LOG_BUDGET;

    minmea_soft_ctx_t *ctx = (minmea_soft_ctx_t *)calloc(1, sizeof(minmea_soft_ctx_t));
    if (!ctx) {
        s_minmea_soft_last_error = ESP_ERR_NO_MEM;
        ESP_LOGE(TAG, "calloc ctx failed");
        return NULL;
    }

    ctx->rx_pin = rx_pin;
    ctx->baud_rate = baud_rate;
    ctx->bit_time_us = (baud_rate == 0) ? 104 : (1000000U / baud_rate);
    ctx->running = true;
    ctx->active_buffer = 0;
    ctx->rearm_pending = false;
    ctx->edge_isr_registered = false;
    ctx->line_len = 0;
    ctx->gps.parent.fix = GPS_FIX_INVALID;
    ctx->gps.parent.fix_mode = GPS_MODE_INVALID;
    ctx->segments_capacity = SOFT_RX_MAX_SYMBOLS * 2;

    ctx->segments = (soft_uart_segment_t *)calloc(ctx->segments_capacity, sizeof(soft_uart_segment_t));
    if (!ctx->segments) {
        s_minmea_soft_last_error = ESP_ERR_NO_MEM;
        ESP_LOGE(TAG, "calloc segments failed");
        free(ctx);
        return NULL;
    }

    ctx->event_queue = xQueueCreate(SOFT_RX_EVENT_QUEUE_LEN, sizeof(soft_rx_event_copy_t));
    if (!ctx->event_queue) {
        s_minmea_soft_last_error = ESP_ERR_NO_MEM;
        ESP_LOGE(TAG,
                 "xQueueCreate failed (item=%u, free=%u, free_internal=%u)",
                 (unsigned)sizeof(soft_rx_event_copy_t),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        free(ctx->segments);
        free(ctx);
        return NULL;
    }

    rmt_rx_channel_config_t rx_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = SOFT_RX_RESOLUTION_HZ,
        .mem_block_symbols = SOFT_RX_MEM_BLOCK_SYMBOLS,
        .gpio_num = rx_pin,
        .flags.with_dma = false,
        .flags.invert_in = false,
    };

    esp_err_t err = rmt_new_rx_channel(&rx_cfg, &ctx->rx_chan);
    if (err != ESP_OK) {
        s_minmea_soft_last_error = err;
        ESP_LOGE(TAG, "rmt_new_rx_channel failed: %s", esp_err_to_name(err));
        vQueueDelete(ctx->event_queue);
        free(ctx->segments);
        free(ctx);
        return NULL;
    }

    rmt_rx_event_callbacks_t cbs = {
        .on_recv_done = minmea_soft_on_rx_done,
    };

    err = rmt_rx_register_event_callbacks(ctx->rx_chan, &cbs, ctx);
    if (err != ESP_OK) {
        s_minmea_soft_last_error = err;
        ESP_LOGE(TAG, "rmt_rx_register_event_callbacks failed: %s", esp_err_to_name(err));
        rmt_del_channel(ctx->rx_chan);
        vQueueDelete(ctx->event_queue);
        free(ctx->segments);
        free(ctx);
        return NULL;
    }

    err = rmt_enable(ctx->rx_chan);
    if (err != ESP_OK) {
        s_minmea_soft_last_error = err;
        ESP_LOGE(TAG, "rmt_enable failed: %s", esp_err_to_name(err));
        rmt_del_channel(ctx->rx_chan);
        vQueueDelete(ctx->event_queue);
        free(ctx->segments);
        free(ctx);
        return NULL;
    }

    bool edge_probe_ok = false;
    if (!s_gpio_isr_service_ready) {
        err = gpio_install_isr_service(0);
        if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
            s_gpio_isr_service_ready = true;
        } else {
            ESP_LOGW(TAG, "gpio_install_isr_service failed: %s", esp_err_to_name(err));
        }
    }

    err = gpio_set_intr_type(rx_pin, GPIO_INTR_ANYEDGE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "gpio_set_intr_type failed: %s", esp_err_to_name(err));
    } else if (!s_gpio_isr_service_ready) {
        ESP_LOGW(TAG, "edge telemetry unavailable: gpio ISR service not ready");
    } else {
        err = gpio_isr_handler_add(rx_pin, minmea_soft_gpio_edge_isr, ctx);
        if (err == ESP_OK) {
            err = gpio_intr_enable(rx_pin);
            if (err == ESP_OK) {
                ctx->edge_isr_registered = true;
                edge_probe_ok = true;
            } else {
                ESP_LOGW(TAG, "gpio_intr_enable failed: %s", esp_err_to_name(err));
                (void)gpio_isr_handler_remove(rx_pin);
            }
        } else {
            ESP_LOGW(TAG, "gpio_isr_handler_add failed: %s", esp_err_to_name(err));
        }
    }

    s_minmea_soft_stats.edge_probe_ok = edge_probe_ok;
    if (!edge_probe_ok) {
        ESP_LOGW(TAG, "GPIO edge telemetry unavailable on IO%d", (int)rx_pin);
    }

    if (xTaskCreate(minmea_soft_task,
                    "gps_soft_rx",
                    6144,
                    ctx,
                    SOFT_RX_TASK_PRIORITY,
                    &ctx->task) != pdPASS) {
        s_minmea_soft_last_error = ESP_ERR_NO_MEM;
        ESP_LOGE(TAG, "Failed to create soft GPS task");
        if (ctx->edge_isr_registered) {
            (void)gpio_intr_disable(rx_pin);
            (void)gpio_isr_handler_remove(rx_pin);
            ctx->edge_isr_registered = false;
        }
        s_minmea_soft_stats.edge_probe_ok = false;
        rmt_disable(ctx->rx_chan);
        rmt_del_channel(ctx->rx_chan);
        vQueueDelete(ctx->event_queue);
        free(ctx->segments);
        free(ctx);
        return NULL;
    }

    if (!minmea_soft_arm_receive(ctx, true)) {
        s_minmea_soft_last_error = ESP_FAIL;
        ESP_LOGE(TAG, "Failed to arm initial RMT receive");
        vTaskDelete(ctx->task);
        if (ctx->edge_isr_registered) {
            (void)gpio_intr_disable(rx_pin);
            (void)gpio_isr_handler_remove(rx_pin);
            ctx->edge_isr_registered = false;
        }
        s_minmea_soft_stats.edge_probe_ok = false;
        rmt_disable(ctx->rx_chan);
        rmt_del_channel(ctx->rx_chan);
        vQueueDelete(ctx->event_queue);
        free(ctx->segments);
        free(ctx);
        return NULL;
    }

    ESP_LOGI(TAG,
             "Soft GPS RMT RX started on IO%d @ %lu (edge_probe=%s range=%u..%u ns symbols=%u)",
             (int)rx_pin,
             (unsigned long)baud_rate,
             edge_probe_ok ? "ok" : "n/a",
             (unsigned)minmea_soft_signal_min_ns(ctx),
             (unsigned)minmea_soft_signal_max_ns(ctx),
             (unsigned)SOFT_RX_MAX_SYMBOLS);
    return (nmea_parser_handle_t)ctx;
}

esp_err_t minmea_soft_stop(nmea_parser_handle_t handle) {
    minmea_soft_ctx_t *ctx = (minmea_soft_ctx_t *)handle;
    if (!ctx) {
        return ESP_ERR_INVALID_ARG;
    }

    ctx->running = false;

    if (ctx->task) {
        vTaskDelete(ctx->task);
        ctx->task = NULL;
    }

    if (ctx->edge_isr_registered) {
        (void)gpio_intr_disable(ctx->rx_pin);
        (void)gpio_isr_handler_remove(ctx->rx_pin);
        ctx->edge_isr_registered = false;
    }

    s_minmea_soft_stats.edge_probe_ok = false;

    if (ctx->rx_chan) {
        (void)rmt_disable(ctx->rx_chan);
        (void)rmt_del_channel(ctx->rx_chan);
        ctx->rx_chan = NULL;
    }

    if (ctx->event_queue) {
        vQueueDelete(ctx->event_queue);
        ctx->event_queue = NULL;
    }

    if (ctx->segments) {
        free(ctx->segments);
        ctx->segments = NULL;
    }

    free(ctx);
    return ESP_OK;
}
