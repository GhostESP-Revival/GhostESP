#include "../../../sdk/ghostesp_plugin_api.h"
#include "../../../sdk/ghostesp_helpers.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

void *memmove(void *dst, const void *src, size_t len) __attribute__((weak));
void *memmove(void *dst, const void *src, size_t len) {
    unsigned char *out = dst;
    const unsigned char *in = src;
    if (out < in) {
        while (len--) *out++ = *in++;
    } else {
        out += len;
        in += len;
        while (len--) *--out = *--in;
    }
    return dst;
}

#define HACKCHAT_CHANNEL 1
#define HACKCHAT_MAX_HISTORY 16
#define HACKCHAT_ANNOUNCE_MS 3000

typedef enum {
    HACKCHAT_PAGE_PEERS,
    HACKCHAT_PAGE_COMPOSE,
    HACKCHAT_PAGE_INBOX,
} hackchat_page_t;

typedef struct {
    bool outgoing;
    char sender_name[GHOSTESP_ESPNOW_NAME_MAX];
    char text[GHOSTESP_ESPNOW_MESSAGE_MAX];
} hackchat_history_t;

static const ghostesp_api_t *api;
static ghostesp_layout_t layout;
static ghostesp_touch_state_t touch_state;
static ghostesp_ui_obj_t screen;
static ghostesp_ui_obj_t page_scroller;
static ghostesp_ui_obj_t inbox_scroller;
static ghostesp_ui_obj_t touch_bar;
static ghostesp_ui_obj_t discovery_button;
static ghostesp_ui_obj_t peer_container;
static ghostesp_ui_obj_t peer_rows[HACKCHAT_MAX_HISTORY];
static ghostesp_ui_obj_t peer_empty;
static ghostesp_ui_obj_t inbox_button;
static ghostesp_ui_obj_t exit_button;
static ghostesp_ui_obj_t inbox_summary_label;
static ghostesp_ui_obj_t inbox_messages_container;
static ghostesp_ui_obj_t message_rows[HACKCHAT_MAX_HISTORY];
static ghostesp_ui_obj_t inbox_empty;
static ghostesp_ui_obj_t buttons[HACKCHAT_MAX_HISTORY + 4];
static int button_count;
static int selected_button;
static hackchat_page_t page;
static ghostesp_espnow_peer_t compose_peer;
static hackchat_history_t history[HACKCHAT_MAX_HISTORY];
static int history_count;
static int unread_count;
static int peer_count;
static uint32_t last_announce_ms;
static uint32_t discovery_feedback_until_ms;
static uint32_t ignore_input_until_ms;

static const char s_app_id[] = "hackchat";
static const char s_app_name[] = "HackChat";

#define HACKCHAT_REQUIRED_API_SIZE \
    (offsetof(ghostesp_api_t, espnow_receive) + sizeof(((ghostesp_api_t *)0)->espnow_receive))

static void show_peers(void);
static void show_inbox(void *user);
static void inbox_back(void *user);
static void open_compose(void *user);
static void start_or_refresh(void *user);
static void scroll_inbox(int direction);
static void refresh_peer_rows(void);
static void refresh_inbox_rows(void);

static void inbox_scroll_up(void *user) {
    (void)user;
    scroll_inbox(-1);
}

static void inbox_scroll_down(void *user) {
    (void)user;
    scroll_inbox(1);
}

static void destroy_page_ui(void) {
    if (touch_bar && api->ui_obj_delete) api->ui_obj_delete(touch_bar);
    touch_bar = NULL;
    if (screen && api->ui_obj_delete) api->ui_obj_delete(screen);
    screen = NULL;
    page_scroller = NULL;
    inbox_scroller = NULL;
    discovery_button = NULL;
    peer_container = NULL;
    peer_empty = NULL;
    inbox_button = NULL;
    exit_button = NULL;
    inbox_summary_label = NULL;
    inbox_messages_container = NULL;
    inbox_empty = NULL;
    memset(peer_rows, 0, sizeof(peer_rows));
    memset(message_rows, 0, sizeof(message_rows));
    button_count = 0;
    selected_button = 0;
    memset(buttons, 0, sizeof(buttons));
}

