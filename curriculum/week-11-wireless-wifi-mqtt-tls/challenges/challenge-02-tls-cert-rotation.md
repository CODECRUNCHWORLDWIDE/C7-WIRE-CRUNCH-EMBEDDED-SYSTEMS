# Challenge 2 — TLS CA Rotation

## Brief

Force a CA-trust-anchor mismatch on your sensor node, observe the exact mbedTLS
error it produces, then walk the full rotation runbook: swap the pinned root,
reflash, confirm the handshake recovers. Capture the handshake — both the failing
and the succeeding one — with mbedTLS debug at verbosity level 3 so you can point
at the precise record where verification fails. Compare the cost of CA pinning on
an MCU against the desktop "trust 150 roots" model, and write the operations
runbook a fleet owner would actually follow when Let's Encrypt rotates ISRG Root X1
in 2030.

The point of this challenge is to make the abstract "what happens when your pinned
CA expires" concrete and survivable *before* it happens in production. A CA
rotation that surprises you is a fleet-wide outage: every device fails its TLS
handshake at the same instant, none can publish, and the only fix is an OTA update
that itself may depend on... a TLS connection. You want to have rehearsed this.

Allocate ~3 hours. The deliverable is `CA-ROTATION-WRITEUP.md` plus the two
mbedTLS debug captures.

## Background — why pinning, and why it bites

On a desktop, your browser trusts ~150 root CAs shipped in the OS trust store, and
the TLS library walks the chain the server presents until it hits any one of them.
That works because a desktop has megabytes of RAM to hold 150 parsed
`mbedtls_x509_crt` structures (~3 KB of RAM each ≈ 450 KB) and a software-update
channel (Windows Update, `apt`) that refreshes the trust store independently of any
single application.

An MCU has neither. The RP2040 has 264 KB of SRAM total — you cannot spend 450 KB
on a trust store you do not have. So you pin: you ship the *one* root your *one*
broker chains to, parse it once at boot (~3 KB of RAM, one cert), and pass it as
the sole trust anchor via `mbedtls_ssl_conf_ca_chain`. test.mosquitto.org's leaf
certificate currently chains `leaf → R3 (Let's Encrypt intermediate) → ISRG Root
X1`, so pinning ISRG Root X1 suffices; mbedTLS receives R3 in the server's
Certificate message and walks leaf → R3 → (matches your pinned root) → trusted.

The bite: when ISRG Root X1's `notAfter` passes (the current cross-sign is valid
to 2035-06-04; an older self-signed X1 already expired 2024-06-04, which is exactly
why Let's Encrypt rolled to the cross-sign), or when Mosquitto re-issues its leaf
under a *different* root, every handshake fails at
`MBEDTLS_ERR_X509_CERT_VERIFY_FAILED` and there is no OS trust store to save you.
Your only recovery is to reflash the firmware with the new root — which is why this
week's mini-project deploys over Week 10's signed-OTA bootloader: the CA rotation
*is* a firmware update, and you must already have a firmware-update path to survive
it.

## Procedure

### Phase 1 — Establish the baseline (20 min)

Build the mini-project with mbedTLS debug enabled. In `mbedtls_config.h` ensure
`MBEDTLS_DEBUG_C` is defined, and in your TLS setup register the debug callback at
level 3:

```c
mbedtls_ssl_conf_dbg(&conf, tls_debug_cb, NULL);
mbedtls_debug_set_threshold(3);
```

where `tls_debug_cb` simply forwards mbedTLS's lines to `printf`:

```c
static void tls_debug_cb(void *ctx, int level, const char *file,
                         int line, const char *str) {
    (void) ctx;
    (void) level;
    (void) printf("[mbedtls] %s:%04d: %s", file, line, str);
}
```

Flash, boot, and capture the *successful* handshake to `tls-ok.log`. You should see
the record sequence ClientHello → ServerHello → Certificate → ServerKeyExchange →
ServerHelloDone → ClientKeyExchange → ChangeCipherSpec → Finished, and crucially the
line where mbedTLS verifies the server cert against your pinned root:

```text
[mbedtls] x509_crt.c:NNNN: verify chain: leaf -> R3 -> ISRG Root X1 (trusted)
[mbedtls] ssl_tls12_client.c:NNNN: <= handshake
```

This is your "known good." Note the cipher suite the line at level 2 reports — it
must be `TLS-ECDHE-RSA-WITH-AES-128-GCM-SHA256`, the one suite your trimmed config
left enabled.

