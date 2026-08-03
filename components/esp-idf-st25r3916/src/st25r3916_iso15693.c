// SPDX-License-Identifier: GPL-3.0-or-later
// Part of GhostESP (https://github.com/GhostESP). GPLv3.
//
// ISO15693 / NFC-V reader layer for ST25R3916.
//
// The ST25R3916 has no native ISO15693 framing engine (unlike NFC-A/B/F): in
// "subcarrier stream" mode (see st25r3916_set_mode_nfcv()) the chip only
// handles subcarrier modulation/demodulation, so VCD->VICC pulse-position
// coding (TX) and VICC->VCD Manchester decoding (RX) are done in software.
//
// The VCD/VICC framing and CRC-16 are the public ISO/IEC 15693-2 / ISO/IEC
// 13239 standards, but nfcv_vcd_encode() and nfcv_vicc_decode() below are
// directly adapted (renamed/restructured, same algorithm and stream-byte bit
// patterns) from Flipper Zero Momentum-Firmware's
// iso15693_3_poller_encode_frame() / iso15693_3_poller_decode_frame() in
// targets/f7/furi_hal/furi_hal_nfc_iso15693.c (GPL-3.0-or-later, same
// ST25R3916 silicon and stream-mode configuration):
//   https://github.com/Next-Flip/Momentum-Firmware/blob/dev/targets/f7/furi_hal/furi_hal_nfc_iso15693.c
//   Copyright (c) Flipper Devices Inc., Momentum-Firmware contributors.
//
// PicoPass command framing on top of this PHY is in picopass.c, based on
// Momentum-Firmware's picopass application (GPL-3.0):
//   https://github.com/Next-Flip/Momentum-Firmware
// Original picopass app by Eric Betts (bettse):
//   https://github.com/bettse/picopass

#include "st25r3916_iso15693.h"
#include "st25r3916.h"
#include "st25r3916_reg.h"

#include <string.h>
#include "esp_log.h"
#include "esp_rom_sys.h"

static const char *TAG = "st25r3916_nfcv";

#define NFCV_DEFAULT_TIMEOUT_MS 20

/* ISO15693 command codes */
#define NFCV_CMD_INVENTORY  0x04

/* ISO15693 inventory flags: 1 slot, high data rate, no protocol extension */
#define NFCV_FLAGS_INVENTORY 0x06

/* --- VCD->VICC "1 out of 4" pulse-position coding (ISO/IEC 15693-2, high
 * data rate). One data byte -> one SOF/EOF-free run of 4 stream bytes, one
 * bit set per byte marking the pulse position for that 2-bit pair. --- */
#define NFCV_VCD_SOF 0x21
#define NFCV_VCD_EOF 0x04
static const uint8_t nfcv_vcd_1of4[4] = {0x02, 0x08, 0x20, 0x80};

#define NFCV_TX_STREAM_MAX 96 /* SOF + up to 23 data/CRC bytes * 4 + EOF */

static uint16_t nfcv_vcd_encode(const uint8_t *data, uint16_t len, uint8_t *out, uint16_t out_cap) {
    uint16_t need = (uint16_t)(2 + (uint32_t)len * 4);
    if (out_cap < need) return 0;

    uint16_t n = 0;
    out[n++] = NFCV_VCD_SOF;
    for (uint16_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        out[n++] = nfcv_vcd_1of4[b & 0x3];
        out[n++] = nfcv_vcd_1of4[(b >> 2) & 0x3];
        out[n++] = nfcv_vcd_1of4[(b >> 4) & 0x3];
        out[n++] = nfcv_vcd_1of4[(b >> 6) & 0x3];
    }
    out[n++] = NFCV_VCD_EOF;
    return n;
}

/* --- VICC->VCD Manchester decode of the subcarrier-stream response. --- */
#define NFCV_VICC_SOF_MASK    0x1F
#define NFCV_VICC_SOF_PATTERN 0x17
#define NFCV_VICC_EOF_PATTERN 0x1D
#define NFCV_VICC_PATTERN_MASK 0x03
#define NFCV_VICC_PATTERN_0   0x01 /* Manchester "0" */
#define NFCV_VICC_PATTERN_1   0x02 /* Manchester "1" */

#define NFCV_RX_STREAM_MAX 96

