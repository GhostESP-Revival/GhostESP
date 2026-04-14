/**
 * @file wpa_crypto.c
 * @brief WPA/WPA2 key derivation and CCMP encryption for GTK abuse testing
 *
 * Implements PRF-512 (WPA2 key derivation), AES-128-KEK unwrapping for GTK
 * extraction from EAPOL M3, and CCMP encryption for broadcast frame injection.
 * Uses ESP-IDF's bundled mbedTLS for all crypto primitives.
 */

#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include "core/wpa_crypto.h"
#include "core/glog.h"
#include "mbedtls/md.h"
#include "mbedtls/private/sha256.h"
#include "mbedtls/private/aes.h"
#include "mbedtls/private/pkcs5.h"
#include <string.h>

static const char *TAG = "WpaCrypto";

void wpa_crypto_init(void) {
}

static void sha1_hmac(const uint8_t *key, size_t key_len,
                      const uint8_t *data, size_t data_len,
                      uint8_t *out) {
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    mbedtls_md_hmac(md_info, key, key_len, data, data_len, out);
}

static void prf(const uint8_t *key, size_t key_len,
                const uint8_t *data, size_t data_len,
                uint8_t *out, size_t out_len) {
    uint8_t counter = 0;
    size_t offset = 0;

    size_t merged_len = data_len + 1;
    uint8_t *merged = (uint8_t *)malloc(merged_len);
    if (!merged) return;
    memcpy(merged, data, data_len);
    merged[data_len] = counter;

    uint8_t hash[20];
    sha1_hmac(key, key_len, merged, merged_len, hash);
    size_t copy = out_len > 20 ? 20 : out_len;
    memcpy(out + offset, hash, copy);
    offset += copy;
    free(merged);

    while (offset < out_len) {
        counter++;
        size_t r_len = 20 + data_len + 1;
        uint8_t *r = (uint8_t *)malloc(r_len);
        if (!r) return;
        memcpy(r, hash, 20);
        memcpy(r + 20, data, data_len);
        r[20 + data_len] = counter;
        sha1_hmac(key, key_len, r, r_len, hash);
        copy = (out_len - offset) > 20 ? 20 : (out_len - offset);
        memcpy(out + offset, hash, copy);
        offset += copy;
        free(r);
    }
}

bool wpa_derive_pmk(const char *ssid, const char *password, uint8_t *pmk_out) {
    if (!ssid || !password || !pmk_out) return false;

    int ret = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA1,
                                         (const unsigned char *)password, strlen(password),
                                         (const unsigned char *)ssid, strlen(ssid),
                                         4096, 32, pmk_out);
    if (ret != 0) {
        glog("PMK derivation failed: -0x%04x\n", -ret);
        return false;
    }
    return true;
}

bool wpa_derive_ptk(const uint8_t *pmk,
                    const uint8_t *anonce,
                    const uint8_t *snonce,
                    const uint8_t *ap_mac,
                    const uint8_t *sta_mac,
                    uint8_t *ptk_out) {
    if (!pmk || !anonce || !snonce || !ap_mac || !sta_mac || !ptk_out)
        return false;

    uint8_t min_mac[6], max_mac[6];
    if (memcmp(ap_mac, sta_mac, 6) < 0) {
        memcpy(min_mac, ap_mac, 6);
        memcpy(max_mac, sta_mac, 6);
    } else {
        memcpy(min_mac, sta_mac, 6);
        memcpy(max_mac, ap_mac, 6);
    }

    uint8_t min_nonce[32], max_nonce[32];
    if (memcmp(anonce, snonce, 32) < 0) {
        memcpy(min_nonce, anonce, 32);
        memcpy(max_nonce, snonce, 32);
    } else {
        memcpy(min_nonce, snonce, 32);
        memcpy(max_nonce, anonce, 32);
    }

    uint8_t pke[100];
    memcpy(pke, "Pairwise key expansion", 23);
    memcpy(pke + 23, min_mac, 6);
    memcpy(pke + 29, max_mac, 6);
    memcpy(pke + 35, min_nonce, 32);
    memcpy(pke + 67, max_nonce, 32);

    prf(pmk, PMK_LEN, pke, 99, ptk_out, PTK_LEN);
    return true;
}