### Phase 2 — Break the trust anchor (30 min)

You will simulate a rotation by pinning the *wrong* root. The cleanest way is to
swap ISRG Root X1 for a root that is real and valid but does *not* sign
test.mosquitto.org's chain — for example, DigiCert Global Root G2. Replace the PEM
in `ca_bundle.h`:

```c
/* WAS: ISRG Root X1. NOW: DigiCert Global Root G2 — a valid root that does
 * NOT sign test.mosquitto.org, to simulate a post-rotation mismatch. */
static const char CC_CA_ROOT_PEM[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBh\n"
    /* ... the rest of the DigiCert Global Root G2 PEM ... */
    "-----END CERTIFICATE-----\n";
```

(Grab the DigiCert Global Root G2 PEM from <https://www.digicert.com/kb/digicert-root-certificates.htm>.
Any real root that does not sign Mosquitto's chain works; do not invent a fake PEM,
because mbedTLS will reject a malformed PEM at *parse* time with a different error
and you want the *verify*-time error.)

Rebuild, reflash, capture the failing handshake to `tls-fail.log`. The handshake
gets *further* than you might expect — TCP connects, ClientHello and ServerHello
exchange fine, the server's Certificate message arrives intact — and then dies at
chain verification:

```text
[mbedtls] x509_crt.c:NNNN: x509_verify_chain: top of chain has no trusted root
[mbedtls] ssl_tls.c:NNNN: ! mbedtls_ssl_handshake returned -0x2700
```

`-0x2700` is `MBEDTLS_ERR_X509_CERT_VERIFY_FAILED`. Confirm your device's higher-level
log reports this cleanly and *aborts the session* rather than hanging or rebooting:

```text
[+5402 ms] TLS handshake FAILED rc=-0x2700 (cert verify) — entering backoff
```

This is the single most important behavioral check in the challenge: a CA mismatch
must be a *handled, logged, backed-off* failure, not a crash. If your firmware
hardfaults or hangs on a verify failure, fix that before continuing — in
production this is the difference between "the fleet stops publishing" and "the
fleet stops publishing *and* the watchdog ping-pongs every device into a reboot
loop."

### Phase 3 — Distinguish the failure modes (20 min)

There are three distinct cert-related failures and they produce three distinct
errors. Reproduce and log each, so you can tell them apart in the field:

| Failure                          | How to induce                                   | mbedTLS error                                  |
|----------------------------------|-------------------------------------------------|------------------------------------------------|
| Wrong/untrusted root (this one)  | Pin DigiCert instead of ISRG                     | `-0x2700` `X509_CERT_VERIFY_FAILED`            |
| Expired pinned root              | Pin a root whose `notAfter` is in the past       | `-0x2700` with `flags & MBEDTLS_X509_BADCERT_EXPIRED` |
| Hostname mismatch (SNI/CN)       | `mbedtls_ssl_set_hostname` set to "wrong.host"   | `-0x2700` with `flags & MBEDTLS_X509_BADCERT_CN_MISMATCH` |

Use `mbedtls_ssl_get_verify_result(&ssl)` after a failed handshake to read the
`flags` bitfield and `mbedtls_x509_crt_verify_info` to render it human-readable:

```c
uint32_t flags = mbedtls_ssl_get_verify_result(&ssl);
char buf[256];
mbedtls_x509_crt_verify_info(buf, sizeof buf, "  ! ", flags);
(void) printf("[tls] verify flags 0x%08lx:\n%s", (unsigned long) flags, buf);
```

Capture each rendered `verify_info` string. In the field, this is the difference
between "we need to rotate the CA" (untrusted/expired) and "someone is
MITM-ing us, or our SNI is wrong" (CN mismatch).

### Phase 4 — Execute the rotation (20 min)

Now do it for real: restore the *correct* root (ISRG Root X1) in `ca_bundle.h`,
rebuild, and deploy. If you have Week 10's bootloader installed, deploy the new
firmware over OTA (`cc-sign` then `cc-flash`) rather than BOOTSEL — that is the
production path and the whole reason a fleet can survive a CA rotation at all.
Confirm the handshake recovers and publishing resumes. Time the full rotation from
"build the new firmware" to "device publishing again."

### Phase 5 — Write the runbook (50 min)

In `CA-ROTATION-WRITEUP.md`, write the operations runbook a fleet owner follows
when Let's Encrypt announces an ISRG Root X1 rotation. Cover:

