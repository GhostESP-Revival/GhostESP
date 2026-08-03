#include "gui/live_chart.h"
#include "gui/accessibility_fonts.h"
#include "gui/design_tokens.h"
#include "gui/theme_palette_api.h"
#include "managers/display_manager.h"
#include "managers/settings_manager.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LIVE_CHART_LABEL_LEN 32
#define LIVE_CHART_FORMAT_LEN 16
#define LIVE_CHART_ALERT_LEN 64
#define LIVE_CHART_MAX_GRID_LINES 8
#define LIVE_CHART_MAX_SERIES 8

struct live_chart_t {
    lv_obj_t *obj;
    float *values;
    float *peaks;
    float *extra_values;
    int *x_values;
    int capacity;
    int count;
    int x_value_count;
    int series_count;
    lv_color_t series_colors[LIVE_CHART_MAX_SERIES];
    bool series_color_set[LIVE_CHART_MAX_SERIES];
    int head;
    int next_write;
    int cursor;
    lv_coord_t bottom_reserved;
    lv_coord_t max_height;
    live_chart_type_t type;
    float y_min;
    float y_max;
    int grid_lines;
    bool show_peaks;
    bool auto_scroll;
    bool flat;
    bool has_peaks;
    bool fullscreen;
    bool alert_visible;
    lv_color_t alert_bg;
    lv_color_t alert_fg;
    char x_label_left[LIVE_CHART_LABEL_LEN];
    char x_label_right[LIVE_CHART_LABEL_LEN];
    char y_label_fmt[LIVE_CHART_FORMAT_LEN];
    char alert_text[LIVE_CHART_ALERT_LEN];
};

static void copy_text(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0) return;
    if (!src) src = "";
    snprintf(dst, dst_size, "%s", src);
}

static void copy_axis_format(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0) return;
    dst[0] = '\0';
    if (!src || strlen(src) >= dst_size) return;

    int conversions = 0;
    for (const char *cursor = src; *cursor; cursor++) {
        if (*cursor != '%') continue;
        cursor++;
        if (*cursor == '%') continue;
        if (*cursor != 'd') return;
        conversions++;
    }
    if (conversions == 1) copy_text(dst, dst_size, src);
}

static float finite_value(float value) {
    return isfinite(value) ? value : 0.0f;
}

static int physical_index(const live_chart_t *chart, int logical_index) {
    return (chart->head + logical_index) % chart->capacity;
}

static float chart_value(const live_chart_t *chart, int logical_index) {
    return chart->values[physical_index(chart, logical_index)];
}

static float chart_series_value(const live_chart_t *chart, int series, int logical_index) {
    int index = physical_index(chart, logical_index);
    if (series <= 0 || !chart->extra_values) return chart->values[index];
    return chart->extra_values[(size_t)(series - 1) * chart->capacity + index];
}

static float chart_peak(const live_chart_t *chart, int logical_index) {
    return chart->peaks[physical_index(chart, logical_index)];
}

static bool reserve_points(live_chart_t *chart, int required) {
    if (!chart || required <= chart->capacity) return true;

    int capacity = required;

    float *values = calloc((size_t)capacity, sizeof(float));
    float *peaks = calloc((size_t)capacity, sizeof(float));
    float *extra_values = chart->series_count > 1 ?
                              calloc((size_t)(chart->series_count - 1) * capacity, sizeof(float)) : NULL;
    if (!values || !peaks || (chart->series_count > 1 && !extra_values)) {
        free(values);
        free(peaks);
        free(extra_values);
        return false;
    }

    for (int i = 0; i < chart->count; i++) {
        values[i] = chart_value(chart, i);
        peaks[i] = chart_peak(chart, i);
        for (int series = 1; series < chart->series_count; series++) {
            extra_values[(size_t)(series - 1) * capacity + i] =
                chart_series_value(chart, series, i);
        }
    }

    free(chart->values);
    free(chart->peaks);
    free(chart->extra_values);
    chart->values = values;
    chart->peaks = peaks;
    chart->extra_values = extra_values;
    chart->capacity = capacity;
    chart->head = 0;
    chart->next_write = chart->count < capacity ? chart->count : 0;
    return true;
}

static const lv_font_t *chart_font(lv_coord_t width, lv_coord_t height) {
    if (width <= 160 || height <= 80) return &lv_font_montserrat_8;
    if (height <= 120) return &lv_font_montserrat_10;
    return accessibility_get_font_small();
}

