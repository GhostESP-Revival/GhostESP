#include "vendor/wifi_l2.h"

#include <string.h>

uint8_t wifi_l2_fc_type(uint16_t fc) {
    return (uint8_t)((fc >> 2) & 0x3);
}

uint8_t wifi_l2_fc_subtype(uint16_t fc) {
    return (uint8_t)((fc >> 4) & 0xF);
}

bool wifi_l2_fc_to_ds(uint16_t fc) {
    return (fc & 0x0100u) != 0u;
}

bool wifi_l2_fc_from_ds(uint16_t fc) {
    return (fc & 0x0200u) != 0u;
}

bool wifi_l2_fc_qos(uint16_t fc) {
    return (wifi_l2_fc_subtype(fc) & 0x8u) != 0u;
}

bool wifi_l2_fc_htc(uint16_t fc) {
    return (fc & 0x8000u) != 0u;
}

size_t wifi_l2_addr4_offset(uint16_t fc) {
    /* Address 4 is only part of the MAC header when the frame is four
     * address (WDS), which is exactly the To DS && From DS case. */
    return (wifi_l2_fc_to_ds(fc) && wifi_l2_fc_from_ds(fc)) ? 24u : 0u;
}

bool wifi_l2_tag_is_set(const uint8_t *frame, size_t len, size_t off) {
    if (frame == NULL || off >= len) {
        return false;
    }
    return (frame[off] & 0x01u) != 0u;
}

size_t wifi_l2_data_header_len(const uint8_t *frame, size_t len) {
    if (frame == NULL || len < 2) {
        return 0;
    }

    const uint16_t fc = (uint16_t)(frame[0] | ((uint16_t)frame[1] << 8));
    const bool four_addr = wifi_l2_fc_to_ds(fc) && wifi_l2_fc_from_ds(fc);

    size_t header_len = four_addr ? WIFI_L2_MAC_HDR_4ADDR_LEN : WIFI_L2_MAC_HDR_LEN;
    if (wifi_l2_fc_qos(fc)) {
        header_len += WIFI_L2_QOS_CTRL_LEN;
    }
    /* The HT Control field is gated on the Order bit, not on the QoS
     * subtype bit, so a plain data frame can carry it too. */
    if (wifi_l2_fc_htc(fc)) {
        header_len += WIFI_L2_HT_CTRL_LEN;
    }

    /* Never report a header the frame could not possibly contain. */
    if (len < header_len) {
        return 0;
    }
    return header_len;
}

static bool is_vlan_ethertype(uint16_t ethertype) {
    return ethertype == WIFI_L2_ETHERTYPE_8021Q ||
           ethertype == WIFI_L2_ETHERTYPE_8021AD ||
           ethertype == WIFI_L2_ETHERTYPE_QINQ_LEGACY;
}

bool wifi_l2_parse(const uint8_t *frame, size_t len, wifi_l2_info_t *out) {
    if (frame == NULL || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    if (len < 2) {
        return false;
    }
    const uint16_t fc = (uint16_t)(frame[0] | ((uint16_t)frame[1] << 8));
    if (wifi_l2_fc_type(fc) != 2) {
        return false;
    }

    const size_t llc_offset = wifi_l2_data_header_len(frame, len);
    if (llc_offset == 0 || len < llc_offset + WIFI_L2_LLC_SNAP_LEN) {
        return false;
    }

    const uint8_t *llc = frame + llc_offset;
    if (llc[0] != 0xAA || llc[1] != 0xAA || llc[2] != 0x03) {
        return false;
    }

    out->llc_offset = llc_offset;

    /* The A-MSDU Present bit is bit 0 of Address 4. It does not shift the
     * LLC/SNAP offset, it only says the payload holds aggregated subframes. */
    const size_t addr4 = wifi_l2_addr4_offset(fc);
    if (addr4 != 0) {
        out->amsdu = wifi_l2_tag_is_set(frame, len, addr4);
    }

    /* The ethertype occupies the last two bytes of the LLC/SNAP header, at
     * llc_offset + 6. When the frame is tagged, that field holds a TPID
     * instead and the payload is pushed out by a 4-byte tag, so the next
     * ethertype field always sits 4 bytes further along. */
    size_t ethertype_off = llc_offset + 6;
    uint16_t ethertype;

    while (true) {
        if (ethertype_off + WIFI_L2_ETHERTYPE_LEN > len) {
            return false;
        }
        ethertype = (uint16_t)((frame[ethertype_off] << 8) | frame[ethertype_off + 1]);
        if (!is_vlan_ethertype(ethertype)) {
            break;
        }
        /* A tag is a TCI immediately after its TPID, then the next ethertype
         * field follows the TCI. */
        if (out->vlan_depth >= WIFI_L2_MAX_VLAN_DEPTH) {
            return false;
        }
        const size_t tci_off = ethertype_off + WIFI_L2_ETHERTYPE_LEN;
        if (tci_off + 2 > len) {
            return false;
        }
        out->vlan_tci[out->vlan_depth] = (uint16_t)((frame[tci_off] << 8) | frame[tci_off + 1]);
        out->vlan_depth++;
        ethertype_off = tci_off + 2;
    }

    out->ethertype = ethertype;
    out->payload_offset = ethertype_off + WIFI_L2_ETHERTYPE_LEN;
    return true;
}
