/*
 * SPDX-FileCopyrightText: 2015-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "vendor/GPS/MicroNMEA.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "core/uart_share.h"
#include "hal/uart_ll.h"
#include <esp_heap_caps.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief NMEA Parser runtime buffer size
 *
 */

#ifndef CONFIG_NMEA_PARSER_RING_BUFFER_SIZE
#define CONFIG_NMEA_PARSER_RING_BUFFER_SIZE 1024
#endif

#ifndef CONFIG_NMEA_PARSER_TASK_STACK_SIZE
#define CONFIG_NMEA_PARSER_TASK_STACK_SIZE 4096
#endif

#ifndef CONFIG_NMEA_PARSER_TASK_PRIORITY
#define CONFIG_NMEA_PARSER_TASK_PRIORITY 3
#endif

#ifndef CONFIG_NMEA_STATEMENT_GGA
#define CONFIG_NMEA_STATEMENT_GGA 1
#endif
#ifndef CONFIG_NMEA_STATEMENT_GSA
#define CONFIG_NMEA_STATEMENT_GSA 1
#endif
#ifndef CONFIG_NMEA_STATEMENT_GSV
#define CONFIG_NMEA_STATEMENT_GSV 1
#endif
#ifndef CONFIG_NMEA_STATEMENT_RMC
#define CONFIG_NMEA_STATEMENT_RMC 1
#endif
#ifndef CONFIG_NMEA_STATEMENT_GLL
#define CONFIG_NMEA_STATEMENT_GLL 1
#endif
#ifndef CONFIG_NMEA_STATEMENT_VTG
#define CONFIG_NMEA_STATEMENT_VTG 1
#endif
#ifndef CONFIG_NMEA_STATEMENT_ZDA
#define CONFIG_NMEA_STATEMENT_ZDA 1
#endif
#ifndef CONFIG_NMEA_STATEMENT_GST
#define CONFIG_NMEA_STATEMENT_GST 1
#endif
#ifndef CONFIG_NMEA_STATEMENT_GBS
#define CONFIG_NMEA_STATEMENT_GBS 1
#endif

#define NMEA_PARSER_RUNTIME_BUFFER_SIZE                                        \
  (CONFIG_NMEA_PARSER_RING_BUFFER_SIZE / 2)
#define NMEA_MAX_STATEMENT_ITEM_LENGTH (32)
#define NMEA_EVENT_LOOP_QUEUE_SIZE (16)

/**
 * @brief Define of NMEA Parser Event base
 *
 */
ESP_EVENT_DEFINE_BASE(ESP_NMEA_EVENT);

static const char *GPS_TAG = "nmea_parser";

static void esp_drain_uart_data(esp_gps_t *esp_gps, size_t pending_len);

/**
 * @brief parse latitude or longitude
 *              format of latitude in NMEA is ddmm.sss and longitude is
 * dddmm.sss
 * @param esp_gps esp_gps_t type object
 * @return float Latitude or Longitude value (unit: degree)
 */
static float parse_lat_long(esp_gps_t *esp_gps) {
  ESP_LOGD(GPS_TAG, "Parsing coordinate: %s", esp_gps->item_str);

  if (!esp_gps->item_str[0] || esp_gps->item_str[0] == ',')
    return 0.0f;

  // Determine if this is latitude (2 digits) or longitude (3 digits)
  bool is_latitude =
      (esp_gps->cur_statement == STATEMENT_GGA && esp_gps->item_num == 2) ||
      (esp_gps->cur_statement == STATEMENT_RMC && esp_gps->item_num == 3) ||
      (esp_gps->cur_statement == STATEMENT_GLL && esp_gps->item_num == 1);
  int deg_width = is_latitude ? 2 : 3;

  // Parse degrees part
  char deg_str[4] = {0};
  strncpy(deg_str, esp_gps->item_str, deg_width);
  int degrees = atoi(deg_str);

  // Parse minutes part
  float minutes = strtof(esp_gps->item_str + deg_width, NULL);

  // Convert to decimal degrees
  float decimal_degrees = degrees + (minutes / 60.0f);

  ESP_LOGD(GPS_TAG, "Parsed %s: %d° %f' = %f°",
           is_latitude ? "latitude" : "longitude", degrees, minutes,
           decimal_degrees);

  return decimal_degrees;
}

/**
 * @brief Converter two continuous numeric character into a uint8_t number
 *
 * @param digit_char numeric character
 * @return uint8_t result of converting
 */
static inline uint8_t convert_two_digit2number(const char *digit_char) {
  return 10 * (digit_char[0] - '0') + (digit_char[1] - '0');
}

/**
 * @brief Parse UTC time in GPS statements
 *
 * @param esp_gps esp_gps_t type object
 */
static void parse_utc_time(esp_gps_t *esp_gps) {
  esp_gps->parent.tim.hour = convert_two_digit2number(esp_gps->item_str + 0);
  esp_gps->parent.tim.minute = convert_two_digit2number(esp_gps->item_str + 2);
  esp_gps->parent.tim.second = convert_two_digit2number(esp_gps->item_str + 4);
  if (esp_gps->item_str[6] == '.') {
    uint16_t tmp = 0;
    uint8_t i = 7;
    while (esp_gps->item_str[i]) {
      tmp = 10 * tmp + esp_gps->item_str[i] - '0';
      i++;
    }
    esp_gps->parent.tim.thousand = tmp;
  }
}

#if CONFIG_NMEA_STATEMENT_GGA
/**
 * @brief Parse GGA statements
 *
 * @param esp_gps esp_gps_t type object
 */
