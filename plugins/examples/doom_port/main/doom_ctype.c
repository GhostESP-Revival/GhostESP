/* Newlib ctype macros reference this table. Keep it local to avoid unsupported
 * external data-symbol relocations in the ESP32-C5 ELF loader. */
__attribute__((visibility("hidden"))) const char _ctype_[257] = {
    [0] = 0,
    [1 ... 9] = 040,
    [10 ... 14] = 040 | 010,
    [15 ... 32] = 040,
    [33] = 010 | 0200,
    [34 ... 48] = 020,
    [49 ... 58] = 04 | 0100,
    [59 ... 65] = 020,
    [66 ... 71] = 01 | 0100,
    [72 ... 91] = 01,
    [92 ... 97] = 020,
    [98 ... 103] = 02 | 0100,
    [104 ... 123] = 02,
    [124 ... 127] = 020,
    [128] = 040,
};
