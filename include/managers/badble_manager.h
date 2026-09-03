#ifndef BADBLE_MANAGER_H
#define BADBLE_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BADBLE_NAME_MAX_LEN 31

esp_err_t badble_manager_init(void);
esp_err_t badble_manager_start(void);
esp_err_t badble_manager_stop(void);

esp_err_t badble_manager_run_script(const char *filename);
esp_err_t badble_manager_run_builtin(void);
int badble_manager_list_scripts(char scripts[][64], int max_scripts);
esp_err_t badble_manager_keyboard_start(void);
esp_err_t badble_manager_keyboard_stop(void);

bool badble_manager_is_running(void);
bool badble_manager_is_connected(void);
bool badble_manager_is_script_running(void);
bool badble_manager_notifications_ready(void);

bool badble_manager_send_keypress(uint8_t modifier, uint8_t keycode);
bool badble_manager_send_text(const char *text);

esp_err_t badble_manager_set_name(const char *name);
const char *badble_manager_get_name(void);
void badble_manager_print_status(void);

/* Called by the shared BLE manager before NimBLE host teardown. */
void badble_manager_stop_profile(void);

#endif // BADBLE_MANAGER_H
