#ifndef UNIVERSAL_IR_H
#define UNIVERSAL_IR_H

#include <stdint.h>
#include <stddef.h>
#include "managers/infrared_manager.h"

typedef struct {
    const char *name;
    const char *protocol;
    uint32_t address;
    uint32_t command;
} universal_ir_signal_t;

#define UNIVERSAL_IR_SIGNAL_COUNT 61

static const universal_ir_signal_t universal_ir_signals[UNIVERSAL_IR_SIGNAL_COUNT] = {
    {"POWER", "NEC", 0x00000004, 0x00000008},
    {"POWER", "Samsung32", 0x00000007, 0x00000002},
    {"POWER", "SIRC", 0x00000001, 0x00000015},
    {"POWER", "Kaseikyo", 0x00200280, 0x000003D0},
    {"POWER", "NECext", 0x00007D02, 0x0000B946},
    {"POWER", "RC5", 0x00000000, 0x0000000C},
    {"POWER", "RC5", 0x00000001, 0x0000000C},
    {"POWER", "RC6", 0x00000000, 0x0000000C},
    {"POWER", "Samsung32", 0x00000007, 0x000000E6},
    {"POWER", "NEC", 0x00000040, 0x00000012},
    {"POWER", "RCA", 0x0000000F, 0x00000054},
    {"POWER", "NECext", 0x0000BF00, 0x0000F20D},
    {"POWER", "NECext", 0x00000586, 0x0000F00F},
    {"POWER", "NECext", 0x0000C7EA, 0x0000E817},
    {"POWER", "NEC", 0x00000020, 0x00000052},
    {"POWER", "NECext", 0x00007F00, 0x0000F50A},
    {"POWER", "NEC", 0x00000001, 0x00000010},
    {"POWER", "RC5", 0x00000003, 0x0000000C},
    {"POWER", "NEC", 0x000000A0, 0x0000005F},
    {"POWER", "NECext", 0x00007F00, 0x0000EA15},
    {"POWER", "NECext", 0x00007F00, 0x0000E11E},
    {"POWER", "NECext", 0x00007201, 0x0000E11E},
    {"POWER", "NECext", 0x0000DD72, 0x0000F10E},
    {"POWER", "NECext", 0x0000E084, 0x0000DF20},
    {"POWER", "NEC", 0x00000040, 0x0000000B},
    {"POWER", "NEC", 0x0000006E, 0x00000002},
    {"POWER", "NECext", 0x0000DF00, 0x0000E31C},
    {"POWER", "NECext", 0x00004040, 0x0000F50A},
    {"POWER", "Samsung32", 0x0000000E, 0x0000000C},
    {"POWER", "NEC", 0x00000000, 0x0000001A},
    {"POWER", "NEC", 0x00000001, 0x0000001C},
    {"POWER", "NEC", 0x00000001, 0x00000040},
    {"POWER", "NEC", 0x00000004, 0x00000040},
    {"POWER", "NEC", 0x00000008, 0x000000D7},
    {"POWER", "NEC", 0x00000019, 0x00000018},
    {"POWER", "NEC", 0x00000028, 0x0000000B},
    {"POWER", "NEC", 0x00000038, 0x00000001},
    {"POWER", "NEC", 0x00000038, 0x00000012},
    {"POWER", "NEC", 0x00000050, 0x00000017},
    {"POWER", "NEC", 0x00000080, 0x00000012},
    {"POWER", "NEC", 0x00000080, 0x00000082},
    {"POWER", "NEC", 0x000000A0, 0x0000001C},
    {"POWER", "NECext", 0x0000BD00, 0x0000FE01},
    {"POWER", "NECext", 0x0000BF00, 0x0000FF00},
    {"POWER", "NECext", 0x0000BF00, 0x0000FC03},
    {"POWER", "NECext", 0x0000F700, 0x0000F30C},
    {"POWER", "NECext", 0x0000FB00, 0x0000F50A},
    {"POWER", "NECext", 0x00003E01, 0x0000F50A},
    {"POWER", "NECext", 0x0000B904, 0x0000FF00},
    {"POWER", "NECext", 0x0000F404, 0x0000F708},
    {"POWER", "NECext", 0x00004664, 0x0000A25D},
    {"POWER", "NECext", 0x00006969, 0x0000FE01},
    {"POWER", "NECext", 0x0000DD72, 0x0000EF10},
    {"POWER", "NECext", 0x00007A83, 0x00000008},
    {"POWER", "NECext", 0x00007C85, 0x00007F80},
    {"POWER", "NECext", 0x0000B7A0, 0x000016E9},
    {"POWER", "NECext", 0x0000EDAD, 0x00004AB5},
    {"POWER", "NECext", 0x0000C7EA, 0x00006897},
    {"POWER", "Pioneer", 0x000000AA, 0x0000001C},
    {"POWER", "SIRC", 0x00000001, 0x0000002E},
    {"POWER", "Samsung32", 0x0000003E, 0x0000000C},
};

bool universal_ir_get_signal(size_t index, infrared_signal_t *signal);
size_t universal_ir_get_signal_count(void);
const char* universal_ir_get_signal_name(size_t index);

#endif
