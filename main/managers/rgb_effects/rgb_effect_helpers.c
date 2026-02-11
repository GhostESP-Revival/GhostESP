#include "managers/rgb_effects/rgb_effect_helpers.h"

// Internal helper for color wheel
static inline void _rgb_color_wheel_internal(uint16_t pos, uint8_t *r, uint8_t *g, uint8_t *b) {
    uint8_t segment = pos >> 8;
    uint8_t offset = pos & 0xFF;

    switch (segment) {
    case 0:
        *r = 255;
        *g = offset;
        *b = 0;
        break;
    case 1:
        *r = 255 - offset;
        *g = 255;
        *b = 0;
        break;
    case 2:
        *r = 0;
        *g = 255;
        *b = offset;
        break;
    case 3:
        *r = 0;
        *g = 255 - offset;
        *b = 255;
        break;
    case 4:
        *r = offset;
        *g = 0;
        *b = 255;
        break;
    default:
        *r = 255;
        *g = 0;
        *b = 255 - offset;
        break;
    }
}

void rgb_helper_scale_brightness(uint8_t *r, uint8_t *g, uint8_t *b, uint8_t base_scale_255, uint8_t global_max_255) {
    uint32_t scale_factor = (uint32_t)base_scale_255 * (uint32_t)global_max_255;
    *r = (uint8_t)(((*r) * scale_factor) >> 16);
    *g = (uint8_t)(((*g) * scale_factor) >> 16);
    *b = (uint8_t)(((*b) * scale_factor) >> 16);
}

void rgb_helper_compute_rainbow_pixel(uint32_t pos_q16, uint8_t *r, uint8_t *g, uint8_t *b) {
    _rgb_color_wheel_internal((uint16_t)(pos_q16 >> 16), r, g, b);
}

bool rgb_helper_compute_knight_rider_pixel(int pos, int led_idx, int tail_length, int direction, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (led_idx == pos) {
        *r = 255; *g = 0; *b = 0;
        return true;
    }
    int dist = (pos - led_idx) * direction;
    if (dist > 0 && dist < tail_length) {
        uint8_t intensity = 255 - ((255 / tail_length) * dist);
        *r = intensity; *g = 0; *b = 0;
        return true;
    }
    return false;
}


// gamma 2.40
static const uint16_t gamma12[256] = {
     0,   0,   0,   0,   0,   0,   1,   1,   1,   1,   2,   2,   3,   3,   4,   5,
     5,   6,   7,   8,   9,  10,  11,  13,  14,  16,  17,  19,  20,  22,  24,  26,
    28,  30,  33,  35,  37,  40,  42,  45,  48,  51,  54,  57,  60,  64,  67,  71,
    74,  78,  82,  86,  90,  94,  99, 103, 108, 112, 117, 122, 127, 132, 137, 143,
   148, 154, 160, 166, 172, 178, 184, 190, 197, 203, 210, 217, 224, 231, 239, 246,
   253, 261, 269, 277, 285, 293, 302, 310, 319, 327, 336, 345, 355, 364, 373, 383,
   393, 403, 413, 423, 433, 444, 454, 465, 476, 487, 498, 509, 521, 533, 544, 556,
   568, 581, 593, 606, 618, 631, 644, 657, 671, 684, 698, 712, 726, 740, 754, 769,
   783, 798, 813, 828, 843, 859, 874, 890, 906, 922, 938, 955, 971, 988,1005,1022,
  1039,1056,1074,1092,1110,1128,1146,1164,1183,1202,1221,1240,1259,1279,1298,1318,
  1338,1358,1378,1399,1420,1441,1462,1483,1504,1526,1548,1569,1592,1614,1636,1659,
  1682,1705,1728,1751,1775,1799,1823,1847,1871,1896,1920,1945,1970,1996,2021,2047,
  2072,2098,2125,2151,2178,2204,2231,2258,2286,2313,2341,2369,2397,2425,2454,2482,
  2511,2540,2570,2599,2629,2659,2689,2719,2749,2780,2811,2842,2873,2905,2936,2968,
  3000,3032,3065,3098,3130,3163,3197,3230,3264,3298,3332,3366,3401,3435,3470,3505,
  3540,3576,3612,3648,3684,3720,3757,3793,3830,3868,3905,3943,3980,4018,4057,4095,
};

void rgb_helper_compute_rainbow_pixel_12bit(uint32_t pos_q16, uint16_t *r, uint16_t *g, uint16_t *b) {
    uint16_t pos = (uint16_t)(pos_q16 >> 16); // 0-65535
    
    uint32_t val = (uint32_t)pos * 6;
    uint8_t segment = val >> 16;
    uint16_t offset = (val & 0xFFFF) >> 4;

    switch (segment) {
        case 0: *r = 4095; *g = offset; *b = 0; break;
        case 1: *r = 4095 - offset; *g = 4095; *b = 0; break;
        case 2: *r = 0; *g = 4095; *b = offset; break;
        case 3: *r = 0; *g = 4095 - offset; *b = 4095; break;
        case 4: *r = offset; *g = 0; *b = 4095; break;
        default: *r = 4095; *g = 0; *b = 4095 - offset; break;
    }
}

void rgb_helper_scale_brightness_12bit(uint16_t *r, uint16_t *g, uint16_t *b, uint8_t base_scale_255, uint8_t global_max_255) {
    uint32_t scale_factor = (uint32_t)base_scale_255 * (uint32_t)global_max_255;
    
    // Input is 0-4095 (12-bit).
    // Result = (val * scale_factor) >> 16.
    // Max val * scale = 4095 * 65025 ~= 266,277,375 (fits in uint32_t).
    *r = (uint16_t)(((*r) * scale_factor) >> 16);
    *g = (uint16_t)(((*g) * scale_factor) >> 16);
    *b = (uint16_t)(((*b) * scale_factor) >> 16);
}

uint8_t rgb_helper_gamma_correct_12bit_to_8bit(uint16_t val_12bit) {
    if (val_12bit > 4095) val_12bit = 4095;
    uint8_t i = val_12bit >> 4;
    uint8_t rem = val_12bit & 0x0F;
    
    uint16_t base = gamma12[i];
    uint16_t next = (i < 255) ? gamma12[i+1] : gamma12[255];
    
    // Interpolate in 12-bit space
    uint32_t result_12bit = base + (((uint32_t)(next - base) * rem) >> 4);
    
    // Convert to 8-bit for hardware
    uint8_t result = (uint8_t)(result_12bit >> 4);
    
    // LIFT: Ensure non-zero input gives visible output if possible
    if (val_12bit > 0 && result == 0) {
        return 1;
    }
    return result;
}

bool rgb_helper_compute_knight_rider_pixel_12bit(int pos, int led_idx, int tail_length, int direction, uint16_t *r, uint16_t *g, uint16_t *b) {
    if (led_idx == pos) {
        *r = 4095; *g = 0; *b = 0;
        return true;
    }
    int dist = (pos - led_idx) * direction;
    if (dist > 0 && dist < tail_length) {
        // tail_length is usually small (e.g. 4)
        // 4095 / tail_length
        uint16_t intensity = 4095 - ((4095 / tail_length) * dist);
        *r = intensity; *g = 0; *b = 0;
        return true;
    }
    return false;
}
