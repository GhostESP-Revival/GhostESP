#include "../../../sdk/ghostesp_plugin_api.h"
#include "../../../sdk/ghostesp_helpers.h"
#include "../../../../components/M5GFX/src/lgfx/utility/lgfx_qrcode.h"

#include <stdio.h>
#include <string.h>

/* Native SD app shared objects cannot import the full libc string ABI. */
char *strcpy(char *dst, const char *src) __attribute__((weak));
char *strcpy(char *dst, const char *src) {
    char *out = dst;
    while ((*dst++ = *src++) != '\0') {}
    return out;
}

/* The encoder is part of M5GFX and is compiled into this app's shared object. */
#define QR_TEXT_MAX 120
#define QR_MAX_VERSION 10
#define QR_BUFFER_SIZE 512
#define QR_QUIET_ZONE 4

typedef enum {
    QR_PAGE_MENU,
    QR_PAGE_PREVIEW,
} qr_page_t;

typedef enum {
    QR_MENU_EDIT,
    QR_MENU_PREVIEW,
    QR_MENU_RESET,
    QR_MENU_EXIT,
} qr_menu_item_t;

static const ghostesp_api_t *api;
static ghostesp_theme_t theme;
static ghostesp_layout_t layout;
static ghostesp_touch_state_t touch_state;

static qr_page_t current_page;
static ghostesp_options_t menu;
static ghostesp_ui_obj_t preview_screen;
static ghostesp_ui_obj_t qr_canvas;
static ghostesp_ui_obj_t touch_bar;

static QRCode qr_code;
static uint8_t qr_buffer[QR_BUFFER_SIZE];
static char payload[QR_TEXT_MAX + 1] = "https://ghostesp.net";
static int canvas_side;
static int module_pixels;
static bool qr_ready;

static char s_app_id[] = "qr_generator";
static char s_app_name[] = "QR Generator";

#define QR_GENERATOR_REQUIRED_API_SIZE \
    (offsetof(ghostesp_api_t, ui_has_touchscreen) + sizeof(((ghostesp_api_t *)0)->ui_has_touchscreen))

static void show_menu(void);
static void show_preview(void);

static int minimum(int a, int b) {
    return a < b ? a : b;
}

static bool encode_payload(void) {
    qr_ready = false;
    module_pixels = 0;
    memset(&qr_code, 0, sizeof(qr_code));
    if (!payload[0]) return false;

    for (int version = 1; version <= QR_MAX_VERSION; version++) {
        memset(qr_buffer, 0, sizeof(qr_buffer));
        if (lgfx_qrcode_initText(&qr_code, qr_buffer, version, ECC_MEDIUM, payload) != 0)
            continue;

        int modules_with_quiet_zone = qr_code.size + QR_QUIET_ZONE * 2;
        module_pixels = canvas_side / modules_with_quiet_zone;
        if (module_pixels >= 2) {
            qr_ready = true;
            return true;
        }
        break;
    }

    return false;
}

static void draw_qr(void) {
    if (!qr_canvas || !api->ui_canvas_fill || !api->ui_canvas_draw_rect) return;

    /* A white quiet zone is required for reliable scanning on every theme. */
    api->ui_canvas_fill(qr_canvas, 0xFFFFFF);
    if (!qr_ready) return;

    int modules_with_quiet_zone = qr_code.size + QR_QUIET_ZONE * 2;
    int drawn_side = modules_with_quiet_zone * module_pixels;
    int offset = (canvas_side - drawn_side) / 2 + QR_QUIET_ZONE * module_pixels;

    for (int y = 0; y < qr_code.size; y++) {
        for (int x = 0; x < qr_code.size; x++) {
            if (lgfx_qrcode_getModule(&qr_code, x, y)) {
                api->ui_canvas_draw_rect(qr_canvas,
                                         offset + x * module_pixels,
                                         offset + y * module_pixels,
                                         module_pixels, module_pixels, 0x000000);
            }
        }
    }
}

static void destroy_menu(void) {
    if (menu && api->ui_options_destroy) api->ui_options_destroy(menu);
    menu = NULL;
}

static void destroy_preview(void) {
    preview_screen = NULL;
    qr_canvas = NULL;
    touch_bar = NULL;
}

static void exit_app(void *user) {
    (void)user;
    GH_VOID(api, app_exit);
}

static void reset_payload(void *user) {
    (void)user;
    snprintf(payload, sizeof(payload), "%s", "https://ghostesp.net");
    GH_VOID(api, toast, "Reset to GhostESP URL");
    show_menu();
}

static void input_submitted(const char *text, void *user) {
    (void)user;
    if (!text) return;

    size_t length = strlen(text);
    if (length > QR_TEXT_MAX) {
        length = QR_TEXT_MAX;
        GH_VOID(api, toast, "Text shortened to 120 bytes");
    }
    memcpy(payload, text, length);
    payload[length] = '\0';
    show_preview();
}

static void edit_payload(void *user) {
    (void)user;
    if (api->ui_input_dialog)
        api->ui_input_dialog("QR text or URL", payload, input_submitted, NULL);
}

static void preview_clicked(void *user) {
    (void)user;
    show_preview();
}

static void show_menu(void) {
    destroy_preview();
    destroy_menu();
    current_page = QR_PAGE_MENU;
    if (!api->ui_options_create) return;

    menu = api->ui_options_create(s_app_name);
    if (!menu) return;
    api->ui_options_add_item(menu, "Edit text or URL", edit_payload, NULL);
    api->ui_options_add_item(menu, "Show QR code", preview_clicked, NULL);
    api->ui_options_add_item(menu, "Reset to GhostESP URL", reset_payload, NULL);
    api->ui_options_add_back(menu, exit_app, NULL);
    GH_VOID(api, ui_options_set_selected, menu, QR_MENU_EDIT);
}

