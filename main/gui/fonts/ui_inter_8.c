/*******************************************************************************
 * Size: 8 px
 * Bpp: 2
 * Opts: --bpp 2 --size 8 --no-compress --font S:\GhostESP2\Ghost_ESP\main\gui\fonts\InterVariable.ttf --range 32-126 --symbols 61441,61448,61451,61452,61453,61457,61459,61461,61465,61468,61473,61478,61479,61480,61502,61507,61512,61515,61516,61517,61521,61522,61523,61524,61543,61544,61550,61552,61553,61556,61559,61560,61561,61563,61587,61589,61636,61637,61639,61641,61664,61671,61674,61683,61724,61732,61787,61931,62016,62017,62018,62019,62020,62087,62099,62189,62212,62810,63426,63650 --format lvgl -o S:\GhostESP2\Ghost_ESP\main\gui\fonts\ui_inter_8.c
 ******************************************************************************/

#ifdef LV_LVGL_H_INCLUDE_SIMPLE
#include "lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

#ifndef UI_INTER_8
#define UI_INTER_8 1
#endif

#if UI_INTER_8

/*-----------------
 *    BITMAPS
 *----------------*/

/*Store the image of the glyphs*/
static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {
    /* U+0020 " " */

    /* U+0021 "!" */
    0x22, 0x22, 0x0, 0x60,

    /* U+0022 "\"" */
    0x28, 0xa1, 0x40,

    /* U+0023 "#" */
    0x11, 0x48, 0x47, 0xb8, 0x88, 0xbb, 0x48, 0x81,
    0x10,

    /* U+0024 "$" */
    0x4, 0xb, 0x85, 0x44, 0xa0, 0x1e, 0x1, 0x65,
    0x48, 0xb8, 0x4, 0x0,

    /* U+0025 "%" */
    0x28, 0x20, 0x91, 0x42, 0x88, 0x0, 0x40, 0x2,
    0x24, 0x21, 0x60, 0x42, 0x80,

    /* U+0026 "&" */
    0x29, 0x2, 0x20, 0x39, 0x3, 0xc0, 0x59, 0x88,
    0x34, 0x2a, 0x80,

    /* U+0027 "'" */
    0x22, 0x10,

    /* U+0028 "(" */
    0x14, 0x82, 0x8, 0x20, 0x82, 0x5,

    /* U+0029 ")" */
    0x50, 0x82, 0x8, 0x20, 0x82, 0x14,

    /* U+002A "*" */
    0x14, 0x28, 0x69, 0x0,

    /* U+002B "+" */
    0x4, 0x2, 0x2, 0xe4, 0x20, 0x8, 0x0,

    /* U+002C "," */
    0x15, 0x40,

    /* U+002D "-" */
    0x68,

    /* U+002E "." */
    0x6,

    /* U+002F "/" */
    0x8, 0x52, 0x8, 0x20, 0x44, 0x20,

    /* U+0030 "0" */
    0x2a, 0x8, 0x95, 0x6, 0x2, 0x50, 0x48, 0x92,
    0xe0,

    /* U+0031 "1" */
    0x39, 0x60, 0x82, 0x8, 0x20, 0x80,

    /* U+0032 "2" */
    0x2e, 0x14, 0x80, 0x20, 0x8, 0xc, 0x8, 0x7,
    0xf0,

    /* U+0033 "3" */
    0x2a, 0x4, 0x80, 0x20, 0x38, 0x1, 0x54, 0x52,
    0xa0,

    /* U+0034 "4" */
    0x3, 0x2, 0xc0, 0x60, 0x88, 0x22, 0x1f, 0xe0,
    0x20,

    /* U+0035 "5" */
    0x3a, 0x4, 0x7, 0xd0, 0xc, 0x2, 0x14, 0x82,
    0xd0,

    /* U+0036 "6" */
    0x1a, 0x8, 0x46, 0xa1, 0x88, 0x51, 0x58, 0x92,
    0xe0,

    /* U+0037 "7" */
    0x7f, 0x0, 0x80, 0x50, 0x20, 0x8, 0x8, 0x2,
    0x0,

    /* U+0038 "8" */
    0x2a, 0x8, 0x82, 0x20, 0xf8, 0x52, 0x54, 0x52,
    0xa0,

    /* U+0039 "9" */
    0x2e, 0x14, 0x84, 0x15, 0x89, 0x2a, 0x44, 0x82,
    0x90,

    /* U+003A ":" */
    0x60, 0x0, 0x60,

    /* U+003B ";" */
    0x20, 0x1, 0x54,

    /* U+003C "<" */
    0x1, 0x46, 0x43, 0x0, 0x28, 0x1, 0x40,

    /* U+003D "=" */
    0x2a, 0x40, 0x2, 0xa4,

    /* U+003E ">" */
    0x20, 0x2, 0x40, 0x28, 0x64, 0x10, 0x0,

    /* U+003F "?" */
    0x28, 0x42, 0x6, 0xc, 0x10, 0x0, 0x24,

    /* U+0040 "@" */
    0x6, 0x90, 0x24, 0x18, 0x12, 0x98, 0x48, 0x54,
    0x48, 0x54, 0x56, 0xa8, 0x20, 0x0, 0xa, 0x90,

    /* U+0041 "A" */
    0xd, 0x0, 0xa0, 0x16, 0x2, 0x20, 0x3b, 0x85,
    0xc, 0x80, 0x80,

    /* U+0042 "B" */
    0x7a, 0x14, 0x55, 0x15, 0xec, 0x50, 0x94, 0x27,
    0xb0,

    /* U+0043 "C" */
    0x1f, 0x42, 0x8, 0x50, 0x4, 0x0, 0x50, 0x2,
    0x8, 0x1f, 0x40,

    /* U+0044 "D" */
    0x7a, 0x5, 0xc, 0x50, 0x85, 0x5, 0x50, 0x85,
    0xc, 0x7a, 0x0,

    /* U+0045 "E" */
    0x7a, 0x14, 0x5, 0x1, 0xe8, 0x50, 0x14, 0x7,
    0xa0,

    /* U+0046 "F" */
    0x7a, 0x14, 0x5, 0x1, 0xf8, 0x50, 0x14, 0x5,
    0x0,

    /* U+0047 "G" */
    0x1f, 0x42, 0x8, 0x50, 0x4, 0x2d, 0x50, 0x52,
    0x8, 0x1f, 0x40,

    /* U+0048 "H" */
    0x50, 0x95, 0x9, 0x50, 0x97, 0xad, 0x50, 0x95,
    0x9, 0x50, 0x90,

    /* U+0049 "I" */
    0x55, 0x55, 0x55, 0x50,

    /* U+004A "J" */
    0x2, 0x2, 0x2, 0x2, 0x42, 0x92, 0x3d,

    /* U+004B "K" */
    0x50, 0x85, 0x30, 0x58, 0x7, 0xc0, 0x72, 0x5,
    0x24, 0x50, 0xc0,

    /* U+004C "L" */
    0x50, 0x14, 0x5, 0x1, 0x40, 0x50, 0x14, 0x7,
    0xa0,

    /* U+004D "M" */
    0x70, 0x29, 0xc0, 0xe6, 0x86, 0x96, 0x26, 0x58,
    0x89, 0x49, 0x25, 0x30, 0x80,

    /* U+004E "N" */
    0x70, 0x57, 0x45, 0x6c, 0x55, 0xa5, 0x53, 0x95,
    0x1d, 0x50, 0xd0,

    /* U+004F "O" */
    0x1f, 0x42, 0x8, 0x50, 0x24, 0x2, 0x50, 0x22,
    0x8, 0x1f, 0x40,

    /* U+0050 "P" */
    0x7b, 0x14, 0x65, 0x9, 0x45, 0x7e, 0x14, 0x5,
    0x0,

    /* U+0051 "Q" */
    0x1f, 0x42, 0x8, 0x50, 0x24, 0x2, 0x50, 0x22,
    0x2c, 0x1f, 0xc0, 0x4,

    /* U+0052 "R" */
    0x7b, 0x14, 0x65, 0x9, 0x45, 0x7f, 0x14, 0x85,
    0x8,

    /* U+0053 "S" */
    0x2a, 0x14, 0x12, 0x0, 0x78, 0x1, 0x94, 0x22,
    0xa0,

    /* U+0054 "T" */
    0x6e, 0x82, 0x0, 0x80, 0x20, 0x8, 0x2, 0x0,
    0x80,

    /* U+0055 "U" */
    0x50, 0x55, 0x5, 0x50, 0x55, 0x5, 0x50, 0x82,
    0xc, 0x1f, 0x40,

    /* U+0056 "V" */
    0x80, 0x85, 0xc, 0x20, 0x82, 0x14, 0x16, 0x0,
    0xa0, 0xd, 0x0,

    /* U+0057 "W" */
    0x82, 0x42, 0x52, 0x85, 0x22, 0x88, 0x25, 0x88,
    0x24, 0x58, 0x28, 0x34, 0x1c, 0x30,

    /* U+0058 "X" */
    0x50, 0xc2, 0x24, 0xa, 0x0, 0xd0, 0x1b, 0x3,
    0x14, 0x50, 0xc0,

    /* U+0059 "Y" */
    0x90, 0xc3, 0x14, 0x27, 0x0, 0xd0, 0x8, 0x0,
    0x80, 0x8, 0x0,

    /* U+005A "Z" */
    0x6b, 0x40, 0x80, 0x50, 0x20, 0x14, 0x8, 0x7,
    0xa4,

    /* U+005B "[" */
    0x34, 0x82, 0x8, 0x20, 0x82, 0x8, 0x34,

    /* U+005C "\\" */
    0x81, 0x1, 0x8, 0x20, 0x81, 0x42,

    /* U+005D "]" */
    0x70, 0x82, 0x8, 0x20, 0x82, 0x8, 0x70,

    /* U+005E "^" */
    0x0, 0x28, 0x28, 0x40,

    /* U+005F "_" */
    0xa9,

    /* U+0060 "`" */
    0x2, 0x0,

    /* U+0061 "a" */
    0x29, 0x2, 0x2b, 0x83, 0x7a,

    /* U+0062 "b" */
    0x50, 0x14, 0x6, 0xa1, 0x85, 0x51, 0x58, 0x56,
    0xa0,

    /* U+0063 "c" */
    0x2a, 0x14, 0x48, 0x1, 0x44, 0x2a, 0x0,

    /* U+0064 "d" */
    0x1, 0x40, 0x52, 0xa5, 0x49, 0x81, 0x54, 0x92,
    0xa4,

    /* U+0065 "e" */
    0x29, 0x14, 0x8b, 0xb1, 0x40, 0x2a, 0x0,

    /* U+0066 "f" */
    0x28, 0x8b, 0x48, 0x20, 0x82, 0x0,

    /* U+0067 "g" */
    0x2a, 0x54, 0x98, 0x15, 0x49, 0x2a, 0x40, 0x42,
    0xa0,

    /* U+0068 "h" */
    0x50, 0x14, 0x7, 0xa1, 0x48, 0x52, 0x14, 0x85,
    0x20,

    /* U+0069 "i" */
    0x50, 0x55, 0x55, 0x50,

    /* U+006A "j" */
    0x14, 0x1, 0x45, 0x14, 0x51, 0x45, 0x30,

    /* U+006B "k" */
    0x50, 0x14, 0x5, 0x21, 0x60, 0x78, 0x16, 0x5,
    0x30,

    /* U+006C "l" */
    0x55, 0x55, 0x55, 0x50,

    /* U+006D "m" */
    0x69, 0xa1, 0x4c, 0x55, 0x21, 0x54, 0x85, 0x52,
    0x14,

    /* U+006E "n" */
    0x6a, 0x14, 0x85, 0x21, 0x48, 0x52, 0x0,

    /* U+006F "o" */
    0x2e, 0x14, 0x88, 0x15, 0x48, 0x2e, 0x0,

    /* U+0070 "p" */
    0x6a, 0x18, 0x55, 0x15, 0x85, 0x6e, 0x14, 0x5,
    0x0,

    /* U+0071 "q" */
    0x2a, 0x54, 0x98, 0x15, 0x49, 0x2a, 0x40, 0x50,
    0x14,

    /* U+0072 "r" */
    0x69, 0x45, 0x14, 0x50,

    /* U+0073 "s" */
    0x29, 0x50, 0x29, 0x2, 0x39,

    /* U+0074 "t" */
    0x1, 0x4f, 0x14, 0x51, 0x43, 0x40,

    /* U+0075 "u" */
    0x52, 0x14, 0x85, 0x21, 0x48, 0x3a, 0x0,

    /* U+0076 "v" */
    0x82, 0x14, 0x82, 0x50, 0xa0, 0x1c, 0x0,

    /* U+0077 "w" */
    0x83, 0x21, 0x58, 0x82, 0x96, 0xa, 0x24, 0x24,
    0xc0,

    /* U+0078 "x" */
    0x52, 0xa, 0x1, 0xc0, 0xa0, 0x52, 0x0,

    /* U+0079 "y" */
    0x82, 0x14, 0x82, 0x50, 0xa0, 0x1c, 0x5, 0x7,
    0x0,

    /* U+007A "z" */
    0x7f, 0x9, 0x8, 0x20, 0x7e,

    /* U+007B "{" */
    0x8, 0x82, 0x8, 0xb0, 0x82, 0x2,

    /* U+007C "|" */
    0xaa, 0xaa, 0xa8,

    /* U+007D "}" */
    0x60, 0x82, 0x8, 0x1c, 0x82, 0x18,

    /* U+007E "~" */
    0x28, 0x41, 0x80
};


