//-----------------------------------------------------------------------------
// Borrowed initially from https://github.com/holiman/loclass
// Copyright (C) 2014 Martin Holst Swende
// Copyright (C) Proxmark3 contributors. See AUTHORS.md for details.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// See LICENSE.txt for the text of the license.
//-----------------------------------------------------------------------------
// Adapted for GhostESP from Momentum-Firmware's picopass application.
// Momentum-Firmware: https://github.com/Next-Flip/Momentum-Firmware
// Original picopass app by Eric Betts (bettse):
//   https://github.com/bettse/picopass
//-----------------------------------------------------------------------------
#include "managers/nfc/loclass/optimized_cipherutils.h"
#include <stdint.h>

bool loclass_headBit(LoclassBitstreamIn_t* stream) {
    int bytepos = stream->position >> 3;
    int bitpos = (stream->position++) & 7;
    return (*(stream->buffer + bytepos) >> (7 - bitpos)) & 1;
}

bool loclass_tailBit(LoclassBitstreamIn_t* stream) {
    int bitpos = stream->numbits - 1 - (stream->position++);
    int bytepos = bitpos >> 3;
    bitpos &= 7;
    return (*(stream->buffer + bytepos) >> (7 - bitpos)) & 1;
}

void loclass_pushBit(LoclassBitstreamOut_t* stream, bool bit) {
    int bytepos = stream->position >> 3;
    int bitpos = stream->position & 7;
    *(stream->buffer + bytepos) |= (bit) << (7 - bitpos);
    stream->position++;
    stream->numbits++;
}

void loclass_push6bits(LoclassBitstreamOut_t* stream, uint8_t bits) {
    loclass_pushBit(stream, bits & 0x20);
    loclass_pushBit(stream, bits & 0x10);
    loclass_pushBit(stream, bits & 0x08);
    loclass_pushBit(stream, bits & 0x04);
    loclass_pushBit(stream, bits & 0x02);
    loclass_pushBit(stream, bits & 0x01);
}

int loclass_bitsLeft(LoclassBitstreamIn_t* stream) {
    return stream->numbits - stream->position;
}

void loclass_x_num_to_bytes(uint64_t n, size_t len, uint8_t* dest) {
    while(len--) {
        dest[len] = (uint8_t)n;
        n >>= 8;
    }
}

uint64_t loclass_x_bytes_to_num(uint8_t* src, size_t len) {
    uint64_t num = 0;
    while(len--) {
        num = (num << 8) | (*src);
        src++;
    }
    return num;
}

uint8_t loclass_reversebytes(uint8_t b) {
    b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
    b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
    b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
    return b;
}

void loclass_reverse_arraybytes(uint8_t* arr, size_t len) {
    uint8_t i;
    for(i = 0; i < len; i++) {
        arr[i] = loclass_reversebytes(arr[i]);
    }
}

void loclass_reverse_arraycopy(uint8_t* arr, uint8_t* dest, size_t len) {
    uint8_t i;
    for(i = 0; i < len; i++) {
        dest[i] = loclass_reversebytes(arr[i]);
    }
}
