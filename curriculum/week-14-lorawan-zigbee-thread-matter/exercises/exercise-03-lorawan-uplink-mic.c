/*
 * exercise-03-lorawan-uplink-mic.c — Build a LoRaWAN 1.0.x uplink and compute
 * its 4-byte Message Integrity Code.
 *
 * Host-side exercise:
 *   cc -std=c11 -Wall -Wextra -I. exercise-03-lorawan-uplink-mic.c -o ex3
 *   ./ex3
 *
 * This is the cryptographic heart of LoRaWAN. Every uplink ends in a 4-byte
 * MIC computed with AES-128-CMAC over a "B0" block prepended to the message.
 * If your MIC is wrong by one bit, The Things Network drops the packet
 * silently — no error, no downlink, nothing. Getting this exactly right is
 * the difference between "my node appears in the TTN console" and "I have no
 * idea why nothing shows up." LoRaWAN L2 1.0.4 §4.4 "Message Integrity Code".
 *
 * We ship a small, self-contained AES-128 (encrypt-only — CMAC only needs the
 * forward cipher) and an AES-CMAC (RFC 4493). This is real, compiling,
 * standards-conformant code; the AES-CMAC test vector from RFC 4493 §4 is
 * checked at startup so you know the primitive is correct before you trust
 * the LoRaWAN layer on top of it.
 *
 * Citations:
 *   - LoRaWAN L2 1.0.4 §4 (MAC Message Formats), §4.4 (MIC computation).
 *   - RFC 4493 "The AES-CMAC Algorithm" (Song, Poovendran, Lee, Iwata).
 *   - FIPS 197 "Advanced Encryption Standard (AES)".
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#include "lpwan_common.h"

/* =========================================================================
 * AES-128 forward cipher (FIPS 197). Encrypt-only — CMAC never decrypts.
 * Compact table-free implementation; correctness over speed (this runs on
 * the host and, in the mini-project, a handful of times per uplink).
 * ===================================================================== */

static const uint8_t kSbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

static uint8_t xtime(uint8_t a) {
    return (uint8_t)((a << 1) ^ ((a >> 7) * 0x1Bu));
}

typedef struct { uint8_t rk[176]; } aes128_ctx_t;

static void aes128_key_expand(aes128_ctx_t *c, const uint8_t key[16]) {
    static const uint8_t rcon[10] = {
        0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1B,0x36,
    };
    memcpy(c->rk, key, 16);
    for (int i = 4; i < 44; i++) {
        uint8_t t[4];
        memcpy(t, &c->rk[(i - 1) * 4], 4);
        if (i % 4 == 0) {
            uint8_t tmp = t[0];
            t[0] = (uint8_t)(kSbox[t[1]] ^ rcon[i / 4 - 1]);
            t[1] = kSbox[t[2]];
            t[2] = kSbox[t[3]];
            t[3] = kSbox[tmp];
        }
        for (int j = 0; j < 4; j++) {
            c->rk[i * 4 + j] = (uint8_t)(c->rk[(i - 4) * 4 + j] ^ t[j]);
        }
    }
}

static void aes128_encrypt_block(const aes128_ctx_t *c,
                                 const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    memcpy(s, in, 16);
    for (int i = 0; i < 16; i++) s[i] ^= c->rk[i];

    for (int round = 1; round <= 10; round++) {
        for (int i = 0; i < 16; i++) s[i] = kSbox[s[i]];

        /* ShiftRows (column-major state). */
        uint8_t t;
        t = s[1];  s[1]=s[5];  s[5]=s[9];  s[9]=s[13]; s[13]=t;
        t = s[2];  s[2]=s[10]; s[10]=t;    t=s[6];     s[6]=s[14]; s[14]=t;
        t = s[15]; s[15]=s[11];s[11]=s[7]; s[7]=s[3];  s[3]=t;

        if (round != 10) {
            for (int col = 0; col < 4; col++) {
                uint8_t *p = &s[col * 4];
                uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
                uint8_t all = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);
                p[0] ^= all ^ xtime((uint8_t)(a0 ^ a1));
                p[1] ^= all ^ xtime((uint8_t)(a1 ^ a2));
                p[2] ^= all ^ xtime((uint8_t)(a2 ^ a3));
                p[3] ^= all ^ xtime((uint8_t)(a3 ^ a0));
            }
        }
        for (int i = 0; i < 16; i++) s[i] ^= c->rk[round * 16 + i];
    }
    memcpy(out, s, 16);
}

/* =========================================================================
 * AES-CMAC (RFC 4493).
 * ===================================================================== */

static void cmac_shift_left(const uint8_t in[16], uint8_t out[16]) {
    uint8_t overflow = 0u;
    for (int i = 15; i >= 0; i--) {
        out[i] = (uint8_t)((in[i] << 1) | overflow);
        overflow = (uint8_t)((in[i] & 0x80u) ? 1u : 0u);
    }
}

