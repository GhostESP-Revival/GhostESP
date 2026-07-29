/*
 * SPDX-FileCopyrightText: 2023-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <reent.h>
#include <pthread.h>
#include <setjmp.h>
#include <getopt.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <math.h>

#include "rom/ets_sys.h"

#include "esp_log.h"
#include "esp_idf_version.h"
#include "esp_elf.h"

#if CONFIG_ELF_DYNAMIC_LOAD_SHARED_OBJECT
#include "private/esp_dlmod.h"
#endif
#include "private/elf_symbol.h"

extern int __ltdf2(double a, double b);
extern unsigned int __fixunsdfsi(double a);
extern int __gtdf2(double a, double b);
extern double __floatunsidf(unsigned int i);
extern double __divdf3(double a, double b);
extern float __divsf3(float a, float b);
extern double __extendsfdf2(float a);
extern long long __divdi3(long long a, long long b);
extern int *__errno(void);
#ifdef __getreent
#undef __getreent
#endif
extern struct _reent *__getreent(void);

static const char *TAG = "ELF_SYMBOL";

/** @brief Libc public functions symbols look-up table */

#ifdef CONFIG_ELF_LOADER_LIBC_SYMBOLS
static const struct esp_elfsym g_esp_libc_elfsyms[] = {

    /* string.h */

    ESP_ELFSYM_EXPORT(strerror),
    ESP_ELFSYM_EXPORT(memset),
    ESP_ELFSYM_EXPORT(memcpy),
    ESP_ELFSYM_EXPORT(strlen),
    ESP_ELFSYM_EXPORT(strtod),
    ESP_ELFSYM_EXPORT(strrchr),
    ESP_ELFSYM_EXPORT(strchr),
    ESP_ELFSYM_EXPORT(strcmp),
    ESP_ELFSYM_EXPORT(strncmp),
    ESP_ELFSYM_EXPORT(strtol),
    ESP_ELFSYM_EXPORT(strcpy),
    ESP_ELFSYM_EXPORT(strcspn),
    ESP_ELFSYM_EXPORT(strncat),
    ESP_ELFSYM_EXPORT(strncpy),
    ESP_ELFSYM_EXPORT(strstr),
    ESP_ELFSYM_EXPORT(strcasecmp),
    ESP_ELFSYM_EXPORT(strncasecmp),
    ESP_ELFSYM_EXPORT(strdup),

    /* stdio.h */

    ESP_ELFSYM_EXPORT(puts),
    ESP_ELFSYM_EXPORT(putchar),
    ESP_ELFSYM_EXPORT(fputc),
    ESP_ELFSYM_EXPORT(fputs),
    ESP_ELFSYM_EXPORT(printf),
    ESP_ELFSYM_EXPORT(snprintf),
    ESP_ELFSYM_EXPORT(vfprintf),
    ESP_ELFSYM_EXPORT(fprintf),
    ESP_ELFSYM_EXPORT(fwrite),
    ESP_ELFSYM_EXPORT(fopen),
    ESP_ELFSYM_EXPORT(fclose),
    ESP_ELFSYM_EXPORT(fread),
    ESP_ELFSYM_EXPORT(fseek),
    ESP_ELFSYM_EXPORT(ftell),
    ESP_ELFSYM_EXPORT(ferror),
    ESP_ELFSYM_EXPORT(fflush),
    ESP_ELFSYM_EXPORT(sscanf),
    ESP_ELFSYM_EXPORT(vsnprintf),

    /* unistd.h */

    ESP_ELFSYM_EXPORT(usleep),
    ESP_ELFSYM_EXPORT(sleep),
    ESP_ELFSYM_EXPORT(exit),
    ESP_ELFSYM_EXPORT(close),
    ESP_ELFSYM_EXPORT(mkdir),
    ESP_ELFSYM_EXPORT(remove),
    ESP_ELFSYM_EXPORT(rename),

    /* stdlib.h */

    ESP_ELFSYM_EXPORT(malloc),
    ESP_ELFSYM_EXPORT(calloc),
    ESP_ELFSYM_EXPORT(realloc),
    ESP_ELFSYM_EXPORT(free),
    ESP_ELFSYM_EXPORT(atoi),
    ESP_ELFSYM_EXPORT(atof),

    /* time.h */

    ESP_ELFSYM_EXPORT(clock_gettime),
    ESP_ELFSYM_EXPORT(strftime),

    /* pthread.h */

    ESP_ELFSYM_EXPORT(pthread_create),
    ESP_ELFSYM_EXPORT(pthread_attr_init),
    ESP_ELFSYM_EXPORT(pthread_attr_setstacksize),
    ESP_ELFSYM_EXPORT(pthread_detach),
    ESP_ELFSYM_EXPORT(pthread_join),
    ESP_ELFSYM_EXPORT(pthread_exit),

    /* newlib */

    ESP_ELFSYM_EXPORT(__errno),
    ESP_ELFSYM_EXPORT(__getreent),
    /* _ctype_ is the ctype lookup table that RISC-V newlib's isX()/toX() macros
       index into. Xtensa newlib exposes no such symbol (its macros go through
       __ctype_ptr__ instead), so referencing _ctype_ there fails to compile.
       Only export it on RISC-V targets; the IDF<6 builds didn't need it at all. */
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0) && defined(CONFIG_IDF_TARGET_ARCH_RISCV)
    { "_ctype_", (void *)_ctype_ },