static ghostesp_ui_obj_t add_card(ghostesp_ui_obj_t parent) {
    if (!parent || !api->ui_card_create) return NULL;
    ghostesp_ui_obj_t card = api->ui_card_create(parent);
    if (card && api->ui_obj_set_scrollable) api->ui_obj_set_scrollable(card, false);
    if (card && api->ui_obj_set_border_width) api->ui_obj_set_border_width(card, 0);
    if (card && api->ui_obj_set_pad) api->ui_obj_set_pad(card, layout.compact ? 5 : 8, layout.compact ? 5 : 8,
                                                           layout.compact ? 4 : 6, layout.compact ? 4 : 6);
    return card;
}

static ghostesp_ui_obj_t add_label(ghostesp_ui_obj_t parent, const char *text, ghostesp_font_size_t size, uint32_t color) {
    if (!parent || !api->ui_label_create) return NULL;
    ghostesp_ui_obj_t label = api->ui_label_create(parent, text);
    if (!label) return NULL;
    if (api->ui_obj_set_font) api->ui_obj_set_font(label, size);
    if (api->ui_obj_set_text_color) api->ui_obj_set_text_color(label, color);
    return label;
}

static void set_selected_button(int index) {
    if (button_count <= 0) return;
    if (index < 0) index = 0;
    if (index >= button_count) index = button_count - 1;
    if (api->ui_button_set_selected && buttons[selected_button])
        api->ui_button_set_selected(buttons[selected_button], false);
    selected_button = index;
    if (api->ui_button_set_selected && buttons[selected_button])
        api->ui_button_set_selected(buttons[selected_button], true);
    if (page == HACKCHAT_PAGE_PEERS && selected_button == 0 && page_scroller && api->ui_obj_scroll_by)
        api->ui_obj_scroll_by(page_scroller, 0, 32767, false);
}

static void add_button(const char *text, ghostesp_ui_button_cb_t on_click, void *user) {
    if (!page_scroller || !api->ui_button_create || button_count >= (int)(sizeof(buttons) / sizeof(buttons[0]))) return;
    ghostesp_ui_obj_t button = api->ui_button_create(page_scroller, text, on_click, user);
    if (button && api->ui_obj_set_scrollable) api->ui_obj_set_scrollable(button, false);
    buttons[button_count++] = button;
}

static ghostesp_ui_obj_t add_button_to(ghostesp_ui_obj_t parent, const char *text,
                                        ghostesp_ui_button_cb_t on_click, void *user) {
    if (!parent || !api->ui_button_create) return NULL;
    ghostesp_ui_obj_t button = api->ui_button_create(parent, text, on_click, user);
    if (button && api->ui_obj_set_scrollable) api->ui_obj_set_scrollable(button, false);
    return button;
}

static void exit_app(void *user) {
    (void)user;
    GH_VOID(api, app_exit);
}

static void move_selection(int delta) {
    if (page == HACKCHAT_PAGE_INBOX) {
        scroll_inbox(delta);
    } else {
        set_selected_button(selected_button + delta);
    }
}

static void scroll_inbox(int direction) {
    if (inbox_scroller && api->ui_obj_scroll_by)
        api->ui_obj_scroll_by(inbox_scroller, 0, -direction * 48, false);
}

static void activate_selection(void) {
    if (page == HACKCHAT_PAGE_INBOX) {
        inbox_back(NULL);
        return;
    }
    if (page != HACKCHAT_PAGE_PEERS) return;
    if (selected_button == 0) {
        start_or_refresh(NULL);
        return;
    }

    const int current_peers = api->espnow_peer_count ? api->espnow_peer_count() : 0;
    if (selected_button >= 1 && selected_button <= current_peers) {
        open_compose((void *)(intptr_t)(selected_button - 1));
        return;
    }
    if (selected_button == current_peers + 1) {
        show_inbox(NULL);
        return;
    }
    exit_app(NULL);
}