/*---------------------
 *  GLYPH DESCRIPTION
 *--------------------*/

static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {
    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id = 0 reserved */,
    {.bitmap_index = 0, .adv_w = 36, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 0, .adv_w = 37, .box_w = 2, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 4, .adv_w = 60, .box_w = 3, .box_h = 3, .ofs_x = 0, .ofs_y = 4},
    {.bitmap_index = 7, .adv_w = 81, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 16, .adv_w = 82, .box_w = 5, .box_h = 9, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 28, .adv_w = 126, .box_w = 7, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 41, .adv_w = 82, .box_w = 6, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 52, .adv_w = 38, .box_w = 2, .box_h = 3, .ofs_x = 0, .ofs_y = 4},
    {.bitmap_index = 54, .adv_w = 47, .box_w = 3, .box_h = 8, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 60, .adv_w = 47, .box_w = 3, .box_h = 8, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 66, .adv_w = 64, .box_w = 4, .box_h = 4, .ofs_x = 0, .ofs_y = 3},
    {.bitmap_index = 70, .adv_w = 85, .box_w = 5, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 77, .adv_w = 37, .box_w = 2, .box_h = 3, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 79, .adv_w = 59, .box_w = 4, .box_h = 1, .ofs_x = 0, .ofs_y = 2},
    {.bitmap_index = 80, .adv_w = 37, .box_w = 2, .box_h = 2, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 81, .adv_w = 46, .box_w = 3, .box_h = 8, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 87, .adv_w = 81, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 96, .adv_w = 52, .box_w = 3, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 102, .adv_w = 78, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 111, .adv_w = 79, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 120, .adv_w = 83, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 129, .adv_w = 76, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 138, .adv_w = 79, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 147, .adv_w = 72, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 156, .adv_w = 79, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 165, .adv_w = 79, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 174, .adv_w = 37, .box_w = 2, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 177, .adv_w = 39, .box_w = 2, .box_h = 6, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 180, .adv_w = 85, .box_w = 5, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 187, .adv_w = 85, .box_w = 5, .box_h = 3, .ofs_x = 0, .ofs_y = 1},
    {.bitmap_index = 191, .adv_w = 85, .box_w = 5, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 198, .adv_w = 65, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 205, .adv_w = 124, .box_w = 8, .box_h = 8, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 221, .adv_w = 88, .box_w = 6, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 232, .adv_w = 84, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 241, .adv_w = 94, .box_w = 6, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 252, .adv_w = 92, .box_w = 6, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 263, .adv_w = 77, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 272, .adv_w = 76, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 281, .adv_w = 96, .box_w = 6, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 292, .adv_w = 95, .box_w = 6, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 303, .adv_w = 34, .box_w = 2, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 307, .adv_w = 73, .box_w = 4, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 314, .adv_w = 86, .box_w = 6, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 325, .adv_w = 72, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 334, .adv_w = 116, .box_w = 7, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 347, .adv_w = 96, .box_w = 6, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 358, .adv_w = 98, .box_w = 6, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 369, .adv_w = 82, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 378, .adv_w = 98, .box_w = 6, .box_h = 8, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 390, .adv_w = 82, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 399, .adv_w = 82, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 408, .adv_w = 83, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 417, .adv_w = 95, .box_w = 6, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 428, .adv_w = 88, .box_w = 6, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 439, .adv_w = 126, .box_w = 8, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 453, .adv_w = 87, .box_w = 6, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 464, .adv_w = 87, .box_w = 6, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 475, .adv_w = 81, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 484, .adv_w = 47, .box_w = 3, .box_h = 9, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 491, .adv_w = 46, .box_w = 3, .box_h = 8, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 497, .adv_w = 47, .box_w = 3, .box_h = 9, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 504, .adv_w = 60, .box_w = 4, .box_h = 4, .ofs_x = 0, .ofs_y = 3},
    {.bitmap_index = 508, .adv_w = 58, .box_w = 4, .box_h = 1, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 509, .adv_w = 41, .box_w = 2, .box_h = 3, .ofs_x = 0, .ofs_y = 5},
    {.bitmap_index = 511, .adv_w = 72, .box_w = 4, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 516, .adv_w = 78, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 525, .adv_w = 73, .box_w = 5, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 532, .adv_w = 78, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 541, .adv_w = 75, .box_w = 5, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 548, .adv_w = 47, .box_w = 3, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 554, .adv_w = 79, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 563, .adv_w = 76, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 572, .adv_w = 31, .box_w = 2, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 576, .adv_w = 31, .box_w = 3, .box_h = 9, .ofs_x = -1, .ofs_y = -2},
    {.bitmap_index = 583, .adv_w = 70, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 592, .adv_w = 31, .box_w = 2, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 596, .adv_w = 112, .box_w = 7, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 605, .adv_w = 76, .box_w = 5, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 612, .adv_w = 77, .box_w = 5, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 619, .adv_w = 78, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 628, .adv_w = 78, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 637, .adv_w = 48, .box_w = 3, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 641, .adv_w = 68, .box_w = 4, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 646, .adv_w = 42, .box_w = 3, .box_h = 7, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 652, .adv_w = 76, .box_w = 5, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 659, .adv_w = 72, .box_w = 5, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 666, .adv_w = 105, .box_w = 7, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 675, .adv_w = 70, .box_w = 5, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 682, .adv_w = 72, .box_w = 5, .box_h = 7, .ofs_x = 0, .ofs_y = -2},
    {.bitmap_index = 691, .adv_w = 71, .box_w = 4, .box_h = 5, .ofs_x = 0, .ofs_y = 0},
    {.bitmap_index = 696, .adv_w = 55, .box_w = 3, .box_h = 8, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 702, .adv_w = 43, .box_w = 1, .box_h = 11, .ofs_x = 1, .ofs_y = -2},
    {.bitmap_index = 705, .adv_w = 55, .box_w = 3, .box_h = 8, .ofs_x = 0, .ofs_y = -1},
    {.bitmap_index = 711, .adv_w = 85, .box_w = 5, .box_h = 2, .ofs_x = 0, .ofs_y = 2}
};