static void chart_range(const live_chart_t *chart, float *out_min, float *out_max) {
    float min_value = chart->y_min;
    float max_value = chart->y_max;

    if (min_value == max_value) {
        if (chart->count == 0) {
            min_value = 0.0f;
            max_value = 1.0f;
        } else {
            min_value = chart_value(chart, 0);
            max_value = min_value;
            for (int series = 0; series < chart->series_count; series++) {
                for (int i = 0; i < chart->count; i++) {
                    float value = chart_series_value(chart, series, i);
                    if (value < min_value) min_value = value;
                    if (value > max_value) max_value = value;
                }
            }
            if (chart->has_peaks) {
                for (int i = 0; i < chart->count; i++) {
                    float value = chart_peak(chart, i);
                    if (value < min_value) min_value = value;
                    if (value > max_value) max_value = value;
                }
            }
            if (chart->type != LIVE_CHART_LINE && min_value > 0.0f) min_value = 0.0f;
            if (chart->type != LIVE_CHART_LINE && max_value < 0.0f) max_value = 0.0f;
            if (min_value == max_value) {
                if (chart->type != LIVE_CHART_LINE && min_value >= 0.0f) {
                    min_value = 0.0f;
                    max_value = max_value > 0.0f ? max_value * 1.1f : 1.0f;
                } else {
                    float padding = min_value < 0.0f ? -min_value : min_value;
                    padding = padding > 0.0f ? padding * 0.1f : 1.0f;
                    min_value -= padding;
                    max_value += padding;
                }
            }
        }
    }

    if (min_value > max_value) {
        float swap = min_value;
        min_value = max_value;
        max_value = swap;
    }
    *out_min = min_value;
    *out_max = max_value;
}

static lv_coord_t value_to_y(float value, float min_value, float max_value,
                             lv_coord_t plot_y1, lv_coord_t plot_y2) {
    value = finite_value(value);
    if (value < min_value) value = min_value;
    if (value > max_value) value = max_value;
    float ratio = (value - min_value) / (max_value - min_value);
    lv_coord_t height = plot_y2 - plot_y1;
    return plot_y2 - (lv_coord_t)(ratio * height + 0.5f);
}

static int rounded_axis_value(float value) {
    if (value >= (float)INT_MAX) return INT_MAX;
    if (value <= (float)INT_MIN) return INT_MIN;
    return value >= 0.0f ? (int)(value + 0.5f) : (int)(value - 0.5f);
}

static void draw_grid_and_labels(live_chart_t *chart, lv_draw_ctx_t *draw_ctx,
                                 const lv_area_t *coords, const lv_area_t *plot,
                                 const lv_font_t *font, float min_value,
                                 float max_value, lv_color_t grid_color,
                                 lv_color_t text_color, bool show_y_labels,
                                 bool show_x_labels) {
    lv_draw_rect_dsc_t grid;
    lv_draw_rect_dsc_init(&grid);
    grid.bg_color = grid_color;
    grid.bg_opa = LV_OPA_50;
    grid.border_width = 0;
    grid.radius = 0;

    lv_draw_label_dsc_t label;
    lv_draw_label_dsc_init(&label);
    label.font = font;
    label.color = text_color;

    int lines = chart->grid_lines;
    if (lines < 0) lines = 0;
    if (lines > LIVE_CHART_MAX_GRID_LINES) lines = LIVE_CHART_MAX_GRID_LINES;
    for (int i = 0; i <= lines && lines > 0; i++) {
        lv_coord_t y = plot->y2 - (lv_coord_t)(((int32_t)(plot->y2 - plot->y1) * i) / lines);
        lv_area_t line = {.x1 = plot->x1, .y1 = y, .x2 = plot->x2, .y2 = y};
        lv_draw_rect(draw_ctx, &grid, &line);

        if (show_y_labels) {
            float value = min_value + ((max_value - min_value) * i) / lines;
            char text[20];
            snprintf(text, sizeof(text), chart->y_label_fmt, rounded_axis_value(value));
            lv_coord_t line_h = lv_font_get_line_height(font);
            lv_area_t label_area = {
                .x1 = coords->x1 + (lv_area_get_width(coords) <= 160 ? 2 : GUI_GRID),
                .y1 = y - line_h / 2,
                .x2 = plot->x1 - 3,
                .y2 = y + line_h,
            };
            label.align = LV_TEXT_ALIGN_RIGHT;
            lv_draw_label(draw_ctx, &label, &label_area, text, NULL);
        }
    }

    if (!show_x_labels) return;

    lv_coord_t line_h = lv_font_get_line_height(font);
    if (chart->type == LIVE_CHART_BAR && chart->x_values &&
        chart->count > 0 && chart->x_value_count == chart->count) {
        int max_digits = 1;
        for (int i = 0; i < chart->count; i++) {
            int64_t value = chart->x_values[i];
            int digits = value < 0 ? 2 : 1;
            if (value < 0) value = -value;
            while (value >= 10) {
                value /= 10;
                digits++;
            }
            if (digits > max_digits) max_digits = digits;
        }

        int plot_width = lv_area_get_width(plot);
        int min_label_width = max_digits * (line_h / 2 + 1) + 2;
        if (plot_width < min_label_width) return;
        int stride = (min_label_width * chart->count + plot_width - 1) / plot_width;
        if (stride < 1) stride = 1;

        label.align = LV_TEXT_ALIGN_CENTER;
        for (int i = 0; i < chart->count; i += stride) {
            lv_coord_t x1 = plot->x1 + (lv_coord_t)(((int64_t)i * plot_width) / chart->count);
            lv_coord_t x2 = plot->x1 + (lv_coord_t)(((int64_t)(i + 1) * plot_width) / chart->count) - 1;
            lv_coord_t center = x1 + (x2 - x1) / 2;
            lv_coord_t label_x1 = center - min_label_width / 2;
            if (label_x1 < plot->x1) label_x1 = plot->x1;
            if (label_x1 + min_label_width - 1 > plot->x2) {
                label_x1 = plot->x2 - min_label_width + 1;
            }
            char text[12];
            snprintf(text, sizeof(text), "%d", chart->x_values[i]);
            lv_area_t category = {
                .x1 = label_x1,
                .y1 = plot->y2 + 2,
                .x2 = label_x1 + min_label_width - 1,
                .y2 = plot->y2 + line_h + 2,
            };
            lv_draw_label(draw_ctx, &label, &category, text, NULL);
        }
        return;
    }

    lv_coord_t middle = plot->x1 + (plot->x2 - plot->x1) / 2;
    lv_area_t left = {
        .x1 = plot->x1,
        .y1 = plot->y2 + 2,
        .x2 = middle - 1,
        .y2 = plot->y2 + line_h + 2,
    };
    lv_area_t right = {
        .x1 = middle + 1,
        .y1 = plot->y2 + 2,
        .x2 = plot->x2,
        .y2 = plot->y2 + line_h + 2,
    };
    label.align = LV_TEXT_ALIGN_LEFT;
    lv_draw_label(draw_ctx, &label, &left, chart->x_label_left, NULL);
    label.align = LV_TEXT_ALIGN_RIGHT;
    lv_draw_label(draw_ctx, &label, &right, chart->x_label_right, NULL);
}