static void parse_gga(esp_gps_t *esp_gps) {
  /* Process GGA statement */
  switch (esp_gps->item_num) {
  case 1: /* Process UTC time */
    parse_utc_time(esp_gps);
    break;
  case 2: /* Latitude */
    esp_gps->parent.latitude = parse_lat_long(esp_gps);
    break;
  case 3: /* Latitude north(1)/south(-1) information */
    if (esp_gps->item_str[0] == 'S' || esp_gps->item_str[0] == 's') {
      esp_gps->parent.latitude *= -1;
    }
    break;
  case 4: /* Longitude */
    esp_gps->parent.longitude = parse_lat_long(esp_gps);
    break;
  case 5: /* Longitude east(1)/west(-1) information */
    if (esp_gps->item_str[0] == 'W' || esp_gps->item_str[0] == 'w') {
      esp_gps->parent.longitude *= -1;
    }
    break;
  case 6: /* Fix status */
    esp_gps->parent.fix = (gps_fix_t)strtol(esp_gps->item_str, NULL, 10);
    /* Some receivers never emit GSA, so fix_mode would stay unset and the fix
     * would be rejected by the fix_mode >= 2D gate. Fall back to 2D when GGA
     * reports a fix; a real GSA (2D/3D) supersedes this via arbitration. */
    if (esp_gps->parent.fix != GPS_FIX_INVALID &&
        esp_gps->parent.fix_mode < GPS_MODE_2D) {
      esp_gps->parent.fix_mode = GPS_MODE_2D;
    }
    break;
  case 7: /* Satellites in use */
    esp_gps->parent.sats_in_use = (uint8_t)strtol(esp_gps->item_str, NULL, 10);
    break;
  case 8: /* HDOP */
    esp_gps->parent.dop_h = strtof(esp_gps->item_str, NULL);
    break;
  case 9: /* Altitude */
    esp_gps->parent.altitude = strtof(esp_gps->item_str, NULL);
    break;
  case 11: /* Altitude above ellipsoid */
    esp_gps->parent.altitude += strtof(esp_gps->item_str, NULL);
    break;
  default:
    break;
  }
}
#endif

#if CONFIG_NMEA_STATEMENT_GSA
/**
 * @brief Parse GSA statements
 *
 * @param esp_gps esp_gps_t type object
 */
static void parse_gsa(esp_gps_t *esp_gps) {
  /* Process GSA statement */
  switch (esp_gps->item_num) {
  case 2: /* Process fix mode */
    esp_gps->parent.fix_mode =
        (gps_fix_mode_t)strtol(esp_gps->item_str, NULL, 10);
    break;
  case 15: /* Process PDOP */
    esp_gps->parent.dop_p = strtof(esp_gps->item_str, NULL);
    break;
  case 16: /* Process HDOP */
    esp_gps->parent.dop_h = strtof(esp_gps->item_str, NULL);
    break;
  case 17: /* Process VDOP */
    esp_gps->parent.dop_v = strtof(esp_gps->item_str, NULL);
    break;
  default:
    /* Parse satellite IDs */
    if (esp_gps->item_num >= 3 && esp_gps->item_num <= 14) {
      esp_gps->parent.sats_id_in_use[esp_gps->item_num - 3] =
          (uint8_t)strtol(esp_gps->item_str, NULL, 10);
    }
    break;
  }
}
#endif

#if CONFIG_NMEA_STATEMENT_GSV
/**
 * @brief Parse GSV statements
 *
 * @param esp_gps esp_gps_t type object
 */
static void parse_gsv(esp_gps_t *esp_gps) {
  /* Process GSV statement */
  switch (esp_gps->item_num) {
  case 1: /* total GSV numbers */
    esp_gps->sat_count = (uint8_t)strtol(esp_gps->item_str, NULL, 10);
    break;
  case 2: /* Current GSV statement number */
    esp_gps->sat_num = (uint8_t)strtol(esp_gps->item_str, NULL, 10);
    break;
  case 3: /* Process satellites in view */
    esp_gps->parent.sats_in_view = (uint8_t)strtol(esp_gps->item_str, NULL, 10);
    break;
  default:
    if (esp_gps->item_num >= 4 && esp_gps->item_num <= 19 &&
        gps_tracker_is_primary_talker(esp_gps, esp_gps->cur_talker)) {
      uint8_t item_num =
          esp_gps->item_num - 4; /* Normalize item number from 4-19 to 0-15 */
      uint8_t index;
      uint32_t value;
      index = 4 * (esp_gps->sat_num - 1) + item_num / 4; /* Get array index */
      if (index < GPS_MAX_SATELLITES_IN_VIEW) {
        value = strtol(esp_gps->item_str, NULL, 10);
        switch (item_num % 4) {
        case 0:
          esp_gps->parent.sats_desc_in_view[index].num = (uint8_t)value;
          break;
        case 1:
          esp_gps->parent.sats_desc_in_view[index].elevation = (uint8_t)value;
          break;
        case 2:
          esp_gps->parent.sats_desc_in_view[index].azimuth = (uint16_t)value;
          break;
        case 3:
          esp_gps->parent.sats_desc_in_view[index].snr = (uint8_t)value;
          break;
        default:
          break;
        }
      }
    }
    break;
  }
}
#endif

#if CONFIG_NMEA_STATEMENT_RMC
/**
 * @brief Parse RMC statements
 *
 * @param esp_gps esp_gps_t type object
 */
