# Quiz — Week 11

Twenty questions. Closed-book on the spec citations (you should be able to recite
the section number and the page); open-book on the C and the wire bytes. ~60
minutes. Answer key is at the bottom; do not peek before completing.

---

## Question 1

On the Pico W, `gpio_put(25, 1)` does not light the onboard LED. Why, and what is
the correct call?

## Question 2

The CYW43439's link to the RP2040 is a 4-wire SPI bus, but it is driven from PIO
state machine 1 rather than the SPI peripheral. Give the reason.

(A) The SPI peripheral is already used by the flash chip.
(B) The CYW43's SPI variant is half-duplex (shared data line) with a gap-insertion
    timing requirement that standard SPI hardware cannot meet, but PIO can.
(C) PIO is faster than the SPI peripheral.
(D) The CYW43 requires a 5-wire bus the SPI peripheral cannot provide.

## Question 3

After `cyw43_arch_wifi_connect_timeout_ms` returns 0, you read
`netif_ip4_addr(netif_default)` and it is `0.0.0.0`. Association succeeded but this
address is unset. What completed, what did not, and what is the most likely cause?

## Question 4

In the lwIP raw API, your `recv_cb` is invoked with a non-NULL pbuf `p`. List, in
order, the three things you must do with `p` before returning, and state what
breaks if you swap the last two.

## Question 5

`err_cb(void *arg, err_t err)` fires. Which of the following is safe to call inside
it?

(A) `tcp_close(pcb)`
(B) `tcp_recved(pcb, n)`
(C) `tcp_abort(pcb)`
(D) None of the above — the pcb is already freed; touch only `arg`-held state.

## Question 6

Encode the Variable Byte Integer for the value 268435455. How many bytes, and what
are they in hex? What value does encoding 268435456 produce in our `mqtt_vbi_encode`
helper?

## Question 7

The MQTT 5 CONNECT packet's variable header contains a Properties Length field even
when you send zero properties. What byte encodes "zero properties", and what
Reason Code does the broker return if you omit it?

## Question 8

A minimal MQTT 5 CONNECT with client-id "cc7-pico-0001" (13 chars), clean-start,
keep-alive 60, no will, no auth encodes to a remaining length of how many bytes?
Show the breakdown.

## Question 9

What are the four-bit packet-type nibbles (the high nibble of the first fixed-header
byte) for CONNECT, PUBLISH, SUBSCRIBE, and PINGREQ? Two of these four also carry
required non-zero flag bits in the low nibble — which two, and what value?

## Question 10

The PUBLISH packet you send at QoS 0 has no packet identifier in its variable
header, but a QoS 1 PUBLISH does. Where exactly does the packet identifier go, and
why is it absent at QoS 0?

## Question 11

Walk the TLS 1.2 handshake message order from ClientHello to the first encrypted
application record. Which message carries the server's certificate chain, and which
carries the ephemeral ECDH public key the session key is derived from?

## Question 12

Our `mbedtls_config.h` enables exactly one cipher suite. Name it, and name the
single elliptic curve we keep. Roughly how much `.text` does the trimmed mbedTLS
weigh versus stock?

## Question 13

We pin ISRG Root X1 as the sole trust anchor, but the broker's leaf certificate is
not signed directly by ISRG Root X1 — there is an intermediate (R3) in between. How
does verification still succeed without us pinning R3?

## Question 14

`mbedtls_ssl_handshake` returns `MBEDTLS_ERR_SSL_WANT_READ`. This is not an error.
What does it mean in our `_lwip_poll` integration, and what must the handshake loop
do in response?

## Question 15

A desktop trusts ~150 root CAs; we pin one. Estimate the RAM a parsed
`mbedtls_x509_crt` costs per root, multiply by 150, and explain in one sentence why
that number forces pinning on the RP2040.

## Question 16

Our backoff is exponential with ±25% jitter, capped at 60 s. Why the jitter? Give
the specific failure mode jitter prevents.

## Question 17

The publish loop increments the sequence number on every publish *attempt*, not
every success. Why does this distinction matter for the soak's delivery-ratio
computation?

## Question 18

The grade metric is "≥ 99.0% delivery over 24 hours". At a 10 s cadence, how many
publishes is that per day, and how many may be missed while still passing? A single
60 s AP outage costs how many publishes?

## Question 19

In `bio_send`, when `tcp_sndbuf(pcb)` returns 0, we return
`MBEDTLS_ERR_SSL_WANT_WRITE` rather than blocking. Why is returning WANT_WRITE the
correct behavior, and what would blocking break?

