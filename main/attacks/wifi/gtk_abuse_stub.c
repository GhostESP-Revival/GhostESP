#include "attacks/wifi/gtk_abuse.h"

static gtk_abuse_result_t s_result;

void gtk_abuse_start(const char *ssid, const char *password) {
    (void)ssid;
    (void)password;
    s_result = (gtk_abuse_result_t){0};
}

void gtk_abuse_stop(void) {}

bool gtk_abuse_is_running(void) {
    return false;
}

void gtk_abuse_display(void) {}

const gtk_abuse_result_t *gtk_abuse_get_result(void) {
    return &s_result;
}
