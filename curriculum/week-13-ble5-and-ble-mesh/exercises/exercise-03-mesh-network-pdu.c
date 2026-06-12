/*
 * exercise-03-mesh-network-pdu.c — Build, parse, validate, and relay a BLE Mesh
 * Network PDU.
 *
 * The Mesh Network PDU (Mesh Protocol 1.1 §3.4.4) is the unit that floods the
 * network. On the air it is obfuscated and partially encrypted; here we work
 * with the *decrypted* header so we can reason about TTL, addressing, the
 * sequence number, and the relay rules without dragging in the AES-CCM crypto
 * (BTstack's mesh stack does the crypto; you must understand the framing).
 *
 * Cleartext network header layout (big-endian multi-byte fields):
 *
 *   byte 0:  IVI(1) | NID(7)
 *   byte 1:  CTL(1) | TTL(7)
 *   bytes 2-4: SEQ (24-bit, big-endian)
 *   bytes 5-6: SRC (16-bit, big-endian)
 *   bytes 7-8: DST (16-bit, big-endian)        <- encrypted on the air
 *   bytes 9..: TransportPDU                     <- encrypted on the air
 *   trailer:  NetMIC (4 bytes if CTL=0, 8 if CTL=1)
 *
 * This exercise runs on your laptop. It is the framing you will see decoded in
 * Wireshark when you sniff the mini-project's mesh.
 *
 * Build:
 *   cc -std=c11 -Wall -Wextra -O2 -o meshpdu exercise-03-mesh-network-pdu.c
 *
 * Citations:
 *   - Mesh Protocol 1.1 §3.4 (lower transport / network layer)
 *   - Mesh Protocol 1.1 §3.4.6.3 (relay)
 *   - Mesh Protocol 1.1 §3.8 (replay protection)
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "ble_common.h"

/* -------------------------------------------------------------------------
 * Serialize a cleartext network header into a byte buffer. Returns the number
 * of header+transport bytes written (excludes the NetMIC, which the crypto
 * layer appends). Returns 0 on overflow or invalid field.
 * ----------------------------------------------------------------------- */

static size_t mesh_pack_header(const mesh_network_pdu_t *pdu,
                               uint8_t *out, size_t cap) {
    if (pdu == NULL || out == NULL) {
        return 0u;
    }
    if (pdu->ttl > MESH_TTL_MAX) {
        return 0u;
    }
    if (pdu->seq > 0x00FFFFFFu) {
        return 0u;  /* SEQ is 24-bit. */
    }
    size_t header = 9u;  /* IVI/NID, CTL/TTL, SEQ(3), SRC(2), DST(2). */
    if (header + pdu->transport_len > cap) {
        return 0u;
    }

    out[0] = (uint8_t)(((pdu->ivi & 0x01u) << 7) | (pdu->nid & 0x7Fu));
    out[1] = (uint8_t)(((pdu->ctl & 0x01u) << 7) | (pdu->ttl & 0x7Fu));
    cc_write_be24(&out[2], pdu->seq);
    cc_write_be16(&out[5], pdu->src);
    cc_write_be16(&out[7], pdu->dst);
    if (pdu->transport_len > 0u && pdu->transport_pdu != NULL) {
        memcpy(&out[9], pdu->transport_pdu, pdu->transport_len);
    }
    return header + pdu->transport_len;
}

/* -------------------------------------------------------------------------
 * Parse a cleartext network header back into a struct. 'buf' points at byte 0
 * of the header; 'len' is header + transport (NetMIC already stripped).
 * ----------------------------------------------------------------------- */

static bool mesh_unpack_header(const uint8_t *buf, size_t len,
                               mesh_network_pdu_t *out) {
    if (buf == NULL || out == NULL || len < 9u) {
        return false;
    }
    out->ivi = (uint8_t)((buf[0] >> 7) & 0x01u);
    out->nid = (uint8_t)(buf[0] & 0x7Fu);
    out->ctl = (uint8_t)((buf[1] >> 7) & 0x01u);
    out->ttl = (uint8_t)(buf[1] & 0x7Fu);
    out->seq = cc_read_be24(&buf[2]);
    out->src = cc_read_be16(&buf[5]);
    out->dst = cc_read_be16(&buf[7]);
    out->transport_pdu = &buf[9];
    out->transport_len = len - 9u;
    return true;
}

