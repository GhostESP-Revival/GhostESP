// SPDX-License-Identifier: GPL-3.0-or-later
// Part of GhostESP (https://github.com/GhostESP). GPLv3.
//
// ISO14443-4A protocol layer for ST25R3916.
//
// Ported from Flipper Zero / Momentum-Firmware:
//   lib/nfc/protocols/iso14443_4a/iso14443_4a_poller.c
//   lib/nfc/protocols/iso14443_4a/iso14443_4a_poller_i.c
//   lib/nfc/protocols/iso14443_4a/iso14443_4a_i.c
//   lib/nfc/helpers/iso14443_4_layer.c
// Original code copyright (c) Flipper Devices and Momentum contributors (GPL-3.0).
// Source: https://github.com/Next-Flip/Momentum-Firmware
//
// Adapted to a synchronous (blocking) API over GhostESP's st25r3916_nfca_transceive
// primitive. Implements RATS/ATS exchange, ATS parsing/storage, I/R/S-block
// framing, block chaining, WTX (waiting time extension), and error recovery.
//
// Protocol references: ISO/IEC 14443-4, NXP AN10927 (DESFire).

#include "st25r3916_iso14443_4a.h"
#include "st25r3916_iso14443a.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"

static const char *TAG = "iso14443_4a";

/* ---------------- ATS (Answer To Select) definitions, ISO/IEC 14443-4 §5.2.4 --- */
#define ISO14443_4A_ATS_T0_TA1 (1U << 4)
#define ISO14443_4A_ATS_T0_TB1 (1U << 5)
#define ISO14443_4A_ATS_T0_TC1 (1U << 6)
#define ISO14443_4A_ATS_TC1_NAD (1U << 0)
#define ISO14443_4A_ATS_TC1_CID (1U << 1)

/* Frame Waiting Time default (fc) */
#define ISO14443_4A_FDT_DEFAULT_FC (1356)

/* PCB (Protocol Control Byte) bit layouts, ISO/IEC 14443-4 §5.3 */
#define ISO14443_4_BLOCK_PCB_I_        (0U << 6)
#define ISO14443_4_BLOCK_PCB_R_        (2U << 6)
#define ISO14443_4_BLOCK_PCB_TYPE_MASK (3U << 6)
#define ISO14443_4_BLOCK_PCB_S_WTX     (3U << 4)
#define ISO14443_4_BLOCK_PCB_S         (3U << 6)

#define ISO14443_4_BLOCK_PCB      (1U << 1)
#define ISO14443_4_BLOCK_PCB_MASK (0x03)

#define ISO14443_4_BLOCK_PCB_I              (0U)
#define ISO14443_4_BLOCK_PCB_I_MASK         (1U << 1)
#define ISO14443_4_BLOCK_PCB_I_ZERO_MASK    (7U << 5)
#define ISO14443_4_BLOCK_PCB_I_NAD_OFFSET   (2)
#define ISO14443_4_BLOCK_PCB_I_CID_OFFSET   (3)
#define ISO14443_4_BLOCK_PCB_I_CHAIN_OFFSET (4)
#define ISO14443_4_BLOCK_PCB_I_NAD_MASK     (1U << ISO14443_4_BLOCK_PCB_I_NAD_OFFSET)
#define ISO14443_4_BLOCK_PCB_I_CID_MASK     (1U << ISO14443_4_BLOCK_PCB_I_CID_OFFSET)
#define ISO14443_4_BLOCK_PCB_I_CHAIN_MASK   (1U << ISO14443_4_BLOCK_PCB_I_CHAIN_OFFSET)

#define ISO14443_4_BLOCK_PCB_R_MASK        (5U << 5)
#define ISO14443_4_BLOCK_PCB_R_NACK_OFFSET (4)
#define ISO14443_4_BLOCK_PCB_R_CID_OFFSET  (3)
#define ISO14443_4_BLOCK_PCB_R_CID_MASK    (1U << ISO14443_4_BLOCK_PCB_R_CID_OFFSET)
#define ISO14443_4_BLOCK_PCB_R_NACK_MASK   (1U << ISO14443_4_BLOCK_PCB_R_NACK_OFFSET)

#define ISO14443_4_BLOCK_PCB_S_MASK                (3U << 6)
#define ISO14443_4_BLOCK_PCB_S_CID_OFFSET          (3)
#define ISO14443_4_BLOCK_PCB_S_WTX_DESELECT_OFFSET (4)
#define ISO14443_4_BLOCK_PCB_S_CID_MASK            (1U << ISO14443_4_BLOCK_PCB_S_CID_OFFSET)
#define ISO14443_4_BLOCK_PCB_S_WTX_DESELECT_MASK   (3U << ISO14443_4_BLOCK_PCB_S_WTX_DESELECT_OFFSET)