static float farther_from(float first, float second, float baseline) {
    float first_distance = first - baseline;
    float second_distance = second - baseline;
    if (first_distance < 0.0f) first_distance = -first_distance;
    if (second_distance < 0.0f) second_distance = -second_distance;
    return second_distance > first_distance ? second : first;
}

static void sample_bin(const live_chart_t *chart, int series, int start, int end,
                       float baseline, float *representative,
                       float *minimum, float *maximum, float *peak) {
    float first = chart_series_value(chart, series, start);
    float rep = first;
    float min_value = first;
    float max_value = first;
    float peak_value = series == 0 && chart->has_peaks ? chart_peak(chart, start) : first;
    for (int i = start + 1; i < end; i++) {
        float value = chart_series_value(chart, series, i);
        if (value < min_value) min_value = value;
        if (value > max_value) max_value = value;
        rep = farther_from(rep, value, baseline);
        if (series == 0 && chart->has_peaks) {
            peak_value = farther_from(peak_value, chart_peak(chart, i), baseline);
        }
    }
    *representative = rep;
    *minimum = min_value;
    *maximum = max_value;
    *peak = peak_value;
}

static void draw_bars(live_chart_t *chart, lv_draw_ctx_t *draw_ctx,
                      const lv_area_t *plot, float min_value, float max_value,
                      lv_color_t accent, lv_color_t peak_color) {
    int count = chart->count;
    int width = lv_area_get_width(plot);
    float baseline = min_value <= 0.0f && max_value >= 0.0f ? 0.0f : min_value;
    lv_coord_t baseline_y = value_to_y(baseline, min_value, max_value, plot->y1, plot->y2);

    lv_draw_rect_dsc_t bar;
    lv_draw_rect_dsc_init(&bar);
    bar.bg_color = accent;
    bar.bg_opa = LV_OPA_COVER;
    bar.border_width = 0;
    bar.radius = 0;

    lv_draw_rect_dsc_t peak_dsc = bar;
    peak_dsc.bg_color = peak_color;

    int bins = count < width ? count : width;
    for (int bin = 0; bin < bins; bin++) {
        int start = count <= width ? bin : (int)(((int64_t)bin * count) / width);
        int end = count <= width ? bin + 1 : (int)(((int64_t)(bin + 1) * count) / width);
        if (end <= start) end = start + 1;

        float value;
        float unused_min;
        float unused_max;
        float peak;
        sample_bin(chart, 0, start, end, baseline, &value, &unused_min, &unused_max, &peak);

        lv_coord_t x1 = count <= width
                            ? plot->x1 + (lv_coord_t)(((int64_t)bin * width) / count)
                            : plot->x1 + bin;
        lv_coord_t x2 = count <= width
                            ? plot->x1 + (lv_coord_t)(((int64_t)(bin + 1) * width) / count) - 1
                            : x1;
        if (x2 < x1) x2 = x1;
        if (x2 > x1 + 1) x2--;

        lv_coord_t value_y = value_to_y(value, min_value, max_value, plot->y1, plot->y2);
        lv_area_t area = {
            .x1 = x1,
            .y1 = LV_MIN(value_y, baseline_y),
            .x2 = x2,
            .y2 = LV_MAX(value_y, baseline_y),
        };
        lv_draw_rect(draw_ctx, &bar, &area);

        if (chart->show_peaks && chart->has_peaks) {
            lv_coord_t peak_y = value_to_y(peak, min_value, max_value, plot->y1, plot->y2);
            lv_area_t peak_area = {.x1 = x1, .y1 = peak_y, .x2 = x2, .y2 = peak_y};
            lv_draw_rect(draw_ctx, &peak_dsc, &peak_area);
        }
    }
}

