// meshcore_packet.h
// MeshCore wire packet codec. Mirrors firmware v1.17.1 src/Packet.h/.cpp.

#ifndef MESHCORE_PACKET_H
#define MESHCORE_PACKET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "managers/meshcore_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t header;
    uint16_t payload_len;
    uint16_t path_len; // encoded: bits 0-5 hop count, bits 6-7 hash size - 1
    uint16_t transport_codes[2];
    uint8_t path[MC_MAX_PATH_SIZE];
    uint8_t payload[MC_MAX_PACKET_PAYLOAD];
    int8_t snr; // scaled SNR * 4
} mc_packet_t;

void mc_packet_init(mc_packet_t *pkt);

static inline uint8_t mc_packet_route_type(const mc_packet_t *pkt) {
    return pkt->header & MC_PH_ROUTE_MASK;
}
static inline uint8_t mc_packet_payload_type(const mc_packet_t *pkt) {
    return (pkt->header >> MC_PH_TYPE_SHIFT) & MC_PH_TYPE_MASK;
}
static inline uint8_t mc_packet_payload_ver(const mc_packet_t *pkt) {
    return (pkt->header >> MC_PH_VER_SHIFT) & MC_PH_VER_MASK;
}
static inline bool mc_packet_is_route_flood(const mc_packet_t *pkt) {
    uint8_t r = mc_packet_route_type(pkt);
    return r == MC_ROUTE_TYPE_FLOOD || r == MC_ROUTE_TYPE_TRANSPORT_FLOOD;
}
static inline bool mc_packet_is_route_direct(const mc_packet_t *pkt) {
    uint8_t r = mc_packet_route_type(pkt);
    return r == MC_ROUTE_TYPE_DIRECT || r == MC_ROUTE_TYPE_TRANSPORT_DIRECT;
}
static inline bool mc_packet_has_transport_codes(const mc_packet_t *pkt) {
    uint8_t r = mc_packet_route_type(pkt);
    return r == MC_ROUTE_TYPE_TRANSPORT_FLOOD || r == MC_ROUTE_TYPE_TRANSPORT_DIRECT;
}
static inline uint8_t mc_packet_path_hash_size(const mc_packet_t *pkt) {
    return (uint8_t)((pkt->path_len >> 6) + 1);
}
static inline uint8_t mc_packet_path_hash_count(const mc_packet_t *pkt) {
    return (uint8_t)(pkt->path_len & 63);
}
static inline void mc_packet_set_path_hash_count(mc_packet_t *pkt, uint8_t n) {
    pkt->path_len = (uint16_t)((pkt->path_len & ~63) | (n & 63));
}
static inline void mc_packet_set_path_hash_size_and_count(mc_packet_t *pkt, uint8_t sz, uint8_t n) {
    pkt->path_len = (uint16_t)(((sz - 1) << 6) | (n & 63));
}
static inline float mc_packet_snr(const mc_packet_t *pkt) {
    return ((float)pkt->snr) / 4.0f;
}
static inline void mc_packet_mark_do_not_retransmit(mc_packet_t *pkt) {
    pkt->header = 0xFF;
}
static inline bool mc_packet_is_marked_do_not_retransmit(const mc_packet_t *pkt) {
    return pkt->header == 0xFF;
}

uint8_t mc_packet_path_byte_len(const mc_packet_t *pkt);
int mc_packet_raw_length(const mc_packet_t *pkt);
uint8_t mc_packet_write_to(const mc_packet_t *pkt, uint8_t *dest);
bool mc_packet_read_from(mc_packet_t *pkt, const uint8_t *src, uint8_t len);
void mc_packet_calculate_hash(const mc_packet_t *pkt, uint8_t *hash /* MC_MAX_HASH_SIZE */);

bool mc_packet_is_valid_path_len(uint8_t path_len);
size_t mc_packet_write_path(uint8_t *dest, const uint8_t *src, uint8_t path_len);
uint8_t mc_packet_copy_path(uint8_t *dest, const uint8_t *src, uint8_t path_len);

#ifdef __cplusplus
}
#endif

#endif // MESHCORE_PACKET_H
