#ifndef OUIS_H
#define OUIS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef bool (*ouis_vendor_iter_cb_t)(const char *vendor, void *user_data);

// lookup vendor by mac address (any format with hex digits)
// returns true if found and fills out_vendor, false otherwise
bool ouis_lookup_vendor(const char *mac, char *out_vendor, size_t out_sz);

// parse an OUI prefix from any format with hex digits (e.g. 001A2B or 00:1A:2B)
bool ouis_parse_prefix(const char *prefix, uint8_t out_oui[3]);

// lookup vendor by a 3-byte OUI prefix
bool ouis_lookup_vendor_bytes(const uint8_t oui[3], char *out_vendor, size_t out_sz);

// enumerate unique vendor names from the embedded OUI list, optionally filtered by substring
int ouis_foreach_unique_vendor(const char *filter, ouis_vendor_iter_cb_t callback,
                               void *user_data, int max_results);

#endif // OUIS_H