static void draw_line_or_area(live_chart_t *chart, lv_draw_ctx_t *draw_ctx,
                              const lv_area_t *plot, float min_value,
                              float max_value, int series, lv_color_t accent,
                              lv_color_t peak_color) {
    int count = chart->count;
    int width = lv_area_get_width(plot);
    float baseline = min_value <= 0.0f && max_value >= 0.0f ? 0.0f : min_value;
    lv_coord_t baseline_y = value_to_y(baseline, min_value, max_value, plot->y1, plot->y2);

    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = accent;
    line.opa = LV_OPA_COVER;
    line.width = width >= 300 ? 3 : (width >= 160 ? 2 : 1);
    line.round_start = 1;
    line.round_end = 1;

    lv_draw_rect_dsc_t area;
    lv_draw_rect_dsc_init(&area);
    area.bg_color = accent;
    area.bg_opa = LV_OPA_50;
    area.border_width = 0;
    area.radius = 0;

    lv_draw_rect_dsc_t peak_dsc = area;
    peak_dsc.bg_color = peak_color;
    peak_dsc.bg_opa = LV_OPA_COVER;

    if (count == 1) {
        lv_coord_t y = value_to_y(chart_series_value(chart, series, 0), min_value, max_value,
                                  plot->y1, plot->y2);
        lv_area_t point = {.x1 = plot->x1, .y1 = y, .x2 = plot->x1 + 1, .y2 = y + 1};
        lv_draw_rect(draw_ctx, &area, &point);
        if (series == 0 && chart->show_peaks && chart->has_peaks) {
            lv_coord_t peak_y = value_to_y(chart_peak(chart, 0), min_value, max_value,
                                           plot->y1, plot->y2);
            lv_area_t peak = {.x1 = plot->x1, .y1 = peak_y, .x2 = plot->x1 + 1, .y2 = peak_y};
            lv_draw_rect(draw_ctx, &peak_dsc, &peak);
        }
        return;
    }

    if (count <= width) {
        lv_point_t previous = {
            .x = plot->x1,
            .y = value_to_y(chart_series_value(chart, series, 0), min_value, max_value,
                            plot->y1, plot->y2),
        };
        for (int i = 1; i < count; i++) {
            lv_point_t current = {
                .x = plot->x1 + (lv_coord_t)(((int64_t)i * (width - 1)) / (count - 1)),
                .y = value_to_y(chart_series_value(chart, series, i), min_value, max_value,
                                plot->y1, plot->y2),
            };
            if (chart->type == LIVE_CHART_AREA) {
                lv_point_t polygon[4] = {
                    previous,
                    current,
                    {.x = current.x, .y = baseline_y},
                    {.x = previous.x, .y = baseline_y},
                };
                lv_draw_polygon(draw_ctx, &area, polygon, 4);
            }
            lv_draw_line(draw_ctx, &line, &previous, &current);
            previous = current;
        }
    } else {
        lv_point_t previous = {0};
        bool have_previous = false;
        for (int x = 0; x < width; x++) {
            int start = (int)(((int64_t)x * count) / width);
            int end = (int)(((int64_t)(x + 1) * count) / width);
            if (end <= start) end = start + 1;

            float representative;
            float bin_min;
            float bin_max;
            float unused_peak;
            sample_bin(chart, series, start, end, baseline, &representative,
                       &bin_min, &bin_max, &unused_peak);

            lv_coord_t px = plot->x1 + x;
            lv_coord_t min_y = value_to_y(bin_min, min_value, max_value, plot->y1, plot->y2);
            lv_coord_t max_y = value_to_y(bin_max, min_value, max_value, plot->y1, plot->y2);
            lv_point_t high = {.x = px, .y = LV_MIN(min_y, max_y)};
            lv_point_t low = {.x = px, .y = LV_MAX(min_y, max_y)};
            lv_draw_line(draw_ctx, &line, &high, &low);

            lv_point_t current = {
                .x = px,
                .y = value_to_y(representative, min_value, max_value, plot->y1, plot->y2),
            };
            if (have_previous) lv_draw_line(draw_ctx, &line, &previous, &current);
            previous = current;
            have_previous = true;

            if (chart->type == LIVE_CHART_AREA) {
                lv_area_t fill = {
                    .x1 = px,
                    .y1 = LV_MIN(high.y, baseline_y),
                    .x2 = px,
                    .y2 = LV_MAX(low.y, baseline_y),
                };
                lv_draw_rect(draw_ctx, &area, &fill);
            }
        }
    }

    if (series == 0 && chart->show_peaks && chart->has_peaks) {
        int markers = count < width ? count : width;
        for (int marker = 0; marker < markers; marker++) {
            int start = count <= width ? marker : (int)(((int64_t)marker * count) / width);
            int end = count <= width ? marker + 1 : (int)(((int64_t)(marker + 1) * count) / width);
            if (end <= start) end = start + 1;
            float peak = chart_peak(chart, start);
            for (int i = start + 1; i < end; i++) {
                peak = farther_from(peak, chart_peak(chart, i), baseline);
            }
            lv_coord_t x = count <= width
                               ? plot->x1 + (lv_coord_t)(((int64_t)marker * (width - 1)) / (count - 1))
                               : plot->x1 + marker;
            lv_coord_t y = value_to_y(peak, min_value, max_value, plot->y1, plot->y2);
            lv_area_t point = {.x1 = x, .y1 = y, .x2 = x, .y2 = y};
            lv_draw_rect(draw_ctx, &peak_dsc, &point);
        }
    }
}