static void append_history(bool outgoing, const char *sender_name, const char *text) {
    if (!text || !text[0]) return;
    if (history_count == HACKCHAT_MAX_HISTORY) {
        memmove(history, history + 1, sizeof(history) - sizeof(history[0]));
        history_count--;
    }
    hackchat_history_t *entry = &history[history_count++];
    memset(entry, 0, sizeof(*entry));
    entry->outgoing = outgoing;
    snprintf(entry->sender_name, sizeof(entry->sender_name), "%s", sender_name ? sender_name : "GhostESP");
    snprintf(entry->text, sizeof(entry->text), "%s", text);
}

static void compose_submitted(const char *text, void *user) {
    (void)user;
    page = HACKCHAT_PAGE_PEERS;
    if (text && text[0] && api->espnow_send && api->espnow_send(compose_peer.mac, text)) {
        const char *name = api->espnow_name ? api->espnow_name() : "You";
        append_history(true, name, text);
        char status[64];
        snprintf(status, sizeof(status), "Sent to %s", compose_peer.name);
        GH_VOID(api, toast, status);
    } else if (text && text[0]) {
        GH_VOID(api, toast, api->espnow_last_error ? api->espnow_last_error() : "Message was not sent");
    }
    show_peers();
}

static void open_compose(void *user) {
    const int index = (int)(intptr_t)user;
    if (!api->espnow_get_peer || !api->espnow_get_peer(index, &compose_peer)) {
        GH_VOID(api, toast, "That peer is no longer nearby");
        show_peers();
        return;
    }
    page = HACKCHAT_PAGE_COMPOSE;
    destroy_page_ui();
    if (!api->ui_input_dialog) {
        GH_VOID(api, toast, "This board has no text input dialog");
        page = HACKCHAT_PAGE_PEERS;
        show_peers();
        return;
    }
    char title[48];
    snprintf(title, sizeof(title), layout.compact ? "%s" : "Message %s", compose_peer.name);
    api->ui_input_dialog(title, "", compose_submitted, NULL);
}

static void start_or_refresh(void *user) {
    (void)user;
    const bool was_active = api->espnow_is_active && api->espnow_is_active();
    if (was_active && api->espnow_stop) api->espnow_stop();
    if (!api->espnow_start || !api->espnow_start(HACKCHAT_CHANNEL)) {
        GH_VOID(api, toast, api->espnow_last_error ? api->espnow_last_error() : "Unable to start HackChat");
        show_peers();
        return;
    }
    if (api->espnow_announce) api->espnow_announce();
    uint32_t now = api->system_uptime_ms ? api->system_uptime_ms() : 1;
    discovery_feedback_until_ms = now + 1800;
    if (!screen) {
        show_peers();
    } else if (discovery_button && api->ui_button_set_text) {
        api->ui_button_set_text(discovery_button, "Discovering...");
    }
    GH_VOID(api, toast, was_active ? "Discovery restarted" : "HackChat is live on channel 1");
}