## Question 20

The MQTT keep-alive is 60 s and we send a PINGREQ at the half-interval (30 s) when
idle. What does the broker do if it does not hear from us within 1.5× the keep-alive
(90 s)? Cite the spec section.

---

## Answers

### Q1

The Pico W's onboard LED is wired to a GPIO of the **CYW43**, not to an RP2040 GPIO.
`gpio_put(25, …)` drives RP2040 pin 25, which on the Pico W is not the LED. The
correct call is `cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1)`, which sends a
command over the PIO-SPI link to the CYW43. Lecture 1; Pico W datasheet.

### Q2: B

The CYW43's "gSPI" variant is half-duplex — data-in and data-out share one wire —
with a required gap between the command and data phases. Standard SPI hardware
cannot insert that gap or reverse the data-line direction mid-transfer; a PIO state
machine, being a tiny programmable I/O engine, meets the timing exactly. (A) is
false on the Pico W (flash uses QSPI, a separate bus). Lecture 1.

### Q3

WiFi **association** (the 4-way WPA handshake) completed — that is what the 0 return
code reports. **DHCP** did not complete: no IPv4 lease was obtained, so the netif's
address is still the all-zero default. The most likely cause is an exhausted DHCP
pool on the AP (common at hackathons), or DHCP packets being dropped. Workaround: a
static IP via `dhcp_release` + `netif_set_ipaddr`. Lecture 1; SOLUTIONS Exercise 1
bug #4.

### Q4

In order: (1) copy the bytes out (`pbuf_copy_partial`), (2) tell lwIP you consumed
them (`tcp_recved(pcb, p->tot_len)`), (3) free the pbuf (`pbuf_free(p)`). If you
swap (2) and (3) — free first, then `tcp_recved` reading `p->tot_len` — you
dereference a freed pbuf, which corrupts the pool or crashes. Capture `tot_len`
before freeing, or call `tcp_recved` before `pbuf_free`. Lecture 2; SOLUTIONS
Exercise 2 pbuf walk.

### Q5: D

The pcb is already freed when `err_cb` fires. Calling any pcb function — `tcp_close`,
`tcp_recved`, `tcp_abort`, anything — is a use-after-free. Touch only the state
reachable through `arg`. Lecture 2; SOLUTIONS Exercise 2 bug #2.

### Q6

268435455 = 0x0FFFFFFF = the maximum VBI. It encodes to **4 bytes: FF FF FF 7F**
(each byte carries 7 bits; the high bit is the continuation flag, clear on the last
byte). Encoding 268435456 (one more than the max) returns `count = 0` from
`mqtt_vbi_encode` because it exceeds `MQTT_VBI_MAX_VALUE`; the caller must treat
`count == 0` as an error. MQTT 5 §1.5.5.

### Q7

Zero properties is encoded as a single VBI byte **0x00** (the Properties Length is a
VBI; its value is 0). Omitting it entirely shifts every subsequent byte and the
broker parses garbage, returning Reason Code **130 (0x82) Protocol Error** in the
CONNACK (or simply closing the connection). SOLUTIONS Exercise 3 bug #3.

### Q8

