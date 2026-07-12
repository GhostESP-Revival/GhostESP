#include "sdk/ghostesp_plugin_api.h"
#include "sdk/ghostesp_helpers.h"

static const ghostesp_api_t *api;
static ghostesp_theme_t theme;
static ghostesp_layout_t layout;

static void {{APP_SYMBOL}}_start(void) {
    gh_theme_init(api, &theme);
    gh_layout_init(api, &layout);

    api->ui_set_title("{{APP_NAME}}");
    api->ui_clear();
    api->ui_print("Hello from {{APP_NAME}}.\n");
}

static void {{APP_SYMBOL}}_input(const ghostesp_input_event_t *event) {
    if (!event) return;
    if (event->type == GHOSTESP_INPUT_SELECT) {
        api->toast("Select pressed");
    }
    if (event->type == GHOSTESP_INPUT_BACK) {
        GH_VOID(api, app_exit);
    }
}

static const ghostesp_app_t app = GHOSTESP_APP_DEFINE(
    "{{APP_ID}}",
    "{{APP_NAME}}",
    {{APP_SYMBOL}}_start,
    0,
    {{APP_SYMBOL}}_input,
    0
);

GHOSTESP_APP_INIT_WITH_API(app, api, "{{APP_ID}}", GHOSTESP_API_STRUCT_SIZE_V1)