static void parse_rmc(esp_gps_t *esp_gps) {
  /* Process GPRMC statement */
  switch (esp_gps->item_num) {
  case 1: /* Process UTC time */
    parse_utc_time(esp_gps);
    break;
  case 2: /* Process valid status */
    esp_gps->parent.valid = (esp_gps->item_str[0] == 'A');
    break;
  case 3: /* Latitude */
    esp_gps->parent.latitude = parse_lat_long(esp_gps);
    break;
  case 4: /* Latitude north(1)/south(-1) information */
    if (esp_gps->item_str[0] == 'S' || esp_gps->item_str[0] == 's') {
      esp_gps->parent.latitude *= -1;
    }
    break;
  case 5: /* Longitude */
    esp_gps->parent.longitude = parse_lat_long(esp_gps);
    break;
  case 6: /* Longitude east(1)/west(-1) information */
    if (esp_gps->item_str[0] == 'W' || esp_gps->item_str[0] == 'w') {
      esp_gps->parent.longitude *= -1;
    }
    break;
  case 7: /* Process ground speed in knots, convert to m/s */
    if (esp_gps->item_str[0]) {
      esp_gps->parent.speed = strtof(esp_gps->item_str, NULL) * 0.51444f;
    }
    break;
  case 8: /* Process true course over ground */
    esp_gps->parent.cog = strtof(esp_gps->item_str, NULL);
    break;
  case 9: /* Process date */
    esp_gps->parent.date.day = convert_two_digit2number(esp_gps->item_str + 0);
    esp_gps->parent.date.month =
        convert_two_digit2number(esp_gps->item_str + 2);
    esp_gps->parent.date.year = convert_two_digit2number(esp_gps->item_str + 4);
    break;
  case 10: /* Process magnetic variation */
    esp_gps->parent.variation = strtof(esp_gps->item_str, NULL);
    break;
  default:
    break;
  }
}
#endif

#if CONFIG_NMEA_STATEMENT_GLL
/**
 * @brief Parse GLL statements
 *
 * @param esp_gps esp_gps_t type object
 */
static void parse_gll(esp_gps_t *esp_gps) {
  /* Process GPGLL statement */
  switch (esp_gps->item_num) {
  case 1: /* Latitude */
    esp_gps->parent.latitude = parse_lat_long(esp_gps);
    break;
  case 2: /* Latitude north(1)/south(-1) information */
    if (esp_gps->item_str[0] == 'S' || esp_gps->item_str[0] == 's') {
      esp_gps->parent.latitude *= -1;
    }
    break;
  case 3: /* Longitude */
    esp_gps->parent.longitude = parse_lat_long(esp_gps);
    break;
  case 4: /* Longitude east(1)/west(-1) information */
    if (esp_gps->item_str[0] == 'W' || esp_gps->item_str[0] == 'w') {
      esp_gps->parent.longitude *= -1;
    }
    break;
  case 5: /* Process UTC time */
    parse_utc_time(esp_gps);
    break;
  case 6: /* Process valid status */
    esp_gps->parent.valid = (esp_gps->item_str[0] == 'A');
    break;
  default:
    break;
  }
}
#endif

#if CONFIG_NMEA_STATEMENT_VTG
/**
 * @brief Parse VTG statements
 *
 * @param esp_gps esp_gps_t type object
 */
static void parse_vtg(esp_gps_t *esp_gps) {
  /* Process GPVGT statement */
  switch (esp_gps->item_num) {
  case 1: /* Process true course over ground */
    esp_gps->parent.cog = strtof(esp_gps->item_str, NULL);
    break;
  case 3: /* Process magnetic variation */
    esp_gps->parent.variation = strtof(esp_gps->item_str, NULL);
    break;
  case 5: /* Process ground speed in knots, convert to m/s */
    if (esp_gps->item_str[0]) {
      esp_gps->parent.speed = strtof(esp_gps->item_str, NULL) * 0.51444f;
    }
    break;
  case 7: /* Process ground speed in km/h, convert to m/s */
    if (esp_gps->item_str[0]) {
      esp_gps->parent.speed = strtof(esp_gps->item_str, NULL) / 3.6f;
    }
    break;
  default:
    break;
  }
}
#endif

#if CONFIG_NMEA_STATEMENT_ZDA
/**
 * @brief Parse ZDA statements (UTC date/time)
 *
 * Not all receivers emit RMC; some supply date/time only via ZDA.
 *
 * @param esp_gps esp_gps_t type object
 */
static void parse_zda(esp_gps_t *esp_gps) {
  switch (esp_gps->item_num) {
  case 1: /* UTC time */
    parse_utc_time(esp_gps);
    break;
  case 2: /* Day */
    esp_gps->parent.date.day = (uint8_t)strtol(esp_gps->item_str, NULL, 10);
    break;
  case 3: /* Month */
    esp_gps->parent.date.month = (uint8_t)strtol(esp_gps->item_str, NULL, 10);
    break;
  case 4: { /* Four-digit year -> offset from 2000 */
    int year = (int)strtol(esp_gps->item_str, NULL, 10);
    if (year >= 2000) {
      year -= 2000;
    } else if (year >= 1900) {
      year -= 1900;
    }
    esp_gps->parent.date.year = (uint16_t)year;
    break;
  }
  default:
    break;
  }
}
#endif

#if CONFIG_NMEA_STATEMENT_GST
/**
 * @brief Recognize GST statements.
 *
 * Position error statistics are not currently mapped onto a gps_t field; the
 * sentence is accepted so it is not treated as unknown.
 */
static void parse_gst(esp_gps_t *esp_gps) {
  (void)esp_gps;
}
#endif

#if CONFIG_NMEA_STATEMENT_GBS
/**
 * @brief Recognize GBS statements (GNSS satellite fault detection).
 *
 * Not currently mapped onto a gps_t field; accepted so it is not unknown.
 */
static void parse_gbs(esp_gps_t *esp_gps) {
  (void)esp_gps;
}
#endif

/**
 * @brief Parse received item
 *
 * @param esp_gps esp_gps_t type object
 * @return esp_err_t ESP_OK on success, ESP_FAIL on error
 */
