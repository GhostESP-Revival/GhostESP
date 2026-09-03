#include "gui/menu_config.h"
#include <string.h>

static bool valid_id(const char *id) {
    return id && id[0] && strnlen(id, MENU_CONFIG_ID_LEN) < MENU_CONFIG_ID_LEN;
}

void menu_config_reset(menu_config_t *config) {
    memset(config, 0, sizeof(*config));
    config->version = 1;
}

size_t menu_config_storage_size(const menu_config_t *config) {
    unsigned count = config->count <= MENU_CONFIG_MAX ? config->count : 0;
    return offsetof(menu_config_t, entries) + count * sizeof(config->entries[0]);
}

const menu_config_entry_t *menu_config_find(const menu_config_t *config, const char *id) {
    if (!config || !id || config->version != 1 || config->count > MENU_CONFIG_MAX) return NULL;
    for (int i = 0; i < config->count; ++i) {
        if (strncmp(config->entries[i].id, id, MENU_CONFIG_ID_LEN) == 0) return &config->entries[i];
    }
    return NULL;
}

static uint8_t safe_placement(const char *id, uint8_t placement) {
    /* Never hide the way back to customization, nor put Apps inside itself. */
    if (strcmp(id, "apps") == 0) return MENU_PLACE_MAIN;
    if (strcmp(id, "settings") == 0) return placement | MENU_PLACE_MAIN;
    return placement;
}

uint8_t menu_config_placement(const menu_config_t *config, const char *id, uint8_t fallback) {
    const menu_config_entry_t *entry = menu_config_find(config, id);
    return safe_placement(id, entry ? entry->placement : fallback);
}

bool menu_config_set(menu_config_t *config, const char *id, uint8_t placement) {
    if (!config || !valid_id(id) || placement < MENU_PLACE_MAIN || placement > MENU_PLACE_BOTH) return false;
    if (config->version != 1 || config->count > MENU_CONFIG_MAX) return false;
    menu_config_entry_t *entry = (menu_config_entry_t *)menu_config_find(config, id);
    if (!entry) {
        if (config->count == MENU_CONFIG_MAX) return false;
        entry = &config->entries[config->count++];
        memset(entry, 0, sizeof(*entry));
        strcpy(entry->id, id);
        entry->order[0] = entry->order[1] = MENU_ORDER_DEFAULT;
    }
    entry->placement = safe_placement(id, placement);
    return true;
}

bool menu_config_order(menu_config_t *config, const char *id, uint8_t fallback, uint8_t menu, uint8_t order) {
    if (menu != MENU_PLACE_MAIN && menu != MENU_PLACE_APPS) return false;
    if (!menu_config_set(config, id, menu_config_placement(config, id, fallback))) return false;
    menu_config_entry_t *entry = (menu_config_entry_t *)menu_config_find(config, id);
    entry->order[menu == MENU_PLACE_APPS] = order;
    return true;
}

void menu_config_validate(menu_config_t *config) {
    if (config->version != 1 || config->count > MENU_CONFIG_MAX) {
        menu_config_reset(config);
        return;
    }
    int count = config->count;
    config->count = 0;
    for (int i = 0; i < count; ++i) {
        menu_config_entry_t entry = config->entries[i];
        if (!valid_id(entry.id) || entry.placement < 1 || entry.placement > 3 ||
            menu_config_find(config, entry.id)) continue;
        entry.placement = safe_placement(entry.id, entry.placement);
        for (int j = 0; j < 2; ++j) {
            if (entry.order[j] >= MENU_CONFIG_MAX) entry.order[j] = MENU_ORDER_DEFAULT;
        }
        config->entries[config->count++] = entry;
    }
    memset(&config->entries[config->count], 0,
           (MENU_CONFIG_MAX - config->count) * sizeof(config->entries[0]));
}
