/*
 * lorawan.h — LoRaWAN 1.0.x MAC for the Week 14 mini-project.
 *
 * Self-contained: AES-128, AES-CMAC (the MIC), and the AES-CTR-like
 * FRMPayload cipher (spec §4.3.3). No dynamic allocation, no libc beyond
 * <string.h>. Compiles on the RP2040 and on the host (cc-ttn-decode.py's C
 * sibling, and the host self-test in lorawan.c).
 *
 * Citations:
 *   - LoRaWAN L2 1.0.4 §4 (MAC Message Formats), §4.3.3 (FRMPayload
 *     encryption), §4.4 (MIC).
 *   - RFC 4493 (AES-CMAC).
 *   - FIPS 197 (AES).
 */

#ifndef CC_LORAWAN_H_
#define CC_LORAWAN_H_

#include <stddef.h>
#include <stdint.h>

#include "lpwan_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* AES-128 forward cipher context (encrypt-only; CMAC and CTR never decrypt). */
typedef struct { uint8_t rk[176]; } aes128_ctx_t;

void aes128_key_expand(aes128_ctx_t *c, const uint8_t key[16]);
void aes128_encrypt_block(const aes128_ctx_t *c,
                          const uint8_t in[16], uint8_t out[16]);

/* AES-CMAC over msg[0..len), keyed by key[16], full 16-byte tag into mac. */
void aes_cmac(const uint8_t key[16], const uint8_t *msg, size_t len,
              uint8_t mac[16]);

/*
 * Build a complete unconfirmed LoRaWAN uplink:
 *   - encrypts frm_payload in place into the PHYPayload using app_skey
 *     (the §4.3.3 stream cipher), if frm_len > 0;
 *   - computes the 4-byte MIC over the B0 block using nwk_skey;
 *   - fills out->phy / out->phy_len.
 * Returns LORAWAN_OK on success.
 */
lorawan_result_t lorawan_build_uplink(const lorawan_session_t *session,
                                      uint8_t fport,
                                      const uint8_t *frm_payload,
                                      size_t frm_len,
                                      lorawan_uplink_t *out);

/*
 * Verify + decrypt a received frame (used by cc-ttn-decode's C sibling and by
 * the downlink path in Challenge 2). On success, recovers the plaintext
 * FRMPayload into out_payload (size out_len) and returns LORAWAN_OK; returns
 * LORAWAN_ERR_MIC_MISMATCH if the MIC does not check.
 *
 * dir is LORAWAN_DIR_UPLINK or LORAWAN_DIR_DOWNLINK (the B0 direction byte).
 */
lorawan_result_t lorawan_verify_and_decrypt(const lorawan_session_t *session,
                                            uint8_t dir,
                                            const uint8_t *phy,
                                            size_t phy_len,
                                            uint8_t *out_payload,
                                            size_t *out_len,
                                            uint8_t *out_fport);

#ifdef __cplusplus
}
#endif

#endif /* CC_LORAWAN_H_ */