static esp_err_t parse_item(esp_gps_t *esp_gps) {
  esp_err_t err = ESP_OK;
  /* start of a statement */
  if (esp_gps->item_num == 0 && esp_gps->item_str[0] == '$') {
    /* Capture the talker id (chars 1-2, e.g. "GP" in $GPGGA) so per-talker
     * arbitration can tell constellations apart. */
    esp_gps->cur_talker[0] = esp_gps->item_str[1];
    esp_gps->cur_talker[1] = esp_gps->item_str[2];
    esp_gps->cur_talker[2] = '\0';
    if (0) {
    }
#if CONFIG_NMEA_STATEMENT_GGA
    else if (strstr(esp_gps->item_str, "GGA")) {
      esp_gps->cur_statement = STATEMENT_GGA;
    }
#endif
#if CONFIG_NMEA_STATEMENT_GSA
    else if (strstr(esp_gps->item_str, "GSA")) {
      esp_gps->cur_statement = STATEMENT_GSA;
    }
#endif
#if CONFIG_NMEA_STATEMENT_RMC
    else if (strstr(esp_gps->item_str, "RMC")) {
      esp_gps->cur_statement = STATEMENT_RMC;
    }
#endif
#if CONFIG_NMEA_STATEMENT_GSV
    else if (strstr(esp_gps->item_str, "GSV")) {
      esp_gps->cur_statement = STATEMENT_GSV;
    }
#endif
#if CONFIG_NMEA_STATEMENT_GLL
    else if (strstr(esp_gps->item_str, "GLL")) {
      esp_gps->cur_statement = STATEMENT_GLL;
    }
#endif
#if CONFIG_NMEA_STATEMENT_VTG
    else if (strstr(esp_gps->item_str, "VTG")) {
      esp_gps->cur_statement = STATEMENT_VTG;
    }
#endif
#if CONFIG_NMEA_STATEMENT_ZDA
    else if (strstr(esp_gps->item_str, "ZDA")) {
      esp_gps->cur_statement = STATEMENT_ZDA;
    }
#endif
#if CONFIG_NMEA_STATEMENT_GST
    else if (strstr(esp_gps->item_str, "GST")) {
      esp_gps->cur_statement = STATEMENT_GST;
    }
#endif
#if CONFIG_NMEA_STATEMENT_GBS
    else if (strstr(esp_gps->item_str, "GBS")) {
      esp_gps->cur_statement = STATEMENT_GBS;
    }
#endif
    else {
      esp_gps->cur_statement = STATEMENT_UNKNOWN;
    }
    goto out;
  }
  /* Parse each item, depend on the type of the statement */
  if (esp_gps->cur_statement == STATEMENT_UNKNOWN) {
    goto out;
  }
#if CONFIG_NMEA_STATEMENT_GGA
  else if (esp_gps->cur_statement == STATEMENT_GGA) {
    parse_gga(esp_gps);
  }
#endif
#if CONFIG_NMEA_STATEMENT_GSA
  else if (esp_gps->cur_statement == STATEMENT_GSA) {
    parse_gsa(esp_gps);
  }
#endif
#if CONFIG_NMEA_STATEMENT_GSV
  else if (esp_gps->cur_statement == STATEMENT_GSV) {
    parse_gsv(esp_gps);
  }
#endif
#if CONFIG_NMEA_STATEMENT_RMC
  else if (esp_gps->cur_statement == STATEMENT_RMC) {
    parse_rmc(esp_gps);
  }
#endif
#if CONFIG_NMEA_STATEMENT_GLL
  else if (esp_gps->cur_statement == STATEMENT_GLL) {
    parse_gll(esp_gps);
  }
#endif
#if CONFIG_NMEA_STATEMENT_VTG
  else if (esp_gps->cur_statement == STATEMENT_VTG) {
    parse_vtg(esp_gps);
  }
#endif
#if CONFIG_NMEA_STATEMENT_ZDA
  else if (esp_gps->cur_statement == STATEMENT_ZDA) {
    parse_zda(esp_gps);
  }
#endif
#if CONFIG_NMEA_STATEMENT_GST
  else if (esp_gps->cur_statement == STATEMENT_GST) {
    parse_gst(esp_gps);
  }
#endif
#if CONFIG_NMEA_STATEMENT_GBS
  else if (esp_gps->cur_statement == STATEMENT_GBS) {
    parse_gbs(esp_gps);
  }
#endif
  else {
    err = ESP_FAIL;
  }
out:
  return err;
}

/**
 * @brief Find (or claim) the arbitration slot for a talker id.
 *
 * @return the slot, or NULL when the id is empty or the table is full
 */
static gps_talker_state_t *gps_talker_slot(esp_gps_t *gps, const char *talker) {
  if (!gps || !talker || talker[0] == '\0') {
    return NULL;
  }

  gps_talker_state_t *free_slot = NULL;
  for (int i = 0; i < GPS_MAX_TALKERS; i++) {
    gps_talker_state_t *slot = &gps->talkers[i];
    if (slot->used) {
      if (slot->id[0] == talker[0] && slot->id[1] == talker[1]) {
        return slot;
      }
    } else if (!free_slot) {
      free_slot = slot;
    }
  }

  if (free_slot) {
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->used = true;
    free_slot->id[0] = talker[0];
    free_slot->id[1] = talker[1];
    free_slot->fix_mode = GPS_MODE_INVALID;
  }
  return free_slot;
}