static void draw_cursor(live_chart_t *chart, lv_draw_ctx_t *draw_ctx,
                        const lv_area_t *plot, lv_color_t color) {
    if (chart->cursor < 0 || chart->cursor >= chart->count) return;

    int width = lv_area_get_width(plot);
    lv_coord_t x;
    if (chart->type == LIVE_CHART_BAR) {
        x = plot->x1 + (lv_coord_t)((((int64_t)chart->cursor * 2 + 1) * width) /
                                     ((int64_t)chart->count * 2));
    } else if (chart->count == 1) {
        x = plot->x1;
    } else if (chart->count > width) {
        x = plot->x1 + (lv_coord_t)(((int64_t)chart->cursor * width) / chart->count);
    } else {
        x = plot->x1 + (lv_coord_t)(((int64_t)chart->cursor * (width - 1)) /
                                     (chart->count - 1));
    }

    lv_draw_line_dsc_t cursor;
    lv_draw_line_dsc_init(&cursor);
    cursor.color = color;
    cursor.opa = chart->type == LIVE_CHART_BAR ? LV_OPA_30 : LV_OPA_70;
    cursor.width = 1;
    lv_point_t top = {.x = x, .y = plot->y1};
    lv_point_t bottom = {.x = x, .y = plot->y2};
    lv_draw_line(draw_ctx, &cursor, &top, &bottom);
}

static void draw_alert(live_chart_t *chart, lv_draw_ctx_t *draw_ctx,
                       const lv_area_t *plot, const lv_font_t *font) {
    if (!chart->alert_visible || chart->alert_text[0] == '\0') return;

    lv_coord_t height = lv_font_get_line_height(font) + 4;
    lv_area_t area = {
        .x1 = plot->x1,
        .y1 = plot->y1,
        .x2 = plot->x2,
        .y2 = LV_MIN(plot->y2, plot->y1 + height),
    };
    lv_draw_rect_dsc_t background;
    lv_draw_rect_dsc_init(&background);
    background.bg_color = chart->alert_bg;
    background.bg_opa = LV_OPA_80;
    background.border_width = 0;
    background.radius = 0;
    lv_draw_rect(draw_ctx, &background, &area);

    lv_draw_label_dsc_t label;
    lv_draw_label_dsc_init(&label);
    label.font = font;
    label.color = chart->alert_fg;
    label.align = LV_TEXT_ALIGN_CENTER;
    lv_draw_label(draw_ctx, &label, &area, chart->alert_text, NULL);
}

