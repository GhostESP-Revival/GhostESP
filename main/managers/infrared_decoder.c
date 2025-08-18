#include "managers/infrared_decoder.h"
#include "managers/infrared_timings.h"
#include <stdlib.h>
#include <string.h>
#include <esp_log.h>


static const char* TAG = "IR_DECODER";

// Protocol specifications
static const InfraredDecoderProtocolSpec infrared_protocol_nec_decoder = {
    .timings = {
        .preamble_mark = INFRARED_NEC_PREAMBLE_MARK,
        .preamble_space = INFRARED_NEC_PREAMBLE_SPACE,
        .bit1_mark = INFRARED_NEC_BIT1_MARK,
        .bit1_space = INFRARED_NEC_BIT1_SPACE,
        .bit0_mark = INFRARED_NEC_BIT0_MARK,
        .bit0_space = INFRARED_NEC_BIT0_SPACE,
        .preamble_tolerance = INFRARED_NEC_PREAMBLE_TOLERANCE,
        .bit_tolerance = INFRARED_NEC_BIT_TOLERANCE,
        .silence_time = INFRARED_NEC_SILENCE,
        .min_split_time = INFRARED_NEC_MIN_SPLIT_TIME,
    },
    .databit_len = {32, 42, 0, 0},
    .manchester_start_from_space = false,
    .decode = infrared_common_decode_pdwm,
    .decode_repeat = infrared_decoder_nec_decode_repeat,
    .interpret = infrared_decoder_nec_interpret,
    .reset = NULL,
};

static const InfraredDecoderProtocolSpec infrared_protocol_samsung32_decoder = {
    .timings = {
        .preamble_mark = 4500,
        .preamble_space = 4500,
        .bit1_mark = 550,
        .bit1_space = 1650,
        .bit0_mark = 550,
        .bit0_space = 550,
        .preamble_tolerance = 200,
        .bit_tolerance = 120,
        .silence_time = 145000,
        .min_split_time = 5000,
    },
    .databit_len = {32, 0, 0, 0},
    .manchester_start_from_space = false,
    .decode = infrared_common_decode_pdwm,
    .decode_repeat = infrared_decoder_samsung32_decode_repeat,
    .interpret = infrared_decoder_samsung32_interpret,
    .reset = NULL,
};

static const InfraredDecoderProtocolSpec infrared_protocol_sirc_decoder = {
    .timings = {
        .preamble_mark = INFRARED_SIRC_PREAMBLE_MARK,
        .preamble_space = INFRARED_SIRC_PREAMBLE_SPACE,
        .bit1_mark = INFRARED_SIRC_BIT1_MARK,
        .bit1_space = INFRARED_SIRC_BIT1_SPACE,
        .bit0_mark = INFRARED_SIRC_BIT0_MARK,
        .bit0_space = INFRARED_SIRC_BIT0_SPACE,
        .preamble_tolerance = INFRARED_SIRC_PREAMBLE_TOLERANCE,
        .bit_tolerance = INFRARED_SIRC_BIT_TOLERANCE,
        .silence_time = INFRARED_SIRC_SILENCE,
        .min_split_time = INFRARED_SIRC_MIN_SPLIT_TIME,
    },
    .databit_len = {20, 15, 12, 0},
    .manchester_start_from_space = false,
    .decode = infrared_common_decode_pdwm,  // Use PDWM like Flipper Zero
    .decode_repeat = NULL,
    .interpret = infrared_decoder_sirc_interpret,
    .reset = NULL,
};

static const InfraredDecoderProtocolSpec infrared_protocol_rc5_decoder = {
    .timings = {
        .preamble_mark = INFRARED_RC5_PREAMBLE_MARK,
        .preamble_space = INFRARED_RC5_PREAMBLE_SPACE,
        .bit1_mark = INFRARED_RC5_BIT,
        .bit1_space = 0,
        .bit0_mark = 0,
        .bit0_space = 0,
        .preamble_tolerance = INFRARED_RC5_PREAMBLE_TOLERANCE,
        .bit_tolerance = INFRARED_RC5_BIT_TOLERANCE,
        .silence_time = INFRARED_RC5_SILENCE,
        .min_split_time = INFRARED_RC5_MIN_SPLIT_TIME,
    },
    .databit_len = {14, 0, 0, 0}, // 1 + 1 + 1 + 5 + 6
    .manchester_start_from_space = true,
    .decode = infrared_common_decode_manchester,
    .decode_repeat = NULL,
    .interpret = infrared_decoder_rc5_interpret,
    .reset = NULL,
};

static const InfraredDecoderProtocolSpec infrared_protocol_rc6_decoder = {
    .timings = {
        .preamble_mark = INFRARED_RC6_PREAMBLE_MARK,
        .preamble_space = INFRARED_RC6_PREAMBLE_SPACE,
        .bit1_mark = INFRARED_RC6_BIT,
        .bit1_space = 0,
        .bit0_mark = 0,
        .bit0_space = 0,
        .preamble_tolerance = INFRARED_RC6_PREAMBLE_TOLERANCE,
        .bit_tolerance = INFRARED_RC6_BIT_TOLERANCE,
        .silence_time = INFRARED_RC6_SILENCE,
        .min_split_time = INFRARED_RC6_MIN_SPLIT_TIME,
    },
    .databit_len = {21, 0, 0, 0}, // 1 + 3 + 1 + 8 + 8
    .manchester_start_from_space = false,
    .decode = infrared_decoder_rc6_decode_manchester,
    .decode_repeat = NULL,
    .interpret = infrared_decoder_rc6_interpret,
    .reset = NULL,
};

static const InfraredDecoderProtocolSpec infrared_protocol_rca_decoder = {
    .timings = {
        .preamble_mark = 4000,
        .preamble_space = 4000,
        .bit1_mark = 500,
        .bit1_space = 2000,
        .bit0_mark = 500,
        .bit0_space = 1000,
        .preamble_tolerance = 200,
        .bit_tolerance = 120,
        .silence_time = 8000,
        .min_split_time = 4000,
    },
    .databit_len = {24, 0, 0, 0},
    .manchester_start_from_space = false,
    .decode = infrared_common_decode_pdwm,
    .decode_repeat = NULL,
    .interpret = infrared_decoder_rca_interpret,
    .reset = NULL,
};