#define ISO14443_4_BLOCK_CID_MASK (0x0F)

#define ISO14443_4_BLOCK_PCB_BITS_ACTIVE(pcb, mask) (((pcb) & (mask)) == (mask))
#define ISO14443_4_BLOCK_PCB_IS_I_BLOCK(pcb)                               \
    (ISO14443_4_BLOCK_PCB_BITS_ACTIVE(pcb, ISO14443_4_BLOCK_PCB_I_MASK) && \
     (((pcb) & ISO14443_4_BLOCK_PCB_I_ZERO_MASK) == 0))
#define ISO14443_4_BLOCK_PCB_IS_R_BLOCK(pcb) \
    ISO14443_4_BLOCK_PCB_BITS_ACTIVE(pcb, ISO14443_4_BLOCK_PCB_R_MASK)
#define ISO14443_4_BLOCK_PCB_IS_S_BLOCK(pcb) \
    ISO14443_4_BLOCK_PCB_BITS_ACTIVE(pcb, ISO14443_4_BLOCK_PCB_S_MASK)
#define ISO14443_4_BLOCK_PCB_IS_CHAIN_ACTIVE(pcb) \
    ISO14443_4_BLOCK_PCB_BITS_ACTIVE(pcb, ISO14443_4_BLOCK_PCB_I_CHAIN_MASK)
#define ISO14443_4_BLOCK_PCB_R_NACK_ACTIVE(pcb) \
    ISO14443_4_BLOCK_PCB_BITS_ACTIVE(pcb, ISO14443_4_BLOCK_PCB_R_NACK_MASK)

/* RATS / poller constants */
#define ISO14443_4A_CMD_READ_ATS      (0xE0)
#define ISO14443_4A_FSDI_256          (0x8U)
#define ISO14443_4A_POLLER_ATS_FWT_FC (40000)
#define ISO14443_4A_SWTX              (0xF2U)
#define ISO14443_4A_WTXM_MASK         (0x3FU)
#define ISO14443_4A_WTXM_MAX          (0x3BU)
#define ISO14443_4A_WTX_MAX_RETRIES   (3)    /* cap WTX rounds so a slow/gone card can't spin ~30s */
#define ISO14443_4A_FWT_MAX           (4096UL << 14)
#define ISO14443_4A_SEND_BLOCK_MAX_ATTEMPTS (20)
#define ISO14443_4A_BUF_SIZE          (256U)

/* Internal session state */
struct st25r3916_iso14443_4a_session {
    /* ISO14443-4 layer PCB state */
    uint8_t pcb;
    uint8_t pcb_prev;
    uint8_t cid;             /* 0xFF = CID not supported */
    uint8_t nad;             /* 0xFF = NAD not supported */
    /* Parsed ATS */
    uint8_t ats_tl;
    uint8_t ats_t0;
    uint8_t ats_ta_1;
    uint8_t ats_tb_1;
    uint8_t ats_tc_1;
    uint8_t ats_hist[ISO14443_4A_BUF_SIZE];
    size_t  ats_hist_len;
    bool activated;
};

/* ---------------- ATS helpers (ported from iso14443_4a.c) ------------------- */

static uint16_t iso14443_4a_frame_size_max(const struct st25r3916_iso14443_4a_session *s) {
    const uint8_t fsci = s->ats_t0 & 0x0F;
    if (fsci < 5) {
        return fsci * 8 + 16;
    } else if (fsci == 5) {
        return 64;
    } else if (fsci == 6) {
        return 96;
    } else if (fsci < 13) {
        return 128U << (fsci - 7);
    }
    return 0;
}

static uint32_t iso14443_4a_fwt_fc_max(const struct st25r3916_iso14443_4a_session *s) {
    uint32_t fwt_fc_max = ISO14443_4A_FDT_DEFAULT_FC;
    if (!(s->ats_tl > 1)) return fwt_fc_max;
    if (!(s->ats_t0 & ISO14443_4A_ATS_T0_TB1)) return fwt_fc_max;
    const uint8_t fwi = s->ats_tb_1 >> 4;
    if (fwi == 0x0F) return fwt_fc_max;
    return 4096UL << fwi;
}

static bool iso14443_4a_supports_cid(const struct st25r3916_iso14443_4a_session *s) {
    if (!(s->ats_t0 & ISO14443_4A_ATS_T0_TC1)) return false;
    return s->ats_tc_1 & ISO14443_4A_ATS_TC1_CID;
}

static bool iso14443_4a_supports_nad(const struct st25r3916_iso14443_4a_session *s) {
    if (!(s->ats_t0 & ISO14443_4A_ATS_T0_TC1)) return false;
    return s->ats_tc_1 & ISO14443_4A_ATS_TC1_NAD;
}

