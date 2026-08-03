//-----------------------------------------------------------------------------
// Borrowed initially from https://github.com/holiman/loclass
// More recently from https://github.com/RfidResearchGroup/proxmark3
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
#ifndef LOCLASS_CIPHERUTILS_H
#define LOCLASS_CIPHERUTILS_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

typedef struct {
    uint8_t* buffer;
    uint8_t numbits;
    uint8_t position;
} LoclassBitstreamIn_t;

typedef struct {
    uint8_t* buffer;
    uint8_t numbits;
    uint8_t position;
} LoclassBitstreamOut_t;

bool loclass_headBit(LoclassBitstreamIn_t* stream);
bool loclass_tailBit(LoclassBitstreamIn_t* stream);
void loclass_pushBit(LoclassBitstreamOut_t* stream, bool bit);
int loclass_bitsLeft(LoclassBitstreamIn_t* stream);

void loclass_push6bits(LoclassBitstreamOut_t* stream, uint8_t bits);
void loclass_x_num_to_bytes(uint64_t n, size_t len, uint8_t* dest);
uint64_t loclass_x_bytes_to_num(uint8_t* src, size_t len);
uint8_t loclass_reversebytes(uint8_t b);
void loclass_reverse_arraybytes(uint8_t* arr, size_t len);
void loclass_reverse_arraycopy(uint8_t* arr, uint8_t* dest, size_t len);

#endif // LOCLASS_CIPHERUTILS_H