static int aes_unwrap(const uint8_t *kek, const uint8_t *input, int input_len,
                      uint8_t *output) {
    if (input_len < 16 || (input_len % 8) != 0) return -1;

    int n = (input_len / 8) - 1;
    uint8_t a[8];
    uint8_t *r = (uint8_t *)malloc(n * 8);
    if (!r) return -1;

    memcpy(a, input, 8);
    memcpy(r, input + 8, n * 8);

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_dec(&aes, kek, 128);
    #define AES_DECRYPT 0

    for (int j = 5; j >= 0; j--) {
        for (int i = n - 1; i >= 0; i--) {
            uint8_t t_val[4];
            uint32_t t_num = (uint32_t)(n * j + (i + 1));
            t_val[0] = (t_num >> 24) & 0xFF;
            t_val[1] = (t_num >> 16) & 0xFF;
            t_val[2] = (t_num >> 8) & 0xFF;
            t_val[3] = t_num & 0xFF;

            uint8_t block_in[16], block_out[16];
            memcpy(block_in, a, 8);
            int idx = i * 8;
            for (int k = 0; k < 4; k++) block_in[k] ^= t_val[k];
            for (int k = 0; k < 4; k++) block_in[4 + k] ^= t_val[k];
            memcpy(block_in + 8, r + idx, 8);

            mbedtls_aes_crypt_ecb(&aes, AES_DECRYPT, block_in, block_out);

            memcpy(a, block_out, 8);
            memcpy(r + idx, block_out + 8, 8);
        }
    }
    mbedtls_aes_free(&aes);

    int valid = 0xA6;
    bool ok = (a[0] == 0xA6 && a[1] == 0xA6 && a[2] == 0xA6 && a[3] == 0xA6);
    memcpy(output, r, n * 8);
    free(r);

    return ok ? (n * 8) : -1;
}

bool wpa_decrypt_gtk_key_data(const uint8_t *encrypted_key_data,
                               int key_data_len,
                               const uint8_t *kek,
                               uint8_t *decrypted_out,
                               int *decrypted_len_out) {
    if (!encrypted_key_data || !kek || !decrypted_out || !decrypted_len_out)
        return false;

    int len = aes_unwrap(kek, encrypted_key_data, key_data_len, decrypted_out);
    if (len < 0) {
        glog("AES unwrap failed\n");
        return false;
    }
    *decrypted_len_out = len;
    return true;
}

bool wpa_extract_gtk_from_m3(const uint8_t *eapol_m3_frame,
                              int frame_len,
                              const uint8_t *kek,
                              uint8_t *gtk_out,
                              uint8_t *gtk_len_out) {
    if (!eapol_m3_frame || !kek || !gtk_out || !gtk_len_out)
        return false;

    const uint8_t *eapol = eapol_m3_frame;

    uint16_t key_info = (eapol[5] << 8) | eapol[6];
    bool encrypted = (key_info & 0x0100) != 0;
    if (!encrypted) {
        glog("M3 key data not encrypted\n");
        return false;
    }

    uint16_t key_data_len = (eapol[7] << 8) | eapol[8];
    int hdr_size = 28;

    if (key_data_len == 0 || (frame_len - hdr_size) < key_data_len) {
        glog("M3 key data length invalid\n");
        return false;
    }

    uint8_t *decrypted = (uint8_t *)malloc(key_data_len);
    if (!decrypted) return false;

    int decrypted_len = 0;
    bool ok = wpa_decrypt_gtk_key_data(eapol + hdr_size + 80, key_data_len,
                                        kek, decrypted, &decrypted_len);
    if (!ok) {
        const uint8_t *raw_kd = eapol + hdr_size + 80;
        ok = wpa_decrypt_gtk_key_data(raw_kd, key_data_len,
                                       kek, decrypted, &decrypted_len);
    }

    if (!ok) {
        free(decrypted);
        return false;
    }

    bool found = false;
    for (int i = 0; i < decrypted_len - 2;) {
        uint8_t eid = decrypted[i];
        uint8_t elen = decrypted[i + 1];
        if (i + 2 + elen > decrypted_len) break;
        if (eid == 0xDD || eid == 0x4E) {
            if (elen >= 14) {
                uint8_t gtk_len = decrypted[i + 13];
                if (gtk_len > 0 && gtk_len <= 32 && elen >= 14 + gtk_len) {
                    memcpy(gtk_out, decrypted + i + 14, gtk_len);
                    *gtk_len_out = gtk_len;
                    found = true;
                    break;
                }
            }
            if (!found && elen >= 8) {
                uint8_t gtk_len = elen - 8;
                if (gtk_len > 0 && gtk_len <= 32) {
                    memcpy(gtk_out, decrypted + i + 10, gtk_len);
                    *gtk_len_out = gtk_len;
                    found = true;
                    break;
                }
            }
        }
        i += 2 + elen;
    }

    free(decrypted);
    if (!found) glog("GTK IE not found in decrypted M3 key data\n");
    return found;
}

