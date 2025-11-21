/**
 * @file     pn532.h
 * @author   D. Braun
 * @license  MIT (see license.txt)
 * This is a PN532 Driver for the ESP32 family and IDF 5.3 for NXP's PN532 NFC/13.56MHz RFID Transceiver.
 * This component is inspired the Adafruit library.
 */

 #include <string.h>
 #include "esp_log.h"
 #include "esp_err.h"
 
 #include "pn532.h"
 #include "pn532_driver.h"
 
 static const char TAG[] = "PN532";
 static bool s_quiet = false;
 static int s_indata_wait_ms = 20;  // default
 static int s_thru_wait_ms = 100;   // default
 static int s_inlist_wait_ms = 40; // default for in_list
 
 const uint8_t pn532response_firmwarevers[] = {0x00, 0xFF, 0x06, 0xFA, 0xD5, 0x03};
 
 static uint8_t pn532_inListedTag;  // Tg number of inlisted tag.
 
 #define PN532_COMMAND_BUFFER_LEN 64
 uint8_t pn532_packetbuffer[PN532_COMMAND_BUFFER_LEN];
 
 esp_err_t pn532_get_firmware_version(pn532_io_handle_t io_handle, uint32_t *fw_version)
 {
     esp_err_t err;
 
     if (io_handle == NULL || io_handle->driver_data == NULL) {
         return ESP_ERR_INVALID_ARG;
     }

 
 
     pn532_packetbuffer[0] = PN532_COMMAND_GETFIRMWAREVERSION;
 
     err = pn532_send_command_wait_ack(io_handle, pn532_packetbuffer, 1, PN532_WRITE_TIMEOUT);
     if (ESP_OK != err) {
         return err;
     }
 
 #ifdef CONFIG_PN532DEBUG
     ESP_LOGD(TAG, "pn532_get_firmware_version(): Waiting for IRQ/ready");
 #endif
     err = pn532_wait_ready(io_handle, 100);
     if (ESP_OK != err) {
 #ifdef CONFIG_PN532DEBUG
         ESP_LOGD(TAG, "pn532_get_firmware_version(): Timeout occurred");
 #endif
         return err;
     }
 
     // read data packet
     err = pn532_read_data(io_handle, pn532_packetbuffer, 12, PN532_READ_TIMEOUT);
     if (ESP_OK != err)
         return err;
 
     // check some basic stuff
     if (0 != memcmp(pn532_packetbuffer + 1, pn532response_firmwarevers, sizeof(pn532response_firmwarevers))) {
 #ifdef CONFIG_PN532DEBUG
         ESP_LOGD(TAG, "pn532_get_firmware_version(): get firmware response invalid!");
 #endif
         return ESP_FAIL;
     }
 
     int offset = 7;
     *fw_version  = pn532_packetbuffer[offset++] << 24;
     *fw_version |= pn532_packetbuffer[offset++] << 16;
     *fw_version |= pn532_packetbuffer[offset++] << 8;
     *fw_version |= pn532_packetbuffer[offset];
 
     return ESP_OK;
 }
 
 esp_err_t pn532_set_passive_activation_retries(pn532_io_handle_t io_handle, uint8_t maxRetries) {
     pn532_packetbuffer[0] = PN532_COMMAND_RFCONFIGURATION;
     pn532_packetbuffer[1] = 5;    // Config item 5 (MaxRetries)
     pn532_packetbuffer[2] = 0xFF; // MxRtyATR (default = 0xFF)
     pn532_packetbuffer[3] = 0x01; // MxRtyPSL (default = 0x01)
     pn532_packetbuffer[4] = maxRetries;
 
 #ifdef CONFIG_MIFAREDEBUG
     ESP_LOGD(TAG, "pn532_set_passive_activation_retries(): Setting MxRtyPassiveActivation to %d", maxRetries);
 #endif
 
     return pn532_send_command_wait_ack(io_handle, pn532_packetbuffer, 5, PN532_WRITE_TIMEOUT);
 }
 
 esp_err_t pn532_read_passive_target_id(pn532_io_handle_t io_handle,
                                        uint8_t baud_rate_and_card_type,
                                        uint8_t *uid,
                                        uint8_t *uid_length,
                                        int32_t timeout)
 {
     pn532_packetbuffer[0] = PN532_COMMAND_INLISTPASSIVETARGET;
     pn532_packetbuffer[1] = 1; // currently only support one card (PN532 can handle two cards)
     pn532_packetbuffer[2] = baud_rate_and_card_type;
 
     esp_err_t err = pn532_send_command_wait_ack(io_handle, pn532_packetbuffer, 3, PN532_WRITE_TIMEOUT);
     if (ESP_OK != err) {
 #ifdef CONFIG_PN532DEBUG
         ESP_LOGD(TAG, "No card(s) read");
 #endif
         return err;
     }
 
 #ifdef CONFIG_PN532DEBUG
     ESP_LOGD(TAG, "Waiting for IRQ (indicates card presence)");
 #endif
     err = pn532_wait_ready(io_handle, timeout);
     if (ESP_OK != err) {
 #ifdef CONFIG_PN532DEBUG
         ESP_LOGD(TAG, "PN532 not ready, timeout or error occurred");
 #endif
         return err;
     }
     err = pn532_read_data(io_handle, pn532_packetbuffer, 32, timeout);
     if (ESP_OK != err)
         return err;
 
     /* ISO14443A card response should be in the following format:
 
      byte            Description
      -------------   ------------------------------------------
      b0..6           Frame header and preamble
      b7              Number of tags Found
      b8              Tag Number (only one used in this example)
      b9..10          SENS_RES
      b11             SEL_RES
      b12             NFCID Length
      b13..NFCIDLen   NFCID                                      */
 
 #ifdef CONFIG_MIFAREDEBUG
     ESP_LOGD(TAG, "Found %d tags", pn532_packetbuffer[7]);
 #endif
     if (pn532_packetbuffer[7] != 1)
         return ESP_FAIL;
 
 #ifdef CONFIG_MIFAREDEBUG
     uint16_t sens_res = pn532_packetbuffer[9] << 8 | pn532_packetbuffer[10];
 
     ESP_LOGD(TAG, "ATQA: 0x%.2X", sens_res);
     ESP_LOGD(TAG, "SAK: 0x%.2X", pn532_packetbuffer[11]);
 #endif
 
    /* Card appears to be Mifare Classic */
    *uid_length = pn532_packetbuffer[12];
#ifdef CONFIG_MIFAREDEBUG
    printf("UID:");
    for (uint8_t i = 0; i < pn532_packetbuffer[12]; i++) {
        printf(" 0x%.2X", pn532_packetbuffer[13 + i]);
    }
    printf("\n");
#endif
    memcpy(uid, pn532_packetbuffer + 13, pn532_packetbuffer[12]);
 
     return ESP_OK;
 }
 
 esp_err_t pn532_read_passive_target_id_ex(pn532_io_handle_t io_handle,
                                           uint8_t baud_rate_and_card_type,
                                           uint8_t *uid,
                                           uint8_t *uid_length,
                                           uint16_t *atqa,
                                           uint8_t *sak,
                                           int32_t timeout)
 {
     pn532_packetbuffer[0] = PN532_COMMAND_INLISTPASSIVETARGET;
     pn532_packetbuffer[1] = 1; // Support one card
     pn532_packetbuffer[2] = baud_rate_and_card_type;

     esp_err_t err = pn532_send_command_wait_ack(io_handle, pn532_packetbuffer, 3, PN532_WRITE_TIMEOUT);
     if (ESP_OK != err) {
 #ifdef CONFIG_PN532DEBUG
         ESP_LOGD(TAG, "pn532_read_passive_target_id_ex(): No card(s) read");
 #endif
         return err;
     }

 #ifdef CONFIG_PN532DEBUG
     ESP_LOGD(TAG, "pn532_read_passive_target_id_ex(): Waiting for IRQ (card presence)");
 #endif
     err = pn532_wait_ready(io_handle, timeout);
     if (ESP_OK != err) {
 #ifdef CONFIG_PN532DEBUG
         ESP_LOGD(TAG, "pn532_read_passive_target_id_ex(): Timeout or error waiting for ready");
 #endif
         return err;
     }

     err = pn532_read_data(io_handle, pn532_packetbuffer, 32, timeout);
     if (ESP_OK != err)
         return err;

     if (pn532_packetbuffer[7] != 1)
        return ESP_FAIL;

    // Save target number for subsequent INDATAEXCHANGE operations
    pn532_inListedTag = pn532_packetbuffer[8];

    if (atqa) {
        *atqa = ((uint16_t)pn532_packetbuffer[9] << 8) | pn532_packetbuffer[10];
    }
    if (sak) {
        *sak = pn532_packetbuffer[11];
     }

    *uid_length = pn532_packetbuffer[12];
#ifdef CONFIG_MIFAREDEBUG
    printf("UID:");
    for (uint8_t i = 0; i < pn532_packetbuffer[12]; i++) {
        printf(" 0x%.2X", pn532_packetbuffer[13 + i]);
    }
    printf("\n");
#endif
    memcpy(uid, pn532_packetbuffer + 13, pn532_packetbuffer[12]);

    return ESP_OK;
}

 esp_err_t ntag2xx_get_version(pn532_io_handle_t io_handle, uint8_t version_out[8])
 {
     if (!version_out) return ESP_ERR_INVALID_ARG;
     uint8_t cmd[1] = { 0x60 }; // GET_VERSION
     uint8_t resp_len = 8;
     esp_err_t err = pn532_in_data_exchange(io_handle, cmd, sizeof(cmd), version_out, &resp_len);
     if (err != ESP_OK) return err;
     return (resp_len == 8) ? ESP_OK : ESP_FAIL;
 }

 esp_err_t ntag2xx_read_signature(pn532_io_handle_t io_handle, uint8_t sig_out[32])
 {
     if (!sig_out) return ESP_ERR_INVALID_ARG;
     uint8_t cmd[2] = { 0x3C, 0x00 }; // READ_SIG, address 0x00
     uint8_t resp_len = 32;
     esp_err_t err = pn532_in_data_exchange(io_handle, cmd, sizeof(cmd), sig_out, &resp_len);
     if (err != ESP_OK) return err;
     return (resp_len == 32) ? ESP_OK : ESP_FAIL;
 }

 esp_err_t ntag2xx_read_counter(pn532_io_handle_t io_handle, uint8_t counter_index, uint32_t *value_out)
 {
     if (!value_out || counter_index > 2) return ESP_ERR_INVALID_ARG;
     uint8_t cmd[2] = { 0x39, counter_index }; // READ_CNT
     uint8_t buf[3] = {0};
     uint8_t resp_len = sizeof(buf);
     esp_err_t err = pn532_in_data_exchange(io_handle, cmd, sizeof(cmd), buf, &resp_len);
     if (err != ESP_OK) return err;
     if (resp_len != 3) return ESP_FAIL;
     // Interpret as 24-bit big-endian
     *value_out = ((uint32_t)buf[0] << 16) | ((uint32_t)buf[1] << 8) | buf[2];
     return ESP_OK;
 }

 esp_err_t ntag2xx_read_tearing(pn532_io_handle_t io_handle, uint8_t counter_index, uint8_t *tearing_out)
 {
     if (!tearing_out || counter_index > 2) return ESP_ERR_INVALID_ARG;
     uint8_t cmd[2] = { 0x3A, counter_index }; // READ_TEARING
     uint8_t resp = 0;
     uint8_t resp_len = 1;
     esp_err_t err = pn532_in_data_exchange(io_handle, cmd, sizeof(cmd), &resp, &resp_len);
     if (err != ESP_OK) return err;
     if (resp_len != 1) return ESP_FAIL;
     *tearing_out = resp;
     return ESP_OK;
 }

 esp_err_t pn532_in_data_exchange(pn532_io_handle_t io_handle,
                                  const uint8_t *send_buffer,
                                  uint8_t send_buffer_length,
                                  uint8_t *response,
                                  uint8_t *response_length) {
     if (send_buffer_length > PN532_COMMAND_BUFFER_LEN - 2) {
       if (!s_quiet) ESP_LOGI(TAG, "APDU length too long for packet buffer");
       return ESP_ERR_INVALID_ARG;
   }

    pn532_packetbuffer[0] = PN532_COMMAND_INDATAEXCHANGE;
    pn532_packetbuffer[1] = pn532_inListedTag;
    memcpy(pn532_packetbuffer + 2, send_buffer, send_buffer_length);

     // detect auth attempts for faster timeout (auth fails quickly)
     bool is_auth = (send_buffer_length == 12 && (send_buffer[0] == MIFARE_CMD_AUTH_A || send_buffer[0] == MIFARE_CMD_AUTH_B));
     int32_t wait_timeout = is_auth ? 10 : s_indata_wait_ms;

     esp_err_t err = pn532_send_command_wait_ack(io_handle, pn532_packetbuffer, send_buffer_length + 2, PN532_WRITE_TIMEOUT);
   if (ESP_OK != err) {
       if (!s_quiet) ESP_LOGI(TAG, "Could not send INDATAEXCHANGE APDU (err=%d)", err);
       return err;
   }

     err = pn532_wait_ready(io_handle, wait_timeout);
   if (ESP_OK != err) {
       if (!s_quiet) ESP_LOGI(TAG, "INDATAEXCHANGE timeout waiting for response");
       return err;
   }

     // detect block reads (MIFARE_CMD_READ = 0x30) - read full frame in one go for speed
     // block read response is always 26 bytes: preamble(3) + len(1) + len_chk(1) + TFI(1) + CMD(1) + status(1) + data(16) + chk(1) + post(1)
     bool is_block_read = (send_buffer_length == 2 && send_buffer[0] == MIFARE_CMD_READ);
     uint8_t initial_read = is_block_read ? 26 : 8;
     
     err = pn532_read_data(io_handle, pn532_packetbuffer, initial_read, PN532_READ_TIMEOUT);
     if (ESP_OK != err)
         return err;

     // quick check for valid frame header
     if (pn532_packetbuffer[0] == 0 && pn532_packetbuffer[1] == 0 && pn532_packetbuffer[2] == 0xff) {
         uint8_t length = pn532_packetbuffer[3];
         if (0 != ((pn532_packetbuffer[4] + length) & 0xFF)) {
          if (!s_quiet) ESP_LOGI(TAG, "INDATAEXCHANGE length check invalid len=0x%02X sum=0x%02X", length, pn532_packetbuffer[4]);
          return ESP_FAIL;
      }
       if (pn532_packetbuffer[5] == PN532_PN532TOHOST && pn532_packetbuffer[6] == PN532_RESPONSE_INDATAEXCHANGE) {
            // check status early for fast auth failure
            if ((pn532_packetbuffer[7] & 0x3f) != 0) {
                if (!s_quiet) ESP_LOGI(TAG, "INDATAEXCHANGE status error: 0x%02X", pn532_packetbuffer[7]);
                return ESP_FAIL;
            }
            
            // for block reads we already have all data, for others read remaining if needed
            if (!is_block_read && length > 3) {
                uint8_t remaining = length - 3;
                if (remaining > 0 && remaining <= (sizeof(pn532_packetbuffer) - 8)) {
                    err = pn532_read_data(io_handle, pn532_packetbuffer + 8, remaining, PN532_READ_TIMEOUT);
                    if (ESP_OK != err) return err;
                }
            }

            // subtract TFI, CMD, status (already validated above)
            length -= 3;

            // clamp to available buffer space and response buffer
            if (length > *response_length) {
                length = *response_length;
            }

            if (length > 0) {
                memcpy(response, pn532_packetbuffer + 8, length);
            }
            *response_length = length;

            return ESP_OK;
        } else {
          if (!s_quiet) ESP_LOGI(TAG, "INDATAEXCHANGE unexpected response CMD=0x%02X", pn532_packetbuffer[6]);
          return ESP_FAIL;
      }
   } else {
      if (!s_quiet) ESP_LOGI(TAG, "INDATAEXCHANGE preamble missing");
      return ESP_FAIL;
  }
}

 esp_err_t pn532_in_communicate_thru(pn532_io_handle_t io_handle,
                                    const uint8_t *send_buffer,
                                    uint8_t send_buffer_length,
                                    uint8_t *response,
                                    uint8_t *response_length) {
    if (send_buffer_length > PN532_COMMAND_BUFFER_LEN - 1) {
      if (!s_quiet) ESP_LOGI(TAG, "InCommunicateThru APDU too long");
      return ESP_ERR_INVALID_ARG;
  }

    pn532_packetbuffer[0] = PN532_COMMAND_INCOMMUNICATETHRU;
    memcpy(pn532_packetbuffer + 1, send_buffer, send_buffer_length);

    esp_err_t err = pn532_send_command_wait_ack(io_handle, pn532_packetbuffer, send_buffer_length + 1, PN532_WRITE_TIMEOUT);
  if (ESP_OK != err) {
      if (!s_quiet) ESP_LOGI(TAG, "Could not send InCommunicateThru APDU (err=%d)", err);
      return err;
  }

    err = pn532_wait_ready(io_handle, s_thru_wait_ms);
  if (ESP_OK != err) {
      if (!s_quiet) ESP_LOGI(TAG, "InCommunicateThru response timeout");
      return err;
  }

    err = pn532_read_data(io_handle, pn532_packetbuffer, sizeof(pn532_packetbuffer), PN532_READ_TIMEOUT);
    if (ESP_OK != err)
        return err;

    if (pn532_packetbuffer[0] == 0 && pn532_packetbuffer[1] == 0 && pn532_packetbuffer[2] == 0xFF) {
        uint8_t length = pn532_packetbuffer[3];
        if (0 != ((pn532_packetbuffer[4] + length) & 0xFF)) {
            if (!s_quiet) ESP_LOGI(TAG, "InCommunicateThru length check invalid len=0x%02X sum=0x%02X", length, pn532_packetbuffer[4]);
            return ESP_FAIL;
        }
        if (pn532_packetbuffer[5] == PN532_PN532TOHOST && pn532_packetbuffer[6] == PN532_RESPONSE_INCOMMUNICATETHRU) {
            if ((pn532_packetbuffer[7] & 0x3F) != 0x00) {
                if (!s_quiet) ESP_LOGI(TAG, "InCommunicateThru status error: 0x%02X", pn532_packetbuffer[7]);
                return ESP_FAIL;
            }
            // subtract TFI, CMD, status; validate and clamp
            if (length < 3) {
                if (!s_quiet) ESP_LOGI(TAG, "InCommunicateThru payload length underflow: %u", (unsigned)length);
                return ESP_FAIL;
            }
            length -= 3;
            size_t max_payload = (sizeof(pn532_packetbuffer) > 8) ? (sizeof(pn532_packetbuffer) - 8) : 0;
            if (length > max_payload) length = (uint8_t)max_payload;
            if (length > *response_length) length = *response_length;
            memcpy(response, pn532_packetbuffer + 8, length);
            *response_length = length;
            return ESP_OK;
        } else {
            if (!s_quiet) ESP_LOGI(TAG, "Unexpected response to InCommunicateThru: 0x%02X", pn532_packetbuffer[6]);
            return ESP_FAIL;
        }
    }
    if (!s_quiet) ESP_LOGI(TAG, "InCommunicateThru preamble missing");
    return ESP_FAIL;
}

