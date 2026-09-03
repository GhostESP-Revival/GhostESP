# Script mode does not inherit policies from the app's CMake configure.
cmake_policy(SET CMP0057 NEW)

execute_process(
    COMMAND "${NM}" -u "${OUT}"
    RESULT_VARIABLE NM_RESULT
    OUTPUT_VARIABLE NM_OUTPUT
    ERROR_VARIABLE NM_ERROR
)
if(NOT NM_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to inspect ${OUT}: ${NM_ERROR}")
endif()

set(ALLOWED_IMPORTS
    __bswapsi2 __divdi3 __divsf3 __errno __extendsfdf2 __ltdf2 __truncdfsf2 __getreent _ctype_
    atof atoi calloc exit fclose ferror fflush fopen fprintf fputc fputs fread free
    fseek ftell fwrite malloc memcpy memset mkdir printf putchar puts realloc remove
    rename snprintf sscanf strcasecmp strchr strcmp strdup strlen memmove strncasecmp strncmp
    strncpy strrchr strstr vfprintf vsnprintf
)

string(REPLACE "\r\n" ";" NM_LINES "${NM_OUTPUT}")
string(REPLACE "\n" ";" NM_LINES "${NM_LINES}")
set(UNSUPPORTED "")
foreach(LINE IN LISTS NM_LINES)
    string(STRIP "${LINE}" LINE)
    if(LINE STREQUAL "")
        continue()
    endif()
    string(REGEX REPLACE "^.*[ \t]U[ \t]+" "" SYMBOL "${LINE}")
    if(SYMBOL STREQUAL LINE)
        string(REGEX REPLACE "^U[ \t]+" "" SYMBOL "${LINE}")
    endif()
    if(NOT SYMBOL IN_LIST ALLOWED_IMPORTS)
        list(APPEND UNSUPPORTED "${SYMBOL}")
    endif()
endforeach()

if(UNSUPPORTED)
    list(REMOVE_DUPLICATES UNSUPPORTED)
    list(JOIN UNSUPPORTED ", " UNSUPPORTED_TEXT)
    message(FATAL_ERROR "${OUT} imports unsupported runtime symbols: ${UNSUPPORTED_TEXT}")
endif()

execute_process(
    COMMAND "${READELF}" -rW "${OUT}"
    RESULT_VARIABLE READELF_RESULT
    OUTPUT_VARIABLE RELOC_OUTPUT
    ERROR_VARIABLE READELF_ERROR
)
if(NOT READELF_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to inspect relocations in ${OUT}: ${READELF_ERROR}")
endif()

set(SUPPORTED_RISCV_RELOCATIONS
    R_RISCV_NONE R_RISCV_32 R_RISCV_RELATIVE R_RISCV_JUMP_SLOT
    R_RISCV_GOT_HI20 R_RISCV_PCREL_HI20 R_RISCV_PCREL_LO12_I R_RISCV_PCREL_LO12_S
    R_RISCV_CALL R_RISCV_CALL_PLT R_RISCV_BRANCH R_RISCV_JAL
    R_RISCV_RVC_BRANCH R_RISCV_RVC_JUMP R_RISCV_ADD32 R_RISCV_SUB32
    R_RISCV_RELAX
)
string(REGEX MATCHALL "R_RISCV_[A-Z0-9_]+" RELOCATIONS "${RELOC_OUTPUT}")
set(UNSUPPORTED_RELOCATIONS "")
foreach(RELOCATION IN LISTS RELOCATIONS)
    if(NOT RELOCATION IN_LIST SUPPORTED_RISCV_RELOCATIONS)
        list(APPEND UNSUPPORTED_RELOCATIONS "${RELOCATION}")
    endif()
endforeach()
if(UNSUPPORTED_RELOCATIONS)
    list(REMOVE_DUPLICATES UNSUPPORTED_RELOCATIONS)
    list(JOIN UNSUPPORTED_RELOCATIONS ", " UNSUPPORTED_RELOCATIONS_TEXT)
    message(FATAL_ERROR "${OUT} uses unsupported RISC-V relocations: ${UNSUPPORTED_RELOCATIONS_TEXT}")
endif()