/* Decodes a raw subcarrier-stream capture (buf, buf_bits valid bits within
 * the first buf_bytes bytes of buf) into payload bytes. Returns ESP_OK with
 * *out_bits set on a well-formed SOF..EOF frame (which may carry zero
 * payload bits, e.g. a PicoPass ACTALL acknowledgement). */
static esp_err_t nfcv_vicc_decode(const uint8_t *buf, uint16_t buf_bytes, uint16_t buf_bits,
                                  uint8_t *out, uint16_t out_cap, uint16_t *out_bits) {
    if (buf_bits < 8 || buf_bytes < 2) return ESP_ERR_INVALID_RESPONSE;
    if ((buf[0] & NFCV_VICC_SOF_MASK) != NFCV_VICC_SOF_PATTERN) return ESP_ERR_INVALID_RESPONSE;

    memset(out, 0, out_cap);
    uint16_t bit_pos = 0;
    bool got_eof = false;

    for (uint32_t i = 5; (i + 8) <= buf_bits; i += 2) {
        uint16_t byte_idx = (uint16_t)(i / 8);
        uint8_t bit_off = (uint8_t)(i % 8);
        if ((uint32_t)byte_idx + 1 >= buf_bytes) break;

        uint8_t sample = (uint8_t)((buf[byte_idx] >> bit_off) | (buf[byte_idx + 1] << (8 - bit_off)));

        if (sample == NFCV_VICC_EOF_PATTERN) {
            got_eof = true;
            break;
        }

        uint8_t pattern = sample & NFCV_VICC_PATTERN_MASK;
        if (pattern == NFCV_VICC_PATTERN_0) {
            bit_pos++;
        } else if (pattern == NFCV_VICC_PATTERN_1) {
            out[bit_pos / 8] = (uint8_t)(out[bit_pos / 8] | (1U << (bit_pos % 8)));
            bit_pos++;
        } else {
            break; /* invalid symbol / collision */
        }
        if ((uint32_t)(bit_pos / 8) >= out_cap) break;
    }

    if (!got_eof) return ESP_ERR_INVALID_RESPONSE;
    if (out_bits) *out_bits = bit_pos;
    return ESP_OK;
}

/* ISO/IEC 13239 CRC-16 (poly 0x8408 reflected), as used by ISO15693 (preset
 * 0xFFFF, final complement) and PicoPass (preset 0xE012, no complement -
 * computed by the caller in picopass.c). */
static uint16_t nfcv_crc16(uint16_t preset, const uint8_t *data, uint16_t len) {
    uint16_t crc = preset;
    for (uint16_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        for (int bit = 0; bit < 8; bit++) {
            bool do_xor = ((crc ^ b) & 0x01) != 0;
            crc >>= 1;
            if (do_xor) crc ^= 0x8408;
            b >>= 1;
        }
    }
    return crc;
}

static esp_err_t nfcv_wait_rxe(int timeout_ms) {
    uint8_t m = 0;
    int iters = timeout_ms * 10; // poll ~every 100 us
    if (iters < 1) iters = 1;
    for (int i = 0; i < iters; i++) {
        st25r3916_irq_update(&m, NULL, NULL);
        if (m & ST25R3916_IRQ_MAIN_RXE) return ESP_OK;
        if (m & ST25R3916_IRQ_MAIN_COL) return ESP_ERR_INVALID_RESPONSE;
        esp_rom_delay_us(100);
    }
    return ESP_ERR_TIMEOUT;
}

static void nfcv_cleanup_after_error(void) {
    st25r3916_fifo_clear();
    st25r3916_irq_clear();
}

