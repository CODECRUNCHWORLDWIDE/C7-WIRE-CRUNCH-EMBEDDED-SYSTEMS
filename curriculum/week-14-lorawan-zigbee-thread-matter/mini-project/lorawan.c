/*
 * lorawan.c — LoRaWAN 1.0.x MAC: AES-128, AES-CMAC, FRMPayload cipher,
 * uplink builder, and a verify/decrypt path.
 *
 * Build a host self-test:
 *   cc -std=c11 -Wall -Wextra -DLORAWAN_SELFTEST -I../exercises -I. \
 *      lorawan.c -o lorawan_test && ./lorawan_test
 *
 * On the RP2040 the same file links into the node firmware with no changes.
 *
 * Citations:
 *   - LoRaWAN L2 1.0.4 §4.3.3 (FRMPayload encryption), §4.4 (MIC).
 *   - RFC 4493 (AES-CMAC).  - FIPS 197 (AES).
 */

#include <string.h>

#include "lorawan.h"

/* =========================================================================
 * AES-128 forward cipher (FIPS 197).
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

void aes128_key_expand(aes128_ctx_t *c, const uint8_t key[16]) {
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

void aes128_encrypt_block(const aes128_ctx_t *c,
                          const uint8_t in[16], uint8_t out[16]) {
    uint8_t s[16];
    memcpy(s, in, 16);
    for (int i = 0; i < 16; i++) s[i] ^= c->rk[i];

    for (int round = 1; round <= 10; round++) {
        for (int i = 0; i < 16; i++) s[i] = kSbox[s[i]];

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

void aes_cmac(const uint8_t key[16], const uint8_t *msg, size_t len,
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
        if (rem > 0u) memcpy(last, last_block, rem);
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
 * FRMPayload cipher (LoRaWAN L2 1.0.4 §4.3.3).
 *
 * The payload is encrypted with an AES-CTR-like keystream. For each 16-byte
 * block i (1-indexed), build:
 *   A_i = 0x01 | 0x00 0x00 0x00 0x00 | Dir | DevAddr(4 LE) | FCnt(4 LE) | 0x00 | i
 * encrypt A_i with the session key, and XOR the keystream into the payload.
 * Encryption and decryption are the same operation (XOR stream cipher).
 * ===================================================================== */

static void frmpayload_crypt(const uint8_t key[16], uint8_t dir,
                             const uint8_t devaddr[4], uint32_t fcnt,
                             uint8_t *payload, size_t len) {
    aes128_ctx_t c;
    aes128_key_expand(&c, key);

    size_t blocks = (len + 15u) / 16u;
    for (size_t i = 1u; i <= blocks; i++) {
        uint8_t a[16];
        memset(a, 0, 16);
        a[0]  = 0x01u;
        a[5]  = dir;
        memcpy(&a[6], devaddr, 4);
        cc_write_le32(&a[10], fcnt);
        a[15] = (uint8_t)i;

        uint8_t s[16];
        aes128_encrypt_block(&c, a, s);

        size_t base = (i - 1u) * 16u;
        size_t n = (len - base < 16u) ? (len - base) : 16u;
        for (size_t j = 0; j < n; j++) payload[base + j] ^= s[j];
    }
}

/* =========================================================================
 * Uplink builder.
 * ===================================================================== */

static lorawan_result_t compute_mic(const uint8_t nwk_skey[16], uint8_t dir,
                                    const uint8_t devaddr[4], uint32_t fcnt,
                                    const uint8_t *msg, size_t msg_len,
                                    uint8_t mic[4]) {
    uint8_t buf[16 + LORAWAN_MAX_PHY_PAYLOAD];
    memset(buf, 0, 16);
    buf[0]  = 0x49u;
    buf[5]  = dir;
    memcpy(&buf[6], devaddr, 4);
    cc_write_le32(&buf[10], fcnt);
    buf[15] = (uint8_t)msg_len;
    memcpy(&buf[16], msg, msg_len);

    uint8_t tag[16];
    aes_cmac(nwk_skey, buf, 16u + msg_len, tag);
    memcpy(mic, tag, 4);
    return LORAWAN_OK;
}

lorawan_result_t lorawan_build_uplink(const lorawan_session_t *session,
                                      uint8_t fport,
                                      const uint8_t *frm_payload,
                                      size_t frm_len,
                                      lorawan_uplink_t *out) {
    if (session == NULL || out == NULL) return LORAWAN_ERR_NULL_ARG;
    if (frm_len > 0u && frm_payload == NULL) return LORAWAN_ERR_NULL_ARG;
    if (1u + LORAWAN_FHDR_MIN_SIZE + 1u + frm_len + LORAWAN_MIC_SIZE
            > LORAWAN_MAX_PHY_PAYLOAD) {
        return LORAWAN_ERR_TOO_LONG;
    }

    uint8_t *p = out->phy;
    size_t i = 0u;

    p[i++] = (uint8_t)((LORAWAN_MTYPE_UNCONFIRMED_UP << 5) | LORAWAN_MAJOR_R1);
    memcpy(&p[i], session->devaddr, 4); i += 4u;   /* DevAddr LE */
    p[i++] = 0x00u;                                /* FCtrl */
    p[i++] = (uint8_t)(session->fcnt_up & 0xFFu);  /* FCnt LE */
    p[i++] = (uint8_t)((session->fcnt_up >> 8) & 0xFFu);

    p[i++] = fport;

    size_t payload_off = i;
    if (frm_len > 0u) {
        memcpy(&p[i], frm_payload, frm_len);
        /* Encrypt in place: FPort 0 uses NwkSKey, 1..223 use AppSKey. */
        const uint8_t *ckey = (fport == 0u) ? session->nwk_skey
                                            : session->app_skey;
        frmpayload_crypt(ckey, LORAWAN_DIR_UPLINK, session->devaddr,
                         session->fcnt_up, &p[payload_off], frm_len);
        i += frm_len;
    }

    size_t msg_len = i;

    uint8_t mic[4];
    compute_mic(session->nwk_skey, LORAWAN_DIR_UPLINK, session->devaddr,
                session->fcnt_up, p, msg_len, mic);
    memcpy(&p[i], mic, 4); i += 4u;

    out->phy_len = i;
    return LORAWAN_OK;
}

