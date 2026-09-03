#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Stable string IDs survive different board feature sets and catalog order.
 * Byte-only versioned storage has no compiler-dependent padding. */
#define MENU_CONFIG_MAX 64
#define MENU_CONFIG_ID_LEN 40
#define MENU_PLACE_MAIN 1
#define MENU_PLACE_APPS 2
#define MENU_PLACE_BOTH 3
#define MENU_ORDER_DEFAULT 255

typedef struct {
    char id[MENU_CONFIG_ID_LEN];
    uint8_t placement;
    uint8_t order[2];
} menu_config_entry_t;

typedef struct {
    uint8_t version;
    uint8_t count;
    menu_config_entry_t entries[MENU_CONFIG_MAX];
} menu_config_t;

void menu_config_reset(menu_config_t *config);
size_t menu_config_storage_size(const menu_config_t *config);
void menu_config_validate(menu_config_t *config);
const menu_config_entry_t *menu_config_find(const menu_config_t *config, const char *id);
uint8_t menu_config_placement(const menu_config_t *config, const char *id, uint8_t fallback);
bool menu_config_set(menu_config_t *config, const char *id, uint8_t placement);
bool menu_config_order(menu_config_t *config, const char *id, uint8_t fallback, uint8_t menu, uint8_t order);