/*---------------------
 *  CHARACTER MAPPING
 *--------------------*/



/*Collect the unicode lists and glyph_id offsets*/
static const lv_font_fmt_txt_cmap_t cmaps[] =
{
    {
        .range_start = 32, .range_length = 95, .glyph_id_start = 1,
        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    }
};

/*-----------------
 *    KERNING
 *----------------*/


/*Pair left and right glyphs for kerning*/
static const uint8_t kern_pair_glyph_ids[] =
{
    3, 7,
    3, 13,
    3, 15,
    3, 21,
    7, 3,
    7, 8,
    7, 61,
    8, 7,
    8, 13,
    8, 15,
    8, 21,
    11, 7,
    11, 13,
    11, 15,
    11, 21,
    11, 33,
    11, 64,
    12, 19,
    12, 20,
    12, 24,
    12, 61,
    13, 3,
    13, 8,
    13, 17,
    13, 18,
    13, 20,
    13, 22,
    13, 23,
    13, 24,
    13, 25,
    13, 26,
    13, 32,
    13, 33,
    14, 19,
    14, 20,
    14, 24,
    14, 61,
    15, 3,
    15, 8,
    15, 17,
    15, 18,
    15, 20,
    15, 22,
    15, 23,
    15, 24,
    15, 25,
    15, 26,
    15, 32,
    15, 33,
    16, 13,
    16, 15,
    17, 13,
    17, 15,
    17, 24,
    17, 61,
    17, 64,
    19, 21,
    20, 11,
    20, 13,
    20, 15,
    20, 63,
    21, 11,
    21, 13,
    21, 15,
    21, 18,
    21, 63,
    22, 13,
    22, 15,
    23, 13,
    23, 15,
    23, 64,
    24, 4,
    24, 7,
    24, 13,
    24, 15,
    24, 17,
    24, 20,
    24, 21,
    24, 22,
    24, 23,
    24, 24,
    24, 25,
    24, 26,
    24, 27,
    24, 28,
    24, 29,
    24, 64,
    25, 11,
    25, 13,
    25, 15,
    25, 63,
    26, 13,
    26, 15,
    26, 24,
    26, 61,
    26, 64,
    27, 61,
    28, 61,
    30, 61,
    31, 24,
    31, 61,
    33, 13,
    33, 15,
    33, 16,
    33, 61,
    33, 64,
    61, 3,
    61, 8,
    61, 11,
    61, 12,
    61, 14,
    61, 16,
    61, 18,
    61, 30,
    61, 32,
    61, 33,
    61, 61,
    61, 63,
    61, 95,
    63, 7,
    63, 13,
    63, 15,
    63, 21,
    63, 33,
    63, 64,
    64, 11,
    64, 17,
    64, 18,
    64, 20,
    64, 21,
    64, 22,
    64, 23,
    64, 25,
    64, 26,
    64, 33,
    64, 61,
    64, 63,
    64, 93,
    95, 19,
    95, 20,
    95, 24,
    95, 61
};

