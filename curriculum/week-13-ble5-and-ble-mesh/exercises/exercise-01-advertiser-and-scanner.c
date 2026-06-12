/*
 * exercise-01-advertiser-and-scanner.c — Build and parse a BLE advertising
 * payload by hand.
 *
 * A BLE advertising payload (Core Spec 5.4 Vol 3 Part C §11) is a sequence of
 * AD (Advertising Data) structures, each of the form:
 *
 *     [length: 1 byte][AD type: 1 byte][data: (length - 1) bytes]
 *
 * The 'length' byte counts the AD-type byte plus the data, but NOT itself. The
 * whole legacy payload is at most 31 bytes. Getting this TLV framing wrong is
 * the single most common reason a new advertiser is invisible to a scanner: a
 * one-byte length error makes the rest of the payload unparseable, and most
 * scanners (including the one on every phone) silently drop the advertisement.
 *
 * This exercise has no radio. It is a pure-byte builder and parser you run on
 * your laptop so that when BTstack's gap_advertisements_set_data() refuses your
 * payload on the Pico W, you already know what a valid payload looks like.
 *
 * Build:
 *   cc -std=c11 -Wall -Wextra -O2 -o adv exercise-01-advertiser-and-scanner.c
 *
 * Citations:
 *   - Core Spec 5.4 Vol 3 Part C §11 (Advertising and Scan Response Data Format)
 *   - Bluetooth SIG Assigned Numbers (AD types)
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "ble_common.h"

/* -------------------------------------------------------------------------
 * Builder: append one AD structure to a payload buffer.
 *
 * Returns the new total used length, or 0 on overflow (so the caller can detect
 * "this AD structure does not fit in 31 bytes" and react).
 * ----------------------------------------------------------------------- */

size_t cc_ad_append(uint8_t *buf, size_t buf_cap, size_t used,
                    uint8_t ad_type, const uint8_t *data, size_t data_len) {
    if (buf == NULL) {
        return 0u;
    }
    /* The AD-length byte stores (1 + data_len) and must fit in one byte. */
    if (data_len > 254u) {
        return 0u;
    }
    size_t needed = 1u /* length byte */ + 1u /* type byte */ + data_len;
    if (used + needed > buf_cap) {
        return 0u;  /* would overflow the payload. */
    }

    buf[used + 0u] = (uint8_t)(1u + data_len);  /* length counts type + data. */
    buf[used + 1u] = ad_type;
    if (data_len > 0u && data != NULL) {
        memcpy(&buf[used + 2u], data, data_len);
    }
    return used + needed;
}

/* -------------------------------------------------------------------------
 * Parser: walk to the next AD structure.
 *
 * On entry *offset points at the length byte of the next AD structure. On a
 * successful parse, fills out_type / out_data / out_len and advances *offset to
 * the following structure. Returns false when there are no more (or a malformed
 * structure runs off the end of the buffer — the canonical "I trusted a length
 * field from the air" bug we refuse to commit).
 * ----------------------------------------------------------------------- */

bool cc_ad_next(const uint8_t *buf, size_t buf_len, size_t *offset,
                uint8_t *out_type, const uint8_t **out_data, size_t *out_len) {
    if (buf == NULL || offset == NULL) {
        return false;
    }
    size_t pos = *offset;

    /* A zero-length byte is the conventional end-of-payload pad. */
    if (pos >= buf_len) {
        return false;
    }
    uint8_t ad_len = buf[pos];
    if (ad_len == 0u) {
        return false;  /* padding / end. */
    }

    /* The structure is: [len][type][data...] where len = 1 + data_len.
       It must not run past the end of the buffer. */
    if (pos + 1u + ad_len > buf_len) {
        return false;  /* malformed: length field overruns the payload. */
    }

    if (out_type != NULL) {
        *out_type = buf[pos + 1u];
    }
    if (out_data != NULL) {
        *out_data = &buf[pos + 2u];
    }
    if (out_len != NULL) {
        *out_len = (size_t)(ad_len - 1u);  /* subtract the type byte. */
    }

    *offset = pos + 1u + ad_len;
    return true;
}

/* -------------------------------------------------------------------------
 * Helper: human-readable AD-type name for the dump.
 * ----------------------------------------------------------------------- */

static const char *ad_type_name(uint8_t t) {
    switch (t) {
        case BLE_AD_FLAGS:                   return "Flags";
        case BLE_AD_INCOMPLETE_16BIT_UUIDS:  return "Incomplete 16-bit UUIDs";
        case BLE_AD_COMPLETE_16BIT_UUIDS:    return "Complete 16-bit UUIDs";
        case BLE_AD_COMPLETE_128BIT_UUIDS:   return "Complete 128-bit UUIDs";
        case BLE_AD_SHORTENED_LOCAL_NAME:    return "Shortened Local Name";
        case BLE_AD_COMPLETE_LOCAL_NAME:     return "Complete Local Name";
        case BLE_AD_TX_POWER_LEVEL:          return "Tx Power Level";
        case BLE_AD_SERVICE_DATA_16BIT:      return "Service Data (16-bit)";
        case BLE_AD_APPEARANCE:              return "Appearance";
        case BLE_AD_MESH_PB_ADV:             return "Mesh PB-ADV";
        case BLE_AD_MESH_MESSAGE:            return "Mesh Message";
        case BLE_AD_MESH_BEACON:             return "Mesh Beacon";
        case BLE_AD_MANUFACTURER_SPECIFIC:   return "Manufacturer Specific";
        default:                             return "(unknown)";
    }
}