lorawan_result_t lorawan_verify_and_decrypt(const lorawan_session_t *session,
                                            uint8_t dir,
                                            const uint8_t *phy,
                                            size_t phy_len,
                                            uint8_t *out_payload,
                                            size_t *out_len,
                                            uint8_t *out_fport) {
    if (session == NULL || phy == NULL || out_len == NULL) {
        return LORAWAN_ERR_NULL_ARG;
    }
    /* Minimum: MHDR(1)+FHDR(7)+MIC(4) = 12; with FPort it is 13. */
    if (phy_len < 1u + LORAWAN_FHDR_MIN_SIZE + LORAWAN_MIC_SIZE) {
        return LORAWAN_ERR_TOO_SHORT;
    }

    size_t msg_len = phy_len - LORAWAN_MIC_SIZE;

    /* The DevAddr in the frame must match the session. */
    if (memcmp(&phy[1], session->devaddr, 4) != 0) {
        return LORAWAN_ERR_MIC_MISMATCH;  /* not for us */
    }
    uint32_t fcnt = cc_read_le16(&phy[6]);   /* low 16 bits on the wire */

    uint8_t mic[4];
    compute_mic(session->nwk_skey, dir, session->devaddr, fcnt,
                phy, msg_len, mic);
    if (memcmp(mic, &phy[phy_len - 4u], 4) != 0) {
        return LORAWAN_ERR_MIC_MISMATCH;
    }

    /* FHDR length: DevAddr(4)+FCtrl(1)+FCnt(2)+FOpts(FOptsLen). */
    uint8_t fopts_len = (uint8_t)(phy[5] & 0x0Fu);
    size_t fhdr_len = 1u + LORAWAN_FHDR_MIN_SIZE + fopts_len; /* incl MHDR */
    if (fhdr_len + 1u > msg_len) {
        /* No FPort/payload present. */
        *out_len = 0u;
        if (out_fport) *out_fport = 0xFFu;
        return LORAWAN_OK;
    }

    uint8_t fport = phy[fhdr_len];
    size_t pl_off = fhdr_len + 1u;
    size_t pl_len = msg_len - pl_off;

    if (pl_len > 0u) {
        memcpy(out_payload, &phy[pl_off], pl_len);
        const uint8_t *ckey = (fport == 0u) ? session->nwk_skey
                                            : session->app_skey;
        frmpayload_crypt(ckey, dir, session->devaddr, fcnt, out_payload, pl_len);
    }
    *out_len = pl_len;
    if (out_fport) *out_fport = fport;
    return LORAWAN_OK;
}

/* =========================================================================
 * Optional host self-test.
 * ===================================================================== */
#ifdef LORAWAN_SELFTEST
#include <stdio.h>

static int cmac_rfc4493(void) {
    const uint8_t key[16] = {
        0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,
        0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c,
    };
    const uint8_t exp0[16] = {
        0xbb,0x1d,0x69,0x29,0xe9,0x59,0x37,0x28,
        0x7f,0xa3,0x7d,0x12,0x9b,0x75,0x67,0x46,
    };
    uint8_t mac[16];
    aes_cmac(key, NULL, 0u, mac);
    return memcmp(mac, exp0, 16) == 0;
}

int main(void) {
    printf("=== lorawan.c self-test ===\n");
    if (!cmac_rfc4493()) { printf("CMAC RFC4493 FAIL\n"); return 1; }
    printf("CMAC RFC4493 empty vector: PASS\n");

    lorawan_session_t s = {
        .devaddr  = { 0x88, 0x1F, 0x01, 0x26 },  /* DevAddr 0x26011F88 LE */
        .nwk_skey = { 0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,
                      0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c },
        .app_skey = { 0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
                      0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff },
        .fcnt_up  = 7u,
    };

    /* A 3-byte telemetry payload. */
    uint8_t payload[3] = { 0x09, 0x7F, 0x5C };
    lorawan_uplink_t up;
    if (lorawan_build_uplink(&s, 1u, payload, 3u, &up) != LORAWAN_OK) {
        printf("build FAIL\n"); return 1;
    }
    printf("PHYPayload (%zu B): ", up.phy_len);
    for (size_t i = 0; i < up.phy_len; i++) printf("%02X ", up.phy[i]);
    printf("\n");

    /* Round-trip: verify + decrypt the frame we just built, recover plaintext. */
    uint8_t rec[64]; size_t rec_len; uint8_t fport;
    lorawan_result_t r = lorawan_verify_and_decrypt(&s, LORAWAN_DIR_UPLINK,
                                                    up.phy, up.phy_len,
                                                    rec, &rec_len, &fport);
    if (r != LORAWAN_OK) { printf("verify FAIL (%d)\n", (int)r); return 1; }
    printf("verify+decrypt: MIC OK, FPort=%u, %zu plaintext bytes: ",
           fport, rec_len);
    for (size_t i = 0; i < rec_len; i++) printf("%02X ", rec[i]);
    printf("\n");

    if (rec_len != 3u || memcmp(rec, payload, 3) != 0) {
        printf("ROUND-TRIP MISMATCH\n"); return 1;
    }
    printf("round-trip plaintext matches: PASS\n");
    return 0;
}
#endif /* LORAWAN_SELFTEST */

/* End of lorawan.c. */
