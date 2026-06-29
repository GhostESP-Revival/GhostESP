#include "managers/plugin_api_internal.h"
#include "managers/gps_manager.h"
#include "managers/settings_manager.h"
#include "managers/views/keyboard_screen.h"
#include "managers/display_manager.h"
#include "vendor/GPS/MicroNMEA.h"
#include <stdlib.h>

extern FSettings G_Settings;
extern GPSManager g_gpsManager;

bool plugin_api_gps_is_available(void) {
    return g_gpsManager.isinitilized || gps_manager_has_recent_update();
}

bool plugin_api_gps_has_fix(void) {
    gps_t snap;
    bool using_peer;
    if (!gps_manager_get_active_gps_snapshot(&snap, &using_peer))
        return false;
    if (!snap.valid)
        return false;
    return snap.fix != GPS_FIX_INVALID;
}

double plugin_api_gps_get_latitude(void) {
    gps_t snap;
    bool using_peer;
    if (!gps_manager_get_active_gps_snapshot(&snap, &using_peer))
        return 0.0;
    return (double)snap.latitude;
}

double plugin_api_gps_get_longitude(void) {
    gps_t snap;
    bool using_peer;
    if (!gps_manager_get_active_gps_snapshot(&snap, &using_peer))
        return 0.0;
    return (double)snap.longitude;
}

double plugin_api_gps_get_altitude(void) {
    gps_t snap;
    bool using_peer;
    if (!gps_manager_get_active_gps_snapshot(&snap, &using_peer))
        return 0.0;
    return (double)snap.altitude;
}

int plugin_api_gps_get_satellites(void) {
    gps_t snap;
    bool using_peer;
    if (!gps_manager_get_active_gps_snapshot(&snap, &using_peer))
        return 0;
    return (int)snap.sats_in_use;
}

float plugin_api_gps_get_speed(void) {
    gps_t snap;
    bool using_peer;
    if (!gps_manager_get_active_gps_snapshot(&snap, &using_peer))
        return 0.0f;
    return snap.speed;
}

float plugin_api_gps_get_heading(void) {
    gps_t snap;
    bool using_peer;
    if (!gps_manager_get_active_gps_snapshot(&snap, &using_peer))
        return 0.0f;
    return snap.cog;
}

uint8_t plugin_api_settings_get_theme(void) {
    return settings_get_menu_theme(&G_Settings);
}

const char *plugin_api_settings_get_device_name(void) {
    return settings_get_ap_ssid(&G_Settings);
}

typedef struct {
    ghostesp_input_submit_cb_t cb;
    void *user;
} plugin_input_ctx_t;

static plugin_input_ctx_t *s_plugin_input_ctx = NULL;

static void plugin_api_keyboard_bridge(const char *text) {
    if (!s_plugin_input_ctx)
        return;
    plugin_input_ctx_t *ctx = s_plugin_input_ctx;
    s_plugin_input_ctx = NULL;
    ghostesp_input_submit_cb_t cb = ctx->cb;
    void *user = ctx->user;
    free(ctx);
    if (cb)
        cb(text, user);
}

void plugin_api_ui_input_dialog(const char *title, const char *default_text,
                                ghostesp_input_submit_cb_t on_submit, void *user) {
    if (!on_submit) return;

    plugin_input_ctx_t *ctx = calloc(1, sizeof(plugin_input_ctx_t));
    if (!ctx) return;
    ctx->cb = on_submit;
    ctx->user = user;

    s_plugin_input_ctx = ctx;

    keyboard_view_set_submit_callback(plugin_api_keyboard_bridge);
    if (default_text)
        keyboard_view_set_initial_text(default_text);

    display_manager_switch_view(&keyboard_view);
}
