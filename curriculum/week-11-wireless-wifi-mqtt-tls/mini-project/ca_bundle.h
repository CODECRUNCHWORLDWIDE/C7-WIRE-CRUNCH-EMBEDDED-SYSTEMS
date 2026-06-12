/*
 * ca_bundle.h — The pinned trust anchor for the sensor node.
 *
 * We do not ship a system CA store; an MCU cannot afford one (see
 * Lecture 3 and Challenge 2). We pin the single root that
 * test.mosquitto.org's leaf certificate chains to: ISRG Root X1, the
 * Let's Encrypt root. The broker presents `leaf -> R3 -> ISRG Root X1`;
 * mbedTLS receives the leaf and R3 in the server's Certificate message
 * and walks the chain up to this pinned root.
 *
 * Rotation: ISRG Root X1's current cross-sign is valid to 2035-06-04.
 * Before that date — and before any earlier broker re-issue under a
 * different root — you reflash with the new root (Challenge 2 is the
 * rehearsal). During an overlap window, concatenate the next root after
 * this one in isrgrootx1.pem; mbedtls_x509_crt_parse accepts a
 * concatenation of PEMs and mbedtls_ssl_conf_ca_chain then trusts all of
 * them.
 *
 * Why this header does NOT contain the literal Base64 of the cert: a
 * pinned root is security-critical, and a hand-transcribed cert that is
 * even one character wrong silently defeats the entire point of pinning.
 * So we fetch the authoritative PEM at configure time and embed it
 * verbatim. CMakeLists.txt downloads
 *   https://letsencrypt.org/certs/isrgrootx1.pem
 * (or copies a vendored, checksum-verified copy from certs/) and runs it
 * through `xxd -i` to generate ca_root_pem.h, which defines
 *   extern const unsigned char isrgrootx1_pem[];
 *   extern const unsigned int  isrgrootx1_pem_len;
 * We expose those under the CC_CA_ROOT_* names the TLS setup expects.
 *
 * The generated array is NOT NUL-terminated by xxd, so we pass a length
 * that is one greater than the file size with a trailing '\0' appended by
 * the generator step (CMake's `printf '\0' >> isrgrootx1.pem` before
 * xxd) — mbedtls_x509_crt_parse requires the PEM length to include the
 * terminator.
 */

#ifndef CA_BUNDLE_H
#define CA_BUNDLE_H

#include <stddef.h>

/* Generated at configure time from isrgrootx1.pem; see CMakeLists.txt. */
#include "ca_root_pem.h"

#define CC_CA_ROOT_PEM      ((const unsigned char *) isrgrootx1_pem)
#define CC_CA_ROOT_PEM_LEN  ((size_t) isrgrootx1_pem_len)

#endif /* CA_BUNDLE_H */
