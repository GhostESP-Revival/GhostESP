#include "../../../sdk/ghostesp_plugin_api.h"

static const ghostesp_api_t *api;

static void {{APP_SYMBOL}}_start(void) {
    api->ui_set_title("{{APP_NAME}}");
    api->ui_clear();
    api->ui_print("Hello from {{APP_NAME}}.\n");
}

static void {{APP_SYMBOL}}_input(const ghostesp_input_event_t *event) {
    if (!event) return;
    if (event->type == GHOSTESP_INPUT_SELECT) {
        api->toast("Select pressed");
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

const ghostesp_app_t *ghostesp_app_init(const ghostesp_api_t *host_api) {
    if (!host_api || host_api->api_version != GHOSTESP_APP_API_VERSION) return 0;
    if (host_api->struct_size < GHOSTESP_API_STRUCT_SIZE_V1) return 0;
    api = host_api;
    return &app;
}

void app_main(void) {
}