bool gps_tracker_is_primary_talker(const esp_gps_t *gps, const char *talker) {
  if (!gps || !talker || talker[0] == '\0') {
    return false;
  }

  bool has_gn = false;
  bool has_gp = false;
  for (int i = 0; i < GPS_MAX_TALKERS; i++) {
    const gps_talker_state_t *slot = &gps->talkers[i];
    if (!slot->used) {
      continue;
    }
    if (slot->id[0] == 'G' && slot->id[1] == 'N') {
      has_gn = true;
    } else if (slot->id[0] == 'G' && slot->id[1] == 'P') {
      has_gp = true;
    }
  }

  if (has_gn) {
    return talker[0] == 'G' && talker[1] == 'N';
  }
  if (has_gp) {
    return talker[0] == 'G' && talker[1] == 'P';
  }
  return true;
}

void gps_tracker_note_statement(esp_gps_t *gps, const char *talker,
                                nmea_statement_t stmt) {
  if (!gps || !talker || talker[0] == '\0') {
    return;
  }

  gps_talker_state_t *slot = gps_talker_slot(gps, talker);
  if (!slot) {
    return;
  }

  slot->tick = xTaskGetTickCount();
  switch (stmt) {
  case STATEMENT_GGA:
    slot->sats_in_use = gps->parent.sats_in_use;
    if (!slot->has_rmc_or_gll) {
      /* GGA is the only validity source when RMC/GLL are absent. */
      slot->valid = (gps->parent.fix != GPS_FIX_INVALID);
    }
    break;
  case STATEMENT_GSA:
    slot->fix_mode = gps->parent.fix_mode;
    slot->has_gsa = true;
    break;
  case STATEMENT_GSV:
    slot->sats_in_view = gps->parent.sats_in_view;
    break;
  case STATEMENT_RMC:
  case STATEMENT_GLL:
    slot->valid = gps->parent.valid;
    slot->has_rmc_or_gll = true;
    break;
  default:
    break;
  }

  uint32_t now = xTaskGetTickCount();
  uint8_t best_view = 0;
  uint8_t best_use = 0;
  gps_fix_mode_t best_mode = (gps_fix_mode_t)0;
  bool any_valid = false;
  bool any_fresh = false;
  bool any_mode = false;

  for (int i = 0; i < GPS_MAX_TALKERS; i++) {
    const gps_talker_state_t *s = &gps->talkers[i];
    if (!s->used) {
      continue;
    }
    if ((now - s->tick) > pdMS_TO_TICKS(GPS_TALKER_STALE_MS)) {
      continue;
    }
    any_fresh = true;
    if (s->sats_in_view > best_view) best_view = s->sats_in_view;
    if (s->sats_in_use > best_use) best_use = s->sats_in_use;
    if (s->has_gsa) {
      any_mode = true;
      if (s->fix_mode > best_mode) best_mode = s->fix_mode;
    }
    if (s->valid) any_valid = true;
  }

  if (any_fresh) {
    gps->parent.sats_in_view = best_view;
    gps->parent.sats_in_use = best_use;
    gps->parent.valid = any_valid;
    /* Only override the published fix mode once a GSA has been seen, so a
     * GGA-derived 2D fallback (software parser) is not clobbered. */
    if (any_mode) {
      gps->parent.fix_mode = best_mode;
    }
  }
}

/**
 * @brief Parse NMEA statements from GPS receiver
 *
 * @param esp_gps esp_gps_t type object
 * @param len number of bytes to decode
 * @return esp_err_t ESP_OK on success, ESP_FAIL on error
 */
