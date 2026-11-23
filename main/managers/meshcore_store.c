// meshcore_store.c
// NVS-backed persistence for MeshCore prefs, contacts and channels.

#include "managers/meshcore_config.h"
#include "sdkconfig.h"

#ifdef CONFIG_HAS_MESHCORE

#include "managers/meshcore_store.h"

#include <string.h>
#include <stdio.h>

#include "nvs.h"
#include "nvs_flash.h"

#define MC_NVS_NAMESPACE "meshcore"
#define MC_KEY_PREFS     "prefs"
#define MC_KEY_CONTACTS  "contacts"
#define MC_KEY_CHANNELS  "channels"

static bool open_ns(nvs_open_mode_t mode, nvs_handle_t *out) {
    return nvs_open(MC_NVS_NAMESPACE, mode, out) == ESP_OK;
}

void mc_store_defaults(mc_prefs_t *prefs) {
    if (!prefs) return;
    memset(prefs, 0, sizeof(*prefs));
#if defined(CONFIG_MESHCORE_REGION_EU)
    prefs->freq_mhz = MC_DEFAULT_FREQ_EU_MHZ;
    prefs->sf = MC_DEFAULT_SF_EU;
    prefs->cr = MC_DEFAULT_CR_EU;
#else
    prefs->freq_mhz = MC_DEFAULT_FREQ_US_MHZ;
    prefs->sf = MC_DEFAULT_SF_US;
    prefs->cr = MC_DEFAULT_CR_US;
#endif
    prefs->bw_khz = MC_DEFAULT_BW_KHZ;
    prefs->tx_dbm = MC_DEFAULT_TX_DBM;
    prefs->advert_loc_policy = 0;
    prefs->multi_acks = 0;
    prefs->manual_add_contacts = 0;
    prefs->path_hash_mode = 0;
    prefs->ble_pin = 0;
    prefs->radio_configured = false;
    // Default node name (upstream uses the configured name; we seed one).
    snprintf(prefs->node_name, sizeof(prefs->node_name), "Ghost-ESP");
}

bool mc_store_load_prefs(mc_prefs_t *prefs) {
    if (!prefs) return false;
    mc_store_defaults(prefs);
    nvs_handle_t h;
    if (!open_ns(NVS_READONLY, &h)) return false;
    // Read into a temp buffer and copy only the stored prefix, so appending
    // fields to mc_prefs_t upgrades an existing install instead of resetting it.
    uint8_t tmp[sizeof(mc_prefs_t)];
    size_t sz = sizeof(tmp);
    esp_err_t r = nvs_get_blob(h, MC_KEY_PREFS, tmp, &sz);
    nvs_close(h);
    if (r != ESP_OK || sz == 0) {
        mc_store_defaults(prefs);
        return false;
    }
    if (sz > sizeof(*prefs)) sz = sizeof(*prefs);
    memcpy(prefs, tmp, sz);
    if (prefs->node_name[0] == '\0') {
        snprintf(prefs->node_name, sizeof(prefs->node_name), "Ghost-ESP");
    }
    return true;
}

bool mc_store_save_prefs(const mc_prefs_t *prefs) {
    if (!prefs) return false;
    nvs_handle_t h;
    if (!open_ns(NVS_READWRITE, &h)) return false;
    bool ok = nvs_set_blob(h, MC_KEY_PREFS, prefs, sizeof(*prefs)) == ESP_OK;
    if (ok) ok = nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool mc_store_load_contacts(mc_contact_t *arr, int *count, int max) {
    if (!arr || !count) return false;
    *count = 0;
    nvs_handle_t h;
    if (!open_ns(NVS_READONLY, &h)) return false;
    size_t sz = 0;
    esp_err_t r = nvs_get_blob(h, MC_KEY_CONTACTS, NULL, &sz);
    if (r != ESP_OK || sz == 0 || (sz % sizeof(mc_contact_t)) != 0) {
        nvs_close(h);
        return false;
    }
    int n = (int)(sz / sizeof(mc_contact_t));
    if (n > max) n = max;
    sz = (size_t)n * sizeof(mc_contact_t);
    r = nvs_get_blob(h, MC_KEY_CONTACTS, arr, &sz);
    nvs_close(h);
    if (r != ESP_OK) return false;
    *count = (int)(sz / sizeof(mc_contact_t));
    return true;
}

bool mc_store_save_contacts(const mc_contact_t *arr, int count) {
    if (!arr || count < 0) return false;
    nvs_handle_t h;
    if (!open_ns(NVS_READWRITE, &h)) return false;
    esp_err_t r;
    if (count == 0) {
        r = nvs_erase_key(h, MC_KEY_CONTACTS);
        if (r == ESP_ERR_NVS_NOT_FOUND) r = ESP_OK;
    } else {
        r = nvs_set_blob(h, MC_KEY_CONTACTS, arr, (size_t)count * sizeof(mc_contact_t));
    }
    if (r == ESP_OK) r = nvs_commit(h);
    nvs_close(h);
    return r == ESP_OK;
}

void mc_store_erase_contacts(void) {
    nvs_handle_t h;
    if (!open_ns(NVS_READWRITE, &h)) return;
    esp_err_t r = nvs_erase_key(h, MC_KEY_CONTACTS);
    if (r == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

bool mc_store_load_channels(mc_channel_t *arr) {
    if (!arr) return false;
    nvs_handle_t h;
    if (!open_ns(NVS_READONLY, &h)) return false;
    size_t sz = (size_t)MC_MAX_GROUP_CHANNELS * sizeof(mc_channel_t);
    esp_err_t r = nvs_get_blob(h, MC_KEY_CHANNELS, arr, &sz);
    nvs_close(h);
    return r == ESP_OK && sz == (size_t)MC_MAX_GROUP_CHANNELS * sizeof(mc_channel_t);
}

bool mc_store_save_channels(const mc_channel_t *arr) {
    if (!arr) return false;
    nvs_handle_t h;
    if (!open_ns(NVS_READWRITE, &h)) return false;
    esp_err_t r = nvs_set_blob(h, MC_KEY_CHANNELS, arr,
                               (size_t)MC_MAX_GROUP_CHANNELS * sizeof(mc_channel_t));
    if (r == ESP_OK) r = nvs_commit(h);
    nvs_close(h);
    return r == ESP_OK;
}

bool mc_store_save_blob(const char *key, const void *data, size_t len) {
    if (!key || !data || len == 0) return false;
    nvs_handle_t h;
    if (!open_ns(NVS_READWRITE, &h)) return false;
    esp_err_t r = nvs_set_blob(h, key, data, len);
    if (r == ESP_OK) r = nvs_commit(h);
    nvs_close(h);
    return r == ESP_OK;
}

bool mc_store_load_blob(const char *key, void *out, size_t max, size_t *out_len) {
    if (!key || !out || max == 0) return false;
    nvs_handle_t h;
    if (!open_ns(NVS_READONLY, &h)) return false;
    size_t sz = max;
    esp_err_t r = nvs_get_blob(h, key, out, &sz);
    nvs_close(h);
    if (r != ESP_OK) return false;
    if (out_len) *out_len = sz;
    return true;
}

#else
typedef int meshcore_store_stub_guard;
#endif // CONFIG_HAS_MESHCORE
