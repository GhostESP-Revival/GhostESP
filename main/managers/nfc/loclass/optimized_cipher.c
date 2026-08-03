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
//
// Optimized MAC-calculation algorithm by Martin Holst Swende (MHS 2015).
// Further Proxmark3 speed optimizations by piwi (2019).
// iCLASS on-device support by iceman (2020).
//-----------------------------------------------------------------------------
#include "managers/nfc/loclass/optimized_cipher.h"
#include "managers/nfc/loclass/optimized_elite.h"
#include "managers/nfc/loclass/optimized_ikeys.h"
#include "managers/nfc/loclass/optimized_cipherutils.h"

static const uint8_t loclass_opt_select_LUT[256] = {
    00, 03, 02, 01, 02, 03, 00, 01, 04, 07, 07, 04, 06, 07, 05, 04, 01, 02, 03, 00, 02, 03, 00, 01,
    05, 06, 06, 05, 06, 07, 05, 04, 06, 05, 04, 07, 04, 05, 06, 07, 06, 05, 05, 06, 04, 05, 07, 06,
    07, 04, 05, 06, 04, 05, 06, 07, 07, 04, 04, 07, 04, 05, 07, 06, 06, 05, 04, 07, 04, 05, 06, 07,
    02, 01, 01, 02, 00, 01, 03, 02, 03, 00, 01, 02, 00, 01, 02, 03, 07, 04, 04, 07, 04, 05, 07, 06,
    00, 03, 02, 01, 02, 03, 00, 01, 00, 03, 03, 00, 02, 03, 01, 00, 05, 06, 07, 04, 06, 07, 04, 05,
    05, 06, 06, 05, 06, 07, 05, 04, 02, 01, 00, 03, 00, 01, 02, 03, 06, 05, 05, 06, 04, 05, 07, 06,
    03, 00, 01, 02, 00, 01, 02, 03, 07, 04, 04, 07, 04, 05, 07, 06, 02, 01, 00, 03, 00, 01, 02, 03,
    02, 01, 01, 02, 00, 01, 03, 02, 03, 00, 01, 02, 00, 01, 02, 03, 03, 00, 00, 03, 00, 01, 03, 02,
    04, 07, 06, 05, 06, 07, 04, 05, 00, 03, 03, 00, 02, 03, 01, 00, 01, 02, 03, 00, 02, 03, 00, 01,
    05, 06, 06, 05, 06, 07, 05, 04, 04, 07, 06, 05, 06, 07, 04, 05, 04, 07, 07, 04, 06, 07, 05, 04,
    01, 02, 03, 00, 02, 03, 00, 01, 01, 02, 02, 01, 02, 03, 01, 00};

static void loclass_opt_successor(const uint8_t* k, LoclassState_t* s, uint8_t y) {
    uint16_t Tt = s->t & 0xc533;
    Tt = Tt ^ (Tt >> 1);
    Tt = Tt ^ (Tt >> 4);
    Tt = Tt ^ (Tt >> 10);
    Tt = Tt ^ (Tt >> 8);

    s->t = (s->t >> 1);
    s->t |= (Tt ^ (s->r >> 7) ^ (s->r >> 3)) << 15;

    uint8_t opt_B = s->b;
    opt_B ^= s->b >> 6;
    opt_B ^= s->b >> 5;
    opt_B ^= s->b >> 4;

    s->b = s->b >> 1;
    s->b |= (opt_B ^ s->r) << 7;

    uint8_t opt_select = loclass_opt_select_LUT[s->r] & 0x04;
    opt_select |= (loclass_opt_select_LUT[s->r] ^ ((Tt ^ y) << 1)) & 0x02;
    opt_select |= (loclass_opt_select_LUT[s->r] ^ Tt) & 0x01;

    uint8_t r = s->r;
    s->r = (k[opt_select] ^ s->b) + s->l;
    s->l = s->r + r;
}

static void loclass_opt_suc(
    const uint8_t* k,
    LoclassState_t* s,
    const uint8_t* in,
    uint8_t length,
    bool add32Zeroes) {
    for(int i = 0; i < length; i++) {
        uint8_t head = in[i];
        for(int j = 0; j < 8; j++) {
            loclass_opt_successor(k, s, head);
            head >>= 1;
        }
    }
    if(add32Zeroes) {
        for(int i = 0; i < 16; i++) {
            loclass_opt_successor(k, s, 0);
            loclass_opt_successor(k, s, 0);
        }
    }
}

static void loclass_opt_output(const uint8_t* k, LoclassState_t* s, uint8_t* buffer) {
    for(uint8_t times = 0; times < 4; times++) {
        uint8_t bout = 0;
        bout |= (s->r & 0x4) >> 2;
        loclass_opt_successor(k, s, 0);
        bout |= (s->r & 0x4) >> 1;
        loclass_opt_successor(k, s, 0);
        bout |= (s->r & 0x4);
        loclass_opt_successor(k, s, 0);
        bout |= (s->r & 0x4) << 1;
        loclass_opt_successor(k, s, 0);
        bout |= (s->r & 0x4) << 2;
        loclass_opt_successor(k, s, 0);
        bout |= (s->r & 0x4) << 3;
        loclass_opt_successor(k, s, 0);
        bout |= (s->r & 0x4) << 4;
        loclass_opt_successor(k, s, 0);
        bout |= (s->r & 0x4) << 5;
        loclass_opt_successor(k, s, 0);
        buffer[times] = bout;
    }
}