static esp_err_t gps_decode(esp_gps_t *esp_gps, size_t len) {
  const uint8_t *d = esp_gps->buffer;
  while (*d) {
    /* Start of a statement */
    if (*d == '$') {
      /* Roll back to the last fully-validated fix so a corrupt or truncated
       * sentence can never leak partial fields into the published state. */
      esp_gps->parent = esp_gps->committed;
      /* Reset runtime information */
      esp_gps->asterisk = 0;
      esp_gps->item_num = 0;
      esp_gps->item_pos = 0;
      esp_gps->cur_statement = 0;
      esp_gps->cur_talker[0] = '\0';
      esp_gps->crc = 0;
      esp_gps->sat_count = 0;
      esp_gps->sat_num = 0;
      /* Add character to item */
      esp_gps->item_str[esp_gps->item_pos++] = *d;
      esp_gps->item_str[esp_gps->item_pos] = '\0';
    }
    /* Detect item separator character */
    else if (*d == ',') {
      /* Parse current item */
      parse_item(esp_gps);
      /* Add character to CRC computation */
      esp_gps->crc ^= (uint8_t)(*d);
      /* Start with next item */
      esp_gps->item_pos = 0;
      esp_gps->item_str[0] = '\0';
      esp_gps->item_num++;
    }
    /* End of CRC computation */
    else if (*d == '*') {
      /* Parse current item */
      parse_item(esp_gps);
      /* Asterisk detected */
      esp_gps->asterisk = 1;
      /* Start with next item */
      esp_gps->item_pos = 0;
      esp_gps->item_str[0] = '\0';
      esp_gps->item_num++;
    }
    /* End of statement */
    else if (*d == '\r' || *d == '\n') {
      if (esp_gps->item_pos == 0 && esp_gps->cur_statement == STATEMENT_UNKNOWN) {
        d++;
        continue;
      }

      /* Convert received CRC from string (hex) to number */
      uint8_t crc = (uint8_t)strtol(esp_gps->item_str, NULL, 16);
      /* CRC passed */
      if (esp_gps->crc == crc) {
        bool sentence_supported = false;
        switch (esp_gps->cur_statement) {
#if CONFIG_NMEA_STATEMENT_GGA
        case STATEMENT_GGA:
          esp_gps->parsed_statement |= 1 << STATEMENT_GGA;
          sentence_supported = true;
          break;
#endif
#if CONFIG_NMEA_STATEMENT_GSA
        case STATEMENT_GSA:
          esp_gps->parsed_statement |= 1 << STATEMENT_GSA;
          sentence_supported = true;
          break;
#endif
#if CONFIG_NMEA_STATEMENT_RMC
        case STATEMENT_RMC:
          esp_gps->parsed_statement |= 1 << STATEMENT_RMC;
          sentence_supported = true;
          break;
#endif
#if CONFIG_NMEA_STATEMENT_GSV
        case STATEMENT_GSV:
          sentence_supported = true;
          if (esp_gps->sat_num == esp_gps->sat_count) {
            esp_gps->parsed_statement |= 1 << STATEMENT_GSV;
          }
          break;
#endif
#if CONFIG_NMEA_STATEMENT_GLL
        case STATEMENT_GLL:
          esp_gps->parsed_statement |= 1 << STATEMENT_GLL;
          sentence_supported = true;
          break;
#endif
#if CONFIG_NMEA_STATEMENT_VTG
        case STATEMENT_VTG:
          esp_gps->parsed_statement |= 1 << STATEMENT_VTG;
          sentence_supported = true;
          break;
#endif
#if CONFIG_NMEA_STATEMENT_ZDA
        case STATEMENT_ZDA:
          sentence_supported = true;
          break;
#endif
#if CONFIG_NMEA_STATEMENT_GST
        case STATEMENT_GST:
          sentence_supported = true;
          break;
#endif
#if CONFIG_NMEA_STATEMENT_GBS
        case STATEMENT_GBS:
          sentence_supported = true;
          break;
#endif
        default:
          break;
        }
        if (sentence_supported) {
          /* Arbitrate multi-constellation fields, then commit this sentence as
           * the new known-good baseline before publishing it. */
          gps_tracker_note_statement(esp_gps, esp_gps->cur_talker,
                                     (nmea_statement_t)esp_gps->cur_statement);
          esp_gps->committed = esp_gps->parent;
          esp_event_post_to(esp_gps->event_loop_hdl, ESP_NMEA_EVENT, GPS_UPDATE,
                            &(esp_gps->parent), sizeof(gps_t),
                            100 / portTICK_PERIOD_MS);
        }

        /* Keep the legacy complete-set tracking for callers that inspect parsed_statement. */
        if (((esp_gps->parsed_statement) & esp_gps->all_statements) ==
            esp_gps->all_statements) {
          esp_gps->parsed_statement = 0;
        }
      } else {
        ESP_LOGD(GPS_TAG, "CRC Error for statement:%s", esp_gps->buffer);
        /* A checksum failure must not leave the partially-parsed fields behind;
         * roll back to the last known-good fix. */
        esp_gps->parent = esp_gps->committed;
      }
      if (esp_gps->cur_statement == STATEMENT_UNKNOWN) {
        /* Send signal to notify that one unknown statement has been met */
        esp_event_post_to(esp_gps->event_loop_hdl, ESP_NMEA_EVENT, GPS_UNKNOWN,
                          esp_gps->buffer, len, 100 / portTICK_PERIOD_MS);
      }

      esp_gps->asterisk = 0;
      esp_gps->item_num = 0;
      esp_gps->item_pos = 0;
      esp_gps->item_str[0] = '\0';
      esp_gps->cur_statement = STATEMENT_UNKNOWN;
      esp_gps->crc = 0;
    }
    /* Other non-space character */
    else {
      if (!(esp_gps->asterisk)) {
        /* Add to CRC */
        esp_gps->crc ^= (uint8_t)(*d);
      }
      /* Add character to item (bounded: over-long fields are truncated rather
       * than overrunning item_str into the rest of the struct, which would
       * corrupt the parent gps_t / UART handles and crash on next use). */
      if (esp_gps->item_pos < NMEA_MAX_STATEMENT_ITEM_LENGTH - 1) {
        esp_gps->item_str[esp_gps->item_pos++] = *d;
        esp_gps->item_str[esp_gps->item_pos] = '\0';
      }
    }
    /* Process next character */
    d++;
  }
  return ESP_OK;
}

/**
 * @brief Handle when a pattern has been detected by uart
 *
 * @param esp_gps esp_gps_t type object
 */
static void esp_handle_uart_pattern(esp_gps_t *esp_gps) {
  while (uart_pattern_pop_pos(esp_gps->uart_port) != -1) {
  }
  esp_drain_uart_data(esp_gps, 0);
}

static void esp_drain_uart_data(esp_gps_t *esp_gps, size_t pending_len) {
  if (!esp_gps || !esp_gps->buffer) {
    return;
  }

  while (1) {
    size_t buffered = 0;
    if (uart_get_buffered_data_len(esp_gps->uart_port, &buffered) == ESP_OK && buffered > pending_len) {
      pending_len = buffered;
    }

    if (pending_len == 0) {
      break;
    }

    size_t to_read = pending_len;
    if (to_read > NMEA_PARSER_RUNTIME_BUFFER_SIZE - 1) {
      to_read = NMEA_PARSER_RUNTIME_BUFFER_SIZE - 1;
    }

    int read_len = uart_read_bytes(esp_gps->uart_port,
                                   esp_gps->buffer,
                                   to_read,
                                   pdMS_TO_TICKS(20));
    if (read_len <= 0) {
      break;
    }

    esp_gps->buffer[read_len] = '\0';
    if (gps_decode(esp_gps, (size_t)read_len) != ESP_OK) {
      ESP_LOGW(GPS_TAG, "GPS decode chunk failed");
    }

    if ((size_t)read_len >= pending_len) {
      pending_len = 0;
    } else {
      pending_len -= (size_t)read_len;
    }
  }
}