static void show_inbox(void *user) {
    (void)user;
    page = HACKCHAT_PAGE_INBOX;
    unread_count = 0;
    destroy_page_ui();
    if (!api->ui_screen_create || !api->ui_card_create) return;
    screen = api->ui_screen_create("HackChat Inbox");
    if (!screen) return;

    const uint32_t muted = api->ui_theme_get_text_muted ? api->ui_theme_get_text_muted() : 0x9AA0A6;
    const uint32_t accent = api->ui_theme_get_accent ? api->ui_theme_get_accent() : 0x00B8D4;
    inbox_scroller = add_card(screen);
    page_scroller = inbox_scroller;
    if (!inbox_scroller) return;
    int32_t content_h = api->ui_screen_get_content_height ? api->ui_screen_get_content_height() : 200;
    if (content_h < 80) content_h = 200;
    if (api->ui_obj_set_height) api->ui_obj_set_height(inbox_scroller, content_h - (layout.compact ? 4 : 12));
    if (api->ui_obj_set_scrollable) api->ui_obj_set_scrollable(inbox_scroller, true);
    if (api->ui_obj_set_scrollbar) api->ui_obj_set_scrollbar(inbox_scroller, true);

    ghostesp_ui_obj_t hero = add_card(inbox_scroller);
    add_label(hero, "INBOX", GHOSTESP_FONT_TITLE, accent);
    char summary[64];
    snprintf(summary, sizeof(summary), "%d message%s  |  ESP-NOW channel 1", history_count,
             history_count == 1 ? "" : "s");
    inbox_summary_label = add_label(hero, summary, GHOSTESP_FONT_CAPTION, muted);
    add_label(hero, "Swipe or use up/down to read", GHOSTESP_FONT_CAPTION, muted);
    inbox_messages_container = add_card(inbox_scroller);
    if (inbox_messages_container && api->ui_obj_set_pad)
        api->ui_obj_set_pad(inbox_messages_container, 0, 0, 0, 0);
    refresh_inbox_rows();
    touch_bar = gh_touch_bar_full(api,
        inbox_back, NULL,
        inbox_scroll_up, NULL,
        inbox_scroll_down, NULL,
        true);
}

static void inbox_back(void *user) {
    (void)user;
    page = HACKCHAT_PAGE_PEERS;
    show_peers();
}

static void refresh_inbox_rows(void) {
    if (!inbox_messages_container) return;
    for (int i = 0; i < HACKCHAT_MAX_HISTORY; ++i) {
        if (message_rows[i] && api->ui_obj_delete) api->ui_obj_delete(message_rows[i]);
        message_rows[i] = NULL;
    }
    if (inbox_empty && api->ui_obj_delete) api->ui_obj_delete(inbox_empty);
    inbox_empty = NULL;

    const uint32_t text = api->ui_theme_get_text ? api->ui_theme_get_text() : 0xE6E6E6;
    const uint32_t muted = api->ui_theme_get_text_muted ? api->ui_theme_get_text_muted() : 0x9AA0A6;
    const uint32_t accent = api->ui_theme_get_accent ? api->ui_theme_get_accent() : 0x00B8D4;
    char summary[64];
    snprintf(summary, sizeof(summary), "%d message%s  |  ESP-NOW channel 1", history_count,
             history_count == 1 ? "" : "s");
    if (inbox_summary_label && api->ui_label_set_text)
        api->ui_label_set_text(inbox_summary_label, summary);

    if (history_count == 0) {
        inbox_empty = add_card(inbox_messages_container);
        add_label(inbox_empty, "No messages yet", GHOSTESP_FONT_BODY, text);
        add_label(inbox_empty, "Messages from nearby HackChat devices will appear here.", GHOSTESP_FONT_CAPTION, muted);
        return;
    }
    for (int i = 0; i < history_count; ++i) {
        ghostesp_ui_obj_t message = add_card(inbox_messages_container);
        message_rows[i] = message;
        size_t text_len = strlen(history[i].text);
        int lines = (int)((text_len + (layout.compact ? 23 : 31)) /
                          (layout.compact ? 24 : 32));
        if (lines < 1) lines = 1;
        if (api->ui_obj_set_height)
            api->ui_obj_set_height(message, (layout.compact ? 34 : 42) +
                                             lines * (layout.compact ? 16 : 20));
        char sender[48];
        snprintf(sender, sizeof(sender), history[i].outgoing ? "YOU  >  %s" : "%s  >  YOU", history[i].sender_name);
        add_label(message, sender, GHOSTESP_FONT_CAPTION, history[i].outgoing ? muted : accent);
        add_label(message, history[i].text, GHOSTESP_FONT_BODY, text);
    }
}