static const InfraredDecoderProtocolSpec infrared_protocol_pioneer_decoder = {
    .timings = {
        .preamble_mark = 8500,
        .preamble_space = 4225,
        .bit1_mark = 500,
        .bit1_space = 1500,
        .bit0_mark = 500,
        .bit0_space = 500,
        .preamble_tolerance = 200,
        .bit_tolerance = 120,
        .silence_time = 26000,
        .min_split_time = 26000,
    },
    .databit_len = {33, 32, 0, 0},
    .manchester_start_from_space = false,
    .decode = infrared_common_decode_pdwm,
    .decode_repeat = NULL,
    .interpret = infrared_decoder_pioneer_interpret,
    .reset = NULL,
};

static const InfraredDecoderProtocolSpec infrared_protocol_kaseikyo_decoder = {
    .timings = {
        .preamble_mark = 3360,
        .preamble_space = 1665,
        .bit1_mark = 420,
        .bit1_space = 1274,
        .bit0_mark = 420,
        .bit0_space = 420,
        .preamble_tolerance = 200,
        .bit_tolerance = 120,
        .silence_time = 74000,
        .min_split_time = 74000,
    },
    .databit_len = {48, 0, 0, 0},
    .manchester_start_from_space = false,
    .decode = infrared_common_decode_pdwm,
    .decode_repeat = NULL,
    .interpret = infrared_decoder_kaseikyo_interpret,
    .reset = NULL,
};

// Utility function to reverse bits in a byte
uint8_t reverse(uint8_t value) {
    uint8_t result = 0;
    for (int i = 0; i < 8; i++) {
        result = (result << 1) | (value & 1);
        value >>= 1;
    }
    return result;
}

// Convert protocol enum to string
const char* infrared_protocol_to_string(InfraredProtocol protocol) {
    switch (protocol) {
        case InfraredProtocolNEC: return "NEC";
        case InfraredProtocolNECext: return "NECext";
        case InfraredProtocolNEC42: return "NEC42";
        case InfraredProtocolNEC42ext: return "NEC42ext";
        case InfraredProtocolSamsung32: return "Samsung32";
        case InfraredProtocolSIRC: return "SIRC";
        case InfraredProtocolSIRC15: return "SIRC15";
        case InfraredProtocolSIRC20: return "SIRC20";
        case InfraredProtocolRC5: return "RC5";
        case InfraredProtocolRC5X: return "RC5X";
        case InfraredProtocolRC6: return "RC6";
        case InfraredProtocolRCA: return "RCA";
        case InfraredProtocolPioneer: return "Pioneer";
        case InfraredProtocolKaseikyo: return "Kaseikyo";
        default: return "Unknown";
    }
}

// Allocate main decoder context
InfraredDecoderContext* infrared_decoder_alloc(void) {
    InfraredDecoderContext* decoder = malloc(sizeof(InfraredDecoderContext));
    if (!decoder) {
        ESP_LOGE(TAG, "Failed to allocate decoder context");
        return NULL;
    }
    
    memset(decoder, 0, sizeof(InfraredDecoderContext));
    
    // Initialize protocol decoders
    decoder->decoders[0] = infrared_common_decoder_alloc(&infrared_protocol_nec_decoder);
    decoder->decoders[1] = infrared_common_decoder_alloc(&infrared_protocol_samsung32_decoder);
    decoder->decoders[2] = infrared_common_decoder_alloc(&infrared_protocol_sirc_decoder);
    decoder->decoders[3] = infrared_common_decoder_alloc(&infrared_protocol_rc5_decoder);
    decoder->decoders[4] = infrared_common_decoder_alloc(&infrared_protocol_rc6_decoder);
    decoder->decoders[5] = infrared_common_decoder_alloc(&infrared_protocol_rca_decoder);
    decoder->decoders[6] = infrared_common_decoder_alloc(&infrared_protocol_pioneer_decoder);
    decoder->decoders[7] = infrared_common_decoder_alloc(&infrared_protocol_kaseikyo_decoder);
    
    // Set up RC5 decoder context
    if (decoder->decoders[3]) {
        InfraredRc5Decoder* rc5_context = malloc(sizeof(InfraredRc5Decoder));
        if (rc5_context) {
            rc5_context->toggle = false;
            decoder->decoders[3]->context = rc5_context;
        }
    }
    
    // Set up RC6 decoder context
    if (decoder->decoders[4]) {
        InfraredRc6Decoder* rc6_context = malloc(sizeof(InfraredRc6Decoder));
        if (rc6_context) {
            rc6_context->toggle = false;
            decoder->decoders[4]->context = rc6_context;
        }
    }
    
    decoder->decoder_count = 8;
    
    ESP_LOGI(TAG, "Decoder context allocated with %d protocols", decoder->decoder_count);
    return decoder;
}

// Free decoder context
void infrared_decoder_free(InfraredDecoderContext* decoder) {
    if (!decoder) return;
    
    for (int i = 0; i < decoder->decoder_count; i++) {
        if (decoder->decoders[i]) {
            if (decoder->decoders[i]->context) {
                free(decoder->decoders[i]->context);
            }
            infrared_common_decoder_free(decoder->decoders[i]);
        }
    }
    
    free(decoder);
    ESP_LOGI(TAG, "Decoder context freed");
}

// Reset all decoders
void infrared_decoder_reset(InfraredDecoderContext* decoder) {
    if (!decoder) return;
    
    for (int i = 0; i < decoder->decoder_count; i++) {
        if (decoder->decoders[i]) {
            infrared_common_decoder_reset(decoder->decoders[i]);
        }
    }
}