esp_err_t st25r3916_nfcv_transceive(const uint8_t *tx, uint16_t tx_len, bool with_crc,
                                    uint8_t *rx, uint16_t rx_cap,
                                    uint16_t *rx_len, int timeout_ms) {
    if (rx_len) *rx_len = 0;

    /* Build the payload (+ optional standard ISO15693 CRC), then VCD-encode
     * it into the raw stream-mode pulse pattern the chip transmits. */
    uint8_t payload[32];
    if (tx_len + 2u > sizeof(payload)) return ESP_ERR_INVALID_SIZE;
    memcpy(payload, tx, tx_len);
    uint16_t payload_len = tx_len;
    if (with_crc) {
        uint16_t crc = (uint16_t)~nfcv_crc16(0xFFFF, tx, tx_len);
        payload[payload_len++] = (uint8_t)(crc & 0xFF);
        payload[payload_len++] = (uint8_t)(crc >> 8);
    }

    uint8_t stream[NFCV_TX_STREAM_MAX];
    uint16_t stream_len = nfcv_vcd_encode(payload, payload_len, stream, sizeof(stream));
    if (stream_len == 0) return ESP_ERR_INVALID_SIZE;

    st25r3916_cmd(ST25R3916_CMD_STOP);
    st25r3916_irq_clear();
    st25r3916_fifo_clear();

    esp_err_t e = st25r3916_fifo_load(stream, stream_len);
    if (e != ESP_OK) return e;
    st25r3916_set_num_tx_bytes(stream_len, 0);
    st25r3916_cmd(ST25R3916_CMD_TRANSMIT_WITHOUT_CRC);

    esp_err_t txe_err = st25r3916_irq_wait_main(ST25R3916_IRQ_MAIN_TXE, 20);
    if (txe_err != ESP_OK) {
        ESP_LOGW(TAG, "transceive: TXE timeout");
    }
    esp_err_t err = nfcv_wait_rxe(timeout_ms);
    if (err != ESP_OK) {
        uint8_t eerr = 0;
        st25r3916_irq_update(NULL, NULL, &eerr);
        ESP_LOGW(TAG, "transceive: RX timeout, error_reg=0x%02X", eerr);
        nfcv_cleanup_after_error();
        return err;
    }

    uint16_t fifo_n = 0;
    uint8_t last_bits = 0;
    st25r3916_fifo_status(&fifo_n, &last_bits);
    if (fifo_n == 0) {
        /* No subcarrier captured at all - treat as no response. */
        st25r3916_irq_clear();
        return ESP_ERR_TIMEOUT;
    }
    if (fifo_n > NFCV_RX_STREAM_MAX) fifo_n = NFCV_RX_STREAM_MAX;

    uint8_t scratch[NFCV_RX_STREAM_MAX] = {0};
    err = st25r3916_fifo_read(scratch, fifo_n);
    if (err != ESP_OK) {
        nfcv_cleanup_after_error();
        return err;
    }
    st25r3916_irq_clear();

    uint16_t stream_bits = last_bits ? (uint16_t)((fifo_n - 1) * 8 + last_bits) : (uint16_t)(fifo_n * 8);

    uint8_t decoded[NFCV_RX_STREAM_MAX];
    uint16_t decoded_bits = 0;
    err = nfcv_vicc_decode(scratch, fifo_n, stream_bits, decoded, sizeof(decoded), &decoded_bits);
    if (err != ESP_OK) {
        return err;
    }

    uint16_t payload_bytes = decoded_bits / 8;
    if (with_crc && payload_bytes >= 2) payload_bytes -= 2;

    if (rx && rx_cap) {
        uint16_t copy = (payload_bytes > rx_cap) ? rx_cap : payload_bytes;
        memcpy(rx, decoded, copy);
        if (rx_len) *rx_len = copy;
    } else if (rx_len) {
        *rx_len = payload_bytes;
    }
    return ESP_OK;
}

esp_err_t st25r3916_nfcv_inventory(uint8_t *uid, int timeout_ms) {
    if (!uid) return ESP_ERR_INVALID_ARG;

    /* ISO15693 INVENTORY with 1-out-of-4 slot coding:
     * Flags(1) + INVENTORY(1) + MaskLen(1), CRC appended by transceive().
     * MaskLen = 0 means all tags respond. */
    uint8_t tx[3] = {
        NFCV_FLAGS_INVENTORY,
        NFCV_CMD_INVENTORY,
        0x00, // mask length = 0 (all tags)
    };

    /* Response: DSFID(1) + UID(8) + CRC(2) = 11 bytes */
    uint8_t rx[11] = {0};
    uint16_t rx_len = 0;

    esp_err_t err = st25r3916_nfcv_transceive(tx, 3, true, rx, sizeof(rx), &rx_len,
                                               timeout_ms > 0 ? timeout_ms : NFCV_DEFAULT_TIMEOUT_MS);
    if (err != ESP_OK) return err;
    if (rx_len < 9) return ESP_ERR_INVALID_RESPONSE; // DSFID + 8-byte UID minimum

    /* UID is bytes 1..8 of the response (byte 0 is DSFID) */
    memcpy(uid, &rx[1], ST25R3916_NFCV_UID_LEN);
    return ESP_OK;
}