/* Kerning between the respective left and right glyphs
 * 4.4 format which needs to scaled with `kern_scale`*/
static const int8_t kern_pair_values[] =
{
    -4, -10, -10, -8, -4, -4, -8, -4,
    -10, -10, -8, -4, -17, -17, -6, -1,
    -9, -4, -1, -3, -6, -10, -10, -3,
    -11, -3, -2, -3, -2, -3, -1, -11,
    -6, -4, -1, -3, -6, -10, -10, -3,
    -11, -3, -2, -3, -2, -3, -1, -11,
    -6, -5, -5, -3, -3, -2, -1, -6,
    -2, -2, -3, -3, -2, -2, -4, -4,
    -2, -2, -3, -3, -4, -4, -6, -7,
    -6, -16, -16, -2, -2, -7, -1, -2,
    3, -2, -1, -4, -4, -12, -20, -2,
    -3, -3, -2, -3, -3, -2, -1, -6,
    -8, -8, -9, -9, -9, -6, -6, -5,
    -4, -5, -10, -10, -10, -4, -4, 3,
    -4, -6, -8, -4, -6, -10, -4, -4,
    -17, -17, -6, -1, -9, -9, -6, -14,
    -6, -7, -6, -6, -6, -6, -5, -10,
    -9, 4, -4, -1, -3, -6
};