void pn532_set_quiet(bool quiet) { s_quiet = quiet; }
void pn532_set_indata_wait_timeout(int ms) { if (ms > 0) s_indata_wait_ms = ms; }
void pn532_set_thru_wait_timeout(int ms) { if (ms > 0) s_thru_wait_ms = ms; }
void pn532_set_inlist_wait_timeout(int ms) { if (ms > 0) s_inlist_wait_ms = ms; }

 esp_err_t pn532_in_list_passive_target(pn532_io_handle_t io_handle) {
     pn532_packetbuffer[0] = PN532_COMMAND_INLISTPASSIVETARGET;
     pn532_packetbuffer[1] = 1;
     pn532_packetbuffer[2] = 0;
 
 #ifdef CONFIG_PN532DEBUG
     ESP_LOGD(TAG, "About to inList passive target");
 #endif
 
     esp_err_t err = pn532_send_command_wait_ack(io_handle, pn532_packetbuffer, 3, PN532_WRITE_TIMEOUT);
     if (ESP_OK != err) {
 #ifdef CONFIG_PN532DEBUG
         ESP_LOGD(TAG, "Could not send inlistPassiveTarget message");
 #endif
         return err;
     }
 
     err = pn532_wait_ready(io_handle, s_inlist_wait_ms);
     if (ESP_OK != err)
         return err;
 
     err = pn532_read_data(io_handle, pn532_packetbuffer, sizeof(pn532_packetbuffer), PN532_READ_TIMEOUT);
     if (ESP_OK != err)
         return err;
 
     if (*(uint32_t*)pn532_packetbuffer == 0x00ff0000)
     {
         uint8_t length = pn532_packetbuffer[3];
         if (0 != ((pn532_packetbuffer[4] + length) & 0xFF))
         {
 #ifdef CONFIG_PN532DEBUG
             ESP_LOGD(TAG, "Length check invalid 0x%.2X 0x%.2X", length, pn532_packetbuffer[4]);
 #endif
             return ESP_FAIL;
         }
         if (pn532_packetbuffer[5] == PN532_PN532TOHOST && pn532_packetbuffer[6] == PN532_RESPONSE_INLISTPASSIVETARGET)
         {
             if (pn532_packetbuffer[7] != 1) {
 #ifdef CONFIG_PN532DEBUG
                 ESP_LOGD(TAG, "Unhandled number of targets inlisted");
 #endif
                 ESP_LOGI(TAG, "Number of tags inListed: %d", pn532_packetbuffer[7]);
                 return ESP_FAIL;
             }
 
             pn532_inListedTag = pn532_packetbuffer[8];
             ESP_LOGI(TAG, "inList tag %d", pn532_inListedTag);
 
             return ESP_OK;
         } else {
 #ifdef CONFIG_PN532DEBUG
             ESP_LOGD(TAG, "Unexpected response to inlist passive host");
 #endif
             return ESP_FAIL;
         }
     }
 
 #ifdef CONFIG_PN532DEBUG
     ESP_LOGD(TAG, "Preamble missing");
 #endif
     return ESP_FAIL;
 }
 
 esp_err_t ntag2xx_get_model(pn532_io_handle_t io_handle, NTAG2XX_MODEL *model)
 {
     if (io_handle == NULL || model == NULL) {
         return ESP_ERR_INVALID_ARG;
     }
 
     *model = NTAG2XX_UNKNOWN;
 
     uint8_t page_mem[16];
     esp_err_t err = ntag2xx_read_page(io_handle, 0, page_mem, sizeof(page_mem));
     if (err != ESP_OK)
         return err;
 
     int raw_capacity = page_mem[14] * 8;
     ESP_LOGD(TAG, "raw capacity: %d bytes", raw_capacity);
 
     switch (page_mem[14]) {
         case 0x12:
             *model = NTAG2XX_NTAG213;
             break;
 
         case 0x3e:
             *model = NTAG2XX_NTAG215;
             break;
 
         case 0x6d:
             *model = NTAG2XX_NTAG216;
             break;
 
         default:
             *model = NTAG2XX_UNKNOWN;
     }
 
     return ESP_OK;
 }
 
 esp_err_t ntag2xx_authenticate(pn532_io_handle_t io_handle, uint8_t page, uint8_t *key, uint8_t *uid, uint8_t uid_length) {
     pn532_packetbuffer[0] = PN532_COMMAND_INDATAEXCHANGE;
     pn532_packetbuffer[1] = 1;
     pn532_packetbuffer[2] = MIFARE_CMD_AUTH_A;
     pn532_packetbuffer[3] = page;
 
     memcpy(&pn532_packetbuffer[4], key, 6);
     if (uid_length > 10) {
         uid_length = 10;
     }
     memcpy(&pn532_packetbuffer[10], uid, uid_length);
 
     esp_err_t err = pn532_send_command_wait_ack(io_handle, pn532_packetbuffer, 10 + uid_length, PN532_WRITE_TIMEOUT);
 
     return err;
 }
 
 esp_err_t ntag2xx_read_page(pn532_io_handle_t io_handle, uint8_t page, uint8_t *buffer, size_t read_len)
 {
     // TAG Type       PAGES   USER START    USER STOP
     // --------       -----   ----------    ---------
     // NTAG 203       42      4             39
     // NTAG 213       45      4             39
     // NTAG 215       135     4             129
     // NTAG 216       231     4             225
 
     if (page >= 231 || read_len == 0) {
 #ifdef CONFIG_MIFAREDEBUG
         ESP_LOGD(TAG, "Page value out of range");
 #endif
         return ESP_ERR_INVALID_ARG;
     }
 
     if (read_len > 16)
         read_len = 16;
 
 #ifdef CONFIG_MIFAREDEBUG
     ESP_LOGD(TAG, "Reading page %d", page);
 #endif
 
     /* Prepare the command */
     pn532_packetbuffer[0] = PN532_COMMAND_INDATAEXCHANGE;
     pn532_packetbuffer[1] = 1; /* Card number */
     pn532_packetbuffer[2] = MIFARE_CMD_READ; /* Mifare Read command = 0x30 */
     pn532_packetbuffer[3] = page; /* Page Number (0..63 in most cases) */
 
     /* Send the command */
     esp_err_t err = pn532_send_command_wait_ack(io_handle, pn532_packetbuffer, 4, PN532_WRITE_TIMEOUT);
     if (err != ESP_OK) {
 #ifdef CONFIG_MIFAREDEBUG
         ESP_LOGD(TAG, "write failed or ACK not received for command");
 #endif
         return err;
     }
 
 #ifdef CONFIG_PN532DEBUG
     ESP_LOGD(TAG, "ntag2xx_ReadPage(): Waiting for IRQ/ready");
 #endif
     err = pn532_wait_ready(io_handle, 100);
     if (ESP_OK != err) {
 #ifdef CONFIG_PN532DEBUG
         ESP_LOGD(TAG, "ntag2xx_ReadPage(): Timeout occurred");
 #endif
         return err;
     }
 
     /* Read the response packet */
     err = pn532_read_data(io_handle, pn532_packetbuffer, 26, PN532_READ_TIMEOUT);
     if (err != ESP_OK)
         return err;
 
 #ifdef CONFIG_MIFAREDEBUG
     ESP_LOGD(TAG, "Received: ");
     ESP_LOG_BUFFER_HEX_LEVEL(TAG, pn532_packetbuffer, 26, ESP_LOG_DEBUG);
 #endif
 
     uint8_t status = pn532_packetbuffer[7];
     // check error code of status byte
     if ((status & 0x3F) == 0x00) {
         memcpy(buffer, pn532_packetbuffer + 8, read_len);
     }
     else {
 #ifdef CONFIG_MIFAREDEBUG
         ESP_LOGD(TAG, "Status byte indicates an error: 0x%02x", pn532_packetbuffer[7]);
 #endif
         return ESP_FAIL;
     }
 
     /* Display data for debug if requested */
 #ifdef CONFIG_MIFAREDEBUG
     ESP_LOGD(TAG, "Page %d", page);
     ESP_LOG_BUFFER_HEX_LEVEL(TAG, buffer, 4, ESP_LOG_DEBUG);
 #endif
 
     // Return OK signal
     return ESP_OK;
 }
 
 esp_err_t ntag2xx_write_page(pn532_io_handle_t io_handle, uint8_t page, const uint8_t * data)
 {
     // TAG Type       PAGES   USER START    USER STOP
     // --------       -----   ----------    ---------
     // NTAG 203       42      4             39
     // NTAG 213       45      4             39
     // NTAG 215       135     4             129
     // NTAG 216       231     4             225
 
     if ((page < 4) || (page > 225)) {
 #ifdef CONFIG_MIFAREDEBUG
         ESP_LOGD(TAG, "Page value out of range");
 #endif
         // Return Failed Signal
         return 0;
     }
 
 #ifdef CONFIG_MIFAREDEBUG
     ESP_LOGD(TAG, "Trying to write 4 byte page %d", page);
 #endif
 
     /* Prepare the first command */
     pn532_packetbuffer[0] = PN532_COMMAND_INDATAEXCHANGE;
     pn532_packetbuffer[1] = 1; /* Card number */
     pn532_packetbuffer[2] = MIFARE_ULTRALIGHT_CMD_WRITE; /* Mifare Ultralight Write command = 0xA2 */
     pn532_packetbuffer[3] = page; /* Page Number (0..63 for most cases) */
     memcpy(pn532_packetbuffer + 4, data, 4); /* Data Payload */
 
     /* Send the command */
     esp_err_t err = pn532_send_command_wait_ack(io_handle, pn532_packetbuffer, 8, PN532_WRITE_TIMEOUT);
     if (err != ESP_OK) {
 #ifdef CONFIG_MIFAREDEBUG
         ESP_LOGD(TAG, "Failed to receive ACK for write command");
 #endif
         return err;
     }
 
 #ifdef CONFIG_PN532DEBUG
     ESP_LOGD(TAG, "ntag2xx_WritePage(): Waiting for IRQ/ready");
 #endif
     err = pn532_wait_ready(io_handle, 100);
     if (ESP_OK != err) {
 #ifdef CONFIG_PN532DEBUG
         ESP_LOGD(TAG, "ntag2xx_WritePage(): Timeout occurred");
 #endif
         return err;
     }
 
     /* Read the response packet */
     err = pn532_read_data(io_handle, pn532_packetbuffer, 26, PN532_READ_TIMEOUT);
     return err;
 }