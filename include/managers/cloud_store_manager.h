#ifndef CLOUD_STORE_MANAGER_H
#define CLOUD_STORE_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CLOUD_STORE_MAX_ITEMS 8
#define CLOUD_STORE_MAX_CATALOG_ITEMS_PER_TYPE 32
#define CLOUD_STORE_MAX_CATALOG_ITEMS (CLOUD_STORE_MAX_CATALOG_ITEMS_PER_TYPE * 3)
#define CLOUD_STORE_ID_MAX 24
#define CLOUD_STORE_NAME_MAX 40
#define CLOUD_STORE_VERSION_MAX 12
#define CLOUD_STORE_AUTHOR_MAX 32
#define CLOUD_STORE_CATEGORY_MAX 20
#define CLOUD_STORE_DESC_MAX 80
#define CLOUD_STORE_URL_MAX 160
#define CLOUD_STORE_ERROR_MAX 96

typedef enum {
    CLOUD_STORE_TYPE_APP = 0,
    CLOUD_STORE_TYPE_ASSET_PACK = 1,
    CLOUD_STORE_TYPE_SCRIPT = 2,
} cloud_store_item_type_t;

typedef enum {
    CLOUD_STORE_STATE_IDLE = 0,
    CLOUD_STORE_STATE_FETCHING,
    CLOUD_STORE_STATE_READY,
    CLOUD_STORE_STATE_DOWNLOADING,
    CLOUD_STORE_STATE_INSTALLING,
    CLOUD_STORE_STATE_DONE,
    CLOUD_STORE_STATE_FAILED,
} cloud_store_state_t;

typedef struct {
    cloud_store_item_type_t type;
    char id[CLOUD_STORE_ID_MAX];
    char name[CLOUD_STORE_NAME_MAX];
    char version[CLOUD_STORE_VERSION_MAX];
    char author[CLOUD_STORE_AUTHOR_MAX];
    char category[CLOUD_STORE_CATEGORY_MAX];
    char description[CLOUD_STORE_DESC_MAX];
    char download_url[CLOUD_STORE_URL_MAX];
    size_t size; // manifest-declared byte size, 0 if the catalog didn't publish one
    uint32_t script_permissions;
    uint32_t script_memory_limit;
} cloud_store_item_t;

typedef struct {
    cloud_store_state_t state;
    cloud_store_item_type_t active_type;
    char active_name[CLOUD_STORE_NAME_MAX];
    size_t bytes_done;
    size_t bytes_total;
    char error[CLOUD_STORE_ERROR_MAX];
} cloud_store_status_t;

bool cloud_store_is_available(void);
bool cloud_store_apps_available(void);
esp_err_t cloud_store_manager_init(void);
void cloud_store_manager_cleanup(void);
esp_err_t cloud_store_refresh_async(void);
esp_err_t cloud_store_install_async(cloud_store_item_type_t type, const char *id);
void cloud_store_cancel_install(void);
cloud_store_status_t cloud_store_get_status(void);
int cloud_store_get_count(cloud_store_item_type_t type);
bool cloud_store_get_item(cloud_store_item_type_t type, int index, cloud_store_item_t *out);
bool cloud_store_find_item(cloud_store_item_type_t type, const char *id, cloud_store_item_t *out);

#ifdef __cplusplus
}
#endif

#endif