static void live_chart_draw(live_chart_t *chart, lv_draw_ctx_t *draw_ctx) {
    lv_area_t coords;
    lv_obj_get_coords(chart->obj, &coords);
    lv_coord_t width = lv_area_get_width(&coords);
    lv_coord_t height = lv_area_get_height(&coords);
    if (width < 3 || height < 3) return;

    uint8_t theme = settings_get_menu_theme(&G_Settings);
    lv_color_t background = lv_color_hex(theme_palette_get_surface(theme));
    lv_color_t grid = lv_color_hex(theme_palette_get_surface_alt(theme));
    lv_color_t text = lv_color_hex(theme_palette_get_text(theme));
    lv_color_t muted = lv_color_hex(theme_palette_get_text_muted(theme));
    lv_color_t accent = lv_color_hex(theme_palette_get_accent(theme));

    if (!chart->flat) {
        lv_draw_rect_dsc_t panel;
        lv_draw_rect_dsc_init(&panel);
        panel.bg_color = background;
        panel.bg_opa = LV_OPA_COVER;
        panel.border_color = grid;
        panel.border_width = width >= 96 && height >= 48 ? 1 : 0;
        panel.radius = settings_get_menu_rounded(&G_Settings) ?
                           (width <= 160 || height <= 80 ? 4 : GUI_RADIUS_SM) :
                           0;
        lv_draw_rect(draw_ctx, &panel, &coords);
    }

    const lv_font_t *font = chart_font(width, height);
    lv_coord_t line_h = lv_font_get_line_height(font);
    lv_coord_t padding = width <= 160 || height <= 80 ? 2 : GUI_GRID * 2;
    float min_value;
    float max_value;
    chart_range(chart, &min_value, &max_value);
    bool show_y_labels = chart->y_label_fmt[0] != '\0' && chart->grid_lines > 0 &&
                         width >= 96 && height >= 44;
    bool has_category_labels = chart->type == LIVE_CHART_BAR && chart->x_values &&
                               chart->count > 0 && chart->x_value_count == chart->count;
    bool show_x_labels = (has_category_labels || chart->x_label_left[0] != '\0' ||
                          chart->x_label_right[0] != '\0') && height >= line_h * 3;
    lv_coord_t y_axis_width = 0;
    if (show_y_labels) {
        int lines = LV_MIN(chart->grid_lines, LIVE_CHART_MAX_GRID_LINES);
        for (int i = 0; i <= lines; i++) {
            float value = min_value + ((max_value - min_value) * i) / lines;
            char label[20];
            snprintf(label, sizeof(label), chart->y_label_fmt, rounded_axis_value(value));
            lv_coord_t label_width = lv_txt_get_width(label, strlen(label), font, 0, LV_TEXT_FLAG_NONE);
            if (label_width > y_axis_width) y_axis_width = label_width;
        }
        y_axis_width += GUI_GRID;
    }
    lv_coord_t x_axis_height = show_x_labels ? line_h + 3 : 0;

    lv_area_t plot = {
        .x1 = coords.x1 + padding + y_axis_width,
        .y1 = coords.y1 + padding,
        .x2 = coords.x2 - padding,
        .y2 = coords.y2 - padding - x_axis_height,
    };
    if (plot.x2 < plot.x1 || plot.y2 < plot.y1) return;

    draw_grid_and_labels(chart, draw_ctx, &coords, &plot, font, min_value,
                         max_value, grid, muted, show_y_labels, show_x_labels);

    if (chart->count > 0) {
        if (chart->type == LIVE_CHART_BAR) {
            draw_bars(chart, draw_ctx, &plot, min_value, max_value, accent, text);
        } else {
            for (int series = 0; series < chart->series_count; series++) {
                lv_color_t series_color = chart->series_color_set[series] ? chart->series_colors[series] :
                    (chart->series_count == 1 ? accent :
                     lv_color_hex(theme_palette_get(theme, series % THEME_PALETTE_SLOT_COUNT)));
                draw_line_or_area(chart, draw_ctx, &plot, min_value, max_value,
                                  series, series_color, text);
            }
        }
        draw_cursor(chart, draw_ctx, &plot,
                    chart->type == LIVE_CHART_BAR ? accent : muted);
    }
    draw_alert(chart, draw_ctx, &plot, font);
}

static void live_chart_event_cb(lv_event_t *event) {
    live_chart_t *chart = lv_event_get_user_data(event);
    if (!chart) return;

    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_DELETE) {
        chart->obj = NULL;
    } else if (code == LV_EVENT_DRAW_MAIN && chart->obj) {
        lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(event);
        if (draw_ctx) live_chart_draw(chart, draw_ctx);
    }
}

static void relayout(live_chart_t *chart) {
    if (!chart || !chart->obj || !lv_obj_is_valid(chart->obj)) return;

    if (chart->fullscreen) {
        lv_coord_t height = LV_VER_RES - GUI_STATUS_BAR_H - chart->bottom_reserved;
        if (chart->max_height > 0 && height > chart->max_height) height = chart->max_height;
        if (height < 1) height = 1;
        lv_obj_set_pos(chart->obj, 0, GUI_STATUS_BAR_H);
        lv_obj_set_size(chart->obj, LV_HOR_RES, height);
    } else {
        lv_obj_t *parent = lv_obj_get_parent(chart->obj);
        lv_obj_set_width(chart->obj, LV_PCT(100));
        if (chart->bottom_reserved == 0 && chart->max_height <= 0) {
            lv_obj_set_height(chart->obj, LV_PCT(100));
        } else if (parent) {
            lv_obj_update_layout(parent);
            lv_coord_t height = lv_obj_get_content_height(parent) - chart->bottom_reserved;
            if (chart->max_height > 0 && height > chart->max_height) height = chart->max_height;
            lv_obj_set_height(chart->obj, LV_MAX(height, 1));
        }
        lv_obj_align(chart->obj, LV_ALIGN_TOP_MID, 0, 0);
    }
}