/* -------------------------------------------------------------------------
 * Validate header fields per the spec's structural rules.
 *
 * - SRC must be a unicast address (a message cannot originate from a group or
 *   virtual address — Mesh Protocol 1.1 §3.4.3).
 * - DST must be assigned (unicast, group, or virtual).
 * - TTL in [0, 127].
 *
 * (The real NetMIC check is an AES-CCM verification we delegate to BTstack; we
 * model it here as a stub that always passes for well-formed input so the
 * exercise can exercise the MESH_ERR_NETMIC_MISMATCH path with a flag.)
 * ----------------------------------------------------------------------- */

static bool g_simulate_netmic_failure = false;

mesh_result_t cc_mesh_validate(const mesh_network_pdu_t *pdu) {
    if (pdu == NULL) {
        return MESH_ERR_DST_UNASSIGNED;
    }
    if (pdu->ttl > MESH_TTL_MAX) {
        return MESH_ERR_TTL_OUT_OF_RANGE;
    }
    if (!mesh_addr_is_unicast(pdu->src)) {
        return MESH_ERR_SRC_NOT_UNICAST;
    }
    if (pdu->dst == MESH_ADDR_UNASSIGNED) {
        return MESH_ERR_DST_UNASSIGNED;
    }
    if (g_simulate_netmic_failure) {
        return MESH_ERR_NETMIC_MISMATCH;
    }
    return MESH_OK;
}

/* -------------------------------------------------------------------------
 * Relay rule (Mesh Protocol 1.1 §3.4.6.3): a relay decrements TTL by 1 before
 * re-broadcasting. A message arriving with TTL 0 or 1 is delivered locally if
 * addressed to this node but is NOT relayed. This is what bounds the flood.
 * ----------------------------------------------------------------------- */

mesh_result_t cc_mesh_relay_ttl(mesh_network_pdu_t *pdu) {
    if (pdu == NULL) {
        return MESH_ERR_NOT_RELAYABLE;
    }
    if (pdu->ttl < 2u) {
        return MESH_ERR_NOT_RELAYABLE;  /* TTL 0 or 1: do not relay. */
    }
    pdu->ttl = (uint8_t)(pdu->ttl - 1u);
    return MESH_OK;
}

/* -------------------------------------------------------------------------
 * A tiny per-source replay cache (Mesh Protocol 1.1 §3.8). A node must reject a
 * message whose (SRC, SEQ) it has already processed, both to dedup the flood
 * and to defeat replay attacks. We keep a high-water SEQ per SRC.
 * ----------------------------------------------------------------------- */

#define REPLAY_CACHE_SIZE 8u
typedef struct { uint16_t src; uint32_t last_seq; bool used; } replay_entry_t;
static replay_entry_t g_replay[REPLAY_CACHE_SIZE];

static mesh_result_t replay_check_and_update(uint16_t src, uint32_t seq) {
    replay_entry_t *slot = NULL;
    for (unsigned i = 0u; i < REPLAY_CACHE_SIZE; i++) {
        if (g_replay[i].used && g_replay[i].src == src) {
            slot = &g_replay[i];
            break;
        }
    }
    if (slot != NULL) {
        if (seq <= slot->last_seq) {
            return MESH_ERR_SEQ_REPLAY;  /* old or duplicate — drop. */
        }
        slot->last_seq = seq;
        return MESH_OK;
    }
    /* New source: claim a free slot. */
    for (unsigned i = 0u; i < REPLAY_CACHE_SIZE; i++) {
        if (!g_replay[i].used) {
            g_replay[i].used = true;
            g_replay[i].src = src;
            g_replay[i].last_seq = seq;
            return MESH_OK;
        }
    }
    return MESH_OK;  /* cache full: a real node evicts LRU; we accept. */
}

static const char *result_name(mesh_result_t r) {
    switch (r) {
        case MESH_OK:                    return "OK";
        case MESH_ERR_TTL_OUT_OF_RANGE:  return "TTL_OUT_OF_RANGE";
        case MESH_ERR_SRC_NOT_UNICAST:   return "SRC_NOT_UNICAST";
        case MESH_ERR_DST_UNASSIGNED:    return "DST_UNASSIGNED";
        case MESH_ERR_SEQ_REPLAY:        return "SEQ_REPLAY";
        case MESH_ERR_NETMIC_MISMATCH:   return "NETMIC_MISMATCH";
        case MESH_ERR_NOT_RELAYABLE:     return "NOT_RELAYABLE";
        default:                         return "?";
    }
}

