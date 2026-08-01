#ifndef GHOSTCHI_IDENTITY_H
#define GHOSTCHI_IDENTITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GHOSTCHI_IDENTITY_NAME_MAX 8

bool ghostchi_identity_from_mac(const uint8_t mac[6], char *buf, size_t buf_len);
bool ghostchi_identity_get_name(char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif
