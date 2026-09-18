#ifndef WARDRIVE_POLICY_H
#define WARDRIVE_POLICY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Independent of ESP-IDF so packet and coverage invariants can be tested on host.
#define WD_PLAN_MAX 64
typedef struct {
    uint8_t bssid[6];
    char ssid[33];
    uint8_t channel;
    int8_t rssi;
    uint8_t auth; // OPEN, WEP, WPA, WPA2, WPA3, OWE (wire protocol values)
} wd_ap_t;

bool wd_channel_valid(uint8_t channel);
bool wd_plan_contains(const uint8_t *plan, size_t count, uint8_t channel);
size_t wd_plan_helper(const uint8_t *primary, size_t primary_count,
                      const uint8_t *helper, size_t helper_count, uint8_t *out);
size_t wd_plan_remaining(const uint8_t *local, size_t local_count,
                         const uint8_t *peer, size_t peer_count, uint8_t *out);
// length must exclude FCS and never exceed the supplied capture buffer.
bool wd_parse_ap(const uint8_t *frame, size_t length, uint8_t channel,
                 int8_t rssi, wd_ap_t *out);
#endif