// Main decode function - tries all protocols
InfraredDecodedMessage* infrared_decoder_decode(InfraredDecoderContext* decoder, bool level, uint32_t timing) {
    if (!decoder) return NULL;
    
    for (int i = 0; i < decoder->decoder_count; i++) {
        InfraredCommonDecoder* common_decoder = decoder->decoders[i];
        if (!common_decoder || !common_decoder->protocol) continue;
        
        InfraredDecoderStatus status = common_decoder->protocol->decode(common_decoder, level, timing);
        
        if (status == InfraredDecoderStatusError) {
            ESP_LOGD(TAG, "Protocol %d failed decode - level=%d, timing=%luµs", i, level, timing);
            // Reset decoder on error to prevent state corruption
            infrared_common_decoder_reset(common_decoder);
        } else if (status == InfraredDecoderStatusOk) {
            ESP_LOGD(TAG, "Protocol %d accepted timing - level=%d, timing=%luµs, databit_cnt=%d", i, level, timing, common_decoder->databit_cnt);
        }
        
        if (status == InfraredDecoderStatusReady) {
            ESP_LOGD(TAG, "Protocol %d ready for interpretation, databit_cnt=%d", i, common_decoder->databit_cnt);
            if (common_decoder->protocol->interpret && common_decoder->protocol->interpret(common_decoder)) {
                decoder->last_message = common_decoder->message;
                ESP_LOGI(TAG, "Decoded %s: addr=0x%08lX cmd=0x%08lX repeat=%d (databit_cnt=%d)", 
                        infrared_protocol_to_string(common_decoder->message.protocol),
                        common_decoder->message.address,
                        common_decoder->message.command,
                        common_decoder->message.repeat,
                        common_decoder->databit_cnt);
                return &decoder->last_message;
            } else {
                ESP_LOGD(TAG, "Protocol %d interpretation failed, databit_cnt=%d", i, common_decoder->databit_cnt);
            }
        }
    }
    
    return NULL;
}

// Helper functions for Flipper Zero architecture
static inline size_t consume_samples(uint32_t* array, size_t len, size_t shift) {
    if (len < shift) return 0;
    len -= shift;
    for (size_t i = 0; i < len; ++i) {
        array[i] = array[i + shift];
    }
    return len;
}

static inline void accumulate_lsb(InfraredCommonDecoder* decoder, bool bit) {
    uint16_t index = decoder->databit_cnt / 8;
    uint8_t shift = decoder->databit_cnt % 8; // LSB first

    if (!shift) decoder->data[index] = 0;

    if (bit) {
        decoder->data[index] |= (0x1 << shift); // add 1
    }
    // else add 0 (already cleared)

    ++decoder->databit_cnt;
}

static bool infrared_check_preamble(InfraredCommonDecoder* decoder) {
    if (!decoder || decoder->timings_cnt == 0) return false;

    bool result = false;
    bool start_level = (decoder->level + decoder->timings_cnt + 1) % 2;

    // align to start at Mark timing
    if (!start_level) {
        decoder->timings_cnt = consume_samples(decoder->timings, decoder->timings_cnt, 1);
    }

    if (decoder->protocol->timings.preamble_mark == 0) {
        return true;
    }

    while ((!result) && (decoder->timings_cnt >= 2)) {
        float preamble_tolerance = decoder->protocol->timings.preamble_tolerance;
        uint16_t preamble_mark = decoder->protocol->timings.preamble_mark;
        uint16_t preamble_space = decoder->protocol->timings.preamble_space;

        if ((MATCH_TIMING(decoder->timings[0], preamble_mark, preamble_tolerance)) &&
           (MATCH_TIMING(decoder->timings[1], preamble_space, preamble_tolerance))) {
            result = true;
        }

        decoder->timings_cnt = consume_samples(decoder->timings, decoder->timings_cnt, 2);
    }

    return result;
}

static InfraredDecoderStatus infrared_common_decode_bits(InfraredCommonDecoder* decoder) {
    if (!decoder) return InfraredDecoderStatusError;

    InfraredDecoderStatus status = InfraredDecoderStatusOk;
    const InfraredTimings* timings = &decoder->protocol->timings;

    while (decoder->timings_cnt && (status == InfraredDecoderStatusOk)) {
        bool level = (decoder->level + decoder->timings_cnt + 1) % 2;
        uint32_t timing = decoder->timings[0];

        // Check for min_split_time (long space) - this is the key Flipper Zero logic
        if (timings->min_split_time && !level) {
            if (timing > timings->min_split_time) {
                // Long low timing - check if we're ready for any protocol variant
                for (size_t i = 0; i < 4 && decoder->protocol->databit_len[i]; ++i) {
                    if (decoder->protocol->databit_len[i] == decoder->databit_cnt) {
                        ESP_LOGD(TAG, "min_split_time detected: timing=%luµs > %luµs, databit_cnt=%d matches variant %zu",
                               timing, timings->min_split_time, decoder->databit_cnt, i);
                        return InfraredDecoderStatusReady;
                    }
                }
            } else if (decoder->protocol->databit_len[0] == decoder->databit_cnt) {
                // Short low timing for longest protocol - signal is longer than expected
                ESP_LOGD(TAG, "Signal longer than expected: timing=%luµs <= %luµs, databit_cnt=%d",
                       timing, timings->min_split_time, decoder->databit_cnt);
                return InfraredDecoderStatusError;
            }
        }

        // Decode the current timing
        status = decoder->protocol->decode(decoder, level, timing);
        if (status == InfraredDecoderStatusError) {
            ESP_LOGD(TAG, "Decode error at databit_cnt=%d, level=%d, timing=%luµs", 
                   decoder->databit_cnt, level, timing);
            break;
        }
        
        decoder->timings_cnt = consume_samples(decoder->timings, decoder->timings_cnt, 1);

        // Check if largest protocol version can be decoded (for protocols without min_split_time)
        if (level && (decoder->protocol->databit_len[0] == decoder->databit_cnt) && !timings->min_split_time) {
            ESP_LOGD(TAG, "Max bits reached without min_split_time: databit_cnt=%d", decoder->databit_cnt);
            status = InfraredDecoderStatusReady;
            break;
        }
    }

    return status;
}

InfraredDecodedMessage* infrared_common_decoder_check_ready_internal(InfraredCommonDecoder* decoder) {
    if (!decoder) return NULL;
    
    InfraredDecodedMessage* message = NULL;
    bool found_length = false;

    // Check if current bit count matches any valid length
    for (size_t i = 0; i < 4 && decoder->protocol->databit_len[i]; ++i) {
        if (decoder->protocol->databit_len[i] == decoder->databit_cnt) {
            found_length = true;
            ESP_LOGD(TAG, "Found valid length: databit_cnt=%d matches variant %zu", decoder->databit_cnt, i);
            break;
        }
    }

    if (found_length && decoder->protocol->interpret && decoder->protocol->interpret(decoder)) {
        ESP_LOGD(TAG, "Interpretation successful for databit_cnt=%d", decoder->databit_cnt);
        decoder->databit_cnt = 0;
        message = &decoder->message;
        if (decoder->protocol->decode_repeat) {
            decoder->state = InfraredCommonDecoderStateProcessRepeat;
        } else {
            decoder->state = InfraredCommonDecoderStateWaitPreamble;
        }
    } else {
        ESP_LOGD(TAG, "Interpretation failed: found_length=%d, databit_cnt=%d", found_length, decoder->databit_cnt);
    }

    return message;
}