1. **Detection** — how do you know a rotation is coming? (Let's Encrypt announces
   on their community forum and `letsencrypt.org/certificates`; the `notAfter` of
   the pinned root is in your firmware and you can alert on it months ahead.) Why
   you must *also* pin the *next* root before the current one expires, shipping a
   trust store of two roots during the overlap window so neither order of rotation
   strands a device.
2. **Staging** — build the new firmware with both old and new roots in the bundle
   (`mbedtls_x509_crt_parse` appends; passing a two-cert chain to
   `mbedtls_ssl_conf_ca_chain` makes both valid anchors). Test it against the
   current broker *and* against a broker presenting the new chain (you can stand up
   a local Mosquitto with a self-signed-to-new-root cert to test the new path
   before the public broker rotates).
3. **Rollout** — OTA the dual-root firmware to the entire fleet *before* the broker
   rotates, with staged canary → 10% → 100% and a soak gate at each stage (this is
   where Challenge 1's soak discipline earns its keep).
4. **The chicken-and-egg** — what happens to a device that is *already* off-network
   when the rotation hits and only comes back online *after* the broker has rotated?
   (It cannot OTA because it cannot connect; you need either a non-TLS fallback
   provisioning path, a longer dual-root overlap window, or physical re-flash.
   State which your design chose and why.)
5. **The cost comparison** — a short section contrasting MCU pinning (1–2 roots,
   reflash to rotate, ~3 KB RAM each) against desktop trust stores (150 roots,
   OS-updated, ~450 KB RAM). Explain why TLS 1.3's `certificate_authorities`
   extension does not actually solve this for MCUs, and why the realistic answer
   at MCU scale is "a small number of pinned roots whose rotation cadence you
   control by controlling the broker."

## Deliverable

1. `CA-ROTATION-WRITEUP.md` (1500–2500 words) — the failure analysis and the
   rotation runbook.
2. `tls-ok.log` — the level-3 mbedTLS capture of a successful handshake.
3. `tls-fail.log` — the level-3 capture of the verify-failure handshake.

Commit with a message like `week-11/challenge-02: CA rotation runbook + verify-fail
captures`.

## Pass criteria

- The successful and failing handshakes are both captured at mbedTLS debug level 3,
  and the writeup points at the exact line where verification diverges.
- All three failure modes (untrusted root, expired root, CN mismatch) are
  reproduced with their distinct `verify_result` flags rendered via
  `mbedtls_x509_crt_verify_info`.
- The firmware *handles* a verify failure as a logged, backed-off event — no crash,
  no hang, no reboot loop. (Demonstrate by leaving the wrong root pinned and showing
  the device cycling through backoff cleanly for at least 5 minutes.)
- The runbook addresses the dual-root overlap window and the off-network-device
  chicken-and-egg problem explicitly.
- The rotation was executed over OTA (if Week 10's bootloader is installed) and the
  end-to-end time was measured.

## Why this challenge matters

CA rotations are scheduled, announced, and inevitable, and they still take down
production fleets every year — because the team that shipped the firmware was not
the team on call when the root expired, and the runbook either did not exist or
described a process nobody had rehearsed. The thirty minutes you spend deliberately
breaking your own trust anchor here is the cheapest rehearsal you will ever get for
a failure that, unrehearsed, is a multi-day fleet-wide outage.

Pinning is not optional on an MCU and it is not free; it trades the desktop's
"trust everything the OS trusts" convenience for a hard operational dependency on
your own firmware-update channel. Owning that trade — knowing exactly what breaks,
when, and how you recover — is what separates a demo that handshakes once from a
product you can run for a decade.

## References

- Week 11 Lecture 3, the TLS 1.2 handshake walkthrough and the mbedTLS trimming /
  CA-pinning sections.
- mbedTLS X.509 verification docs.
  <https://mbed-tls.readthedocs.io/en/latest/kb/how-to/how-to-determine-tls-certificate-validation-failures/>
- RFC 5246 §7.4.2 (Server Certificate), <https://www.rfc-editor.org/rfc/rfc5246>.
- Let's Encrypt chain of trust and root rotation.
  <https://letsencrypt.org/certificates/>
- test.mosquitto.org TLS / certificate page. <https://test.mosquitto.org/ssl/>
- DigiCert root certificates (for the wrong-root simulation).
  <https://www.digicert.com/kb/digicert-root-certificates.htm>
