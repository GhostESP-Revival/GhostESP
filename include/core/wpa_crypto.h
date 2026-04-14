#ifndef WPA_CRYPTO_H
#define WPA_CRYPTO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define PMK_LEN 32
#define PTK_LEN 64
#define GTK_MAX_LEN 32
#define MIC_LEN 16
#define NONCE_LEN 32
#define MAC_LEN 6
#define EAPOL_MSG_LEN 99

typedef struct {
    uint8_t pmk[PMK_LEN];
    uint8_t ptk[PTK_LEN];
    uint8_t gtk[GTK_MAX_LEN];
    uint8_t gtk_len;
    uint8_t kek[16];
    uint8_t kck[16];
    uint8_t tk[16];
    uint8_t ap_mac[MAC_LEN];
    uint8_t sta_mac[MAC_LEN];
    uint8_t anonce[NONCE_LEN];
    uint8_t snonce[NONCE_LEN];
    bool ptk_derived;
    bool gtk_derived;
} wpa_key_material_t;

void wpa_crypto_init(void);

bool wpa_derive_pmk(const char *ssid, const char *password, uint8_t *pmk_out);

bool wpa_derive_ptk(const uint8_t *pmk,
                    const uint8_t *anonce,
                    const uint8_t *snonce,
                    const uint8_t *ap_mac,
                    const uint8_t *sta_mac,
                    uint8_t *ptk_out);

bool wpa_extract_gtk_from_m3(const uint8_t *eapol_m3_frame,
                              int frame_len,
                              const uint8_t *kek,
                              uint8_t *gtk_out,
                              uint8_t *gtk_len_out);

bool wpa_decrypt_gtk_key_data(const uint8_t *encrypted_key_data,
                               int key_data_len,
                               const uint8_t *kek,
                               uint8_t *decrypted_out,
                               int *decrypted_len_out);

bool wpa_ccmp_encrypt(const uint8_t *gtk,
                      int gtk_len,
                      const uint8_t *mac_hdr,
                      int mac_hdr_len,
                      const uint8_t *payload,
                      int payload_len,
                      uint8_t *encrypted_out,
                      int *encrypted_len_out,
                      uint64_t pn);

int wpa_parse_eapol_m1(const uint8_t *frame, int len, uint8_t *anonce_out);
int wpa_parse_eapol_m3(const uint8_t *frame, int len,
                       uint8_t *anonce_out,
                       const uint8_t **key_data_out,
                       int *key_data_len_out);

bool wpa_mic_verify_eapol(const uint8_t *kck,
                           const uint8_t *eapol_frame,
                           int eapol_len);

#endif