/* ATS parse (ported from iso14443_4a_ats_parse in iso14443_4a_i.c) */
static bool iso14443_4a_ats_parse(struct st25r3916_iso14443_4a_session *s,
                                  const uint8_t *buf, size_t buf_size) {
    if (buf_size == 0) return false;
    size_t idx = 0;
    const uint8_t tl = buf[idx++];
    if (tl != buf_size) return false;
    s->ats_tl = tl;
    if (tl > 1) {
        const uint8_t t0 = buf[idx++];
        const bool has_ta_1 = t0 & ISO14443_4A_ATS_T0_TA1;
        const bool has_tb_1 = t0 & ISO14443_4A_ATS_T0_TB1;
        const bool has_tc_1 = t0 & ISO14443_4A_ATS_T0_TC1;
        const size_t min_size = 2 + (has_ta_1 ? 1 : 0) + (has_tb_1 ? 1 : 0) + (has_tc_1 ? 1 : 0);
        if (buf_size < min_size) return false;
        s->ats_t0 = t0;
        if (has_ta_1) s->ats_ta_1 = buf[idx++];
        if (has_tb_1) s->ats_tb_1 = buf[idx++];
        if (has_tc_1) s->ats_tc_1 = buf[idx++];
        const size_t hist_size = buf_size - min_size;
        if (hist_size > 0) {
            if (hist_size > sizeof(s->ats_hist)) return false;
            memcpy(s->ats_hist, buf + idx, hist_size);
            s->ats_hist_len = hist_size;
        }
    }
    return true;
}

/* -------------- ISO14443-4 layer (ported from iso14443_4_layer.c) ----------- */

static void iso14443_4_layer_update_pcb(struct st25r3916_iso14443_4a_session *s, bool toggle_num) {
    s->pcb_prev = s->pcb;
    if (toggle_num) {
        s->pcb ^= (uint8_t)0x01;
    }
}

static void iso14443_4_layer_reset(struct st25r3916_iso14443_4a_session *s) {
    s->pcb_prev = 0;
    s->pcb = ISO14443_4_BLOCK_PCB_I | ISO14443_4_BLOCK_PCB;
    s->cid = 0xFF;   /* ISO14443_4_LAYER_CID_NOT_SUPPORTED */
    s->nad = 0xFF;   /* ISO14443_4_LAYER_NAD_NOT_SUPPORTED */
}

/* Encode an outgoing block: prepend PCB (and CID/NAD when present) to the payload. */
static size_t iso14443_4_layer_encode_command(struct st25r3916_iso14443_4a_session *s,
                                              const uint8_t *input, size_t input_len,
                                              uint8_t *out, size_t out_cap) {
    size_t pos = 0;
    if (pos >= out_cap) return 0;
    out[pos++] = s->pcb;
    if (s->pcb & ISO14443_4_BLOCK_PCB_I_CID_MASK && s->cid != 0xFF) {
        if (pos >= out_cap) return 0;
        out[pos++] = s->cid;
    }
    if (s->pcb & ISO14443_4_BLOCK_PCB_I_NAD_MASK && s->nad != 0xFF) {
        if (pos >= out_cap) return 0;
        out[pos++] = s->nad;
    }
    if (pos + input_len > out_cap) return 0;
    memcpy(out + pos, input, input_len);
    pos += input_len;
    iso14443_4_layer_update_pcb(s, true);
    return pos;
}

/* Decode an incoming block. On success copies INF field into out and returns
 * its length, or 0 when the response was an empty acknowledgement. Returns
 * ESP_ERR_PROTOCOL on a framing/PCB mismatch. */
