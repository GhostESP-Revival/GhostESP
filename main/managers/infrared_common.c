#include "managers/infrared_common.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static InfraredStatus infrared_common_encode_bits(InfraredCommonEncoder* encoder, uint32_t* duration, bool* level) {
    const InfraredCommonProtocolSpec* proto = encoder->protocol;
    InfraredStatus status = proto->encode(encoder, duration, level);
    assert(status == InfraredStatusOk);
    ++encoder->timings_encoded;
    encoder->timings_sum += *duration;
    if((encoder->bits_encoded == encoder->bits_to_encode) && *level) {
        status = InfraredStatusDone;
    }
    return status;
}

InfraredStatus infrared_common_encode_manchester(InfraredCommonEncoder* encoder, uint32_t* duration, bool* level) {
    assert(encoder && duration && level);
    const InfraredTimings* t = &encoder->protocol->timings;
    uint8_t idx = encoder->bits_encoded / 8;
    uint8_t shift = encoder->bits_encoded % 8; // LSB first
    bool val = !!(encoder->data[idx] & (1 << shift));
    bool even = !(encoder->timings_encoded % 2);
    *level = even ^ val;
    *duration = t->bit1_mark;
    if(even) {
        ++encoder->bits_encoded;
    } else if(*level && (encoder->bits_encoded + 1 == encoder->bits_to_encode)) {
        ++encoder->bits_encoded;
    }
    return InfraredStatusOk;
}

InfraredStatus infrared_common_encode_pdwm(InfraredCommonEncoder* encoder, uint32_t* duration, bool* level) {
    assert(encoder && duration && level);
    const InfraredTimings* t = &encoder->protocol->timings;
    uint8_t idx = encoder->bits_encoded / 8;
    uint8_t shift = encoder->bits_encoded % 8;
    bool val = !!(encoder->data[idx] & (1 << shift));
    bool pwm = (t->bit1_space == t->bit0_space);
    if(encoder->timings_encoded % 2) {
        *duration = val ? t->bit1_mark : t->bit0_mark;
        *level = true;
        if(pwm) ++encoder->bits_encoded;
    } else {
        *duration = val ? t->bit1_space : t->bit0_space;
        *level = false;
        if(!pwm) ++encoder->bits_encoded;
    }
    return InfraredStatusOk;
}

InfraredStatus infrared_common_encode(InfraredCommonEncoder* encoder, uint32_t* duration, bool* level) {
    assert(encoder && duration && level);
    InfraredStatus status = InfraredStatusOk;
    const InfraredCommonProtocolSpec* proto = encoder->protocol;
    switch(encoder->state) {
        case InfraredCommonEncoderStateSilence:
            *duration = proto->timings.silence_time;
            *level = false;
            encoder->state = InfraredCommonEncoderStatePreamble;
            ++encoder->timings_encoded;
            encoder->timings_sum = 0;
            break;
        case InfraredCommonEncoderStatePreamble:
            if(proto->timings.preamble_mark) {
                if(encoder->timings_encoded == 1) {
                    *duration = proto->timings.preamble_mark;
                    *level = true;
                } else {
                    *duration = proto->timings.preamble_space;
                    *level = false;
                    encoder->state = InfraredCommonEncoderStateEncode;
                }
                ++encoder->timings_encoded;
                encoder->timings_sum += *duration;
                break;
            } else {
                encoder->state = InfraredCommonEncoderStateEncode;
            }
            /* fall through */
        case InfraredCommonEncoderStateEncode:
            status = infrared_common_encode_bits(encoder, duration, level);
            if(status == InfraredStatusDone) {
                if(proto->encode_repeat) {
                    encoder->state = InfraredCommonEncoderStateEncodeRepeat;
                } else {
                    encoder->timings_encoded = 0;
                    encoder->timings_sum = 0;
                    encoder->bits_encoded = 0;
                    encoder->state = InfraredCommonEncoderStateSilence;
                }
            }
            break;
        case InfraredCommonEncoderStateEncodeRepeat:
            status = proto->encode_repeat(encoder, duration, level);
            break;
    }
    return status;
}

void* infrared_common_encoder_alloc(const InfraredCommonProtocolSpec* protocol) {
    assert(protocol);
    uint8_t max_bits = 0;
    for(size_t i = 0; i < 4; ++i) {
        if(protocol->databit_len[i] > max_bits) max_bits = protocol->databit_len[i];
    }
    size_t alloc = sizeof(InfraredCommonEncoder) + (max_bits + 7) / 8;
    InfraredCommonEncoder* e = malloc(alloc);
    memset(e, 0, alloc);
    e->protocol = protocol;
    return e;
}

void infrared_common_encoder_reset(InfraredCommonEncoder* encoder) {
    assert(encoder);
    encoder->timings_encoded = 0;
    encoder->timings_sum = 0;
    encoder->bits_encoded = 0;
    encoder->state = InfraredCommonEncoderStateSilence;
    uint8_t max_bits = 0;
    for(size_t i = 0; i < 4; ++i) {
        if(encoder->protocol->databit_len[i] > max_bits) max_bits = encoder->protocol->databit_len[i];
    }
    memset(encoder->data, 0, (max_bits + 7) / 8);
}

void infrared_common_encoder_free(InfraredCommonEncoder* encoder) {
    assert(encoder);
    free(encoder);
} 