static void loclass_opt_MAC(uint8_t* k, uint8_t* input, uint8_t* out) {
    LoclassState_t _init = {
        ((k[0] ^ 0x4c) + 0xEC) & 0xFF,
        ((k[0] ^ 0x4c) + 0x21) & 0xFF,
        0x4c,
        0xE012};

    loclass_opt_suc(k, &_init, input, 12, false);
    loclass_opt_output(k, &_init, out);
}

static void loclass_opt_MAC_N(uint8_t* k, uint8_t* input, uint8_t in_size, uint8_t* out) {
    LoclassState_t _init = {
        ((k[0] ^ 0x4c) + 0xEC) & 0xFF,
        ((k[0] ^ 0x4c) + 0x21) & 0xFF,
        0x4c,
        0xE012};

    loclass_opt_suc(k, &_init, input, in_size, false);
    loclass_opt_output(k, &_init, out);
}

void loclass_opt_doReaderMAC(uint8_t* cc_nr_p, uint8_t* div_key_p, uint8_t mac[4]) {
    uint8_t dest[] = {0, 0, 0, 0, 0, 0, 0, 0};
    loclass_opt_MAC(div_key_p, cc_nr_p, dest);
    memcpy(mac, dest, 4);
}

void loclass_opt_doReaderMAC_2(
    LoclassState_t _init,
    uint8_t* nr,
    uint8_t mac[4],
    const uint8_t* div_key_p) {
    loclass_opt_suc(div_key_p, &_init, nr, 4, false);
    loclass_opt_output(div_key_p, &_init, mac);
}

void loclass_doMAC_N(uint8_t* in_p, uint8_t in_size, uint8_t* div_key_p, uint8_t mac[4]) {
    uint8_t dest[] = {0, 0, 0, 0, 0, 0, 0, 0};
    loclass_opt_MAC_N(div_key_p, in_p, in_size, dest);
    memcpy(mac, dest, 4);
}

void loclass_opt_doTagMAC(uint8_t* cc_p, const uint8_t* div_key_p, uint8_t mac[4]) {
    LoclassState_t _init = {
        ((div_key_p[0] ^ 0x4c) + 0xEC) & 0xFF,
        ((div_key_p[0] ^ 0x4c) + 0x21) & 0xFF,
        0x4c,
        0xE012};
    loclass_opt_suc(div_key_p, &_init, cc_p, 12, true);
    loclass_opt_output(div_key_p, &_init, mac);
}

LoclassState_t loclass_opt_doTagMAC_1(uint8_t* cc_p, const uint8_t* div_key_p) {
    LoclassState_t _init = {
        ((div_key_p[0] ^ 0x4c) + 0xEC) & 0xFF,
        ((div_key_p[0] ^ 0x4c) + 0x21) & 0xFF,
        0x4c,
        0xE012};
    loclass_opt_suc(div_key_p, &_init, cc_p, 8, false);
    return _init;
}

void loclass_opt_doTagMAC_2(
    LoclassState_t _init,
    uint8_t* nr,
    uint8_t mac[4],
    const uint8_t* div_key_p) {
    loclass_opt_suc(div_key_p, &_init, nr, 4, true);
    loclass_opt_output(div_key_p, &_init, mac);
}

void loclass_opt_doBothMAC_2(
    LoclassState_t _init,
    uint8_t* nr,
    uint8_t rmac[4],
    uint8_t tmac[4],
    const uint8_t* div_key_p) {
    loclass_opt_suc(div_key_p, &_init, nr, 4, false);
    LoclassState_t nr_state = _init;
    loclass_opt_output(div_key_p, &_init, rmac);
    loclass_opt_suc(div_key_p, &nr_state, NULL, 0, true);
    loclass_opt_output(div_key_p, &nr_state, tmac);
}

void loclass_iclass_calc_div_key(uint8_t* csn, const uint8_t* key, uint8_t* div_key, bool elite) {
    if(elite) {
        uint8_t keytable[128] = {0};
        uint8_t key_index[8] = {0};
        uint8_t key_sel[8] = {0};
        uint8_t key_sel_p[8] = {0};
        loclass_hash2(key, keytable);
        loclass_hash1(csn, key_index);
        for(uint8_t i = 0; i < 8; i++) key_sel[i] = keytable[key_index[i]];
        loclass_permutekey_rev(key_sel, key_sel_p);
        loclass_diversifyKey(csn, key_sel_p, div_key);
    } else {
        loclass_diversifyKey(csn, key, div_key);
    }
}