static void show_peers(void) {
    if (page == HACKCHAT_PAGE_COMPOSE) return;
    page = HACKCHAT_PAGE_PEERS;
    destroy_page_ui();
    if (!api->ui_screen_create) return;

    const bool active = api->espnow_is_active && api->espnow_is_active();
    const char *name = active && api->espnow_name ? api->espnow_name() : "Offline";
    screen = api->ui_screen_create(s_app_name);
    if (!screen) return;
    const uint32_t text = api->ui_theme_get_text ? api->ui_theme_get_text() : 0xE6E6E6;
    const uint32_t muted = api->ui_theme_get_text_muted ? api->ui_theme_get_text_muted() : 0x9AA0A6;
    const uint32_t accent = api->ui_theme_get_accent ? api->ui_theme_get_accent() : 0x00B8D4;

    page_scroller = add_card(screen);
    if (!page_scroller) return;
    int32_t content_h = api->ui_screen_get_content_height ? api->ui_screen_get_content_height() : 200;
    if (content_h < 80) content_h = 200;
    if (api->ui_obj_set_height) api->ui_obj_set_height(page_scroller, content_h - (layout.compact ? 4 : 12));
    if (api->ui_obj_set_scrollable) api->ui_obj_set_scrollable(page_scroller, true);
    if (api->ui_obj_set_scrollbar) api->ui_obj_set_scrollbar(page_scroller, true);

    ghostesp_ui_obj_t hero = add_card(page_scroller);
    add_label(hero, "HACKCHAT", GHOSTESP_FONT_TITLE, accent);
    char identity[64];
    snprintf(identity, sizeof(identity), "You are %s", name);
    add_label(hero, identity, GHOSTESP_FONT_BODY, text);
    add_label(hero, active ? "ESP-NOW live  |  Channel 1  |  Plaintext" : "Starting nearby chat on channel 1", GHOSTESP_FONT_CAPTION, muted);
    uint32_t now = api->system_uptime_ms ? api->system_uptime_ms() : 0;
    if (discovery_feedback_until_ms && now < discovery_feedback_until_ms)
        add_label(hero, "Discovery broadcast sent", GHOSTESP_FONT_CAPTION, accent);

    const bool discovering = discovery_feedback_until_ms && now < discovery_feedback_until_ms;
    const char *state_label = discovering ? "Discovering..." :
                              (active ? "Refresh discovery" : "Start nearby chat");
    add_button(state_label, start_or_refresh, NULL);
    discovery_button = buttons[button_count - 1];

    add_label(page_scroller, "NEARBY GHOSTS", GHOSTESP_FONT_CAPTION, muted);
    peer_container = add_card(page_scroller);
    if (peer_container && api->ui_obj_set_pad) api->ui_obj_set_pad(peer_container, 0, 0, 0, 0);
    char inbox_label[48];
    snprintf(inbox_label, sizeof(inbox_label), unread_count ? "Inbox  |  %d new" : "Open inbox", unread_count);
    add_button(inbox_label, show_inbox, NULL);
    inbox_button = buttons[button_count - 1];
    add_button("Exit HackChat", exit_app, NULL);
    exit_button = buttons[button_count - 1];
    refresh_peer_rows();
    set_selected_button(0);
}