static void ccmp_inc_pn(uint8_t *pn) {
    for (int i = 5; i >= 0; i--) {
        if (++pn[i] != 0) break;
    }
}

static void ccm_nonce(const uint8_t *key, int key_len,
                      const uint8_t *pn,
                      const uint8_t *aad, int aad_len,
                      const uint8_t *plain, int plain_len,
                      uint8_t *crypt, uint8_t *mic) {
    uint8_t flags = 0x61;
    uint8_t nonce[13];
    nonce[0] = flags;
    memcpy(nonce + 1, pn, 6);
    memset(nonce + 7, 0, 5);

    uint8_t b0[16];
    memset(b0, 0, 16);
    b0[0] = flags;
    memcpy(b0 + 1, nonce, 13);
    b0[14] = (plain_len >> 8) & 0xFF;
    b0[15] = plain_len & 0xFF;

    uint8_t mic_mask[16];
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, key, key_len * 8);
    #define AES_ENCRYPT 1
    mbedtls_aes_crypt_ecb(&aes, AES_ENCRYPT, b0, mic_mask);

    int auth_blocks = (aad_len + 16 - 1) / 16;
    uint8_t block[16];
    memset(block, 0, 16);
    block[0] = (aad_len >> 8) & 0xFF;
    block[1] = aad_len & 0xFF;
    int copy1 = aad_len > 14 ? 14 : aad_len;
    memcpy(block + 2, aad, copy1);
    for (int i = 0; i < 16; i++) mic_mask[i] ^= block[i];
    mbedtls_aes_crypt_ecb(&aes, AES_ENCRYPT, mic_mask, mic_mask);

    for (int b = 0; b < auth_blocks; b++) {
        int off = b * 16;
        if (b == 0) off = copy1;
        int rem = aad_len - off;
        if (rem > 16) rem = 16;
        memset(block, 0, 16);
        if (rem > 0) memcpy(block, aad + off, rem);
        for (int i = 0; i < 16; i++) mic_mask[i] ^= block[i];
        mbedtls_aes_crypt_ecb(&aes, AES_ENCRYPT, mic_mask, mic_mask);
    }

    int data_blocks = (plain_len + 16 - 1) / 16;
    for (int b = 0; b < data_blocks; b++) {
        int off = b * 16;
        int rem = plain_len - off;
        if (rem > 16) rem = 16;
        memset(block, 0, 16);
        memcpy(block, plain + off, rem);
        for (int i = 0; i < 16; i++) mic_mask[i] ^= block[i];
        mbedtls_aes_crypt_ecb(&aes, AES_ENCRYPT, mic_mask, mic_mask);
    }
    memcpy(mic, mic_mask, 8);

    uint8_t ctr0[16];
    memset(ctr0, 0, 16);
    ctr0[0] = flags;
    memcpy(ctr0 + 1, nonce, 13);
    ctr0[15] = 0;

    uint8_t keystream[16];
    mbedtls_aes_crypt_ecb(&aes, AES_ENCRYPT, ctr0, keystream);
    for (int i = 0; i < 8; i++) mic[i] ^= keystream[i];

    for (int b = 0; b < data_blocks; b++) {
        int off = b * 16;
        int rem = plain_len - off;
        if (rem > 16) rem = 16;
        uint8_t ctr[16];
        memset(ctr, 0, 16);
        ctr[0] = flags;
        memcpy(ctr + 1, nonce, 13);
        ctr[14] = ((b + 1) >> 8) & 0xFF;
        ctr[15] = (b + 1) & 0xFF;
        mbedtls_aes_crypt_ecb(&aes, AES_ENCRYPT, ctr, keystream);
        for (int i = 0; i < rem; i++)
            crypt[off + i] = plain[off + i] ^ keystream[i];
    }

    mbedtls_aes_free(&aes);
}

static void ccmp_build_aad(const uint8_t *mac_hdr, uint8_t *aad_out, int *aad_len_out) {
    uint16_t fc = mac_hdr[0] | (mac_hdr[1] << 8);
    fc &= ~0x4000;
    fc &= ~0x000F;
    aad_out[0] = fc & 0xFF;
    aad_out[1] = (fc >> 8) & 0xFF;
    memcpy(aad_out + 2, mac_hdr + 4, 6);
    memcpy(aad_out + 8, mac_hdr + 10, 6);
    memcpy(aad_out + 14, mac_hdr + 16, 6);
    *aad_len_out = 20;
}