26 bytes. Breakdown: Protocol Name = 2 (length) + 4 ("MQTT") = 6; Protocol Level =
1; Connect Flags = 1; Keep-Alive = 2; Properties Length (VBI 0) = 1; Client-ID
length prefix = 2; Client-ID body (13 chars) = 13. Total = 6+1+1+2+1+2+13 = **26**,
which encodes as the single VBI byte 0x1A. (Exercise 3's `expected[]` uses this
13-char id; the lecture's worked example used a 12-char id and got 25.)

### Q9

CONNECT = 0x1, PUBLISH = 0x3, SUBSCRIBE = 0x8, PINGREQ = 0xC (high nibbles). The two
that carry required low-nibble flags are **SUBSCRIBE** and PUBREL/UNSUBSCRIBE — of
the four asked, SUBSCRIBE requires low nibble **0b0010 (0x2)**, making its full
first byte 0x82. PUBLISH's low nibble carries DUP/QoS/RETAIN, which are all zero for
our QoS-0 retain-0 case (0x30). MQTT 5 §2.1.3, Table 2-2.

### Q10

The packet identifier, when present, sits in the PUBLISH variable header
**immediately after the topic name and before the Properties**. It is present only
for QoS 1 and QoS 2, where the protocol needs to correlate the PUBLISH with its
PUBACK/PUBREC. At QoS 0 there is no acknowledgement, so there is nothing to
correlate and the field is omitted. MQTT 5 §3.3.2.2.

### Q11

ClientHello → ServerHello → **Certificate** (server cert + intermediate R3) →
**ServerKeyExchange** (the ephemeral ECDH public key, signed by the server cert's
RSA key) → ServerHelloDone → ClientKeyExchange (client's ephemeral ECDH public key)
→ ChangeCipherSpec → Finished → server ChangeCipherSpec → Finished → first encrypted
application record. The Certificate message carries the chain; the
ServerKeyExchange carries the server's ECDH public key. RFC 5246 §7.3, §7.4.

### Q12

Suite: **ECDHE-RSA-AES128-GCM-SHA256** (`MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256`).
Curve: **secp256r1** (P-256). Trimmed mbedTLS is ~**95 KB** of `.text` versus ~250 KB
stock. Lecture 3; `mbedtls_config.h`.

### Q13

The broker sends its **full chain** (leaf + R3) in the TLS Certificate message.
mbedTLS walks leaf → R3 → and finds that R3's issuer is ISRG Root X1, which is in
our trust store. Pinning the root suffices because the intermediate arrives on the
wire; we never need to pin intermediates. (If the broker sent only the leaf,
verification would fail — "chain too short" — and you would have to pin R3 too.)
Lecture 3; Challenge 2 background.

### Q14

`WANT_READ` means mbedTLS needs more bytes from the transport to make progress, but
none are available yet. In our `_lwip_poll` model the bytes only arrive when
`cyw43_arch_poll()` runs, so the handshake loop must call `cyw43_arch_poll()` (and
`cyw43_arch_wait_for_work_until`) and then call `mbedtls_ssl_handshake` again. It is
a normal, expected, repeated condition — not an error. Lecture 3; `tls_handshake`
in main.c.

### Q15

A parsed `mbedtls_x509_crt` costs ~3 KB of RAM per root (each ASN.1 OID is
heap-allocated). 150 × 3 KB ≈ 450 KB, which exceeds the RP2040's 264 KB of total
SRAM — so a desktop-style trust store does not fit, and you must pin the small
number of roots you actually need. Lecture 3.

### Q16

Jitter (±25%) prevents **retry synchronization**: after a shared outage (a power cut,
an AP reboot) a fleet of devices would otherwise all back off on the same schedule
and hammer the broker in synchronized waves, effectively self-DoSing it. Random
jitter spreads the retries out. AWS "Exponential Backoff and Jitter". Lecture 1
objectives; `backoff_next_ms` in main.c.

### Q17

The delivery ratio is `received / attempted`. Incrementing the sequence on every
*attempt* means a publish that the device tried but the transport dropped still
consumes a sequence number, so it shows up as `missing` in the reconciliation rather
than silently never existing. If you incremented only on success, a dropped publish
would leave no gap in the sequence space and the ratio would be inflated to a
meaningless 100%. Challenge 1 Phase 1.

### Q18

8640 publishes/day (86400 s ÷ 10 s). 99.0% allows ~86 missed. A single 60 s AP
outage at 10 s cadence costs **6 publishes**. So your budget is ~14 such outages per
day, or fewer-but-longer ones; the three scheduled disconnects in Challenge 1 (5 s,
30 s, 5 min) cost ~1 + ~4 + ~30 = ~35, leaving ~51 for the unplanned. Challenge 1
Phase 4; README metric section.

### Q19

`bio_send` is mbedTLS's BIO write callback; mbedTLS expects it to be non-blocking and
to signal "try again later" via `WANT_WRITE`. When the lwIP send buffer is full,
returning `WANT_WRITE` lets mbedTLS unwind and lets the main loop run
`cyw43_arch_poll()` to flush the buffer (ACKs free send space). **Blocking** inside
`bio_send` would starve the poll loop, the buffer would never drain, and you would
deadlock — the classic "spin waiting for progress that only the loop you're blocking
can make". `bio_send` in main.c.

### Q20

If the broker does not receive any packet from the client within **1.5× the keep-alive**
(90 s for our 60 s keep-alive), it treats the connection as dead, closes the network
connection, and (if a Will was set) publishes the Will message. That is why a dead
TCP connection can go undetected for up to 90 s — and why a too-long keep-alive
shows up as a long publish gap in the soak. MQTT 5 §3.1.2.10 (Keep Alive).

(End of quiz.)