static void refresh_peer_rows(void) {
    if (!peer_container) return;
    for (int i = 0; i < HACKCHAT_MAX_HISTORY; ++i) {
        if (peer_rows[i] && api->ui_obj_delete) api->ui_obj_delete(peer_rows[i]);
        peer_rows[i] = NULL;
    }
    if (peer_empty && api->ui_obj_delete) api->ui_obj_delete(peer_empty);
    peer_empty = NULL;

    peer_count = api->espnow_peer_count ? api->espnow_peer_count() : 0;
    if (peer_count > HACKCHAT_MAX_HISTORY) peer_count = HACKCHAT_MAX_HISTORY;
    const uint32_t text = api->ui_theme_get_text ? api->ui_theme_get_text() : 0xE6E6E6;
    const uint32_t muted = api->ui_theme_get_text_muted ? api->ui_theme_get_text_muted() : 0x9AA0A6;
    for (int i = 0; i < peer_count; ++i) {
        ghostesp_espnow_peer_t peer;
        if (!api->espnow_get_peer || !api->espnow_get_peer(i, &peer)) continue;
        char label[64];
        snprintf(label, sizeof(label), "Message %s", peer.name);
        peer_rows[i] = add_button_to(peer_container, label, open_compose, (void *)(intptr_t)i);
    }
    if (peer_count == 0) {
        peer_empty = add_card(peer_container);
        add_label(peer_empty, "Listening for nearby HackChat devices", GHOSTESP_FONT_BODY, text);
        add_label(peer_empty, "Keep this screen open while discovery runs.", GHOSTESP_FONT_CAPTION, muted);
    }

    button_count = 0;
    buttons[button_count++] = discovery_button;
    for (int i = 0; i < peer_count; ++i) {
        if (peer_rows[i]) buttons[button_count++] = peer_rows[i];
    }
    buttons[button_count++] = inbox_button;
    buttons[button_count++] = exit_button;
    if (selected_button >= button_count) selected_button = button_count - 1;
    if (selected_button < 0) selected_button = 0;
    if (api->ui_button_set_selected && buttons[selected_button])
        api->ui_button_set_selected(buttons[selected_button], true);
}

static void drain_messages(void) {
    if (!api->espnow_receive) return;
    ghostesp_espnow_message_t message;
    bool received = false;
    while (api->espnow_receive(&message)) {
        append_history(false, message.sender_name, message.text);
        unread_count++;
        received = true;
        char notice[64];
        snprintf(notice, sizeof(notice), "New message from %s", message.sender_name);
        GH_VOID(api, toast, notice);
    }
    if (received) {
        if (page == HACKCHAT_PAGE_INBOX) {
            unread_count = 0;
            refresh_inbox_rows();
        } else if (page == HACKCHAT_PAGE_PEERS && inbox_button && api->ui_button_set_text) {
            char inbox_label[48];
            snprintf(inbox_label, sizeof(inbox_label), "Inbox  |  %d new", unread_count);
            api->ui_button_set_text(inbox_button, inbox_label);
        }
    }
}

static void hackchat_start(void) {
    memset(history, 0, sizeof(history));
    history_count = 0;
    unread_count = 0;
    peer_count = 0;
    last_announce_ms = 0;
    discovery_feedback_until_ms = 0;
    ignore_input_until_ms = 0;
    page = HACKCHAT_PAGE_PEERS;
    gh_layout_init(api, &layout);
    gh_touch_reset(&touch_state);
    start_or_refresh(NULL);
}

static void hackchat_stop(void) {
    if (api->espnow_stop) api->espnow_stop();
    destroy_page_ui();
}

static void hackchat_resume(void) {
    uint32_t now = api->system_uptime_ms ? api->system_uptime_ms() : 0;
    ignore_input_until_ms = now + 500;
    if (page == HACKCHAT_PAGE_COMPOSE) {
        page = HACKCHAT_PAGE_PEERS;
        show_peers();
    }
}

