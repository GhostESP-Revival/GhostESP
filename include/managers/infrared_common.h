#ifndef INFRARED_COMMON_H
#define INFRARED_COMMON_H

#include <stdbool.h>
#include <stdint.h>
#include "infrared_timings.h"

// Forward declarations
typedef struct InfraredCommonEncoder InfraredCommonEncoder;
typedef struct InfraredCommonProtocolSpec InfraredCommonProtocolSpec;
typedef struct InfraredMessage InfraredMessage;
typedef struct InfraredCommonDecoder InfraredCommonDecoder;
typedef struct InfraredDecoderContext InfraredDecoderContext;

typedef enum {
    InfraredStatusOk,
    InfraredStatusDone,
    InfraredStatusError,
} InfraredStatus;

typedef enum {
    InfraredCommonEncoderStateSilence,
    InfraredCommonEncoderStatePreamble,
    InfraredCommonEncoderStateEncode,
    InfraredCommonEncoderStateEncodeRepeat,
} InfraredCommonEncoderState;

typedef enum {
    InfraredDecoderStatusOk,
    InfraredDecoderStatusReady,
    InfraredDecoderStatusError,
} InfraredDecoderStatus;

// Encoder function types
typedef InfraredStatus (*InfraredEncoderEncode)(InfraredCommonEncoder* encoder, uint32_t* duration, bool* level);
typedef InfraredStatus (*InfraredEncoderEncodeRepeat)(InfraredCommonEncoder* encoder, uint32_t* duration, bool* level);
typedef void (*InfraredEncoderReset)(InfraredCommonEncoder* encoder, const InfraredMessage* message);

// Decoder function types
typedef InfraredDecoderStatus (*InfraredDecoderDecode)(InfraredCommonDecoder* decoder, bool level, uint32_t timing);
typedef InfraredDecoderStatus (*InfraredDecoderDecodeRepeat)(InfraredCommonDecoder* decoder);
typedef bool (*InfraredDecoderInterpret)(InfraredCommonDecoder* decoder);
typedef void (*InfraredDecoderReset)(InfraredCommonDecoder* decoder);

typedef struct {
    uint32_t preamble_mark;
    uint32_t preamble_space;
    uint32_t bit1_mark;
    uint32_t bit1_space;
    uint32_t bit0_mark;
    uint32_t bit0_space;
    uint32_t silence_time;
    uint32_t repeat_mark;
    uint32_t repeat_space;
    uint32_t preamble_tolerance;
    uint32_t bit_tolerance;
    uint32_t min_split_time;
} InfraredTimings;

struct InfraredCommonProtocolSpec {
    const InfraredTimings timings;
    bool manchester_start_from_space;
    const uint8_t databit_len[4];
    InfraredEncoderReset reset;
    InfraredEncoderEncode encode;
    InfraredEncoderEncodeRepeat encode_repeat;
    uint32_t carrier_frequency;
    float duty_cycle;
};

// Decoder-specific protocol specification
typedef struct {
    const InfraredTimings timings;
    const uint8_t databit_len[4];
    bool manchester_start_from_space;
    InfraredDecoderDecode decode;
    InfraredDecoderDecodeRepeat decode_repeat;
    InfraredDecoderInterpret interpret;
    InfraredDecoderReset reset;
} InfraredDecoderProtocolSpec;

struct InfraredCommonEncoder {
    const InfraredCommonProtocolSpec* protocol;
    InfraredCommonEncoderState state;
    uint32_t timings_encoded;
    uint64_t timings_sum;
    uint8_t bits_to_encode;
    uint8_t bits_encoded;
    uint8_t data[];
};

struct InfraredMessage {
    char protocol[32];
    uint32_t address;
    uint32_t command;
};

