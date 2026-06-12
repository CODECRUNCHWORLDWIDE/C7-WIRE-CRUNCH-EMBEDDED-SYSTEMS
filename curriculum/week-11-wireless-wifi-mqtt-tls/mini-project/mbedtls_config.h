/*
 * mbedtls_config.h — trimmed mbedTLS configuration for the sensor node.
 *
 * The stock mbedTLS build enables hundreds of cipher suites, every named
 * curve, TLS 1.0 through 1.3, OCSP, ALPN, client certificates, and ~30
 * certificate-parsing paths — roughly 250 KB of flash. We need exactly
 * one client-side TLS 1.2 cipher suite to talk to test.mosquitto.org, so
 * we #undef everything else. The result is ~95 KB of .text.
 *
 * The single suite we keep: ECDHE-RSA-AES128-GCM-SHA256, which the broker
 * offers and which handshakes in ~1.2 s on the Pico at 125 MHz. The one
 * curve: secp256r1 (P-256). TLS 1.2 only.
 *
 * Reference: mbedTLS configuration guide,
 * https://mbed-tls.readthedocs.io/en/latest/kb/how-to/how-do-i-configure-mbedtls/
 *
 * This file is selected via -DMBEDTLS_CONFIG_FILE in CMakeLists.txt.
 */

#ifndef MBEDTLS_CONFIG_H
#define MBEDTLS_CONFIG_H

/* ----- System / platform ------------------------------------------------ */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_NO_PLATFORM_ENTROPY        /* we supply our own RNG source */
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_ENTROPY_HARDWARE_ALT       /* RP2040 ring-oscillator RNG */
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ERROR_C

/* ----- TLS protocol: 1.2 client only ------------------------------------ */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_SERVER_NAME_INDICATION  /* SNI for test.mosquitto.org */

/* ----- Key exchange: ECDHE-RSA only ------------------------------------- */
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED

/* ----- Crypto primitives the one suite needs ---------------------------- */
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21                  /* RSASSA-PSS in cert signatures */
#define MBEDTLS_OID_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C

/* The one curve we keep. */
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED

/* ----- X.509: parse the server chain and our pinned root ---------------- */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_BASE64_C

/* ----- Optional: TLS debug for Challenge 2 ------------------------------ */
#define MBEDTLS_DEBUG_C

/* ----- Memory tuning for a 264 KB-SRAM target --------------------------- */
/* One outbound + one inbound TLS record at the negotiated max fragment. We
 * never send records larger than a CONNECT/PUBLISH (< 512 B) and the broker
 * cert chain (leaf + R3) arrives in a few KB, so a 4 KB I/O buffer is ample
 * and saves ~28 KB of RAM versus the 16 KB default. */
#define MBEDTLS_SSL_IN_CONTENT_LEN   4096
#define MBEDTLS_SSL_OUT_CONTENT_LEN  4096
#define MBEDTLS_SSL_MAX_FRAGMENT_LENGTH

/* ----- Things we explicitly do NOT want (documented for reviewers) ------ */
/* No server role, no TLS 1.0/1.1/1.3, no DTLS, no client certs, no OCSP,
 * no ALPN, no session tickets, no curve other than P-256, no cipher other
 * than AES-GCM. None of the corresponding *_C macros are defined above, so
 * they are absent by omission; we do not #undef because this is a from-
 * scratch config, not a patch over the default config.h. */

#include "mbedtls/check_config.h"

#endif /* MBEDTLS_CONFIG_H */
