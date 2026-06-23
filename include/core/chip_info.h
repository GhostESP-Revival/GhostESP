#ifndef CHIP_INFO_H
#define CHIP_INFO_H

#ifdef __cplusplus
extern "C" {
#endif

// One row of device/chip info, suitable for the GUI info screen and the
// `chipinfo` CLI command. `label` is always a string literal with static
// lifetime. `value` is a fixed-size caller-owned buffer that this module
// fills in.
typedef struct {
    const char *label;
    char        value[96];
} chip_info_line_t;

typedef struct {
    const char *title;
    char        body[512];
} chip_info_card_t;

// Fill `out[0..max-1]` with the device info rows (firmware, model, heap,
// IDF version, ...). Returns the number of rows written. The order matches
// the `chipinfo` command.
int chip_info_collect_device_info(chip_info_line_t *out, int max);

// Fill `out[0..max-1]` with the enabled build features (one row per feature,
// label = display name, value = "Yes"). Returns the number of rows written.
int chip_info_collect_enabled_features(chip_info_line_t *out, int max);

// Fill `out[0..max-1]` with all rows, device info first followed by enabled
// features. Equivalent to calling both helpers in sequence. Returns the
// total number of rows written.
int chip_info_collect_lines(chip_info_line_t *out, int max);

// Fill `out[0..max-1]` with a small number of longer, display-ready cards.
// This is intended for LVGL menus where many individual rows are expensive
// to scroll/draw on small embedded displays.
int chip_info_collect_cards(chip_info_card_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif // CHIP_INFO_H