live_chart_t *live_chart_create(lv_obj_t *parent, const char *title,
                                const live_chart_config_t *config) {
    if (!config) return NULL;

    live_chart_t *chart = calloc(1, sizeof(live_chart_t));
    if (!chart) return NULL;

    chart->series_count = 1;
    chart->capacity = config->data_points > 0 ? config->data_points : 1;
    chart->values = calloc((size_t)chart->capacity, sizeof(float));
    chart->peaks = calloc((size_t)chart->capacity, sizeof(float));
    if (!chart->values || !chart->peaks) {
        free(chart->values);
        free(chart->peaks);
        free(chart);
        return NULL;
    }

    chart->type = config->type;
    chart->y_min = finite_value(config->y_min);
    chart->y_max = finite_value(config->y_max);
    chart->grid_lines = config->grid_lines;
    chart->max_height = LV_MAX(config->max_height, 0);
    chart->flat = config->flat;
    chart->show_peaks = config->show_peaks;
    chart->auto_scroll = config->auto_scroll;
    chart->cursor = -1;
    copy_text(chart->x_label_left, sizeof(chart->x_label_left), config->x_label_left);
    copy_text(chart->x_label_right, sizeof(chart->x_label_right), config->x_label_right);
    copy_axis_format(chart->y_label_fmt, sizeof(chart->y_label_fmt), config->y_label_fmt);

    chart->fullscreen = parent == NULL;
    if (!parent) parent = lv_scr_act();
    chart->obj = lv_obj_create(parent);
    if (!chart->obj) {
        free(chart->values);
        free(chart->peaks);
        free(chart);
        return NULL;
    }

    lv_obj_set_style_bg_opa(chart->obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chart->obj, 0, 0);
    lv_obj_set_style_pad_all(chart->obj, 0, 0);
    lv_obj_clear_flag(chart->obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scrollbar_mode(chart->obj, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(chart->obj, live_chart_event_cb, LV_EVENT_ALL, chart);
    relayout(chart);

    if (title && title[0] != '\0') display_manager_add_status_bar(title);
    return chart;
}

lv_obj_t *live_chart_get_obj(live_chart_t *chart) {
    return chart ? chart->obj : NULL;
}

static int prepare_push(live_chart_t *chart) {
    int index;
    if (chart->count < chart->capacity) {
        index = physical_index(chart, chart->count);
        chart->count++;
        chart->next_write = chart->count < chart->capacity ? chart->count : 0;
    } else if (chart->auto_scroll) {
        index = chart->head;
        chart->head = (chart->head + 1) % chart->capacity;
    } else {
        index = chart->next_write;
        chart->next_write = (chart->next_write + 1) % chart->capacity;
    }
    return index;
}

static void push_value(live_chart_t *chart, float value, bool invalidate) {
    if (!chart || chart->capacity <= 0) return;

    int index = prepare_push(chart);
    chart->values[index] = finite_value(value);
    for (int series = 1; series < chart->series_count; series++) {
        chart->extra_values[(size_t)(series - 1) * chart->capacity + index] = 0.0f;
    }
    if (chart->has_peaks) chart->peaks[index] = chart->values[index];
    if (invalidate && chart->obj) lv_obj_invalidate(chart->obj);
}

void live_chart_push(live_chart_t *chart, float value) {
    push_value(chart, value, true);
}

void live_chart_push_array(live_chart_t *chart, const float *values, int count) {
    if (!chart || !values || count <= 0) return;
    for (int i = 0; i < count; i++) push_value(chart, values[i], false);
    if (chart->obj) lv_obj_invalidate(chart->obj);
}

bool live_chart_set_series_count(live_chart_t *chart, int series_count) {
    if (!chart || series_count < 1 || series_count > LIVE_CHART_MAX_SERIES) return false;
    if (series_count == chart->series_count) return true;

    float *extra_values = series_count > 1 ?
                              calloc((size_t)(series_count - 1) * chart->capacity, sizeof(float)) : NULL;
    if (series_count > 1 && !extra_values) return false;

    int copied = LV_MIN(series_count, chart->series_count);
    for (int series = 1; series < copied; series++) {
        memcpy(extra_values + (size_t)(series - 1) * chart->capacity,
               chart->extra_values + (size_t)(series - 1) * chart->capacity,
               sizeof(float) * (size_t)chart->capacity);
    }
    free(chart->extra_values);
    chart->extra_values = extra_values;
    chart->series_count = series_count;
    if (chart->obj) lv_obj_invalidate(chart->obj);
    return true;
}

bool live_chart_set_series_color(live_chart_t *chart, int series, lv_color_t color) {
    if (!chart || series < 0 || series >= LIVE_CHART_MAX_SERIES) return false;
    chart->series_colors[series] = color;
    chart->series_color_set[series] = true;
    if (chart->obj) lv_obj_invalidate(chart->obj);
    return true;
}

void live_chart_push_series(live_chart_t *chart, const float *values, int series_count) {
    if (!chart || !values || series_count <= 0 || chart->capacity <= 0) return;
    int index = prepare_push(chart);
    chart->values[index] = finite_value(values[0]);
    for (int series = 1; series < chart->series_count; series++) {
        float value = series < series_count ? values[series] : 0.0f;
        chart->extra_values[(size_t)(series - 1) * chart->capacity + index] = finite_value(value);
    }
    if (chart->obj) lv_obj_invalidate(chart->obj);
}

bool live_chart_set_data(live_chart_t *chart, const float *values, int count) {
    if (!chart) return false;
    if (!values || count <= 0) {
        live_chart_clear(chart);
        return true;
    }
    if (!reserve_points(chart, count)) return false;

    for (int i = 0; i < count; i++) chart->values[i] = finite_value(values[i]);
    memset(chart->peaks, 0, sizeof(float) * (size_t)chart->capacity);
    if (chart->extra_values) {
        memset(chart->extra_values, 0,
               sizeof(float) * (size_t)(chart->series_count - 1) * chart->capacity);
    }
    chart->count = count;
    chart->head = 0;
    chart->next_write = count < chart->capacity ? count : 0;
    chart->has_peaks = false;
    if (chart->cursor >= count) chart->cursor = -1;
    if (chart->obj) lv_obj_invalidate(chart->obj);
    return true;
}

void live_chart_set_peaks(live_chart_t *chart, const float *peaks, int count) {
    if (!chart || !chart->show_peaks || !peaks || count <= 0 || chart->count <= 0) return;

    int copied = LV_MIN(count, chart->count);
    for (int i = 0; i < chart->count; i++) {
        int index = physical_index(chart, i);
        chart->peaks[index] = i < copied ? finite_value(peaks[i]) : chart->values[index];
    }
    chart->has_peaks = true;
    if (chart->obj) lv_obj_invalidate(chart->obj);
}

void live_chart_clear(live_chart_t *chart) {
    if (!chart) return;
    chart->count = 0;
    chart->head = 0;
    chart->next_write = 0;
    chart->cursor = -1;
    chart->has_peaks = false;
    if (chart->obj) lv_obj_invalidate(chart->obj);
}

void live_chart_set_cursor(live_chart_t *chart, int index) {
    if (!chart) return;
    chart->cursor = index >= 0 && index < chart->count ? index : -1;
    if (chart->obj) lv_obj_invalidate(chart->obj);
}

void live_chart_set_x_labels(live_chart_t *chart, const char *left, const char *right) {
    if (!chart) return;
    copy_text(chart->x_label_left, sizeof(chart->x_label_left), left);
    copy_text(chart->x_label_right, sizeof(chart->x_label_right), right);
    if (chart->obj) lv_obj_invalidate(chart->obj);
}

bool live_chart_set_x_values(live_chart_t *chart, const int *values, int count) {
    if (!chart) return false;
    if (!values || count <= 0) {
        free(chart->x_values);
        chart->x_values = NULL;
        chart->x_value_count = 0;
        if (chart->obj) lv_obj_invalidate(chart->obj);
        return true;
    }

    int *copy = malloc(sizeof(int) * (size_t)count);
    if (!copy) return false;
    memcpy(copy, values, sizeof(int) * (size_t)count);
    free(chart->x_values);
    chart->x_values = copy;
    chart->x_value_count = count;
    if (chart->obj) lv_obj_invalidate(chart->obj);
    return true;
}

void live_chart_set_alert(live_chart_t *chart, const char *text,
                          lv_color_t background, lv_color_t foreground) {
    if (!chart) return;
    copy_text(chart->alert_text, sizeof(chart->alert_text), text);
    chart->alert_bg = background;
    chart->alert_fg = foreground;
    chart->alert_visible = chart->alert_text[0] != '\0';
    if (chart->obj) lv_obj_invalidate(chart->obj);
}

void live_chart_clear_alert(live_chart_t *chart) {
    if (!chart) return;
    chart->alert_visible = false;
    chart->alert_text[0] = '\0';
    if (chart->obj) lv_obj_invalidate(chart->obj);
}

void live_chart_set_bottom_reserved(live_chart_t *chart, lv_coord_t reserved_h) {
    if (!chart) return;
    chart->bottom_reserved = LV_MAX(reserved_h, 0);
    relayout(chart);
}

void live_chart_set_title(live_chart_t *chart, const char *title) {
    if (!chart || !title || title[0] == '\0') return;
    display_manager_add_status_bar(title);
}

void live_chart_destroy(live_chart_t *chart) {
    if (!chart) return;
    if (chart->obj && lv_obj_is_valid(chart->obj)) lv_obj_del(chart->obj);
    free(chart->values);
    free(chart->peaks);
    free(chart->extra_values);
    free(chart->x_values);
    free(chart);
}
