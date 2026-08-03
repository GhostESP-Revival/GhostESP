#pragma once

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct live_chart_t live_chart_t;

typedef enum {
    LIVE_CHART_LINE,
    LIVE_CHART_BAR,
    LIVE_CHART_AREA,
} live_chart_type_t;

typedef struct {
    live_chart_type_t type;
    int data_points;
    float y_min;
    float y_max;
    const char *x_label_left;
    const char *x_label_right;
    /* printf-style format for one int, for example "%d%%". */
    const char *y_label_fmt;
    int grid_lines;
    lv_coord_t max_height;
    bool flat;
    bool show_peaks;
    bool auto_scroll;
} live_chart_config_t;

/*
 * Creates a chart which fills parent. A NULL parent creates a full-screen
 * chart below the status bar. Configuration strings are copied by the chart.
 * Create, update, and destroy charts from the LVGL task only.
 */
live_chart_t *live_chart_create(lv_obj_t *parent, const char *title,
                                const live_chart_config_t *config);

/* Access the LVGL object for flex placement or additional sizing. */
lv_obj_t *live_chart_get_obj(live_chart_t *chart);

/*
 * Append one sample. Full auto-scroll charts discard the oldest sample;
 * non-scrolling charts wrap and replace fixed display slots.
 */
void live_chart_push(live_chart_t *chart, float value);
void live_chart_push_array(live_chart_t *chart, const float *values, int count);
/* Configure and append synchronized line series. Colors use theme palette slots. */
bool live_chart_set_series_count(live_chart_t *chart, int series_count);
bool live_chart_set_series_color(live_chart_t *chart, int series, lv_color_t color);
void live_chart_push_series(live_chart_t *chart, const float *values, int series_count);

/*
 * Replaces the visible data. Capacity grows when necessary, so no channels
 * are dropped if count is larger than config.data_points.
 */
bool live_chart_set_data(live_chart_t *chart, const float *values, int count);
void live_chart_set_peaks(live_chart_t *chart, const float *peaks, int count);
void live_chart_clear(live_chart_t *chart);

/* Cursor is a data index; pass -1 to hide it. */
void live_chart_set_cursor(live_chart_t *chart, int index);
void live_chart_set_x_labels(live_chart_t *chart, const char *left, const char *right);
/* Set one numeric category label per data point, such as Wi-Fi channels. */
bool live_chart_set_x_values(live_chart_t *chart, const int *values, int count);

void live_chart_set_alert(live_chart_t *chart, const char *text,
                          lv_color_t background, lv_color_t foreground);
void live_chart_clear_alert(live_chart_t *chart);

/* Primarily for full-screen charts sharing space with a touch control bar. */
void live_chart_set_bottom_reserved(live_chart_t *chart, lv_coord_t reserved_h);
void live_chart_set_title(live_chart_t *chart, const char *title);

void live_chart_destroy(live_chart_t *chart);

#ifdef __cplusplus
}
#endif
