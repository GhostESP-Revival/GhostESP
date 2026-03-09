#ifndef MINMEA_SOFT_H
#define MINMEA_SOFT_H

#include "driver/gpio.h"
#include "esp_err.h"
#include "vendor/GPS/MicroNMEA.h"
#include <stdbool.h>

typedef struct {
    uint32_t rx_events;
    uint32_t raw_gpio_edges;
    bool edge_probe_ok;
    uint32_t rx_queue_drops;
    uint32_t rx_rearm_failures;
    uint32_t rx_rearm_recovers;
    uint32_t rx_local_turnovers;
    uint32_t rx_local_turnover_failures;
    uint32_t line_overflow;
    uint32_t max_line_len;
    uint32_t rx_symbols;
    uint32_t bytes_decoded;
    uint32_t frame_attempts;
    uint32_t framing_errors;
    uint32_t lines_seen;
    uint32_t valid_sentences;
    uint32_t checksum_fail;
    uint32_t sentence_unknown;
    uint32_t rmc_count;
    uint32_t gga_count;
    uint32_t gsa_count;
    uint32_t gsv_count;
    uint32_t vtg_count;
} minmea_soft_stats_t;

nmea_parser_handle_t minmea_soft_start(gpio_num_t rx_pin, uint32_t baud_rate);
esp_err_t minmea_soft_stop(nmea_parser_handle_t handle);
esp_err_t minmea_soft_get_last_error(void);
void minmea_soft_get_stats(minmea_soft_stats_t *out_stats);

#endif