static void infrared_common_decoder_reset_state(InfraredCommonDecoder* decoder) {
    if (!decoder) return;
    
    decoder->state = InfraredCommonDecoderStateWaitPreamble;
    decoder->databit_cnt = 0;
    decoder->switch_detect = false;
    decoder->message.protocol = InfraredProtocolUnknown;
    
    if (decoder->protocol->timings.preamble_mark == 0) {
        if (decoder->timings_cnt > 0) {
            decoder->timings_cnt = consume_samples(decoder->timings, decoder->timings_cnt, 1);
        }
    }
}

// Main Flipper Zero decode function
InfraredDecodedMessage* infrared_common_decode(InfraredCommonDecoder* decoder, bool level, uint32_t duration) {
    if (!decoder) return NULL;

    InfraredDecodedMessage* message = NULL;
    InfraredDecoderStatus status = InfraredDecoderStatusError;

    // Reset if level didn't change (should alternate)
    if (decoder->level == level) {
        infrared_common_decoder_reset(decoder);
    }
    decoder->level = level; // start with low level (Space timing)

    // Add timing to buffer
    if (decoder->timings_cnt < INFRARED_MAX_TIMINGS) {
        decoder->timings[decoder->timings_cnt] = duration;
        decoder->timings_cnt++;
    } else {
        ESP_LOGW(TAG, "Timing buffer overflow, resetting decoder");
        infrared_common_decoder_reset(decoder);
        return NULL;
    }

    // State machine processing
    while (1) {
        switch (decoder->state) {
        case InfraredCommonDecoderStateWaitPreamble:
            if (infrared_check_preamble(decoder)) {
                ESP_LOGD(TAG, "Preamble detected, switching to decode state");
                decoder->state = InfraredCommonDecoderStateDecode;
                decoder->databit_cnt = 0;
                decoder->switch_detect = false;
                continue;
            }
            break;
            
        case InfraredCommonDecoderStateDecode:
            status = infrared_common_decode_bits(decoder);
            if (status == InfraredDecoderStatusReady) {
                message = infrared_common_decoder_check_ready_internal(decoder);
                if (message) {
                    continue;
                } else if (decoder->protocol->databit_len[0] == decoder->databit_cnt) {
                    // Error: can't decode largest protocol - begin from start
                    ESP_LOGD(TAG, "Cannot decode largest protocol variant, resetting");
                    decoder->state = InfraredCommonDecoderStateWaitPreamble;
                }
            } else if (status == InfraredDecoderStatusError) {
                ESP_LOGD(TAG, "Decode error, resetting state");
                infrared_common_decoder_reset_state(decoder);
                continue;
            }
            break;
            
        // Handle legacy states (not used in Flipper Zero architecture)
        case InfraredDecoderStateIdle:
        case InfraredDecoderStatePreambleMark:
        case InfraredDecoderStatePreambleSpace:
        case InfraredDecoderStateData:
            // These states are not used in the new architecture
            decoder->state = InfraredCommonDecoderStateWaitPreamble;
            break;
            
        case InfraredCommonDecoderStateProcessRepeat:
            if (decoder->protocol->decode_repeat) {
                status = decoder->protocol->decode_repeat(decoder);
                if (status == InfraredDecoderStatusError) {
                    infrared_common_decoder_reset_state(decoder);
                    continue;
                } else if (status == InfraredDecoderStatusReady) {
                    decoder->message.repeat = true;
                    message = &decoder->message;
                }
            }
            break;
        }
        break;
    }

    return message;
}

// Allocate common decoder
InfraredCommonDecoder* infrared_common_decoder_alloc(const InfraredDecoderProtocolSpec* protocol) {
    if (!protocol) return NULL;
    
    // Validate protocol databit_len array - find maximum bit length
    uint32_t max_bits = 0;
    for (size_t i = 0; i < 4; ++i) {
        if (protocol->databit_len[i] > max_bits) {
            max_bits = protocol->databit_len[i];
        }
    }
    
    if (max_bits == 0) {
        ESP_LOGE(TAG, "Invalid protocol: no valid databit_len found");
        return NULL;
    }
    
    // Calculate size needed for data buffer
    size_t data_size = (max_bits + 7) / 8;  // Round up to nearest byte
    size_t total_size = sizeof(InfraredCommonDecoder) + data_size;
    
    InfraredCommonDecoder* decoder = (InfraredCommonDecoder*)malloc(total_size);
    if (!decoder) return NULL;
    
    memset(decoder, 0, total_size);
    decoder->protocol = protocol;
    decoder->state = InfraredDecoderStateIdle;  // Simple architecture initial state
    decoder->level = true;  // Initialize level
    
    return decoder;
}

// Free common decoder
void infrared_common_decoder_free(InfraredCommonDecoder* decoder) {
    if (decoder) {
        free(decoder);
    }
}

// Reset common decoder
void infrared_common_decoder_reset(InfraredCommonDecoder* decoder) {
    if (!decoder) return;
    
    // Simple architecture reset logic
    decoder->timings_cnt = 0;
    decoder->databit_cnt = 0;
    decoder->switch_detect = false;
    decoder->level = true;
    decoder->state = InfraredDecoderStateIdle;  // Use simple state for PDWM
    decoder->message.protocol = InfraredProtocolUnknown;
    decoder->message.address = 0;
    decoder->message.command = 0;
    decoder->message.repeat = false;
    memset(decoder->data, 0, sizeof(decoder->data));
    memset(decoder->timings, 0, sizeof(decoder->timings));
}

// Check if decoder is ready (public interface)
InfraredDecodedMessage* infrared_common_decoder_check_ready(InfraredCommonDecoder* decoder) {
    if (!decoder) return NULL;
    
    // Use the internal function for actual logic
    return infrared_common_decoder_check_ready_internal(decoder);
}