static void hackchat_tick(uint32_t elapsed_ms) {
    (void)elapsed_ms;
    drain_messages();
    if (!api->espnow_is_active || !api->espnow_is_active()) return;

    uint32_t now = api->system_uptime_ms ? api->system_uptime_ms() : last_announce_ms + HACKCHAT_ANNOUNCE_MS;
    if (last_announce_ms == 0 || now - last_announce_ms >= HACKCHAT_ANNOUNCE_MS) {
        if (api->espnow_announce) api->espnow_announce();
        last_announce_ms = now;
    }
    if (page == HACKCHAT_PAGE_PEERS && discovery_feedback_until_ms && now >= discovery_feedback_until_ms) {
        discovery_feedback_until_ms = 0;
        if (discovery_button && api->ui_button_set_text)
            api->ui_button_set_text(discovery_button, "Refresh discovery");
    }
    int current_peers = api->espnow_peer_count ? api->espnow_peer_count() : 0;
    if (page == HACKCHAT_PAGE_PEERS && current_peers != peer_count) refresh_peer_rows();
}

static bool is_back_key(int key) {
    return key == 27 || key == 8 || key == 29 || key == 127 || key == '`' || key == 'q' || key == 'Q';
}

static void hackchat_input(const ghostesp_input_event_t *event) {
    if (!event) return;
    uint32_t now = api->system_uptime_ms ? api->system_uptime_ms() : 0;
    if (ignore_input_until_ms && now < ignore_input_until_ms) return;
    ignore_input_until_ms = 0;
    if (event->type != GHOSTESP_INPUT_TOUCH && !event->pressed) return;

    if (event->type == GHOSTESP_INPUT_TOUCH) {
        ghostesp_input_type_t swipe = gh_touch_update(&touch_state, event);
        if (swipe == GHOSTESP_INPUT_RIGHT) {
            if (page == HACKCHAT_PAGE_INBOX) inbox_back(NULL);
            else if (page == HACKCHAT_PAGE_PEERS) exit_app(NULL);
        }
        else if (swipe == GHOSTESP_INPUT_UP) move_selection(-1);
        else if (swipe == GHOSTESP_INPUT_DOWN) move_selection(1);
        return;
    }
    if (event->type == GHOSTESP_INPUT_BACK) {
        if (page == HACKCHAT_PAGE_INBOX) inbox_back(NULL);
        else if (page == HACKCHAT_PAGE_PEERS) exit_app(NULL);
        return;
    }
    if (event->type == GHOSTESP_INPUT_LEFT || event->type == GHOSTESP_INPUT_UP) {
        move_selection(-1);
        return;
    }
    if (event->type == GHOSTESP_INPUT_RIGHT || event->type == GHOSTESP_INPUT_DOWN) {
        move_selection(1);
        return;
    }
    if (event->type == GHOSTESP_INPUT_SELECT) {
        activate_selection();
        return;
    }
    if (event->type != GHOSTESP_INPUT_KEY) return;
    if (is_back_key(event->value)) {
        if (page == HACKCHAT_PAGE_INBOX) inbox_back(NULL);
        else if (page == HACKCHAT_PAGE_PEERS) exit_app(NULL);
    } else if (event->value == 10 || event->value == 13 || event->value == ' ') {
        activate_selection();
    } else if (event->value == 'k' || event->value == 'K' || event->value == 'w' || event->value == 'W') {
        move_selection(-1);
    } else if (event->value == 'j' || event->value == 'J' || event->value == 's' || event->value == 'S') {
        move_selection(1);
    } else if (event->value == 'r' || event->value == 'R') {
        start_or_refresh(NULL);
    } else if (event->value == 'i' || event->value == 'I') {
        show_inbox(NULL);
    }
}

static const ghostesp_app_t app = {
    .api_version = GHOSTESP_APP_API_VERSION,
    .struct_size = sizeof(ghostesp_app_t),
    .id = s_app_id,
    .name = s_app_name,
    .on_start = hackchat_start,
    .on_stop = hackchat_stop,
    .on_input = hackchat_input,
    .on_tick = hackchat_tick,
    .on_pause = NULL,
    .on_resume = hackchat_resume,
};

GHOSTESP_APP_INIT_WITH_API(app, api, "hackchat", HACKCHAT_REQUIRED_API_SIZE)