bool wpa_ccmp_encrypt(const uint8_t *gtk,
                      int gtk_len,
                      const uint8_t *mac_hdr,
                      int mac_hdr_len,
                      const uint8_t *payload,
                      int payload_len,
                      uint8_t *encrypted_out,
                      int *encrypted_len_out,
                      uint64_t pn) {
    if (!gtk || !mac_hdr || !payload || !encrypted_out || !encrypted_len_out)
        return false;

    if (gtk_len != 16) {
        glog("CCMP requires 16-byte GTK, got %d\n", gtk_len);
        return false;
    }

    uint8_t aad[20];
    int aad_len;
    ccmp_build_aad(mac_hdr, aad, &aad_len);

    uint8_t pn_bytes[6];
    pn_bytes[0] = (pn >> 40) & 0xFF;
    pn_bytes[1] = (pn >> 32) & 0xFF;
    pn_bytes[2] = (pn >> 24) & 0xFF;
    pn_bytes[3] = (pn >> 16) & 0xFF;
    pn_bytes[4] = (pn >> 8) & 0xFF;
    pn_bytes[5] = pn & 0xFF;

    memcpy(encrypted_out, mac_hdr, mac_hdr_len);

    uint8_t iv[8];
    iv[0] = 0x00;
    memcpy(iv + 1, pn_bytes, 6);
    iv[7] = mac_hdr_len == 26 ? mac_hdr[24] : 0;
    memcpy(encrypted_out + mac_hdr_len, iv, 8);

    uint8_t *crypt_start = encrypted_out + mac_hdr_len + 8;
    uint8_t mic[8];

    ccm_nonce(gtk, gtk_len, pn_bytes, aad, aad_len,
              payload, payload_len, crypt_start, mic);

    memcpy(crypt_start + payload_len, mic, 8);

    *encrypted_len_out = mac_hdr_len + 8 + payload_len + 8;

    uint16_t fc = mac_hdr[0] | (mac_hdr[1] << 8);
    fc |= 0x4000;
    encrypted_out[0] = fc & 0xFF;
    encrypted_out[1] = (fc >> 8) & 0xFF;

    return true;
}

int wpa_parse_eapol_m1(const uint8_t *frame, int len, uint8_t *anonce_out) {
    const uint8_t *llc = frame;
    while (llc < frame + len - 8) {
        if (llc[0] == 0xAA && llc[1] == 0xAA && llc[2] == 0x03 &&
            llc[6] == 0x88 && llc[7] == 0x8E) {
            break;
        }
        llc++;
    }
    if (llc >= frame + len - 8) return -1;

    const uint8_t *eapol = llc + 8;
    int eapol_remaining = len - (int)(eapol - frame);
    if (eapol_remaining < 17) return -1;

    uint8_t key_desc = eapol[4];
    if (key_desc != 2) return -1;

    uint16_t key_info = (eapol[5] << 8) | eapol[6];
    bool has_mic = (key_info & 0x0100) != 0;
    bool is_ack = (key_info & 0x0080) != 0;
    bool is_install = (key_info & 0x0040) != 0;

    if (has_mic || !is_ack || is_install) return -1;

    if (anonce_out) memcpy(anonce_out, eapol + 9, 32);
    return 0;
}

int wpa_parse_eapol_m3(const uint8_t *frame, int len,
                       uint8_t *anonce_out,
                       const uint8_t **key_data_out,
                       int *key_data_len_out) {
    const uint8_t *llc = frame;
    while (llc < frame + len - 8) {
        if (llc[0] == 0xAA && llc[1] == 0xAA && llc[2] == 0x03 &&
            llc[6] == 0x88 && llc[7] == 0x8E) {
            break;
        }
        llc++;
    }
    if (llc >= frame + len - 8) return -1;

    const uint8_t *eapol = llc + 8;
    int eapol_remaining = len - (int)(eapol - frame);
    if (eapol_remaining < 17) return -1;

    uint8_t key_desc = eapol[4];
    if (key_desc != 2) return -1;

    uint16_t key_info = (eapol[5] << 8) | eapol[6];
    bool has_mic = (key_info & 0x0100) != 0;
    bool is_ack = (key_info & 0x0080) != 0;
    bool is_install = (key_info & 0x0040) != 0;

    if (!has_mic || !is_ack || !is_install) return -1;

    if (anonce_out) memcpy(anonce_out, eapol + 9, 32);

    uint16_t kd_len = (eapol[7] << 8) | eapol[8];
    if (key_data_out) *key_data_out = eapol + 28 + 80;
    if (key_data_len_out) *key_data_len_out = kd_len;

    return 0;
}