/**
 * @brief NMEA Parser Task Entry
 *
 * @param arg argument
 */
static void nmea_parser_task_entry(void *arg) {
  esp_gps_t *esp_gps = (esp_gps_t *)arg;
  uart_event_t event;
  while (1) {
    if (xQueueReceive(esp_gps->event_queue, &event, pdMS_TO_TICKS(200))) {
      switch (event.type) {
      case UART_DATA:
        esp_drain_uart_data(esp_gps, event.size);
        break;
      case UART_FIFO_OVF:
        ESP_LOGW(GPS_TAG, "HW FIFO Overflow");
        uart_flush(esp_gps->uart_port);
        xQueueReset(esp_gps->event_queue);
        break;
      case UART_BUFFER_FULL:
        ESP_LOGW(GPS_TAG, "Ring Buffer Full");
        /* Drop the whole RX buffer in one shot and resync. The previous
         * 64-bytes-at-a-time drain with a 1ms delay per chunk could never keep
         * up at high baud, so it just re-overflowed continuously. */
        uart_flush_input(esp_gps->uart_port);
        xQueueReset(esp_gps->event_queue);
        break;
      case UART_BREAK:
        ESP_LOGW(GPS_TAG, "Rx Break");
        break;
      case UART_PARITY_ERR:
        ESP_LOGE(GPS_TAG, "Parity Error");
        break;
      case UART_FRAME_ERR:
        ESP_LOGE(GPS_TAG, "Frame Error");
        break;
      case UART_PATTERN_DET:
        esp_handle_uart_pattern(esp_gps);
        break;
      default:
        ESP_LOGW(GPS_TAG, "unknown uart event type: %d", event.type);
        break;
      }
    }
    /* Drive the event loop. Keep this short so the task returns to draining the
     * UART promptly; a long block here lets the RX ring buffer overflow at high
     * baud (this was 50ms, which caused continuous "Ring Buffer Full" at 115200). */
    esp_event_loop_run(esp_gps->event_loop_hdl, pdMS_TO_TICKS(10));
  }
  vTaskDelete(NULL);
}

/**
 * @brief Init NMEA Parser
 *
 * @param config Configuration of NMEA Parser
 * @return nmea_parser_handle_t handle of nmea_parser
 */