// Common PDWM (Pulse Distance Width Modulation) decoder
InfraredDecoderStatus infrared_common_decode_pdwm(InfraredCommonDecoder* decoder, bool level, uint32_t timing) {
    if (!decoder || !decoder->protocol) {
        ESP_LOGE(TAG, "PDWM: Invalid decoder state: %d", decoder ? decoder->state : -1);
        return InfraredDecoderStatusError;
    }
    
    // Handle end-of-signal (timing=0) - check if we have a valid bit count
    if (timing == 0 && decoder->state == InfraredDecoderStateData) {
        ESP_LOGD(TAG, "PDWM: End of signal detected, checking for valid bit count (%lu)", (unsigned long)decoder->databit_cnt);
        for (int i = 0; i < 4 && decoder->protocol->databit_len[i]; i++) {
            if (decoder->protocol->databit_len[i] == decoder->databit_cnt) {
                ESP_LOGD(TAG, "PDWM: Valid bit count (%lu) found at end of signal, ready for interpretation", 
                         (unsigned long)decoder->databit_cnt);
                return InfraredDecoderStatusReady;
            }
        }
        ESP_LOGD(TAG, "PDWM: No valid bit count found at end of signal (%lu)", (unsigned long)decoder->databit_cnt);
        return InfraredDecoderStatusError;
    }
    
    const InfraredTimings* timings = &decoder->protocol->timings;
    uint32_t bit_tolerance = timings->bit_tolerance;
    uint16_t bit1_mark = timings->bit1_mark;
    uint16_t bit1_space = timings->bit1_space;
    uint16_t bit0_mark = timings->bit0_mark;
    uint16_t bit0_space = timings->bit0_space;

    // State machine for PDWM decoding
    switch (decoder->state) {
        case InfraredDecoderStateIdle:
            // Waiting for preamble mark
            if (level && MATCH_TIMING(timing, timings->preamble_mark, timings->preamble_tolerance)) {
                decoder->state = InfraredDecoderStatePreambleMark;
                ESP_LOGD(TAG, "PDWM: Preamble mark detected: %luµs", timing);
                return InfraredDecoderStatusOk;
            }
            return InfraredDecoderStatusError;
            
        case InfraredDecoderStatePreambleMark:
            // Waiting for preamble space
            if (!level && MATCH_TIMING(timing, timings->preamble_space, timings->preamble_tolerance)) {
                decoder->state = InfraredDecoderStateData;
                decoder->databit_cnt = 0;
                memset(decoder->data, 0, sizeof(decoder->data));
                ESP_LOGD(TAG, "PDWM: Preamble complete, starting data decode");
                return InfraredDecoderStatusOk;
            }
            decoder->state = InfraredDecoderStateIdle;
            return InfraredDecoderStatusError;
            
        case InfraredDecoderStateData: {
            // PDWM logic: determine which timing carries the bit information
            // For SIRC: bit1_mark != bit0_mark, so analyze_timing = level ^ false = level
            bool analyze_timing = level ^ (timings->bit1_mark == timings->bit0_mark);
            uint32_t bit1_timing = level ? timings->bit1_mark : timings->bit1_space;
            uint32_t bit0_timing = level ? timings->bit0_mark : timings->bit0_space;
            uint32_t no_info_timing = (timings->bit1_mark == timings->bit0_mark) ? timings->bit1_mark : timings->bit1_space;
            
            if (analyze_timing) {
                // This timing carries bit information - decode it
                bool bit_value;
                if (MATCH_TIMING(timing, bit1_timing, timings->bit_tolerance)) {
                    bit_value = true;
                } else if (MATCH_TIMING(timing, bit0_timing, timings->bit_tolerance)) {
                    bit_value = false;
                } else {
                    ESP_LOGD(TAG, "PDWM: Invalid %s timing: %luµs (bit1=%lu±%lu, bit0=%lu±%lu)", 
                             level ? "mark" : "space", timing, bit1_timing, timings->bit_tolerance, 
                             bit0_timing, timings->bit_tolerance);
                    return InfraredDecoderStatusError;
                }
                
                // Store bit LSB-first (like Flipper Zero)
                uint8_t byte_index = decoder->databit_cnt / 8;
                uint8_t bit_index = decoder->databit_cnt % 8;
                
                if (byte_index < sizeof(decoder->data)) {
                    if (!bit_index) decoder->data[byte_index] = 0;  // Clear byte on first bit
                    if (bit_value) {
                        decoder->data[byte_index] |= (1 << bit_index);  // LSB first
                    }
                    decoder->databit_cnt++;
                    
                    ESP_LOGD(TAG, "PDWM: Bit %lu = %d (%s=%luµs, total bits: %lu)", 
                             (unsigned long)(decoder->databit_cnt - 1), bit_value, level ? "mark" : "space", 
                             timing, (unsigned long)decoder->databit_cnt);
                    
                    // Check if we have a valid bit count for any SIRC variant
                    for (int i = 0; i < 4 && decoder->protocol->databit_len[i]; i++) {
                        if (decoder->protocol->databit_len[i] == decoder->databit_cnt) {
                            ESP_LOGD(TAG, "PDWM: Valid bit count (%lu) reached for variant %d, checking for completion", 
                                   (unsigned long)decoder->databit_cnt, i);
                            // For protocols with min_split_time, wait for the long space
                            // For others, or if this is the maximum variant, signal ready
                            if (!timings->min_split_time || i == 0) {
                                ESP_LOGD(TAG, "PDWM: Ready for interpretation (no min_split_time or max variant)");
                                return InfraredDecoderStatusReady;
                            }
                            break;
                        }
                    }
                }
            } else {
                // This timing doesn't carry bit info - validate it and check for min_split_time
                if (timings->min_split_time && !level && timing > timings->min_split_time) {
                    // Long space detected - check if we have a valid bit count (like Flipper Zero)
                    ESP_LOGD(TAG, "PDWM: Long space detected (%luµs > %luµs), checking for valid bit count", 
                             timing, timings->min_split_time);
                    
                    for (int i = 0; i < 4 && decoder->protocol->databit_len[i]; i++) {
                        if (decoder->protocol->databit_len[i] == decoder->databit_cnt) {
                            ESP_LOGD(TAG, "PDWM: Valid bit count (%lu) found, ready for interpretation", 
                                     (unsigned long)decoder->databit_cnt);
                            return InfraredDecoderStatusReady;
                        }
                    }
                    ESP_LOGD(TAG, "PDWM: No valid bit count found (%lu), continuing", (unsigned long)decoder->databit_cnt);
                } else if (!MATCH_TIMING(timing, no_info_timing, timings->bit_tolerance)) {
                    ESP_LOGD(TAG, "PDWM: Invalid %s timing: %luµs (expected %lu±%lu)", 
                             level ? "mark" : "space", timing, no_info_timing, timings->bit_tolerance);
                    return InfraredDecoderStatusError;
                }
            }
            break;
        }
            
        default:
            ESP_LOGE(TAG, "PDWM: Invalid decoder state: %d", (int)decoder->state);
            return InfraredDecoderStatusError;
    }
    
    return InfraredDecoderStatusOk;
}

