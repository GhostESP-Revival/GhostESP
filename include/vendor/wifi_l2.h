/* 802.11 data-frame MAC header geometry and IEEE 802.1Q tag unwrapping.
 *
 * The promiscuous callbacks and the packet feed each need "where does the
 * payload actually start", and each previously re-derived it from the raw
 * Frame Control bits. That math is easy to get subtly wrong and the failure
 * is silent (a missed EAPOL, a missed ARP), so it lives here once and is
 * unit tested on the host.
 *
 * Deliberately free of ESP-IDF dependencies so it can be compiled and
 * exercised outside a firmware build.
 *
 * Frame Control layout as used on the wire (IEEE 802.11-2020 9.2), with the
 * Frame Control field transmitted low octet first:
 *
 *   octet 0 (frame[0]): bit 0 Protocol Version | bits 1-2 Type
 *                       | bits 3-6 Subtype
 *   octet 1 (frame[1]): bit 0 To DS | bit 1 From DS | bit 2 More Fragments
 *                       | bit 3 Retry | bit 4 Power Management
 *                       | bit 5 More Data | bit 6 Protected Frame
 *                       | bit 7 Order (+HTC)
 *
 * Verified against real frame values used by the attack builders in this
 * tree: beacon 0x80 0x00, probe request 0x40 0x00, authentication 0xB0 0x00,
 * deauthentication 0xC0 0x00, RTS 0xB4 0x00, CTS 0xC4 0x00, ACK 0xD4 0x00,
 * To DS data 0x08 0x01, From DS data 0x08 0x02, WDS data 0x08 0x03.
 */
#ifndef VENDOR_WIFI_L2_H
#define VENDOR_WIFI_L2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Standard MAC header: FC(2) + Duration(2) + Addr1/2/3(18) + SeqCtrl(2). */
#define WIFI_L2_MAC_HDR_LEN 24
/* Four-address (WDS) header, used when To DS and From DS are both set. */
#define WIFI_L2_MAC_HDR_4ADDR_LEN 30
/* QoS Control field. */
#define WIFI_L2_QOS_CTRL_LEN 2
/* HT Control field, present when the Order/+HTC bit is set. */
#define WIFI_L2_HT_CTRL_LEN 4
/* LLC/SNAP header: AA AA 03 00 00 00 <ethertype:2>. */
#define WIFI_L2_LLC_SNAP_LEN 8
/* One 802.1Q / 802.1ad tag: TPID(2) + TCI(2). */
#define WIFI_L2_VLAN_TAG_LEN 4
/* A single ethertype field sits immediately after the last VLAN tag. */
#define WIFI_L2_ETHERTYPE_LEN 2
/* 802.1Q C-tag, 802.1ad S-tag and the deprecated 0x9100 QinQ form all chain. */
#define WIFI_L2_ETHERTYPE_8021Q 0x8100
#define WIFI_L2_ETHERTYPE_8021AD 0x88A8
#define WIFI_L2_ETHERTYPE_QINQ_LEGACY 0x9100
/* Two tags is enough: one C-tag, or one S-tag wrapping one C-tag. Deeper
 * stacks are treated as untagged so a hostile tag chain cannot walk the
 * parser off the end of a short frame. */
#define WIFI_L2_MAX_VLAN_DEPTH 2

#define WIFI_L2_ETHERTYPE_ARP 0x0806
#define WIFI_L2_ETHERTYPE_IPV4 0x0800
#define WIFI_L2_ETHERTYPE_IPV6 0x86DD
#define WIFI_L2_ETHERTYPE_EAPOL 0x888E

typedef struct {
    /* Offset of the 8-byte LLC/SNAP header from the start of the MAC header. */
    size_t llc_offset;
    /* Offset of the encapsulated payload: past the LLC/SNAP header and past
     * any VLAN tags, so this is where the real protocol header starts. */
    size_t payload_offset;
    /* Innermost ethertype, with any VLAN tags peeled off. */
    uint16_t ethertype;
    /* Number of VLAN tags peeled: 0, 1 or 2. */
    uint8_t vlan_depth;
    /* Raw VLAN tag fields, least-significant tag first. PCP is bits 15-13,
     * DEI is bit 12, VID is bits 11-0. */
    uint16_t vlan_tci[WIFI_L2_MAX_VLAN_DEPTH];
    /* True when the A-MSDU Present bit was set. The bit lives in Address 4,
     * so it is only meaningful on four-address frames. It does not move the
     * LLC/SNAP offset, because the aggregated subframes sit inside the
     * payload behind the outer LLC/SNAP header. */
    bool amsdu;
} wifi_l2_info_t;

/* Splits a 16-bit Frame Control (low octet first, as frame[0] | frame[1] << 8)
 * into its type and subtype. */
uint8_t wifi_l2_fc_type(uint16_t fc);
uint8_t wifi_l2_fc_subtype(uint16_t fc);
bool wifi_l2_fc_to_ds(uint16_t fc);
bool wifi_l2_fc_from_ds(uint16_t fc);
/* True when the QoS Control field is present (subtype bit 3). */
bool wifi_l2_fc_qos(uint16_t fc);
/* True when the HT Control field is present (Order/+HTC bit). */
bool wifi_l2_fc_htc(uint16_t fc);

/* Length of the data-frame MAC header including any QoS Control and HT
 * Control fields, but excluding the LLC/SNAP header. Returns 0 if the frame
 * is too short to hold the fields it claims to carry. */
size_t wifi_l2_data_header_len(const uint8_t *frame, size_t len);

/* Offset of the A-MSDU Present bit (bit 0 of Address 4), or 0 when the frame
 * has no fourth address. */
size_t wifi_l2_addr4_offset(uint16_t fc);

/* True when the bit at `off` is set in `len`-byte big-endian. */
bool wifi_l2_tag_is_set(const uint8_t *frame, size_t len, size_t off);

/* Walks a data frame from the LLC/SNAP header, peeling any 802.1Q/802.1ad
 * tags, and fills out. Returns false when the frame is not a data frame, is
 * shorter than its own headers, or is not LLC/SNAP encapsulated. */
bool wifi_l2_parse(const uint8_t *frame, size_t len, wifi_l2_info_t *out);

#ifdef __cplusplus
}
#endif

#endif /* VENDOR_WIFI_L2_H */
