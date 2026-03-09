#include "gui/paged_menu.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "paged_menu";

struct paged_menu_t {
    int page_size;
    int page_offset;
    bool has_next_page;
    bool options_valid;
    
    paged_menu_load_fn load_fn;
    paged_menu_select_fn select_fn;
    paged_menu_nav_fn prev_fn;
    paged_menu_nav_fn next_fn;
    void *user_data;
    
    char *names_buffer;
    const char **options;
    int options_count;
};

static const char *empty_options[] = {"No items found", NULL};
static const char *prev_label = "< Prev";
static const char *next_label = "Next >";

paged_menu_t *paged_menu_create(int page_size, paged_menu_load_fn load_fn, void *user_data) {
    paged_menu_t *pm = calloc(1, sizeof(paged_menu_t));
    if (!pm) {
        ESP_LOGE(TAG, "Failed to allocate paged_menu");
        return NULL;
    }
    
    pm->page_size = page_size > 0 ? page_size : 8;
    pm->page_offset = 0;
    pm->has_next_page = false;
    pm->load_fn = load_fn;
    pm->user_data = user_data;
    pm->options_valid = false;
    
    return pm;
}

void paged_menu_destroy(paged_menu_t *pm) {
    if (!pm) return;
    
    if (pm->names_buffer) {
        free(pm->names_buffer);
        pm->names_buffer = NULL;
    }
    if (pm->options) {
        free(pm->options);
        pm->options = NULL;
    }
    
    free(pm);
}

void paged_menu_set_page_size(paged_menu_t *pm, int page_size) {
    if (pm && page_size > 0) {
        pm->page_size = page_size;
        pm->options_valid = false;
    }
}

void paged_menu_set_callbacks(paged_menu_t *pm, paged_menu_select_fn select_fn, 
                               paged_menu_nav_fn prev_fn, paged_menu_nav_fn next_fn, 
                               void *user_data) {
    if (pm) {
        pm->select_fn = select_fn;
        pm->prev_fn = prev_fn;
        pm->next_fn = next_fn;
        if (user_data) {
            pm->user_data = user_data;
        }
    }
}

void paged_menu_reset(paged_menu_t *pm) {
    if (!pm) return;
    
    pm->page_offset = 0;
    pm->has_next_page = false;
    pm->options_valid = false;
    
    if (pm->names_buffer) {
        free(pm->names_buffer);
        pm->names_buffer = NULL;
    }
    if (pm->options) {
        free(pm->options);
        pm->options = NULL;
    }
    pm->options_count = 0;
}

const char **paged_menu_get_options(paged_menu_t *pm) {
    if (!pm || !pm->load_fn) {
        return empty_options;
    }
    
    if (pm->options_valid && pm->options) {
        return pm->options;
    }
    
    if (pm->names_buffer) {
        free(pm->names_buffer);
        pm->names_buffer = NULL;
    }
    if (pm->options) {
        free(pm->options);
        pm->options = NULL;
    }
    
    char (*temp_names)[PAGED_MENU_NAME_MAX] = malloc(pm->page_size * PAGED_MENU_NAME_MAX);
    if (!temp_names) {
        ESP_LOGE(TAG, "OOM for temp name buffer");
        return empty_options;
    }
    
    int count = pm->load_fn(pm->page_offset, pm->page_size, temp_names, &pm->has_next_page, pm->user_data);
    
    if (count < 0) {
        free(temp_names);
        return empty_options;
    }
    
    bool show_prev = (pm->page_offset > 0);
    bool show_next = pm->has_next_page;
    int total = (show_prev ? 1 : 0) + count + (show_next ? 1 : 0);
    
    if (total == 0) {
        free(temp_names);
        return empty_options;
    }
    
    pm->names_buffer = malloc(PAGED_MENU_NAME_MAX * (size_t)total);
    pm->options = malloc(sizeof(char *) * ((size_t)total + 1));
    
    if (!pm->names_buffer || !pm->options) {
        ESP_LOGE(TAG, "OOM for options buffers");
        free(temp_names);
        if (pm->names_buffer) { free(pm->names_buffer); pm->names_buffer = NULL; }
        if (pm->options) { free(pm->options); pm->options = NULL; }
        return empty_options;
    }
    
    int idx = 0;
    
    if (show_prev) {
        strcpy(pm->names_buffer + idx * PAGED_MENU_NAME_MAX, prev_label);
        pm->options[idx] = pm->names_buffer + idx * PAGED_MENU_NAME_MAX;
        idx++;
    }
    
    for (int i = 0; i < count; i++) {
        strcpy(pm->names_buffer + idx * PAGED_MENU_NAME_MAX, temp_names[i]);
        pm->options[idx] = pm->names_buffer + idx * PAGED_MENU_NAME_MAX;
        idx++;
    }
    
    if (show_next) {
        strcpy(pm->names_buffer + idx * PAGED_MENU_NAME_MAX, next_label);
        pm->options[idx] = pm->names_buffer + idx * PAGED_MENU_NAME_MAX;
        idx++;
    }
    
    pm->options[idx] = NULL;
    pm->options_count = idx;
    pm->options_valid = true;
    
    ESP_LOGD(TAG, "Loaded page: offset=%d items=%d prev=%d next=%d", 
             pm->page_offset, count, show_prev, show_next);
    
    free(temp_names);
    
    return pm->options;
}

bool paged_menu_handle_select(paged_menu_t *pm, const char *option) {
    if (!pm || !option) return false;
    
    if (strcmp(option, prev_label) == 0) {
        paged_menu_page_prev(pm);
        if (pm->prev_fn) {
            pm->prev_fn(pm->user_data);
        }
        return true;
    }
    
    if (strcmp(option, next_label) == 0) {
        paged_menu_page_next(pm);
        if (pm->next_fn) {
            pm->next_fn(pm->user_data);
        }
        return true;
    }
    
    if (strcmp(option, "No items found") == 0) {
        return false;
    }
    
    if (pm->select_fn) {
        pm->select_fn(option, pm->user_data);
        return true;
    }
    
    return false;
}

int paged_menu_get_page_offset(const paged_menu_t *pm) {
    return pm ? pm->page_offset : 0;
}

bool paged_menu_has_prev(const paged_menu_t *pm) {
    return pm ? (pm->page_offset > 0) : false;
}

bool paged_menu_has_next(const paged_menu_t *pm) {
    return pm ? pm->has_next_page : false;
}

void paged_menu_page_next(paged_menu_t *pm) {
    if (!pm) return;
    pm->page_offset += pm->page_size;
    pm->options_valid = false;
}

void paged_menu_page_prev(paged_menu_t *pm) {
    if (!pm) return;
    pm->page_offset -= pm->page_size;
    if (pm->page_offset < 0) pm->page_offset = 0;
    pm->options_valid = false;
}