// Common Manchester decoder
InfraredDecoderStatus infrared_common_decode_manchester(InfraredCommonDecoder* decoder, bool level, uint32_t timing) {
    if (!decoder || !decoder->protocol) return InfraredDecoderStatusError;
    
    const InfraredTimings* timings = &decoder->protocol->timings;
    uint32_t bit_time = timings->bit1_mark;
    uint32_t tolerance = timings->bit_tolerance;
    
    bool single_timing = MATCH_TIMING(timing, bit_time, tolerance);
    bool double_timing = MATCH_TIMING(timing, 2 * bit_time, tolerance);
    
    if (!single_timing && !double_timing) {
        return InfraredDecoderStatusError;
    }
    
    // Manchester decoding logic
    if (single_timing) {
        if (decoder->switch_detect) {
            // Complete bit
            uint8_t byte_index = decoder->databit_cnt / 8;
            uint8_t bit_index = decoder->databit_cnt % 8;
            
            if (byte_index < sizeof(decoder->data)) {
                bool bit_value = decoder->protocol->manchester_start_from_space ? !level : level;
                if (bit_value) {
                    decoder->data[byte_index] |= (1 << bit_index);
                }
                decoder->databit_cnt++;
                decoder->switch_detect = false;
                
                // Check completion
                if (decoder->databit_cnt >= decoder->protocol->databit_len[0]) {
                    return InfraredDecoderStatusReady;
                }
            }
        } else {
            decoder->switch_detect = true;
        }
    } else if (double_timing) {
        // Double timing - complete bit immediately
        uint8_t byte_index = decoder->databit_cnt / 8;
        uint8_t bit_index = decoder->databit_cnt % 8;
        
        if (byte_index < sizeof(decoder->data)) {
            bool bit_value = decoder->protocol->manchester_start_from_space ? level : !level;
            if (bit_value) {
                decoder->data[byte_index] |= (1 << bit_index);
            }
            decoder->databit_cnt++;
            
            // Check completion
            if (decoder->databit_cnt >= decoder->protocol->databit_len[0]) {
                return InfraredDecoderStatusReady;
            }
        }
    }
    
    return InfraredDecoderStatusOk;
}

// NEC protocol interpreter
bool infrared_decoder_nec_interpret(InfraredCommonDecoder* decoder) {
    if (!decoder) return false;
    
    bool result = false;
    
    if (decoder->databit_cnt == 32) {
        uint8_t address = decoder->data[0];
        uint8_t address_inverse = decoder->data[1];
        uint8_t command = decoder->data[2];
        uint8_t command_inverse = decoder->data[3];
        uint8_t inverse_command_inverse = (uint8_t)~command_inverse;
        uint8_t inverse_address_inverse = (uint8_t)~address_inverse;
        
        if ((command == inverse_command_inverse) && (address == inverse_address_inverse)) {
            decoder->message.protocol = InfraredProtocolNEC;
            decoder->message.address = address;
            decoder->message.command = command;
            decoder->message.repeat = false;
            result = true;
        } else {
            decoder->message.protocol = InfraredProtocolNECext;
            decoder->message.address = decoder->data[0] | (decoder->data[1] << 8);
            decoder->message.command = decoder->data[2] | (decoder->data[3] << 8);
            decoder->message.repeat = false;
            result = true;
        }
    } else if (decoder->databit_cnt == 42) {
        uint32_t* data1 = (void*)decoder->data;
        uint16_t* data2 = (void*)(data1 + 1);
        uint16_t address = *data1 & 0x1FFF;
        uint16_t address_inverse = (*data1 >> 13) & 0x1FFF;
        uint16_t command = ((*data1 >> 26) & 0x3F) | ((*data2 & 0x3) << 6);
        uint16_t command_inverse = (*data2 >> 2) & 0xFF;
        
        if ((address == (~address_inverse & 0x1FFF)) && (command == (~command_inverse & 0xFF))) {
            decoder->message.protocol = InfraredProtocolNEC42;
            decoder->message.address = address;
            decoder->message.command = command;
            decoder->message.repeat = false;
            result = true;
        } else {
            decoder->message.protocol = InfraredProtocolNEC42ext;
            decoder->message.address = address | (address_inverse << 13);
            decoder->message.command = command | (command_inverse << 8);
            decoder->message.repeat = false;
            result = true;
        }
    }
    
    return result;
}

// Samsung32 protocol interpreter
bool infrared_decoder_samsung32_interpret(InfraredCommonDecoder* decoder) {
    if (!decoder) return false;
    
    bool result = false;
    uint8_t address1 = decoder->data[0];
    uint8_t address2 = decoder->data[1];
    uint8_t command = decoder->data[2];
    uint8_t command_inverse = decoder->data[3];
    uint8_t inverse_command_inverse = (uint8_t)~command_inverse;
    
    if ((address1 == address2) && (command == inverse_command_inverse)) {
        decoder->message.command = command;
        decoder->message.address = address1;
        decoder->message.protocol = InfraredProtocolSamsung32;
        decoder->message.repeat = false;
        result = true;
    }
    
    return result;
}