static esp_err_t iso14443_4_layer_decode_response(struct st25r3916_iso14443_4a_session *s,
                                                  const uint8_t *block, size_t block_len,
                                                  uint8_t *out, size_t out_cap,
                                                  size_t *out_len) {
    *out_len = 0;
    if (block_len == 0) return ESP_ERR_INVALID_RESPONSE;
    const uint8_t resp_pcb = block[0];

    if (ISO14443_4_BLOCK_PCB_IS_R_BLOCK(s->pcb_prev)) {
        /* Response to an R-block: must be R-block, not NAK */
        if (!ISO14443_4_BLOCK_PCB_IS_R_BLOCK(resp_pcb)) return ESP_ERR_INVALID_RESPONSE;
        if (ISO14443_4_BLOCK_PCB_R_NACK_ACTIVE(resp_pcb)) return ESP_ERR_INVALID_RESPONSE;
        s->pcb &= ISO14443_4_BLOCK_PCB_MASK;
        iso14443_4_layer_update_pcb(s, true);
        return ESP_OK;
    } else if (ISO14443_4_BLOCK_PCB_IS_CHAIN_ACTIVE(s->pcb_prev)) {
        /* Chained I-block: card ACKs with an R-block */
        if (!ISO14443_4_BLOCK_PCB_IS_R_BLOCK(resp_pcb)) return ESP_ERR_INVALID_RESPONSE;
        if (ISO14443_4_BLOCK_PCB_R_NACK_ACTIVE(resp_pcb)) return ESP_ERR_INVALID_RESPONSE;
        s->pcb &= ~(ISO14443_4_BLOCK_PCB_I_CHAIN_MASK);
        return ESP_OK;
    } else if (ISO14443_4_BLOCK_PCB_IS_S_BLOCK(s->pcb_prev)) {
        /* Response to S-block must echo the same PCB */
        if (resp_pcb != s->pcb_prev) return ESP_ERR_INVALID_RESPONSE;
        if (block_len > 1) {
            size_t copy = block_len - 1;
            if (copy > out_cap) copy = out_cap;
            memcpy(out, block + 1, copy);
            *out_len = copy;
        }
        return ESP_OK;
    } else {
        /* I-block response: PCB must echo ours */
        if (resp_pcb != s->pcb_prev) return ESP_ERR_INVALID_RESPONSE;
        if (block_len > 1) {
            size_t copy = block_len - 1;
            if (copy > out_cap) copy = out_cap;
            memcpy(out, block + 1, copy);
            *out_len = copy;
        }
        return ESP_OK;
    }
}

/* Decode a response that may include WTX S-blocks (pwt_ext variant).
 * Returns ESP_OK with the final INF payload, ESP_ERR_INVALID_RESPONSE on
 * framing error, or ESP_ERR_TIMEOUT if the card keeps requesting WTX beyond
 * the retry budget. On WTX the caller must re-send the assembled output as
 * an S(WTX) response; this function fills `out` with the S-block reply. */