static void cmac_subkeys(const aes128_ctx_t *c, uint8_t k1[16], uint8_t k2[16]) {
    static const uint8_t zero[16] = { 0 };
    uint8_t l[16];
    aes128_encrypt_block(c, zero, l);

    cmac_shift_left(l, k1);
    if (l[0] & 0x80u) k1[15] ^= 0x87u;

    cmac_shift_left(k1, k2);
    if (k1[0] & 0x80u) k2[15] ^= 0x87u;
}

static void aes_cmac(const uint8_t key[16], const uint8_t *msg, size_t len,
                     uint8_t mac[16]) {
    aes128_ctx_t c;
    aes128_key_expand(&c, key);

    uint8_t k1[16], k2[16];
    cmac_subkeys(&c, k1, k2);

    size_t n = (len + 15u) / 16u;
    int complete;
    if (n == 0u) { n = 1u; complete = 0; }
    else complete = (len % 16u == 0u);

    uint8_t last[16];
    const uint8_t *last_block = msg + (n - 1u) * 16u;
    if (complete) {
        for (int i = 0; i < 16; i++) last[i] = (uint8_t)(last_block[i] ^ k1[i]);
    } else {
        size_t rem = len % 16u;
        memset(last, 0, 16);
        memcpy(last, last_block, rem);
        last[rem] = 0x80u;
        for (int i = 0; i < 16; i++) last[i] ^= k2[i];
    }

    uint8_t x[16] = { 0 };
    uint8_t y[16];
    for (size_t i = 0; i + 1u < n; i++) {
        for (int j = 0; j < 16; j++) y[j] = (uint8_t)(x[j] ^ msg[i * 16 + j]);
        aes128_encrypt_block(&c, y, x);
    }
    for (int j = 0; j < 16; j++) y[j] = (uint8_t)(x[j] ^ last[j]);
    aes128_encrypt_block(&c, y, mac);
}

/* =========================================================================
 * LoRaWAN uplink builder.
 *
 * PHYPayload = MHDR | MACPayload | MIC
 * MACPayload = FHDR | FPort | FRMPayload
 * FHDR       = DevAddr | FCtrl | FCnt | FOpts
 *
 * The MIC is AES-CMAC over (B0 | MHDR | FHDR | FPort | FRMPayload), keyed by
 * NwkSKey, truncated to the first 4 bytes. B0 is a 16-byte block:
 *   0x49 | 0x00 0x00 0x00 0x00 | Dir | DevAddr(4 LE) | FCntUp(4 LE) | 0x00 | msgLen
 * LoRaWAN L2 1.0.4 §4.4.
 *
 * Note: for a non-empty FRMPayload you would first encrypt it with AppSKey
 * (the §4.3.3 AES-CTR-like scheme). This exercise sends an empty payload (a
 * bare "I'm alive" uplink on FPort 1) so we focus on the MIC; the mini-project
 * adds the FRMPayload encryption.
 * ===================================================================== */

lorawan_result_t cc_lorawan_build_uplink(const lorawan_session_t *session,
                                         uint8_t fport,
                                         const uint8_t *frm_payload,
                                         size_t frm_len,
                                         lorawan_uplink_t *out) {
    if (session == NULL || out == NULL) return LORAWAN_ERR_NULL_ARG;
    if (frm_len > 0u && frm_payload == NULL) return LORAWAN_ERR_NULL_ARG;

    /* Worst case: MHDR(1)+FHDR(7)+FPort(1)+payload+MIC(4). */
    if (1u + LORAWAN_FHDR_MIN_SIZE + 1u + frm_len + LORAWAN_MIC_SIZE
            > LORAWAN_MAX_PHY_PAYLOAD) {
        return LORAWAN_ERR_TOO_LONG;
    }

    uint8_t *p = out->phy;
    size_t i = 0u;

    /* MHDR: unconfirmed data up, major R1. */
    p[i++] = (uint8_t)((LORAWAN_MTYPE_UNCONFIRMED_UP << 5) | LORAWAN_MAJOR_R1);

    /* FHDR: DevAddr (LE), FCtrl=0 (no ADR, no FOpts), FCnt (LE16). */
    p[i++] = session->devaddr[0];
    p[i++] = session->devaddr[1];
    p[i++] = session->devaddr[2];
    p[i++] = session->devaddr[3];
    p[i++] = 0x00u;                               /* FCtrl */
    p[i++] = (uint8_t)(session->fcnt_up & 0xFFu); /* FCnt LSB */
    p[i++] = (uint8_t)((session->fcnt_up >> 8) & 0xFFu);

    /* FPort + FRMPayload (sent in the clear here; see note above). */
    p[i++] = fport;
    if (frm_len > 0u) {
        memcpy(&p[i], frm_payload, frm_len);
        i += frm_len;
    }

    size_t msg_len = i;   /* MHDR..FRMPayload, before the MIC. */

    /* Build B0 and compute the MIC. */
    uint8_t b0_and_msg[16 + LORAWAN_MAX_PHY_PAYLOAD];
    uint8_t *b0 = b0_and_msg;
    memset(b0, 0, 16);
    b0[0]  = 0x49u;
    b0[5]  = LORAWAN_DIR_UPLINK;
    b0[6]  = session->devaddr[0];
    b0[7]  = session->devaddr[1];
    b0[8]  = session->devaddr[2];
    b0[9]  = session->devaddr[3];
    cc_write_le32(&b0[10], session->fcnt_up);
    b0[15] = (uint8_t)msg_len;
    memcpy(&b0_and_msg[16], out->phy, msg_len);

    uint8_t mac[16];
    aes_cmac(session->nwk_skey, b0_and_msg, 16u + msg_len, mac);

    /* MIC is the first 4 bytes of the CMAC. */
    memcpy(&p[i], mac, LORAWAN_MIC_SIZE);
    i += LORAWAN_MIC_SIZE;

    out->phy_len = i;
    return LORAWAN_OK;
}

