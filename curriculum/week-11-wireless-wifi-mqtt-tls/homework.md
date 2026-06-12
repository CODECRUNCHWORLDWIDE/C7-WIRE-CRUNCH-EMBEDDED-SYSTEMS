# Homework — Week 11

Five problems. Estimated total time: ~6 hours over the week. Submit answers as
commits to your fork of the course repo; each problem has a designated file path
under `homework/week-11/`. A grading rubric is at the bottom.

---

## Problem 1 — Hand-encode the ClientHello SNI extension (1 hour)

When the Pico connects to `test.mosquitto.org`, mbedTLS puts a Server Name
Indication (SNI) extension in the ClientHello so the broker (which may host many
virtual hosts on one IP) knows which certificate to present. You will hand-encode
that extension and verify it byte-for-byte against what mbedTLS emits.

Write `homework/week-11/sni-encode.c` (host-compiled, plain C, no SDK) that:

1. Takes a hostname string (`"test.mosquitto.org"`, 18 bytes).
2. Builds the `server_name` extension exactly per RFC 6066 §3
   (<https://www.rfc-editor.org/rfc/rfc6066#section-3>): extension type `0x0000`,
   then a 2-byte extension length, then a 2-byte ServerNameList length, then one
   entry: name type `0x00` (host_name), a 2-byte host name length, and the host
   name bytes.
3. Prints the full extension as space-separated hex.
4. Asserts the total byte count matches `2 + 2 + 2 + 1 + 2 + len(hostname)` = 27 for
   the Mosquitto hostname.

Then capture a real ClientHello from your device (Wireshark on a laptop sharing its
connection, or mbedTLS debug at level 3 which prints the extension bytes) and
confirm your hand-encoded extension matches the bytes on the wire. Write the
comparison in `homework/week-11/sni-encode.md`.

The deliverable is the `.c` (≈70 lines) plus the markdown comparison.

---

## Problem 2 — The MQTT topic-filter wildcard rules (1 hour)

MQTT topic filters use two wildcards: `+` (single level) and `#` (multi level,
must be the last character). Getting the matching rules exactly right is the
difference between a command channel that works and one that silently drops
messages.

Write `homework/week-11/topic-match.c` (host-compiled) implementing:

```c
/* Returns 1 if `topic` matches the subscription `filter`, else 0.
 * Implements MQTT 5 sec 4.7 (Topic Names and Topic Filters, pp. 252-255). */
int topic_matches(const char *filter, const char *topic);
```

It must satisfy this truth table (which you include as a test harness in `main`):

| Filter                         | Topic                              | Match? |
|--------------------------------|------------------------------------|:------:|
| `cc7/devices/+/cmd`            | `cc7/devices/a1b2c3/cmd`           | yes    |
| `cc7/devices/+/cmd`            | `cc7/devices/a1b2c3/telemetry`     | no     |
| `cc7/devices/+/cmd`            | `cc7/devices/a1b2c3/sub/cmd`       | no     |
| `cc7/#`                        | `cc7/devices/a1b2c3/cmd`           | yes    |
| `cc7/#`                        | `cc7`                              | yes    |
| `#`                            | `cc7/devices/a1b2c3/telemetry`     | yes    |
| `+/devices/+/telemetry`        | `cc7/devices/a1b2c3/telemetry`     | yes    |
| `sport/tennis/+`               | `sport/tennis/player1/ranking`     | no     |
| `$share/g/cc7/+/cmd`           | `cc7/a1b2c3/cmd`                   | no     |

Pay attention to the spec's subtle rules: `#` matches the parent level too
(`sport/#` matches `sport`); `+` matches exactly one level including an empty level;
and a leading `$` topic (e.g. `$SYS/...`, shared subscriptions) is not matched by a
wildcard at the first level. Document each rule you implemented with a comment citing
the relevant bullet of §4.7.

The deliverable is `topic-match.c` (≈120 lines) that passes all nine cases plus four
more you invent and justify.

---

## Problem 3 — Trim the cipher suite list and measure the flash delta (1.5 hours)

Our `mbedtls_config.h` keeps exactly one cipher suite. Quantify what that buys you.

1. Build the mini-project once as-shipped. Record `arm-none-eabi-size sensor-node.elf`
   (`text`, `data`, `bss`).
2. Edit `mbedtls_config.h` to *also* enable a second key exchange and suite —
   add `MBEDTLS_KEY_EXCHANGE_RSA_ENABLED` and the
   `MBEDTLS_TLS_RSA_WITH_AES_128_GCM_SHA256` path (plain-RSA key transport, no
   ECDHE). Rebuild, record the size.
3. Now go the other way: from the as-shipped config, additionally enable TLS 1.3
   (`MBEDTLS_SSL_PROTO_TLS1_3` and its dependencies). Rebuild, record the size.
4. Build a table of `(configuration, text bytes, delta from baseline)`.

Then write `homework/week-11/cipher-trimming.md` answering:

- How many bytes did the second (RSA) suite add? Why is plain-RSA key transport a
  security downgrade you would not want even though it is smaller in handshake CPU?
  (Hint: forward secrecy.)
- How many bytes did TLS 1.3 add? Given that the Pico's RSA-PSS verification of the
  server cert is slower than ECDSA, is TLS 1.3 worth it for this node today? Justify
  with the handshake-CPU trade-off from Lecture 3.
- What is the single biggest `.text` contributor in the mbedTLS build? Use
  `arm-none-eabi-nm --size-sort --print-size sensor-node.elf | tail -40` to find the
  largest symbols and name the top three.

The deliverable is the markdown with the size table and the three analysis answers.

---

## Problem 4 — Backoff math and the retry-synchronization simulation (1 hour)

Our backoff is `1, 2, 4, 8, 16, 32, 60, 60, …` seconds (doubling, capped at 60) with
±25% uniform jitter, resetting to 1 s on each successful CONNECT.

Write `homework/week-11/backoff-sim.py` that:

1. Simulates **200 devices** that all lose Wi-Fi at t=0 (a neighborhood power cut)
   and all come back when the AP returns at t=45 s.
2. Each device retries on the backoff schedule. A retry "succeeds" only if it lands
   after t=45 s; before that it fails and the device advances to the next backoff
   interval.
3. Runs the simulation twice: once with **no jitter** and once with **±25% jitter**.
4. For each run, produces a histogram (text or matplotlib) of how many devices issue
   a reconnect attempt in each 1-second bucket from t=44 s to t=70 s.

Then in `homework/week-11/backoff-sim.md`:

- Show the two histograms. In the no-jitter run, the attempts cluster into sharp
  spikes at the schedule boundaries (45, 47, 49, … after the cut); with jitter they
  spread out. Quantify the peak attempts-per-second in each run.
- Compute the worst-case publish gap a single device sees for the 45 s outage. Does
  it stay within the 60 s grade-boundary? Show the arithmetic.
- The broker can accept ~10 new connections/second before it starts refusing. With
  200 devices and no jitter, does the peak exceed that? With jitter?

The deliverable is the `.py` and the markdown analysis.

---

## Problem 5 — The soak-metric reconciliation, from scratch (1.5 hours)

`cc-listen.py analyze` reconciles a `device.log` and a `listen.log` by sequence
number. Reimplement that logic yourself so you understand exactly what the metric
measures, then stress it against synthetic logs with known answers.

Write `homework/week-11/reconcile.py` that:

1. Parses `PUB seq=N` lines from a device log and `RECV seq=N` lines from a listen
   log into two sets.
2. Computes `delivered`, `missing`, `phantom`, and the ratio.
3. Clusters the `missing` set into contiguous runs (a run is a disconnect window).
4. For each run, given the 10 s cadence, estimates the outage duration in seconds.
5. Emits a one-line PASS/FAIL verdict against the 99.0% bar.

Then generate three synthetic test pairs in `homework/week-11/test-logs/`:

- **clean**: 8640 sent, 8640 received → expect 100.0%, PASS, zero runs.
- **one-outage**: 8640 sent, with seqs 4000–4005 missing → expect 99.93%, PASS, one
  run of 6 (≈60 s outage).
- **failing**: 8640 sent, with 100 scattered missing → expect 98.84%, FAIL.

Write `homework/week-11/reconcile.md` showing your script's output on all three and
confirming the numbers match the expected values above. Explain why the
`one-outage` case's estimated outage duration is ≈60 s and not ≈50 s (hint: 6 missed
publishes at 10 s spacing bound an outage of 50–60 s depending on phase alignment).

