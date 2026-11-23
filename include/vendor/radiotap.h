/* Minimal radiotap header writer for DLT_IEEE802_11_RADIO captures.
 *
 * GhostESP previously wrote a fixed 8-byte radiotap with a zero present-word,
 * which is a valid but empty header: Wireshark showed no channel, no RSSI and
 * no rate for any capture. This emits the fields the radio already hands us.
 *
 * Only fields that exist across every supported target are emitted. The
 * ESP-IDF `wifi_pkt_rx_ctrl_t` layout differs between HE and non-HE chips
 * (`sig_mode` and `ant` are absent on HE; `noise_floor` is absent on most
 * non-HE chips), so those are deliberately not read. See rx_ctrl notes in
 * esp_wifi_types_native.h and esp_wifi_he_types.h.
 *
 * `channel` is 4 bits on the non-HE layout but 8 bits on the MAC v3 layout used
 * by the C5, so 5 GHz channel numbers survive intact on C5 and friends and are
 * resolved by value. GhostESP has no 6 GHz support, and 6 GHz overlaps 2.4 GHz
 * at channels 1-14, so that case would need an explicit `band`.
 *
 * Free of ESP-IDF dependencies so it can be unit tested on the host.
 */
#ifndef VENDOR_RADIOTAP_H
#define VENDOR_RADIOTAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Present-word bit positions, from the radiotap field descriptor table. */
#define RADIOTAP_BIT_RATE 2    /* u8, 500 kbps units, align 1 */
#define RADIOTAP_BIT_CHANNEL 3 /* u16 freq + u16 flags, align 2 */
#define RADIOTAP_BIT_SIGNAL 5  /* s8 dBm, align 1 */
#define RADIOTAP_BIT_ANTENNA 11 /* u8 index, align 1 */

/* CHANNEL flags subfield. */
#define RADIOTAP_CHAN_FREQ 0x0001
#define RADIOTAP_CHAN_FLAGS 0x0002
#define RADIOTAP_CHAN_FREQ_2GHZ 0x0008
#define RADIOTAP_CHAN_FREQ_5GHZ 0x0010
#define RADIOTAP_CHAN_FREQ_6GHZ 0x0040

/* radiotap RATE is documented as 0x0f when the rate is not a legacy rate. */
#define RADIOTAP_RATE_NOT_LEGACY 0x0F

/* Comfortably above the largest layout this writer can produce (20 bytes). */
#define RADIOTAP_MAX_LEN 32

/* Band of a channel number, used to disambiguate ranges that overlap between
 * bands. 6 GHz and 2.4 GHz both start at channel 1, so a band hint is needed to
 * tell them apart; 5 GHz starts at 32 and never overlaps 2.4 GHz. */
typedef enum {
    RADIOTAP_BAND_UNKNOWN = 0,
    RADIOTAP_BAND_2GHZ = 1,
    RADIOTAP_BAND_5GHZ = 2,
    RADIOTAP_BAND_6GHZ = 3,
} radiotap_band_t;

/* Per-frame radio metadata. A field is emitted only when its `has_*` flag is
 * set, so an unknown value is left out of the capture rather than written as a
 * plausible-looking lie. */
typedef struct {
    int8_t rssi;  /* dBm */
    bool has_rssi;
    uint8_t rate; /* ESP-IDF PHY rate encoding (L-SIG rate index) */
    bool has_rate;
    uint8_t channel; /* primary channel number */
    bool has_channel;
    radiotap_band_t band; /* disambiguates overlapping channel ranges */
    uint8_t antenna;      /* antenna index */
    bool has_antenna;
} radiotap_meta_t;

/* Converts an ESP-IDF PHY rate encoding to radiotap's 500 kbps unit using the
 * 802.11 L-SIG rate table. Returns RADIOTAP_RATE_NOT_LEGACY for index 14/15,
 * which the standard marks as "not a valid rate". */
uint8_t radiotap_rate_from_index(uint8_t rate_index);

/* Maps a channel number to its centre frequency in MHz and sets *band_flag to
 * the matching RADIOTAP_CHAN_FREQ_* flag. Returns 0 when the channel cannot be
 * expressed.
 *
 * Centre frequencies follow the usual per-band formula:
 *   2.4 GHz  ch 1-13  -> 2407 + 5n,  ch 14 -> 2484
 *   5 GHz    ch 32-177 -> 5000 + 5n
 *   6 GHz    ch 1-233  -> 5950 + 5n
 *
 * When band is RADIOTAP_BAND_UNKNOWN the band is inferred from the channel
 * number, which is unambiguous for 2.4 versus 5 GHz. An explicit band is
 * required to distinguish 6 GHz from 2.4 GHz, and to reject a channel that is
 * out of range for the band actually in use. */
uint16_t radiotap_channel_to_freq(uint8_t channel, radiotap_band_t band,
                                  uint16_t *band_flag);

/* Writes a radiotap header describing `meta` into out. Returns the number of
 * bytes written, or 0 if out is NULL or cap is too small. The header is always
 * padded to a 4-byte boundary. */
size_t radiotap_build(uint8_t *out, size_t cap, const radiotap_meta_t *meta);

#ifdef __cplusplus
}
#endif

#endif /* VENDOR_RADIOTAP_H */
