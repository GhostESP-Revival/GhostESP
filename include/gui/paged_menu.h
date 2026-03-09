#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAGED_MENU_NAME_MAX 128

typedef struct paged_menu_t paged_menu_t;

typedef int (*paged_menu_load_fn)(int offset, int page_size, char names[][PAGED_MENU_NAME_MAX], bool *has_more, void *user_data);

typedef void (*paged_menu_select_fn)(const char *name, void *user_data);

typedef void (*paged_menu_nav_fn)(void *user_data);

paged_menu_t *paged_menu_create(int page_size, paged_menu_load_fn load_fn, void *user_data);

void paged_menu_destroy(paged_menu_t *pm);

void paged_menu_set_page_size(paged_menu_t *pm, int page_size);

void paged_menu_set_callbacks(paged_menu_t *pm, paged_menu_select_fn select_fn, paged_menu_nav_fn prev_fn, paged_menu_nav_fn next_fn, void *user_data);

void paged_menu_reset(paged_menu_t *pm);

const char **paged_menu_get_options(paged_menu_t *pm);

bool paged_menu_handle_select(paged_menu_t *pm, const char *option);

int paged_menu_get_page_offset(const paged_menu_t *pm);

bool paged_menu_has_prev(const paged_menu_t *pm);

bool paged_menu_has_next(const paged_menu_t *pm);

void paged_menu_page_next(paged_menu_t *pm);

void paged_menu_page_prev(paged_menu_t *pm);

#ifdef __cplusplus
}
#endif
