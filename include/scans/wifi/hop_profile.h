#ifndef HOP_PROFILE_H
#define HOP_PROFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Hop channel profile: a single user-selectable channel plan that applies to
// every WiFi channel-hopping feature (deauth, beacon spam, station scan,
// airspace monitor, wireshark capture hop).
//
// HOP_MODE_DEFAULT preserves the historical per-feature behavior (country
// channel list, or AP-result list where a feature already uses one). The other
// modes force every hopper onto the same explicit plan.

typedef enum {
  HOP_MODE_DEFAULT = 0, // Auto: feature decides (country / AP channel list)
  HOP_MODE_ALL,         // Every channel the current WiFi country config allows
  HOP_MODE_BASIC,       // Fixed 1, 6, 11
  HOP_MODE_CUSTOM,      // User list from hop_custom_channels
  HOP_MODE_COUNT
} hop_mode_t;

#define HOP_CUSTOM_CHANNELS_MAX (WIFI_CHANNELS_MAX)

void hop_profile_get_mode(hop_mode_t *mode);
void hop_profile_set_mode(hop_mode_t mode);

// Parse and store a custom list like "1,6,11" or "1 6 11". Returns false on
// invalid input (no change is made). Rejects channels that the current chip
// cannot monitor.
bool hop_profile_set_custom_from_string(const char *text);

// The stored custom list, always normalized to "a,b,c".
const char *hop_profile_get_custom_str(void);

// Resolve the currently selected mode for transmit-capable features. DFS
// channels are omitted because active radar/CAC transmit handling is not
// implemented. On HOP_MODE_DEFAULT writes nothing into out/out_count.
void hop_profile_resolve(uint8_t *out, size_t out_cap, size_t *out_count);

// Resolve the selected profile for receive-only monitoring. This keeps all
// target- and country-allowed C5 DFS channels.
void hop_profile_resolve_monitor(uint8_t *out, size_t out_cap, size_t *out_count);

#endif // HOP_PROFILE_H