static void hexdump(const uint8_t *p, size_t n) {
    for (size_t i = 0u; i < n; i++) {
        printf("%02x", p[i]);
        if (i + 1u < n) {
            printf(" ");
        }
    }
}

/* -------------------------------------------------------------------------
 * Dump a full payload, structure by structure.
 * ----------------------------------------------------------------------- */

static void dump_payload(const char *label, const uint8_t *buf, size_t len) {
    printf("%s (%zu bytes): ", label, len);
    hexdump(buf, len);
    printf("\n");

    size_t offset = 0u;
    int structures = 0;
    for (;;) {
        uint8_t type;
        const uint8_t *data;
        size_t dlen;
        if (!cc_ad_next(buf, len, &offset, &type, &data, &dlen)) {
            break;
        }
        printf("  [%d] type=0x%02x %-24s data(%zu)=",
               structures, type, ad_type_name(type), dlen);
        hexdump(data, dlen);

        /* Decode a couple of common types inline. */
        if (type == BLE_AD_COMPLETE_LOCAL_NAME ||
            type == BLE_AD_SHORTENED_LOCAL_NAME) {
            printf("  (\"%.*s\")", (int)dlen, (const char *)data);
        } else if (type == BLE_AD_FLAGS && dlen == 1u) {
            printf("  (");
            if (data[0] & BLE_FLAG_LE_GENERAL_DISCOVERABLE) printf("GeneralDisc ");
            if (data[0] & BLE_FLAG_LE_LIMITED_DISCOVERABLE) printf("LimitedDisc ");
            if (data[0] & BLE_FLAG_BR_EDR_NOT_SUPPORTED)    printf("NoBR/EDR ");
            printf(")");
        } else if (type == BLE_AD_COMPLETE_16BIT_UUIDS && dlen >= 2u) {
            printf("  (");
            for (size_t i = 0u; i + 1u < dlen; i += 2u) {
                printf("0x%04x ", cc_read_le16(&data[i]));
            }
            printf(")");
        }
        printf("\n");
        structures++;
    }
    printf("  -> %d AD structures, %zu bytes consumed\n\n", structures, offset);
}

/* -------------------------------------------------------------------------
 * main: build a representative peripheral advertisement, dump it, then feed it
 * a deliberately malformed payload and confirm the parser rejects it.
 * ----------------------------------------------------------------------- */

int main(void) {
    printf("=== Exercise 1: BLE advertising payload builder/parser ===\n\n");

    /* Build a typical connectable-peripheral advertisement:
       Flags + complete local name + a 16-bit service UUID list. */
    uint8_t payload[BLE_ADV_LEGACY_MAX_PAYLOAD];
    size_t used = 0u;

    const uint8_t flags = BLE_FLAG_LE_GENERAL_DISCOVERABLE |
                          BLE_FLAG_BR_EDR_NOT_SUPPORTED;
    used = cc_ad_append(payload, sizeof(payload), used,
                        BLE_AD_FLAGS, &flags, 1u);

    const char *name = "CC Sensor";
    used = cc_ad_append(payload, sizeof(payload), used,
                        BLE_AD_COMPLETE_LOCAL_NAME,
                        (const uint8_t *)name, strlen(name));

    /* Advertise that we host the Environmental Sensing service. */
    uint8_t uuids[2];
    cc_write_le16(uuids, UUID16_ENV_SENSING_SERVICE);
    used = cc_ad_append(payload, sizeof(payload), used,
                        BLE_AD_COMPLETE_16BIT_UUIDS, uuids, sizeof(uuids));

    if (used == 0u) {
        fprintf(stderr, "error: payload overflowed 31 bytes\n");
        return 1;
    }
    dump_payload("built advertisement", payload, used);

    /* Now demonstrate the overflow guard: try to append a 30-byte name to an
       already-full payload. cc_ad_append must return 0, not corrupt memory. */
    uint8_t big_name[30];
    memset(big_name, 'X', sizeof(big_name));
    size_t after = cc_ad_append(payload, sizeof(payload), used,
                                BLE_AD_COMPLETE_LOCAL_NAME,
                                big_name, sizeof(big_name));
    printf("overflow test: appending 30-byte name to a %zu-byte payload -> %s\n\n",
           used, after == 0u ? "rejected (good)" : "ACCEPTED (BUG!)");

    /* Parse a malformed payload: an AD structure whose length claims more
       bytes than remain. A robust parser stops; a naive one reads off the end. */
    const uint8_t malformed[] = {
        0x02, 0x01, 0x06,        /* valid Flags structure.                  */
        0x10, 0x09, 'A', 'B'     /* Local Name claims len=0x10 but only 2.  */
    };
    printf("parsing a deliberately malformed payload:\n");
    dump_payload("malformed", malformed, sizeof(malformed));
    printf("(the parser stopped at the truncated structure rather than "
           "overreading — that is the correct behavior)\n\n");

    /* Sanity: the empty/zero-padded payload yields zero structures. */
    uint8_t empty[8] = {0};
    dump_payload("zero-padded empty", empty, sizeof(empty));

    printf("Done. On the Pico W, pass the 'built advertisement' bytes to\n"
           "  gap_advertisements_set_data(used, payload);\n"
           "  gap_advertisements_enable(1);\n"
           "and confirm in nRF Connect that the name and service UUID show up.\n");
    return 0;
}

/* End of exercise-01-advertiser-and-scanner.c. */
