#ifndef GTK_ABUSE_H
#define GTK_ABUSE_H

#include <stdbool.h>

typedef struct {
    char ssid[33];
    bool connected;
    bool gtk_derived;
    bool gtk_validation_available;
    bool gtk_validated;
    bool frame_injected;
    bool reply_received;
    bool isolation_broken;
    int eapol_frames_captured;
    char our_ip[16];
    char gateway_ip[16];
} gtk_abuse_result_t;

void gtk_abuse_start(const char *ssid, const char *password);
void gtk_abuse_stop(void);
bool gtk_abuse_is_running(void);
void gtk_abuse_display(void);
const gtk_abuse_result_t *gtk_abuse_get_result(void);

#endif
