#ifndef INFRARED_DECODER_H
#define INFRARED_DECODER_H

#include <stdbool.h>
#include <stdint.h>
#include "infrared_common.h"

// Function declarations
InfraredDecoderContext* infrared_decoder_alloc(void);
void infrared_decoder_free(InfraredDecoderContext* decoder);
void infrared_decoder_reset(InfraredDecoderContext* decoder);
InfraredDecodedMessage* infrared_decoder_decode(InfraredDecoderContext* decoder, bool level, uint32_t timing);

// Common decoder functions
InfraredCommonDecoder* infrared_common_decoder_alloc(const InfraredDecoderProtocolSpec* protocol);
void infrared_common_decoder_free(InfraredCommonDecoder* decoder);
void infrared_common_decoder_reset(InfraredCommonDecoder* decoder);
InfraredDecodedMessage* infrared_common_decoder_check_ready(InfraredCommonDecoder* decoder);

// Main Flipper Zero decode function
InfraredDecodedMessage* infrared_common_decode(InfraredCommonDecoder* decoder, bool level, uint32_t duration);

// Common decoding algorithms
InfraredDecoderStatus infrared_common_decode_pdwm(InfraredCommonDecoder* decoder, bool level, uint32_t timing);
InfraredDecoderStatus infrared_common_decode_manchester(InfraredCommonDecoder* decoder, bool level, uint32_t timing);
InfraredDecoderStatus infrared_common_decode_sirc(InfraredCommonDecoder* decoder, bool level, uint32_t timing);

// Protocol-specific interpret functions
bool infrared_decoder_nec_interpret(InfraredCommonDecoder* decoder);
bool infrared_decoder_samsung32_interpret(InfraredCommonDecoder* decoder);
bool infrared_decoder_sirc_interpret(InfraredCommonDecoder* decoder);
bool infrared_decoder_rc5_interpret(InfraredCommonDecoder* decoder);
bool infrared_decoder_rc6_interpret(InfraredCommonDecoder* decoder);
bool infrared_decoder_rca_interpret(InfraredCommonDecoder* decoder);
bool infrared_decoder_pioneer_interpret(InfraredCommonDecoder* decoder);
bool infrared_decoder_kaseikyo_interpret(InfraredCommonDecoder* decoder);

// Protocol-specific decode repeat functions
InfraredDecoderStatus infrared_decoder_nec_decode_repeat(InfraredCommonDecoder* decoder);
InfraredDecoderStatus infrared_decoder_samsung32_decode_repeat(InfraredCommonDecoder* decoder);

// Special decode functions
InfraredDecoderStatus infrared_decoder_rc6_decode_manchester(InfraredCommonDecoder* decoder, bool level, uint32_t timing);

// Utility functions
uint8_t reverse(uint8_t value);
const char* infrared_protocol_to_string(InfraredProtocol protocol);

#endif // INFRARED_DECODER_H