/*Collect the kern pair's data in one place*/
static const lv_font_fmt_txt_kern_pair_t kern_pairs =
{
    .glyph_ids = kern_pair_glyph_ids,
    .values = kern_pair_values,
    .pair_cnt = 142,
    .glyph_ids_size = 0
};

/*--------------------
 *  ALL CUSTOM DATA
 *--------------------*/

#if LVGL_VERSION_MAJOR == 8
/*Store all the custom data of the font*/
static  lv_font_fmt_txt_glyph_cache_t cache;
#endif

#if LVGL_VERSION_MAJOR >= 8
static const lv_font_fmt_txt_dsc_t font_dsc = {
#else
static lv_font_fmt_txt_dsc_t font_dsc = {
#endif
    .glyph_bitmap = glyph_bitmap,
    .glyph_dsc = glyph_dsc,
    .cmaps = cmaps,
    .kern_dsc = &kern_pairs,
    .kern_scale = 16,
    .cmap_num = 1,
    .bpp = 2,
    .kern_classes = 0,
    .bitmap_format = 0,
#if LVGL_VERSION_MAJOR == 8
    .cache = &cache
#endif
};



/*-----------------
 *  PUBLIC FONT
 *----------------*/

/*Initialize a public general font descriptor*/
#if LVGL_VERSION_MAJOR >= 8
const lv_font_t ui_inter_8 = {
#else
lv_font_t ui_inter_8 = {
#endif
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,    /*Function pointer to get glyph's data*/
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,    /*Function pointer to get glyph's bitmap*/
    .line_height = 11,          /*The maximum line height required by the font*/
    .base_line = 2,             /*Baseline measured from the bottom of the line*/
#if !(LVGL_VERSION_MAJOR == 6 && LVGL_VERSION_MINOR == 0)
    .subpx = LV_FONT_SUBPX_NONE,
#endif
#if LV_VERSION_CHECK(7, 4, 0) || LVGL_VERSION_MAJOR >= 8
    .underline_position = -1,
    .underline_thickness = 1,
#endif
    .dsc = &font_dsc,          /*The custom font data. Will be accessed by `get_glyph_bitmap/dsc` */
#if LV_VERSION_CHECK(8, 2, 0) || LVGL_VERSION_MAJOR >= 9
    .fallback = &lv_font_montserrat_8,
#endif
    .user_data = NULL,
};



#endif /*#if UI_INTER_8*/

