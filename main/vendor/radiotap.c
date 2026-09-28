#include "vendor/radiotap.h"

#include <string.h>

/* 802.11 L-SIG rate table, indexed by the 4-bit Rate field carried in the
 * PHY header, expressed in 500 kbps units as radiotap expects. Entries 14 and
 * 15 are reserved and reported as "not legacy". */
static const uint8_t k_l_sig_rate_500kbps[16] = {
    1,   /* 0: 0.5 Mbps */
    2,   /* 1: 1 Mbps */
    4,   /* 2: 2 Mbps */
    6,   /* 3: 3 Mbps */
    8,   /* 4: 4 Mbps */
    11,  /* 5: 5.5 Mbps */
    12,  /* 6: 6 Mbps */
    18,  /* 7: 9 Mbps */
    24,  /* 8: 12 Mbps */
    36,  /* 9: 18 Mbps */
    48,  /* 10: 24 Mbps */
    72,  /* 11: 36 Mbps */
    96,  /* 12: 48 Mbps */
    108, /* 13: 54 Mbps */
    RADIOTAP_RATE_NOT_LEGACY,
    RADIOTAP_RATE_NOT_LEGACY,
};

uint8_t radiotap_rate_from_index(uint8_t rate_index) {
    if (rate_index > 13) {
        return RADIOTAP_RATE_NOT_LEGACY;
    }
    return k_l_sig_rate_500kbps[rate_index];
}

/* Channel numbering per band, and the centre frequency formula for each.
 * 2.4 GHz runs 1-13 at 2407+5n with channel 14 the lone odd one out at 2484.
 * 5 GHz runs 32-177 at 5000+5n. 6 GHz runs 1-233 at 5950+5n. */
static bool band_2ghz_freq(uint8_t ch, uint16_t *out) {
    if (ch == 14) {
        *out = 2484;
        return true;
    }
    if (ch >= 1 && ch <= 13) {
        *out = (uint16_t)(2407 + 5 * ch);
        return true;
    }
    return false;
}

static bool band_5ghz_freq(uint8_t ch, uint16_t *out) {
    if (ch < 32 || ch > 177) {
        return false;
    }
    *out = (uint16_t)(5000 + 5 * ch);
    return true;
}

static bool band_6ghz_freq(uint8_t ch, uint16_t *out) {
    if (ch < 1 || ch > 233) {
        return false;
    }
    *out = (uint16_t)(5950 + 5 * ch);
    return true;
}

uint16_t radiotap_channel_to_freq(uint8_t channel, radiotap_band_t band,
                                  uint16_t *band_flag) {
    if (band_flag != NULL) {
        *band_flag = 0;
    }

    radiotap_band_t resolved = band;
    if (resolved == RADIOTAP_BAND_UNKNOWN) {
        /* 2.4 GHz and 5 GHz never share a channel number (1-14 versus 32-177),
         * so the band can be inferred. 6 GHz does overlap 2.4 GHz at 1-14 and
         * cannot be inferred; an explicit band is required for it. */
        if (channel >= 32 && channel <= 177) {
            resolved = RADIOTAP_BAND_5GHZ;
        } else if (channel >= 1 && channel <= 14) {
            resolved = RADIOTAP_BAND_2GHZ;
        } else {
            return 0;
        }
    }

    uint16_t freq = 0;
    bool ok = false;
    switch (resolved) {
        case RADIOTAP_BAND_2GHZ:
            ok = band_2ghz_freq(channel, &freq);
            break;
        case RADIOTAP_BAND_5GHZ:
            ok = band_5ghz_freq(channel, &freq);
            break;
        case RADIOTAP_BAND_6GHZ:
            ok = band_6ghz_freq(channel, &freq);
            break;
        default:
            return 0;
    }
    if (!ok) {
        return 0;
    }

    if (band_flag != NULL) {
        switch (resolved) {
            case RADIOTAP_BAND_2GHZ:
                *band_flag = RADIOTAP_CHAN_FREQ_2GHZ;
                break;
            case RADIOTAP_BAND_5GHZ:
                *band_flag = RADIOTAP_CHAN_FREQ_5GHZ;
                break;
            case RADIOTAP_BAND_6GHZ:
                *band_flag = RADIOTAP_CHAN_FREQ_6GHZ;
                break;
            default:
                break;
        }
    }
    return freq;
}

static void put_u16le(uint8_t *out, uint16_t v) {
    out[0] = (uint8_t)(v & 0xFF);
    out[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void put_u32le(uint8_t *out, uint32_t v) {
    out[0] = (uint8_t)(v & 0xFF);
    out[1] = (uint8_t)((v >> 8) & 0xFF);
    out[2] = (uint8_t)((v >> 16) & 0xFF);
    out[3] = (uint8_t)((v >> 24) & 0xFF);
}

static size_t align_up(size_t offset, size_t alignment) {
    const size_t rem = offset % alignment;
    return rem == 0 ? offset : offset + (alignment - rem);
}

size_t radiotap_build(uint8_t *out, size_t cap, const radiotap_meta_t *meta) {
    if (out == NULL || meta == NULL) {
        return 0;
    }

    uint16_t band_flag = 0;
    uint16_t freq = 0;
    if (meta->has_channel) {
        freq = radiotap_channel_to_freq(meta->channel, meta->band, &band_flag);
    }
    /* An unrepresentable channel is left out entirely rather than written as
     * a wrong frequency. */
    const bool emit_channel = (freq != 0);

    uint32_t present = 0;
    if (meta->has_rate) {
        present |= 1u << RADIOTAP_BIT_RATE;
    }
    if (emit_channel) {
        present |= 1u << RADIOTAP_BIT_CHANNEL;
    }
    if (meta->has_rssi) {
        present |= 1u << RADIOTAP_BIT_SIGNAL;
    }
    if (meta->has_antenna) {
        present |= 1u << RADIOTAP_BIT_ANTENNA;
    }

    /* Walk the layout once to find the total length, padding each field to its
     * natural alignment and the whole header to 4 bytes. */
    size_t offset = 8; /* Version(1) + Pad(1) + Length(2) + Present(4) */
    if (meta->has_rate) {
        offset += 1;
    }
    if (emit_channel) {
        offset = align_up(offset, 2);
        offset += 4;
    }
    if (meta->has_rssi) {
        offset += 1;
    }
    if (meta->has_antenna) {
        offset += 1;
    }
    offset = align_up(offset, 4);

    if (offset > cap || offset > RADIOTAP_MAX_LEN) {
        return 0;
    }
    const size_t total = offset;

    /* Zero the whole header so the alignment and tail padding are well defined
     * and no stale caller bytes leak into the capture. */
    memset(out, 0, offset);
    out[0] = 0; /* it_version */
    out[1] = 0; /* it_pad */
    put_u16le(out + 2, (uint16_t)offset);
    put_u32le(out + 4, present);

    offset = 8;
    if (meta->has_rate) {
        out[offset++] = radiotap_rate_from_index(meta->rate);
    }
    if (emit_channel) {
        offset = align_up(offset, 2);
        put_u16le(out + offset, freq);
        put_u16le(out + offset + 2,
                  (uint16_t)(RADIOTAP_CHAN_FREQ | RADIOTAP_CHAN_FLAGS | band_flag));
        offset += 4;
    }
    if (meta->has_rssi) {
        out[offset++] = (uint8_t)meta->rssi;
    }
    if (meta->has_antenna) {
        out[offset++] = meta->antenna;
    }

    /* Return the padded length recorded in it_len, not the raw field end, so
     * the caller's buffer accounting stays in step with the header. */
    return total;
}