static esp_err_t iso14443_4_layer_decode_response_pwt_ext(
    struct st25r3916_iso14443_4a_session *s,
    const uint8_t *block, size_t block_len,
    uint8_t *out, size_t out_cap, size_t *out_len,
    bool *send_extra) {
    *out_len = 0;
    *send_extra = false;
    if (block_len == 0) return ESP_ERR_INVALID_RESPONSE;

    const uint8_t pcb_field = block[0];
    const uint8_t block_type = pcb_field & ISO14443_4_BLOCK_PCB_TYPE_MASK;

    switch (block_type) {
    case ISO14443_4_BLOCK_PCB_I_:
        if (pcb_field == s->pcb_prev) {
            if (block_len > 1) {
                size_t copy = block_len - 1;
                if (copy > out_cap) copy = out_cap;
                memcpy(out, block + 1, copy);
                *out_len = copy;
            }
            return ESP_OK;
        }
        /* PCB mismatch: request retransmission by signalling send-extra */
        *send_extra = true;
        return ESP_OK;
    case ISO14443_4_BLOCK_PCB_R_:
        /* R-block responses are handled by the caller (chaining) */
        return ESP_ERR_INVALID_RESPONSE;
    case ISO14443_4_BLOCK_PCB_S:
        if ((pcb_field & ISO14443_4_BLOCK_PCB_S_WTX) == ISO14443_4_BLOCK_PCB_S_WTX) {
            if (block_len < 2) return ESP_ERR_INVALID_RESPONSE;
            const uint8_t inf_field = block[1];
            const uint8_t wtxm = inf_field & ISO14443_4A_WTXM_MASK;
            if (wtxm > ISO14443_4A_WTXM_MAX) return ESP_ERR_INVALID_RESPONSE;
            /* Build S(WTX) response: echo PCB + WTXM */
            if (out_cap < 2) return ESP_ERR_INVALID_SIZE;
            out[0] = ISO14443_4_BLOCK_PCB_S | ISO14443_4_BLOCK_PCB_S_WTX | ISO14443_4_BLOCK_PCB;
            out[1] = wtxm;
            *out_len = 2;
            *send_extra = true;
            return ESP_OK;
        }
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_ERR_INVALID_RESPONSE;
}

/* Block type setters (iso14443_4_layer_set_*_block) */
static void iso14443_4_layer_set_i_block(struct st25r3916_iso14443_4a_session *s,
                                         bool chaining, bool cid_present) {
    const uint8_t block_pcb = s->pcb & ISO14443_4_BLOCK_PCB_MASK;
    s->pcb = ISO14443_4_BLOCK_PCB_I | (chaining << ISO14443_4_BLOCK_PCB_I_CHAIN_OFFSET) |
             (cid_present << ISO14443_4_BLOCK_PCB_I_CID_OFFSET) | block_pcb;
}

static void iso14443_4_layer_set_r_block(struct st25r3916_iso14443_4a_session *s,
                                         bool acknowledged, bool cid_present) {
    const uint8_t block_pcb = s->pcb & ISO14443_4_BLOCK_PCB_MASK;
    s->pcb = ISO14443_4_BLOCK_PCB_R_MASK |
             (!acknowledged << ISO14443_4_BLOCK_PCB_R_NACK_OFFSET) |
             (cid_present << ISO14443_4_BLOCK_PCB_R_CID_OFFSET) | block_pcb;
}

static void iso14443_4_layer_set_s_block(struct st25r3916_iso14443_4a_session *s,
                                         bool deselect, bool cid_present) {
    const uint8_t des_wtx = !deselect ? ISO14443_4_BLOCK_PCB_S_WTX_DESELECT_MASK : 0;
    s->pcb = ISO14443_4_BLOCK_PCB_S_MASK | des_wtx |
             (cid_present << ISO14443_4_BLOCK_PCB_S_CID_OFFSET) | ISO14443_4_BLOCK_PCB;
}

/* ---------------- Poller implementation (ported from iso14443_4a_poller_i.c) -- */

/* Low-level frame exchange. on ST25R this is a CRC'd NFC-A transceive. */
static esp_err_t poller_send_frame(struct st25r3916_iso14443_4a_session *s,
                                   const uint8_t *tx, uint16_t tx_len,
                                   uint8_t *rx, uint16_t rx_cap, uint16_t *rx_len,
                                   uint32_t fwt_fc) {
    /* fwt_fc is in carrier cycles; 1356 fc ≈ 100 µs. Convert to ms with a
     * generous floor; the caller already applies the FWT from ATS. */
    uint32_t timeout_ms = (fwt_fc + 1355) / 1356 + 20;
    if (timeout_ms > 5000) timeout_ms = 5000;
    return st25r3916_nfca_transceive(tx, tx_len, true, false, rx, rx_cap, rx_len,
                                     (int)timeout_ms);
}

/* Read ATS via RATS. */
esp_err_t st25r3916_iso14443_4a_read_ats(st25r3916_iso14443_4a_session_t *session) {
    if (!session) return ESP_ERR_INVALID_ARG;
    struct st25r3916_iso14443_4a_session *s = session;

    uint8_t tx[2] = {
        ISO14443_4A_CMD_READ_ATS,
        (uint8_t)(ISO14443_4A_FSDI_256 << 4),
    };
    uint8_t rx[ISO14443_4A_BUF_SIZE];
    uint16_t rx_len = 0;

    esp_err_t err = poller_send_frame(s, tx, sizeof(tx), rx, sizeof(rx), &rx_len,
                                      ISO14443_4A_POLLER_ATS_FWT_FC);
    if (err != ESP_OK) return err;
    if (!iso14443_4a_ats_parse(s, rx, rx_len)) return ESP_ERR_INVALID_RESPONSE;
    return ESP_OK;
}

/* Send one block, handling WTX S-block negotiation and response chaining. */
esp_err_t st25r3916_iso14443_4a_send_block(st25r3916_iso14443_4a_session_t *session,
                                           const uint8_t *tx, uint16_t tx_len,
                                           uint8_t *rx, uint16_t rx_cap, uint16_t *rx_len) {
    if (!session || !tx || !rx || !rx_len) return ESP_ERR_INVALID_ARG;
    struct st25r3916_iso14443_4a_session *s = session;

    uint8_t enc[ISO14443_4A_BUF_SIZE + 4];
    size_t enc_len = iso14443_4_layer_encode_command(s, tx, tx_len, enc, sizeof(enc));
    if (enc_len == 0) return ESP_ERR_INVALID_SIZE;

    uint8_t rxb[ISO14443_4A_BUF_SIZE + 4];
    uint16_t rxb_len = 0;
    esp_err_t err = poller_send_frame(s, enc, (uint16_t)enc_len, rxb, sizeof(rxb), &rxb_len,
                                      iso14443_4a_fwt_fc_max(s));
    if (err != ESP_OK) return err;

    /* Handle WTX S-blocks (card requesting more time) */
    for (uint8_t wtx_retry = 0;
         wtx_retry < ISO14443_4A_WTX_MAX_RETRIES && rxb_len >= 1 && rxb[0] == ISO14443_4A_SWTX;
         wtx_retry++) {
        const uint8_t wtxm = rxb[1] & ISO14443_4A_WTXM_MASK;
        if (wtxm > ISO14443_4A_WTXM_MAX) return ESP_ERR_INVALID_RESPONSE;
        uint8_t wtx_reply[2] = {ISO14443_4A_SWTX, wtxm};
        uint16_t wr_len = 0;
        err = poller_send_frame(s, wtx_reply, sizeof(wtx_reply), rxb, sizeof(rxb), &wr_len,
                                iso14443_4a_fwt_fc_max(s) * wtxm > ISO14443_4A_FWT_MAX
                                    ? ISO14443_4A_FWT_MAX
                                    : iso14443_4a_fwt_fc_max(s) * wtxm);
        if (err != ESP_OK) return err;
        rxb_len = wr_len;
    }
    if (rxb_len >= 1 && rxb[0] == ISO14443_4A_SWTX) return ESP_ERR_TIMEOUT;

    /* If the response is not an I-block (e.g. an R-block ACK to one of our
     * chained I-blocks, or an S-block), defer to the layer decoder. */
    if (rxb_len < 1 || !ISO14443_4_BLOCK_PCB_IS_I_BLOCK(rxb[0])) {
        size_t out_len = 0;
        err = iso14443_4_layer_decode_response(s, rxb, rxb_len, rx, rx_cap, &out_len);
        if (err != ESP_OK) return err;
        *rx_len = (uint16_t)out_len;
        return ESP_OK;
    }

    /* I-block response: validate the block number (bit 0 only, so the chaining
     * bit doesn't cause a mismatch) and accumulate INF. If the card set the
     * chaining bit (long response split across several I-blocks), acknowledge
     * each block with an R(ACK) and collect the rest. The R(ACK) is built by
     * hand so the I-block number state for the next exchange is preserved. */
    const uint8_t expected_num = s->pcb_prev & 0x01;
    size_t total_len = 0;
    unsigned chain_blocks = 0;

    for (;;) {
        if (!ISO14443_4_BLOCK_PCB_IS_I_BLOCK(rxb[0])) return ESP_ERR_INVALID_RESPONSE;
        if ((rxb[0] & 0x01) != expected_num) return ESP_ERR_INVALID_RESPONSE;

        if (rxb_len > 1) {
            size_t chunk = rxb_len - 1;  /* skip PCB */
            if (total_len + chunk > rx_cap) chunk = rx_cap - total_len;
            if (chunk > 0) {
                memcpy(rx + total_len, rxb + 1, chunk);
                total_len += chunk;
            }
        }

        if (!ISO14443_4_BLOCK_PCB_IS_CHAIN_ACTIVE(rxb[0])) break;

        /* R(ACK): same block number, no CID, no INF. Built without touching
         * s->pcb so the next I-block we send still alternates correctly. */
        uint8_t rack = (uint8_t)(ISO14443_4_BLOCK_PCB_R_MASK |
                                 (s->pcb_prev & ISO14443_4_BLOCK_PCB_MASK));
        rxb_len = 0;
        err = poller_send_frame(s, &rack, 1, rxb, sizeof(rxb), &rxb_len,
                                iso14443_4a_fwt_fc_max(s));
        if (err != ESP_OK) return err;

        /* Resolve WTX requests on the chained block. */
        for (uint8_t wtx_retry = 0;
             wtx_retry < ISO14443_4A_WTX_MAX_RETRIES && rxb_len >= 1 && rxb[0] == ISO14443_4A_SWTX;
             wtx_retry++) {
            const uint8_t wtxm = rxb[1] & ISO14443_4A_WTXM_MASK;
            if (wtxm > ISO14443_4A_WTXM_MAX) return ESP_ERR_INVALID_RESPONSE;
            uint8_t wtx_reply[2] = {ISO14443_4A_SWTX, wtxm};
            uint16_t wr_len = 0;
            err = poller_send_frame(s, wtx_reply, sizeof(wtx_reply), rxb, sizeof(rxb), &wr_len,
                                    iso14443_4a_fwt_fc_max(s) * wtxm > ISO14443_4A_FWT_MAX
                                        ? ISO14443_4A_FWT_MAX
                                        : iso14443_4a_fwt_fc_max(s) * wtxm);
            if (err != ESP_OK) return err;
            rxb_len = wr_len;
        }
        if (rxb_len >= 1 && rxb[0] == ISO14443_4A_SWTX) return ESP_ERR_TIMEOUT;
        chain_blocks++;
    }

    if (chain_blocks > 0) {
        ESP_LOGI(TAG, "assembled chained response: %u blocks, %u bytes",
                 chain_blocks + 1, (unsigned)total_len);
    }

    *rx_len = (uint16_t)total_len;
    return ESP_OK;
}

/* Send one block with extended WTX/PCB-mismatch retry handling. */
esp_err_t st25r3916_iso14443_4a_send_block_pwt_ext(st25r3916_iso14443_4a_session_t *session,
                                                   const uint8_t *tx, uint16_t tx_len,
                                                   uint8_t *rx, uint16_t rx_cap, uint16_t *rx_len) {
    if (!session || !tx || !rx || !rx_len) return ESP_ERR_INVALID_ARG;
    struct st25r3916_iso14443_4a_session *s = session;

    uint8_t attempts = ISO14443_4A_SEND_BLOCK_MAX_ATTEMPTS;
    uint8_t enc[ISO14443_4A_BUF_SIZE + 4];
    size_t enc_len = iso14443_4_layer_encode_command(s, tx, tx_len, enc, sizeof(enc));
    if (enc_len == 0) return ESP_ERR_INVALID_SIZE;

    while (true) {
        uint8_t rxb[ISO14443_4A_BUF_SIZE + 4];
        uint16_t rxb_len = 0;
        esp_err_t err = poller_send_frame(s, enc, (uint16_t)enc_len, rxb, sizeof(rxb), &rxb_len,
                                          iso14443_4a_fwt_fc_max(s));
        if (err != ESP_OK) return err;

        size_t out_len = 0;
        bool send_extra = false;
        err = iso14443_4_layer_decode_response_pwt_ext(s, rxb, rxb_len, rx, rx_cap, &out_len,
                                                       &send_extra);
        if (err != ESP_OK) return err;
        if (!send_extra) {
            *rx_len = (uint16_t)out_len;
            return ESP_OK;
        }
        if (--attempts == 0) return ESP_ERR_TIMEOUT;
        /* If the decoder produced an S(WTX) reply, transmit it as-is. */
        if (out_len > 0) {
            memcpy(enc, rx, out_len);
            enc_len = out_len;
        }
        /* Otherwise re-send the original block (PCB mismatch retransmission). */
    }
}

/* Send a chained I-block. */
esp_err_t st25r3916_iso14443_4a_send_chain_block(st25r3916_iso14443_4a_session_t *session,
                                                 const uint8_t *tx, uint16_t tx_len,
                                                 uint8_t *rx, uint16_t rx_cap, uint16_t *rx_len) {
    if (!session) return ESP_ERR_INVALID_ARG;
    struct st25r3916_iso14443_4a_session *s = session;
    iso14443_4_layer_set_i_block(s, true, false);
    return st25r3916_iso14443_4a_send_block(session, tx, tx_len, rx, rx_cap, rx_len);
}

/* Send an R-block (receive-ready / ACK or NAK). */
esp_err_t st25r3916_iso14443_4a_send_r_block(st25r3916_iso14443_4a_session_t *session,
                                             bool acknowledged,
                                             const uint8_t *tx, uint16_t tx_len,
                                             uint8_t *rx, uint16_t rx_cap, uint16_t *rx_len) {
    if (!session) return ESP_ERR_INVALID_ARG;
    struct st25r3916_iso14443_4a_session *s = session;
    const bool cid_present = tx_len != 0;
    iso14443_4_layer_set_r_block(s, acknowledged, cid_present);
    return st25r3916_iso14443_4a_send_block(session, tx, tx_len, rx, rx_cap, rx_len);
}

/* Send an S-block (WTX or DESELECT). */
esp_err_t st25r3916_iso14443_4a_send_s_block(st25r3916_iso14443_4a_session_t *session,
                                             bool deselect,
                                             const uint8_t *tx, uint16_t tx_len,
                                             uint8_t *rx, uint16_t rx_cap, uint16_t *rx_len) {
    if (!session) return ESP_ERR_INVALID_ARG;
    struct st25r3916_iso14443_4a_session *s = session;
    const bool cid_present = tx_len != 0;
    iso14443_4_layer_set_s_block(s, deselect, cid_present);
    return st25r3916_iso14443_4a_send_block(session, tx, tx_len, rx, rx_cap, rx_len);
}

/* ---------------- Session lifecycle ---------------------------------------- */

esp_err_t st25r3916_iso14443_4a_session_alloc(st25r3916_iso14443_4a_session_t **out_session) {
    if (!out_session) return ESP_ERR_INVALID_ARG;
    struct st25r3916_iso14443_4a_session *s = calloc(1, sizeof(*s));
    if (!s) return ESP_ERR_NO_MEM;
    iso14443_4_layer_reset(s);
    s->activated = false;
    *out_session = s;
    return ESP_OK;
}

void st25r3916_iso14443_4a_session_free(st25r3916_iso14443_4a_session_t *session) {
    free(session);
}

esp_err_t st25r3916_iso14443_4a_activate(st25r3916_iso14443_4a_session_t *session) {
    if (!session) return ESP_ERR_INVALID_ARG;
    struct st25r3916_iso14443_4a_session *s = session;
    iso14443_4_layer_reset(s);
    esp_err_t err = st25r3916_iso14443_4a_read_ats(session);
    if (err != ESP_OK) return err;

    /* [DIAG] Report the ATS and the FWT it implies. If tb1 is absent the card
     * falls back to the ~1356 fc default -> only a ~22 ms receive window, which
     * a crypto-delayed GPO can overrun. fwt_fc/rx_timeout mirror the value
     * poller_send_frame() actually uses for every APDU on this card. */
    {
        const uint32_t fwt_fc = iso14443_4a_fwt_fc_max(s);
        uint32_t rx_timeout = (fwt_fc + 1355) / 1356 + 20;
        if (rx_timeout > 5000) rx_timeout = 5000;
        const uint8_t fwi = (s->ats_tl > 1 && (s->ats_t0 & ISO14443_4A_ATS_T0_TB1))
                                ? (uint8_t)(s->ats_tb_1 >> 4)
                                : 0xFF;
        ESP_LOGI(TAG,
                 "[DIAG] ATS tl=%u t0=0x%02X ta1=0x%02X tb1=0x%02X tc1=0x%02X "
                 "hist=%u -> FWI=%d fwt_fc=%lu rx_timeout=%lums",
                 s->ats_tl, s->ats_t0, s->ats_ta_1, s->ats_tb_1, s->ats_tc_1,
                 (unsigned)s->ats_hist_len, (fwi == 0xFF) ? -1 : (int)fwi,
                 (unsigned long)fwt_fc, (unsigned long)rx_timeout);
    }

    /* Apply CID/NAD support discovered in the ATS */
    if (iso14443_4a_supports_cid(s)) {
        s->cid = 0; /* Use CID 0; the card will echo it in blocks */
    }
    if (!iso14443_4a_supports_nad(s)) {
        s->nad = 0xFF;
    }
    s->activated = true;
    return ESP_OK;
}

esp_err_t st25r3916_iso14443_4a_halt(st25r3916_iso14443_4a_session_t *session) {
    if (!session) return ESP_ERR_INVALID_ARG;
    struct st25r3916_iso14443_4a_session *s = session;
    uint8_t rx[8];
    uint16_t rx_len = 0;
    /* S(DESELECT), tolerate no-response */
    esp_err_t err = st25r3916_iso14443_4a_send_s_block(session, true, NULL, 0, rx, sizeof(rx), &rx_len);
    s->activated = false;
    return (err == ESP_ERR_TIMEOUT) ? ESP_OK : err;
}

/* Convenience: exchange a raw APDU (e.g. a DESFire native command wrapped in
 * 0x90 ... ISO7816 framing) over an activated session. Chains large APDUs
 * automatically using I-block chaining. */
esp_err_t st25r3916_iso14443_4a_transceive(st25r3916_iso14443_4a_session_t *session,
                                           const uint8_t *apdu, uint16_t apdu_len,
                                           uint8_t *rx, uint16_t rx_cap, uint16_t *rx_len,
                                           int timeout_ms) {
    if (!session || !apdu || !rx || !rx_len) return ESP_ERR_INVALID_ARG;
    struct st25r3916_iso14443_4a_session *s = session;
    if (!s->activated) return ESP_ERR_INVALID_STATE;

    const uint16_t fsci_max = iso14443_4a_frame_size_max(s);
    const uint16_t max_inf = (fsci_max > 4) ? (uint16_t)(fsci_max - 4) : 252;

    if (apdu_len <= max_inf) {
        // EMV APDUs are always single-I-block — route through the PWT-ext
        // helper so PCB mismatches and S(WTX) replies are recovered
        // transparently (matches Momentum's send_block_pwt_ext). The chained
        // path below stays on send_block because the response there is an
        // R-block, which pwt_ext does not decode.
        return st25r3916_iso14443_4a_send_block_pwt_ext(
            session, apdu, apdu_len, rx, rx_cap, rx_len);
    }

    /* APDU exceeds the card's frame size: split into chained I-blocks. */
    size_t offset = 0;
    while (offset < apdu_len) {
        const size_t chunk = (apdu_len - offset > max_inf) ? max_inf : (apdu_len - offset);
        const bool last = (offset + chunk >= apdu_len);
        esp_err_t err;
        if (!last) {
            err = st25r3916_iso14443_4a_send_chain_block(session, apdu + offset, (uint16_t)chunk,
                                                         rx, rx_cap, rx_len);
        } else {
            err = st25r3916_iso14443_4a_send_block(session, apdu + offset, (uint16_t)chunk,
                                                   rx, rx_cap, rx_len);
        }
        if (err != ESP_OK) return err;
        offset += chunk;
    }
    return ESP_OK;
}

/* Getters for ATS metadata */
uint16_t st25r3916_iso14443_4a_get_frame_size_max(const st25r3916_iso14443_4a_session_t *session) {
    if (!session) return 0;
    return iso14443_4a_frame_size_max(session);
}

const uint8_t *st25r3916_iso14443_4a_get_historical_bytes(
    const st25r3916_iso14443_4a_session_t *session, size_t *out_len) {
    if (!session || !out_len) return NULL;
    *out_len = session->ats_hist_len;
    return session->ats_hist_len ? session->ats_hist : NULL;
}