/* =========================================================================
 * Self-tests + demo.
 * ===================================================================== */

static int check_cmac_rfc4493(void) {
    /* RFC 4493 §4: key K, and the MAC of the empty message. */
    const uint8_t key[16] = {
        0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,
        0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c,
    };
    /* Example 1: M = <empty>, AES-CMAC = bb1d6929 e9593728 7fa37d12 9b756746. */
    const uint8_t expect_empty[16] = {
        0xbb,0x1d,0x69,0x29,0xe9,0x59,0x37,0x28,
        0x7f,0xa3,0x7d,0x12,0x9b,0x75,0x67,0x46,
    };
    uint8_t mac[16];
    aes_cmac(key, NULL, 0u, mac);
    if (memcmp(mac, expect_empty, 16) != 0) {
        printf("  RFC 4493 empty-message vector FAILED.\n");
        return 0;
    }

    /* Example 2: M = first 16 bytes of the RFC's sample message. */
    const uint8_t m16[16] = {
        0x6b,0xc1,0xbe,0xe2,0x2e,0x40,0x9f,0x96,
        0xe9,0x3d,0x7e,0x11,0x73,0x93,0x17,0x2a,
    };
    const uint8_t expect_16[16] = {
        0x07,0x0a,0x16,0xb4,0x6b,0x4d,0x41,0x44,
        0xf7,0x9b,0xdd,0x9d,0xd0,0x4a,0x28,0x7c,
    };
    aes_cmac(key, m16, 16u, mac);
    if (memcmp(mac, expect_16, 16) != 0) {
        printf("  RFC 4493 16-byte vector FAILED.\n");
        return 0;
    }
    return 1;
}

static void hexdump(const char *label, const uint8_t *b, size_t n) {
    printf("  %-14s", label);
    for (size_t i = 0; i < n; i++) printf("%02X ", b[i]);
    printf("(%zu bytes)\n", n);
}

int main(void) {
    printf("=== Exercise 3: LoRaWAN uplink + MIC ===\n\n");

    printf("AES-CMAC primitive self-test (RFC 4493):\n");
    if (!check_cmac_rfc4493()) {
        printf("  CMAC is broken; the LoRaWAN MIC below cannot be trusted.\n");
        return 1;
    }
    printf("  PASS: empty + 16-byte RFC 4493 vectors match.\n\n");

    /* A made-up but well-formed ABP session. In a real deployment these come
     * from the TTN console after you register the device. */
    lorawan_session_t session = {
        .devaddr  = { 0x12, 0x34, 0x56, 0x78 },   /* DevAddr 0x78563412 LE */
        .nwk_skey = { 0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
                      0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F },
        .app_skey = { 0 },
        .fcnt_up  = 0u,
    };

    printf("Building three uplinks (empty FRMPayload, FPort 1):\n");
    for (uint32_t fcnt = 0u; fcnt < 3u; fcnt++) {
        session.fcnt_up = fcnt;
        lorawan_uplink_t up;
        lorawan_result_t r = cc_lorawan_build_uplink(&session, 1u, NULL, 0u, &up);
        if (r != LORAWAN_OK) {
            printf("  build failed for FCnt=%u (err %d)\n", fcnt, (int)r);
            return 1;
        }
        printf("FCnt=%u:\n", fcnt);
        hexdump("PHYPayload", up.phy, up.phy_len);
        hexdump("  MIC", &up.phy[up.phy_len - 4u], 4u);
    }

    printf("\nNote: the MIC changes every uplink even though the message is\n");
    printf("identical, because FCnt feeds into B0. That monotonic counter is\n");
    printf("LoRaWAN's replay defence. Reuse a FCnt and the network server\n");
    printf("rejects the frame as a replay.\n");
    return 0;
}

/* End of exercise-03-lorawan-uplink-mic.c. */