#endif
#ifdef __HAVE_LOCALE_INFO__
    ESP_ELFSYM_EXPORT(__locale_ctype_ptr),
#elif defined(_CTYPE_DATA)
    ESP_ELFSYM_EXPORT(_ctype_),
#endif

    /* math */

    ESP_ELFSYM_EXPORT(__ltdf2),
    ESP_ELFSYM_EXPORT(__fixunsdfsi),
    ESP_ELFSYM_EXPORT(__gtdf2),
    ESP_ELFSYM_EXPORT(__floatunsidf),
    ESP_ELFSYM_EXPORT(__divdf3),
    ESP_ELFSYM_EXPORT(__divsf3),
    ESP_ELFSYM_EXPORT(__extendsfdf2),
    ESP_ELFSYM_EXPORT(__divdi3),
    ESP_ELFSYM_EXPORT(sinf),
    ESP_ELFSYM_EXPORT(cosf),

    /* getopt.h */

    ESP_ELFSYM_EXPORT(getopt_long),
    ESP_ELFSYM_EXPORT(optind),
    ESP_ELFSYM_EXPORT(opterr),
    ESP_ELFSYM_EXPORT(optarg),
    ESP_ELFSYM_EXPORT(optopt),

    /* setjmp.h */

    ESP_ELFSYM_EXPORT(longjmp),
    ESP_ELFSYM_EXPORT(setjmp),

    ESP_ELFSYM_END
};
#endif

/** @brief ESP-IDF public functions symbols look-up table */

#ifdef CONFIG_ELF_LOADER_ESPIDF_SYMBOLS
static const struct esp_elfsym g_esp_espidf_elfsyms[] = {

    /* sys/socket.h */

    ESP_ELFSYM_EXPORT(lwip_bind),
    ESP_ELFSYM_EXPORT(lwip_setsockopt),
    ESP_ELFSYM_EXPORT(lwip_socket),
    ESP_ELFSYM_EXPORT(lwip_listen),
    ESP_ELFSYM_EXPORT(lwip_accept),
    ESP_ELFSYM_EXPORT(lwip_recv),
    ESP_ELFSYM_EXPORT(lwip_recvfrom),
    ESP_ELFSYM_EXPORT(lwip_send),
    ESP_ELFSYM_EXPORT(lwip_sendto),
    ESP_ELFSYM_EXPORT(lwip_connect),

    /* arpa/inet.h */

    ESP_ELFSYM_EXPORT(ipaddr_addr),
    ESP_ELFSYM_EXPORT(lwip_htons),
    ESP_ELFSYM_EXPORT(lwip_htonl),
    ESP_ELFSYM_EXPORT(ip4addr_ntoa),

    /* ROM functions */

    ESP_ELFSYM_EXPORT(ets_printf),

    ESP_ELFSYM_END
};
#endif

/**
 * @brief Find symbol address by name.
 *
 * @param sym_name - Symbol name
 *
 * @return Symbol address if success or 0 if failed.
 */
uintptr_t elf_find_sym_default(const char *sym_name)
{
    if (!sym_name) {
        ESP_LOGE(TAG, "Invalid parameter: sym_name is NULL");
        return 0;
    }

#ifdef CONFIG_ELF_LOADER_LIBC_SYMBOLS
    {
    esp_elf_symbol_table_t *syms = g_esp_libc_elfsyms;
    while (syms->name) {
        if (!strcmp(syms->name, sym_name)) {
            return (uintptr_t)syms->sym;
        }

        syms++;
    }
    }
#endif

#ifdef CONFIG_ELF_LOADER_ESPIDF_SYMBOLS
    {
    esp_elf_symbol_table_t *syms = g_esp_espidf_elfsyms;
    while (syms->name) {
        if (!strcmp(syms->name, sym_name)) {
            return (uintptr_t)syms->sym;
        }

        syms++;
    }
    }
#endif

#ifdef CONFIG_ELF_LOADER_CUSTOMER_SYMBOLS
    {
    extern const struct esp_elfsym g_customer_elfsyms[];

    esp_elf_symbol_table_t *syms = g_customer_elfsyms;
    while (syms->name) {
        if (!strcmp(syms->name, sym_name)) {
            return (uintptr_t)syms->sym;
        }

        syms++;
    }
    }

#endif

    uintptr_t sym_addr = esp_elf_find_symbol(sym_name);
    if (sym_addr) {
        return sym_addr;
    }

#if CONFIG_ELF_DYNAMIC_LOAD_SHARED_OBJECT
    return (uintptr_t)dlmod_getaddr(sym_name);
#else
    return 0;
#endif
}