static const char *addr_kind(uint16_t a) {
    if (a == MESH_ADDR_UNASSIGNED)  return "unassigned";
    if (mesh_addr_is_unicast(a))    return "unicast";
    if (mesh_addr_is_virtual(a))    return "virtual";
    if (mesh_addr_is_group(a))      return "group";
    return "?";
}

static void print_pdu(const char *label, const mesh_network_pdu_t *p) {
    printf("%s: IVI=%u NID=0x%02x CTL=%u TTL=%u SEQ=%" PRIu32
           " SRC=0x%04x(%s) DST=0x%04x(%s) tlen=%zu\n",
           label, p->ivi, p->nid, p->ctl, p->ttl, p->seq,
           p->src, addr_kind(p->src), p->dst, addr_kind(p->dst),
           p->transport_len);
}

int main(void) {
    printf("=== Exercise 3: BLE Mesh Network PDU ===\n\n");

    /* The occupancy node (unicast 0x0002) publishes a Generic OnOff Status
       (transport payload, opcode 0x8204 + state byte) to the kitchen-lights
       group (0xC000), TTL 7. */
    uint8_t transport[3];
    cc_write_be16(transport, MESH_GENERIC_ONOFF_STATUS);  /* 0x8204 */
    transport[2] = 0x01u;                                  /* state = ON */

    mesh_network_pdu_t tx = {
        .ivi = 0u, .nid = 0x68u, .ctl = 0u, .ttl = MESH_TTL_DEFAULT,
        .seq = 0x000123u, .src = 0x0002u, .dst = MESH_ADDR_GROUP_MIN,
        .transport_pdu = transport, .transport_len = sizeof(transport),
    };
    print_pdu("origin ", &tx);

    /* Serialize, then parse back, to prove the framing round-trips. */
    uint8_t wire[32];
    size_t n = mesh_pack_header(&tx, wire, sizeof(wire));
    if (n == 0u) {
        fprintf(stderr, "error: pack failed\n");
        return 1;
    }
    printf("packed header (%zu bytes): ", n);
    for (size_t i = 0u; i < n; i++) printf("%02x ", wire[i]);
    printf("\n");

    mesh_network_pdu_t rx;
    if (!mesh_unpack_header(wire, n, &rx)) {
        fprintf(stderr, "error: unpack failed\n");
        return 1;
    }
    print_pdu("parsed ", &rx);

    /* Validate, then push through the replay cache, then relay. */
    printf("\nvalidate: %s\n", result_name(cc_mesh_validate(&rx)));
    printf("replay (first time): %s\n",
           result_name(replay_check_and_update(rx.src, rx.seq)));
    printf("replay (same SEQ again): %s   <- the flood dedup that stops loops\n",
           result_name(replay_check_and_update(rx.src, rx.seq)));

    mesh_result_t rr = cc_mesh_relay_ttl(&rx);
    printf("\nrelay node decrements TTL: %s\n", result_name(rr));
    print_pdu("relayed", &rx);

    /* Walk the TTL down to show the flood terminates. */
    printf("\nrepeated relays until the flood dies:\n");
    while (cc_mesh_relay_ttl(&rx) == MESH_OK) {
        printf("  relayed, TTL now %u\n", rx.ttl);
    }
    printf("  TTL=%u: not relayed (flood terminates)\n", rx.ttl);

    /* Negative cases. */
    printf("\nnegative cases:\n");
    mesh_network_pdu_t bad = tx;
    bad.src = MESH_ADDR_GROUP_MIN;  /* a group cannot be a source. */
    printf("  SRC=group: %s\n", result_name(cc_mesh_validate(&bad)));
    bad = tx; bad.dst = MESH_ADDR_UNASSIGNED;
    printf("  DST=unassigned: %s\n", result_name(cc_mesh_validate(&bad)));
    bad = tx; bad.ttl = 1u;
    printf("  TTL=1 relay attempt: %s\n", result_name(cc_mesh_relay_ttl(&bad)));
    g_simulate_netmic_failure = true;
    printf("  simulated bad NetMIC: %s\n", result_name(cc_mesh_validate(&tx)));
    g_simulate_netmic_failure = false;

    printf("\nDone. In the mini-project, BTstack does the AES-CCM that produces\n"
           "the NetMIC and obfuscates bytes 0-1; your relay node runs exactly\n"
           "the TTL-decrement and replay-cache logic shown above.\n");
    return 0;
}

/* End of exercise-03-mesh-network-pdu.c. */