// SIRC protocol interpreter
bool infrared_decoder_sirc_interpret(InfraredCommonDecoder* decoder) {
    if (!decoder) return false;
    
    uint32_t* data = (void*)&decoder->data[0];
    uint16_t address = 0;
    uint8_t command = 0;
    InfraredProtocol protocol = InfraredProtocolUnknown;
    
    ESP_LOGD(TAG, "SIRC interpreter: databit_cnt=%lu, data=0x%08lX", (unsigned long)decoder->databit_cnt, *data);
    
    if (decoder->databit_cnt == 12) {
        address = (*data >> 7) & 0x1F;
        command = *data & 0x7F;
        protocol = InfraredProtocolSIRC;
        ESP_LOGD(TAG, "SIRC: 12-bit variant selected");
    } else if (decoder->databit_cnt == 15) {
        address = (*data >> 7) & 0xFF;
        command = *data & 0x7F;
        protocol = InfraredProtocolSIRC15;
        ESP_LOGD(TAG, "SIRC: 15-bit variant selected");
    } else if (decoder->databit_cnt == 20) {
        address = (*data >> 7) & 0x1FFF;
        command = *data & 0x7F;
        protocol = InfraredProtocolSIRC20;
        ESP_LOGD(TAG, "SIRC: 20-bit variant selected");
    } else {
        ESP_LOGD(TAG, "SIRC: Invalid bit count %lu", (unsigned long)decoder->databit_cnt);
        return false;
    }
    
    decoder->message.protocol = protocol;
    decoder->message.address = address;
    decoder->message.command = command;
    decoder->message.repeat = false;
    
    ESP_LOGD(TAG, "SIRC interpreter result: protocol=%d, address=0x%04X, command=0x%02X", 
             (int)protocol, address, command);
    
    return true;
}

// RC5 protocol interpreter
bool infrared_decoder_rc5_interpret(InfraredCommonDecoder* decoder) {
    if (!decoder) return false;
    
    // RC5 must be exactly 14 bits - reject anything else
    if (decoder->databit_cnt != 14) {
        ESP_LOGD(TAG, "RC5: Invalid bit count %lu (expected 14)", (unsigned long)decoder->databit_cnt);
        return false;
    }
    
    bool result = false;
    uint32_t* data = (void*)&decoder->data[0];
    
    ESP_LOGD(TAG, "RC5: Raw data before inversion: 0x%08lX 0x%08lX (bits=%d)", 
             decoder->data[0], decoder->data[1], decoder->databit_cnt);
    
    /* Manchester (inverse):
     *      0->1 : 1
     *      1->0 : 0
     */
    decoder->data[0] = ~decoder->data[0];
    decoder->data[1] = ~decoder->data[1];
    
    ESP_LOGD(TAG, "RC5: Raw data after inversion: 0x%08lX 0x%08lX", 
             decoder->data[0], decoder->data[1]);
    
    // MSB first
    uint8_t address = reverse((uint8_t)decoder->data[0]) & 0x1F;
    uint8_t command = (reverse((uint8_t)decoder->data[1]) >> 2) & 0x3F;
    bool start_bit1 = *data & 0x01;
    bool start_bit2 = *data & 0x02;
    bool toggle = !!(*data & 0x04);
    
    if (start_bit1 == 1) {
        InfraredProtocol protocol = start_bit2 ? InfraredProtocolRC5 : InfraredProtocolRC5X;
        InfraredDecodedMessage* message = &decoder->message;
        InfraredRc5Decoder* rc5_decoder = decoder->context;
        
        if (rc5_decoder) {
            bool* prev_toggle = &rc5_decoder->toggle;
            if ((message->address == address) && (message->command == command) &&
               (message->protocol == protocol)) {
                message->repeat = (toggle == *prev_toggle);
            } else {
                message->repeat = false;
            }
            *prev_toggle = toggle;
        } else {
            message->repeat = false;
        }
        
        message->command = command;
        message->address = address;
        message->protocol = protocol;
        result = true;
    }
    
    return result;
}

// RC6 protocol interpreter
bool infrared_decoder_rc6_interpret(InfraredCommonDecoder* decoder) {
    if (!decoder) return false;
    
    bool result = false;
    uint32_t* data = (void*)&decoder->data[0];
    
    // MSB first
    uint8_t address = reverse((uint8_t)(*data >> 5));
    uint8_t command = reverse((uint8_t)(*data >> 13));
    bool start_bit = *data & 0x01;
    bool toggle = !!(*data & 0x10);
    uint8_t mode = (*data >> 1) & 0x7;
    
    if ((start_bit == 1) && (mode == 0)) {
        InfraredDecodedMessage* message = &decoder->message;
        InfraredRc6Decoder* rc6_decoder = decoder->context;
        
        if (rc6_decoder) {
            bool* prev_toggle = &rc6_decoder->toggle;
            if ((message->address == address) && (message->command == command) &&
               (message->protocol == InfraredProtocolRC6)) {
                message->repeat = (toggle == *prev_toggle);
            } else {
                message->repeat = false;
            }
            *prev_toggle = toggle;
        } else {
            message->repeat = false;
        }
        
        message->command = command;
        message->address = address;
        message->protocol = InfraredProtocolRC6;
        result = true;
    }
    
    return result;
}

// NEC repeat decoder
InfraredDecoderStatus infrared_decoder_nec_decode_repeat(InfraredCommonDecoder* decoder) {
    if (!decoder) return InfraredDecoderStatusError;
    
    float preamble_tolerance = decoder->protocol->timings.preamble_tolerance;
    uint32_t bit_tolerance = decoder->protocol->timings.bit_tolerance;
    InfraredDecoderStatus status = InfraredDecoderStatusError;
    
    if (decoder->timings_cnt < 4) return InfraredDecoderStatusOk;
    
    if ((decoder->timings[0] > INFRARED_NEC_REPEAT_PAUSE_MIN) &&
       (decoder->timings[0] < INFRARED_NEC_REPEAT_PAUSE_MAX) &&
       MATCH_TIMING(decoder->timings[1], INFRARED_NEC_REPEAT_MARK, preamble_tolerance) &&
       MATCH_TIMING(decoder->timings[2], INFRARED_NEC_REPEAT_SPACE, preamble_tolerance) &&
       MATCH_TIMING(decoder->timings[3], decoder->protocol->timings.bit1_mark, bit_tolerance)) {
        status = InfraredDecoderStatusReady;
        decoder->timings_cnt = 0;
    } else {
        status = InfraredDecoderStatusError;
    }
    
    return status;
}