The deliverable is `reconcile.py`, the three synthetic log pairs, and the markdown.

---

## Submission

Commit all five problems to your fork under `homework/week-11/`. Tag the commit
`week-11-homework`.

```bash
git add homework/week-11/
git commit -m "week 11 homework: sni encode, topic match, cipher trimming, backoff sim, soak reconcile"
git tag week-11-homework
git push origin main --tags
```

The teaching team reviews homework asynchronously; expect feedback within ~5
business days.

---

## Grading rubric

Each problem is worth 20 points; 100 total. Partial credit per the breakdown.

| Problem | Full credit (20)                                                     | Partial                                                  |
|--------:|----------------------------------------------------------------------|----------------------------------------------------------|
| 1 SNI   | Extension encodes byte-exact (12) and matches a real capture (8)     | Encoding correct but no wire capture: 12                 |
| 2 Topic | All 9 table cases pass (12) + 4 justified extra cases (4) + §4.7 cites (4) | Misses the `#`-matches-parent or leading-`$` rule: −6 each |
| 3 Cipher| Size table for all 3 configs (10) + 3 analysis answers (10)          | Table only, no analysis: 10                              |
| 4 Backoff| Sim runs both modes (8) + histograms (6) + gap/throughput math (6)  | No-jitter only, or no throughput comparison: −6 each     |
| 5 Soak  | Reconciler correct on 3 synthetic pairs (12) + phase explanation (8) | Reconciler works but expected numbers not matched: −4 each |

Deductions across all problems: any placeholder/TODO left in submitted code (−5);
code that does not compile or run (−10 for that problem); claimed measurements with
no command output to back them (−5).

---

## References for the homework set

- MQTT 5.0 §4.7 (Topic Names and Filters, pp. 252–255), §3.1 (CONNECT), §3.3
  (PUBLISH). <https://docs.oasis-open.org/mqtt/mqtt/v5.0/mqtt-v5.0.html>
- RFC 6066 §3 (Server Name Indication). <https://www.rfc-editor.org/rfc/rfc6066>
- RFC 5246 (TLS 1.2) and RFC 8446 (TLS 1.3) for the cipher trade-offs.
- mbedTLS configuration guide. <https://mbed-tls.readthedocs.io/>
- AWS "Exponential Backoff And Jitter".
  <https://aws.amazon.com/blogs/architecture/exponential-backoff-and-jitter/>
- Week 11's lecture notes and the mini-project README's delivery-metric section.