// Protocol types
typedef enum {
    InfraredProtocolUnknown,
    InfraredProtocolNEC,
    InfraredProtocolNECext,
    InfraredProtocolNEC42,
    InfraredProtocolNEC42ext,
    InfraredProtocolSamsung32,
    InfraredProtocolSIRC,
    InfraredProtocolSIRC15,
    InfraredProtocolSIRC20,
    InfraredProtocolRC5,
    InfraredProtocolRC5X,
    InfraredProtocolRC6,
    InfraredProtocolRCA,
    InfraredProtocolPioneer,
    InfraredProtocolKaseikyo,
} InfraredProtocol;

// Decoded message structure
typedef struct {
    InfraredProtocol protocol;
    uint32_t address;
    uint32_t command;
    bool repeat;
} InfraredDecodedMessage;

// Decoder state enumeration (Flipper Zero compatible)
typedef enum {
    InfraredDecoderStateIdle,
    InfraredDecoderStatePreambleMark,
    InfraredDecoderStatePreambleSpace,
    InfraredDecoderStateData,
    // Flipper Zero states
    InfraredCommonDecoderStateWaitPreamble,
    InfraredCommonDecoderStateDecode,
    InfraredCommonDecoderStateProcessRepeat
} InfraredDecoderState;

// Constants
#define INFRARED_MAX_TIMINGS 200

// Common decoder structure
struct InfraredCommonDecoder {
    const InfraredDecoderProtocolSpec* protocol;
    InfraredDecodedMessage message;
    uint32_t timings[200];  // Buffer for timing data
    uint32_t timings_cnt;
    uint8_t data[8];        // Buffer for decoded data
    uint32_t databit_cnt;
    bool switch_detect;
    bool level;             // Current signal level (Flipper Zero)
    InfraredDecoderState state;  // Current decoder state
    void* context;          // Protocol-specific context
};

// Protocol-specific decoder contexts
typedef struct {
    bool toggle;
} InfraredRc5Decoder;

typedef struct {
    bool toggle;
} InfraredRc6Decoder;

// Main decoder interface
struct InfraredDecoderContext {
    InfraredCommonDecoder* decoders[16];  // Array of protocol decoders
    uint8_t decoder_count;
    InfraredDecodedMessage last_message;
};

// Utility macros
#define MATCH_TIMING(x, v, delta) (((x) < ((v) + (delta))) && ((x) > ((v) - (delta))))

// Encoder function declarations
void* infrared_common_encoder_alloc(const InfraredCommonProtocolSpec* protocol);
void infrared_common_encoder_free(InfraredCommonEncoder* encoder);
void infrared_common_encoder_reset(InfraredCommonEncoder* encoder);
InfraredStatus infrared_common_encode(InfraredCommonEncoder* encoder, uint32_t* duration, bool* level);
InfraredStatus infrared_common_encode_manchester(InfraredCommonEncoder* encoder, uint32_t* duration, bool* level);
InfraredStatus infrared_common_encode_pdwm(InfraredCommonEncoder* encoder, uint32_t* duration, bool* level);

// Decoder function declarations
InfraredDecoderContext* infrared_decoder_alloc(void);
void infrared_decoder_free(InfraredDecoderContext* decoder);
void infrared_decoder_reset(InfraredDecoderContext* decoder);
InfraredDecodedMessage* infrared_decoder_decode(InfraredDecoderContext* decoder, bool level, uint32_t timing);
InfraredCommonDecoder* infrared_common_decoder_alloc(const InfraredDecoderProtocolSpec* protocol);
void infrared_common_decoder_free(InfraredCommonDecoder* decoder);
void infrared_common_decoder_reset(InfraredCommonDecoder* decoder);
InfraredDecodedMessage* infrared_common_decoder_check_ready(InfraredCommonDecoder* decoder);
InfraredDecoderStatus infrared_common_decode_pdwm(InfraredCommonDecoder* decoder, bool level, uint32_t timing);
InfraredDecoderStatus infrared_common_decode_manchester(InfraredCommonDecoder* decoder, bool level, uint32_t timing);

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

#endif // INFRARED_COMMON_H