// Samsung32 repeat decoder
InfraredDecoderStatus infrared_decoder_samsung32_decode_repeat(InfraredCommonDecoder* decoder) {
    if (!decoder) return InfraredDecoderStatusError;
    
    float preamble_tolerance = decoder->protocol->timings.preamble_tolerance;
    uint32_t bit_tolerance = decoder->protocol->timings.bit_tolerance;
    InfraredDecoderStatus status = InfraredDecoderStatusError;
    
    if (decoder->timings_cnt < 6) return InfraredDecoderStatusOk;
    
    if ((decoder->timings[0] > INFRARED_SAMSUNG_REPEAT_PAUSE_MIN) &&
       (decoder->timings[0] < INFRARED_SAMSUNG_REPEAT_PAUSE_MAX) &&
       MATCH_TIMING(decoder->timings[1], INFRARED_SAMSUNG_REPEAT_MARK, preamble_tolerance) &&
       MATCH_TIMING(decoder->timings[2], INFRARED_SAMSUNG_REPEAT_SPACE, preamble_tolerance) &&
       MATCH_TIMING(decoder->timings[3], decoder->protocol->timings.bit1_mark, bit_tolerance) &&
       MATCH_TIMING(decoder->timings[4], decoder->protocol->timings.bit1_space, bit_tolerance) &&
       MATCH_TIMING(decoder->timings[5], decoder->protocol->timings.bit1_mark, bit_tolerance)) {
        status = InfraredDecoderStatusReady;
        decoder->timings_cnt = 0;
    } else {
        status = InfraredDecoderStatusError;
    }
    
    return status;
}

// RC6 special Manchester decoder (handles 4th bit double timing)
InfraredDecoderStatus infrared_decoder_rc6_decode_manchester(InfraredCommonDecoder* decoder, bool level, uint32_t timing) {
    if (!decoder || !decoder->protocol) return InfraredDecoderStatusError;
    
    InfraredDecoderStatus status = InfraredDecoderStatusError;
    uint32_t bit = decoder->protocol->timings.bit1_mark;
    uint32_t tolerance = decoder->protocol->timings.bit_tolerance;
    
    bool single_timing = MATCH_TIMING(timing, bit, tolerance);
    bool double_timing = MATCH_TIMING(timing, 2 * bit, tolerance);
    bool triple_timing = MATCH_TIMING(timing, 3 * bit, tolerance);
    
    if (decoder->databit_cnt == 4) {
        // 4th bit (toggle) lasts 2x times more
        if (single_timing ^ triple_timing) {
            ++decoder->databit_cnt;
            decoder->data[0] |= (single_timing ? !level : level) << 4;
            status = InfraredDecoderStatusOk;
        }
    } else if (decoder->databit_cnt == 5) {
        if (single_timing || triple_timing) {
            if (triple_timing) timing = bit;
            decoder->switch_detect = false;
            status = infrared_common_decode_manchester(decoder, level, timing);
        } else if (double_timing) {
            status = InfraredDecoderStatusOk;
        }
    } else {
        status = infrared_common_decode_manchester(decoder, level, timing);
    }
    
    return status;
}

// RCA protocol interpreter
bool infrared_decoder_rca_interpret(InfraredCommonDecoder* decoder) {
    if (!decoder) return false;
    
    uint32_t* data = (void*)&decoder->data;
    
    uint8_t address = (*data & 0xF);
    uint8_t command = (*data >> 4) & 0xFF;
    uint8_t address_inverse = (*data >> 12) & 0xF;
    uint8_t command_inverse = (*data >> 16) & 0xFF;
    uint8_t inverse_address_inverse = (uint8_t)~address_inverse & 0xF;
    uint8_t inverse_command_inverse = (uint8_t)~command_inverse;
    
    if ((command == inverse_command_inverse) && (address == inverse_address_inverse)) {
        decoder->message.protocol = InfraredProtocolRCA;
        decoder->message.address = address;
        decoder->message.command = command;
        decoder->message.repeat = false;
        return true;
    }
    
    return false;
}

bool infrared_decoder_pioneer_interpret(InfraredCommonDecoder* decoder) {
    if (!decoder) return false;
    
    uint32_t* data = (void*)&decoder->data[0];
    uint8_t address = 0;
    uint8_t command = 0;
    InfraredProtocol protocol = InfraredProtocolUnknown;
    
    if (decoder->databit_cnt == decoder->protocol->databit_len[0] ||
       decoder->databit_cnt == decoder->protocol->databit_len[1]) {
        address = *data & 0xFF;
        uint8_t real_address_checksum = ~address;
        uint8_t address_checksum = (*data >> 8) & 0xFF;
        command = (*data >> 16) & 0xFF;
        uint8_t real_command_checksum = ~command;
        uint8_t command_checksum = (*data >> 24) & 0xFF;
        
        if (address_checksum != real_address_checksum) {
            return false;
        }
        if (command_checksum != real_command_checksum) {
            return false;
        }
        protocol = InfraredProtocolPioneer;
    } else {
        return false;
    }
    
    decoder->message.protocol = protocol;
    decoder->message.address = address;
    decoder->message.command = command;
    decoder->message.repeat = false;
    
    return true;
}

bool infrared_decoder_kaseikyo_interpret(InfraredCommonDecoder* decoder) {
    if (!decoder) return false;
    
    bool result = false;
    uint16_t vendor_id = ((uint16_t)(decoder->data[1]) << 8) | (uint16_t)decoder->data[0];
    uint8_t vendor_parity = decoder->data[2] & 0x0f;
    uint8_t genre1 = decoder->data[2] >> 4;
    uint8_t genre2 = decoder->data[3] & 0x0f;
    uint16_t data = (uint16_t)(decoder->data[3] >> 4) | ((uint16_t)(decoder->data[4] & 0x3f) << 4);
    uint8_t id = decoder->data[4] >> 6;
    uint8_t parity = decoder->data[5];
    
    uint8_t vendor_parity_check = decoder->data[0] ^ decoder->data[1];
    vendor_parity_check = (vendor_parity_check & 0xf) ^ (vendor_parity_check >> 4);
    uint8_t parity_check = decoder->data[2] ^ decoder->data[3] ^ decoder->data[4];
    
    if (vendor_parity == vendor_parity_check && parity == parity_check) {
        decoder->message.command = (uint32_t)data;
        decoder->message.address = ((uint32_t)id << 24) | ((uint32_t)vendor_id << 8) |
                                   ((uint32_t)genre1 << 4) | (uint32_t)genre2;
        decoder->message.protocol = InfraredProtocolKaseikyo;
        decoder->message.repeat = false;
        result = true;
    }
    
    return result;
}