nmea_parser_handle_t nmea_parser_init(const nmea_parser_config_t *config) {
  esp_gps_t *esp_gps = calloc(1, sizeof(esp_gps_t));
  if (!esp_gps) {
    ESP_LOGE(GPS_TAG, "calloc memory for esp_fps failed");
    goto err_gps;
  }
  esp_gps->buffer = calloc(1, NMEA_PARSER_RUNTIME_BUFFER_SIZE);
  if (!esp_gps->buffer) {
    ESP_LOGE(GPS_TAG, "calloc memory for runtime buffer failed");
    goto err_buffer;
  }
#if CONFIG_NMEA_STATEMENT_GSA
  esp_gps->all_statements |= (1 << STATEMENT_GSA);
#endif
#if CONFIG_NMEA_STATEMENT_GSV
  esp_gps->all_statements |= (1 << STATEMENT_GSV);
#endif
#if CONFIG_NMEA_STATEMENT_GGA
  esp_gps->all_statements |= (1 << STATEMENT_GGA);
#endif
#if CONFIG_NMEA_STATEMENT_RMC
  esp_gps->all_statements |= (1 << STATEMENT_RMC);
#endif
#if CONFIG_NMEA_STATEMENT_GLL
  esp_gps->all_statements |= (1 << STATEMENT_GLL);
#endif
#if CONFIG_NMEA_STATEMENT_VTG
  esp_gps->all_statements |= (1 << STATEMENT_VTG);
#endif
  /* Set attributes */
  esp_gps->uart_port = config->uart.uart_port;
  esp_gps->all_statements &= 0xFE;
  /* Install UART friver */
  uart_config_t uart_config = {
      .baud_rate = config->uart.baud_rate,
      .data_bits = config->uart.data_bits,
      .parity = config->uart.parity,
      .stop_bits = config->uart.stop_bits,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .source_clk = UART_SCLK_DEFAULT,
  };
  if (uart_share_ensure_installed(esp_gps->uart_port, CONFIG_NMEA_PARSER_RING_BUFFER_SIZE, 0,
                                 (int)config->uart.event_queue_size) != ESP_OK) {
    ESP_LOGE(GPS_TAG, "install uart driver failed");
    goto err_uart_install;
  }

  if (uart_share_acquire(esp_gps->uart_port, UART_SHARE_OWNER_GPS, pdMS_TO_TICKS(2000)) != ESP_OK) {
    ESP_LOGE(GPS_TAG, "acquire uart failed");
    goto err_uart_install;
  }

  uart_flush_input(esp_gps->uart_port);

  esp_gps->event_queue = uart_share_get_event_queue(esp_gps->uart_port);
  if (!esp_gps->event_queue) {
    ESP_LOGE(GPS_TAG, "uart event queue unavailable");
    goto err_uart_install;
  }

  xQueueReset(esp_gps->event_queue);

#if CONFIG_IDF_TARGET_ESP32C5
  uart_ll_enable_bus_clock(esp_gps->uart_port, true);
  uart_ll_sclk_enable(UART_LL_GET_HW(esp_gps->uart_port));
#endif
  if (uart_param_config(esp_gps->uart_port, &uart_config) != ESP_OK) {
    ESP_LOGE(GPS_TAG, "config uart parameter failed");
    goto err_uart_config;
  }
  if (uart_set_pin(esp_gps->uart_port, UART_PIN_NO_CHANGE, config->uart.rx_pin,
                   UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
    ESP_LOGE(GPS_TAG, "config uart gpio failed");
    goto err_uart_config;
  }
  /* Set pattern interrupt, used to detect the end of a line */
  uart_enable_pattern_det_baud_intr(esp_gps->uart_port, '\n', 1, 9, 0, 0);
  /* Set pattern queue size */
  uart_pattern_queue_reset(esp_gps->uart_port, config->uart.event_queue_size);
  uart_flush(esp_gps->uart_port);
  /* Create Event loop */
  esp_event_loop_args_t loop_args = {.queue_size = NMEA_EVENT_LOOP_QUEUE_SIZE,
                                     .task_name = NULL};
  if (esp_event_loop_create(&loop_args, &esp_gps->event_loop_hdl) != ESP_OK) {
    ESP_LOGE(GPS_TAG, "create event loop faild");
    goto err_eloop;
  }
  /* Allocate task stack from PSRAM if available, else internal */
#if CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY
  esp_gps->task_stack = (StackType_t *)heap_caps_malloc(
      CONFIG_NMEA_PARSER_TASK_STACK_SIZE * sizeof(StackType_t),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!esp_gps->task_stack) {
    esp_gps->task_stack = (StackType_t *)heap_caps_malloc(
        CONFIG_NMEA_PARSER_TASK_STACK_SIZE * sizeof(StackType_t),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
#else
  esp_gps->task_stack = (StackType_t *)heap_caps_malloc(
      CONFIG_NMEA_PARSER_TASK_STACK_SIZE * sizeof(StackType_t),
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
  esp_gps->task_tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t),
                                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!esp_gps->task_stack || !esp_gps->task_tcb) {
    ESP_LOGE(GPS_TAG,
             "alloc NMEA task resources failed (stack_bytes=%d free_internal=%u free_heap=%u)",
             (int)CONFIG_NMEA_PARSER_TASK_STACK_SIZE,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    goto err_task_create;
  }
  esp_gps->tsk_hdl = xTaskCreateStatic(
      nmea_parser_task_entry, "nmea_parser", CONFIG_NMEA_PARSER_TASK_STACK_SIZE,
      esp_gps, CONFIG_NMEA_PARSER_TASK_PRIORITY,
      esp_gps->task_stack, esp_gps->task_tcb);
  if (!esp_gps->tsk_hdl) {
    ESP_LOGE(GPS_TAG, "xTaskCreateStatic failed");
    goto err_task_create;
  }

  ESP_LOGI(GPS_TAG, "NMEA Parser init OK");
  return esp_gps;
  /*Error Handling*/
err_task_create:
  if (esp_gps->task_stack) heap_caps_free(esp_gps->task_stack);
  if (esp_gps->task_tcb) heap_caps_free(esp_gps->task_tcb);
  esp_event_loop_delete(esp_gps->event_loop_hdl);
err_eloop:
err_uart_install:
  (void)uart_share_release(esp_gps->uart_port, UART_SHARE_OWNER_GPS);
err_uart_config:
err_buffer:
  free(esp_gps->buffer);
err_gps:
  free(esp_gps);
  return NULL;
}

/**
 * @brief Deinit NMEA Parser
 *
 * @param nmea_hdl handle of NMEA parser
 * @return esp_err_t ESP_OK on success,ESP_FAIL on error
 */
esp_err_t nmea_parser_deinit(nmea_parser_handle_t nmea_hdl) {
  esp_gps_t *esp_gps = (esp_gps_t *)nmea_hdl;
  vTaskDelete(esp_gps->tsk_hdl);
  esp_event_loop_delete(esp_gps->event_loop_hdl);
  uart_disable_pattern_det_intr(esp_gps->uart_port);
  (void)uart_flush_input(esp_gps->uart_port);
  esp_err_t err = uart_share_release(esp_gps->uart_port, UART_SHARE_OWNER_GPS);
  if (esp_gps->task_stack) heap_caps_free(esp_gps->task_stack);
  if (esp_gps->task_tcb) heap_caps_free(esp_gps->task_tcb);
  free(esp_gps->buffer);
  free(esp_gps);
  return err;
}

/**
 * @brief Add user defined handler for NMEA parser
 *
 * @param nmea_hdl handle of NMEA parser
 * @param event_handler user defined event handler
 * @param handler_args handler specific arguments
 * @return esp_err_t
 *  - ESP_OK: Success
 *  - ESP_ERR_NO_MEM: Cannot allocate memory for the handler
 *  - ESP_ERR_INVALIG_ARG: Invalid combination of event base and event id
 *  - Others: Fail
 */
esp_err_t nmea_parser_add_handler(nmea_parser_handle_t nmea_hdl,
                                  esp_event_handler_t event_handler,
                                  void *handler_args) {
  esp_gps_t *esp_gps = (esp_gps_t *)nmea_hdl;
  return esp_event_handler_register_with(esp_gps->event_loop_hdl,
                                         ESP_NMEA_EVENT, ESP_EVENT_ANY_ID,
                                         event_handler, handler_args);
}

/**
 * @brief Remove user defined handler for NMEA parser
 *
 * @param nmea_hdl handle of NMEA parser
 * @param event_handler user defined event handler
 * @return esp_err_t
 *  - ESP_OK: Success
 *  - ESP_ERR_INVALIG_ARG: Invalid combination of event base and event id
 *  - Others: Fail
 */
esp_err_t nmea_parser_remove_handler(nmea_parser_handle_t nmea_hdl,
                                     esp_event_handler_t event_handler) {
  esp_gps_t *esp_gps = (esp_gps_t *)nmea_hdl;
  return esp_event_handler_unregister_with(
      esp_gps->event_loop_hdl, ESP_NMEA_EVENT, ESP_EVENT_ANY_ID, event_handler);
}
