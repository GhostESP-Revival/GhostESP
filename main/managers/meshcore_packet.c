// meshcore_packet.c
// MeshCore wire packet codec. Mirrors firmware v1.17.1 src/Packet.cpp.

#include "managers/meshcore_packet.h"
#include "sdkconfig.h"

#ifdef CONFIG_HAS_MESHCORE

#include "managers/meshcore_crypto.h"

#include <string.h>

void mc_packet_init(mc_packet_t *pkt) {
    if (!pkt) return;
    memset(pkt, 0, sizeof(*pkt));
}

bool mc_packet_is_valid_path_len(uint8_t path_len) {
    uint8_t hash_count = path_len & 63;
    uint8_t hash_size = (uint8_t)((path_len >> 6) + 1);
    if (hash_size == 4) return false; // reserved for future
    return (uint16_t)hash_count * hash_size <= MC_MAX_PATH_SIZE;
}

uint8_t mc_packet_path_byte_len(const mc_packet_t *pkt) {
    return (uint8_t)(mc_packet_path_hash_count(pkt) * mc_packet_path_hash_size(pkt));
}

size_t mc_packet_write_path(uint8_t *dest, const uint8_t *src, uint8_t path_len) {
    uint8_t hash_count = path_len & 63;
    uint8_t hash_size = (uint8_t)((path_len >> 6) + 1);
    size_t len = (size_t)hash_count * hash_size;
    if (len > MC_MAX_PATH_SIZE) return 0;
    memcpy(dest, src, len);
    return len;
}

uint8_t mc_packet_copy_path(uint8_t *dest, const uint8_t *src, uint8_t path_len) {
    mc_packet_write_path(dest, src, path_len);
    return path_len;
}

int mc_packet_raw_length(const mc_packet_t *pkt) {
    return 2 + mc_packet_path_byte_len(pkt) + pkt->payload_len +
           (mc_packet_has_transport_codes(pkt) ? 4 : 0);
}

void mc_packet_calculate_hash(const mc_packet_t *pkt, uint8_t *hash) {
    uint8_t t = mc_packet_payload_type(pkt);
    if (t == MC_PAYLOAD_TYPE_TRACE) {
        // TRACE packets can revisit the same node on the return path, so mix
        // the path in as upstream does.
        uint8_t buf[3];
        buf[0] = t;
        memcpy(&buf[1], &pkt->path_len, sizeof(pkt->path_len));
        mc_sha256_2(hash, MC_MAX_HASH_SIZE, buf, sizeof(buf), pkt->payload, pkt->payload_len);
    } else {
        mc_sha256_2(hash, MC_MAX_HASH_SIZE, &t, 1, pkt->payload, pkt->payload_len);
    }
}

uint8_t mc_packet_write_to(const mc_packet_t *pkt, uint8_t *dest) {
    uint8_t i = 0;
    dest[i++] = pkt->header;
    if (mc_packet_has_transport_codes(pkt)) {
        memcpy(&dest[i], &pkt->transport_codes[0], 2);
        i += 2;
        memcpy(&dest[i], &pkt->transport_codes[1], 2);
        i += 2;
    }
    dest[i++] = (uint8_t)pkt->path_len;
    i = (uint8_t)(i + mc_packet_write_path(&dest[i], pkt->path, (uint8_t)pkt->path_len));
    memcpy(&dest[i], pkt->payload, pkt->payload_len);
    i = (uint8_t)(i + pkt->payload_len);
    return i;
}

bool mc_packet_read_from(mc_packet_t *pkt, const uint8_t *src, uint8_t len) {
    if (!pkt || !src || len < 2) return false;

    uint8_t i = 0;
    pkt->header = src[i++];
    if (mc_packet_has_transport_codes(pkt)) {
        if (i + 4 > len) return false;
        memcpy(&pkt->transport_codes[0], &src[i], 2);
        i += 2;
        memcpy(&pkt->transport_codes[1], &src[i], 2);
        i += 2;
    } else {
        pkt->transport_codes[0] = pkt->transport_codes[1] = 0;
    }

    if (i >= len) return false;
    pkt->path_len = src[i++];
    if (!mc_packet_is_valid_path_len((uint8_t)pkt->path_len)) return false;

    uint8_t bl = mc_packet_path_byte_len(pkt);
    if (i + bl > len) return false;
    memcpy(pkt->path, &src[i], bl);
    i = (uint8_t)(i + bl);

    if (i >= len) return false;
    pkt->payload_len = (uint16_t)(len - i);
    if (pkt->payload_len > sizeof(pkt->payload)) return false;
    memcpy(pkt->payload, &src[i], pkt->payload_len);
    return true;
}

#else
typedef int meshcore_packet_stub_guard;
#endif // CONFIG_HAS_MESHCORE
