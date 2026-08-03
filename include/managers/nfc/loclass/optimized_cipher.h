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
#ifndef LOCLASS_CIPHER_H
#define LOCLASS_CIPHER_H

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t l;
    uint8_t r;
    uint8_t b;
    uint16_t t;
} LoclassState_t;

void loclass_opt_doReaderMAC(uint8_t* cc_nr_p, uint8_t* div_key_p, uint8_t mac[4]);
void loclass_opt_doReaderMAC_2(
    LoclassState_t _init,
    uint8_t* nr,
    uint8_t mac[4],
    const uint8_t* div_key_p);

void loclass_opt_doTagMAC(uint8_t* cc_p, const uint8_t* div_key_p, uint8_t mac[4]);

LoclassState_t loclass_opt_doTagMAC_1(uint8_t* cc_p, const uint8_t* div_key_p);

void loclass_opt_doTagMAC_2(
    LoclassState_t _init,
    uint8_t* nr,
    uint8_t mac[4],
    const uint8_t* div_key_p);

void loclass_opt_doBothMAC_2(
    LoclassState_t _init,
    uint8_t* nr,
    uint8_t rmac[4],
    uint8_t tmac[4],
    const uint8_t* div_key_p);

void loclass_doMAC_N(uint8_t* in_p, uint8_t in_size, uint8_t* div_key_p, uint8_t mac[4]);
void loclass_iclass_calc_div_key(uint8_t* csn, const uint8_t* key, uint8_t* div_key, bool elite);

#endif // LOCLASS_CIPHER_H