static void preview_back(void *user) {
    (void)user;
    show_menu();
}

static void show_preview(void) {
    destroy_menu();
    destroy_preview();
    current_page = QR_PAGE_PREVIEW;

    int margin = layout.compact ? 2 : 4;
    canvas_side = minimum(layout.content_w - margin * 2, layout.content_h - margin * 2);
    if (canvas_side < 1) canvas_side = 1;

    preview_screen = GH_CALL(api, ui_screen_create, "QR Preview");
    if (!preview_screen) return;
    GH_VOID(api, ui_obj_set_bg_color, preview_screen, theme.bg);
    GH_VOID(api, ui_obj_set_pad, preview_screen, margin, margin, margin, margin);
    GH_VOID(api, ui_obj_set_flex_flow, preview_screen, GHOSTESP_FLEX_FLOW_COLUMN);
    GH_VOID(api, ui_obj_set_flex_align, preview_screen, GHOSTESP_FLEX_ALIGN_CENTER,
            GHOSTESP_FLEX_ALIGN_CENTER, GHOSTESP_FLEX_ALIGN_CENTER);
    GH_VOID(api, ui_obj_set_scrollable, preview_screen, false);
    GH_VOID(api, ui_obj_set_size, preview_screen, layout.content_w, layout.content_h);

    qr_canvas = GH_CALL(api, ui_canvas_create, preview_screen, canvas_side, canvas_side);
    if (qr_canvas) GH_VOID(api, ui_obj_set_size, qr_canvas, canvas_side, canvas_side);

    encode_payload();
    draw_qr();
    if (!qr_ready) {
        GH_VOID(api, toast, "Payload is too dense for this screen");
    }
    touch_bar = gh_touch_bar(api, true, preview_back, NULL);
}

static void activate_menu_selection(void) {
    int selected = api->ui_options_get_selected ? api->ui_options_get_selected(menu) : -1;
    switch (selected) {
        case QR_MENU_EDIT: edit_payload(NULL); break;
        case QR_MENU_PREVIEW: show_preview(); break;
        case QR_MENU_RESET: reset_payload(NULL); break;
        case QR_MENU_EXIT: exit_app(NULL); break;
        default: break;
    }
}

static void qr_generator_start(void) {
    GH_VOID(api, log, "QR Generator started");
    gh_theme_init(api, &theme);
    gh_layout_init(api, &layout);
    gh_touch_reset(&touch_state);
    show_menu();
}

static void qr_generator_stop(void) {
    destroy_menu();
    destroy_preview();
    GH_VOID(api, log, "QR Generator stopped");
}

static void qr_generator_input(const ghostesp_input_event_t *event) {
    if (!event) return;

    if (event->type == GHOSTESP_INPUT_TOUCH) {
        ghostesp_input_type_t swipe = gh_touch_update(&touch_state, event);
        if (swipe == GHOSTESP_INPUT_RIGHT) {
            if (current_page == QR_PAGE_PREVIEW) show_menu();
            else exit_app(NULL);
        } else if (current_page == QR_PAGE_MENU && swipe == GHOSTESP_INPUT_UP) {
            GH_VOID(api, ui_options_move_selection, menu, -1);
        } else if (current_page == QR_PAGE_MENU && swipe == GHOSTESP_INPUT_DOWN) {
            GH_VOID(api, ui_options_move_selection, menu, 1);
        }
        return;
    }

    if (current_page == QR_PAGE_MENU && menu) {
        if (event->type == GHOSTESP_INPUT_LEFT || event->type == GHOSTESP_INPUT_UP) {
            GH_VOID(api, ui_options_move_selection, menu, -1);
            return;
        }
        if (event->type == GHOSTESP_INPUT_RIGHT || event->type == GHOSTESP_INPUT_DOWN) {
            GH_VOID(api, ui_options_move_selection, menu, 1);
            return;
        }
        if (event->type == GHOSTESP_INPUT_SELECT) {
            activate_menu_selection();
            return;
        }
    }

    if (event->type == GHOSTESP_INPUT_BACK) {
        if (current_page == QR_PAGE_PREVIEW) show_menu();
        else exit_app(NULL);
        return;
    }

    if (event->type == GHOSTESP_INPUT_KEY) {
        int key = event->value;
        if (key == 27 || key == 8 || key == 127 || key == 'q' || key == 'Q') {
            if (current_page == QR_PAGE_PREVIEW) show_menu();
            else exit_app(NULL);
        } else if (current_page == QR_PAGE_MENU && (key == 10 || key == 13 || key == ' ')) {
            activate_menu_selection();
        } else if (current_page == QR_PAGE_MENU && (key == 'e' || key == 'E' || key == 'i' || key == 'I')) {
            edit_payload(NULL);
        } else if (current_page == QR_PAGE_MENU && (key == 'p' || key == 'P')) {
            show_preview();
        } else if (current_page == QR_PAGE_MENU && (key == 'r' || key == 'R')) {
            reset_payload(NULL);
        }
    }
}

static const ghostesp_app_t app = GHOSTESP_APP_DEFINE(
    s_app_id, s_app_name,
    qr_generator_start, qr_generator_stop, qr_generator_input, NULL
);

GHOSTESP_APP_INIT_WITH_API(app, api, "qr_generator", QR_GENERATOR_REQUIRED_API_SIZE)
