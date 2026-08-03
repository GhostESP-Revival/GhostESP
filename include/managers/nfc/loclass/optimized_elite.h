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
#ifndef LOCLASS_ELITE_H
#define LOCLASS_ELITE_H

#include <stdint.h>
#include <stdlib.h>

void loclass_permutekey(const uint8_t key[8], uint8_t dest[8]);
void loclass_permutekey_rev(const uint8_t key[8], uint8_t dest[8]);
void loclass_hash1(const uint8_t* csn, uint8_t* k);
void loclass_hash2(const uint8_t* key64, uint8_t* outp_keytable);

#endif // LOCLASS_ELITE_H
