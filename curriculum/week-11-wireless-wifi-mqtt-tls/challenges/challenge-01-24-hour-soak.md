# Challenge 1 — The 24-Hour Soak

## Brief

Run the mini-project sensor node for 24 hours against `test.mosquitto.org:8883`,
log every PUBLISH on the device side and every receive on the laptop side, then
reconcile the two logs to compute the publish-delivery ratio. Inject three forced
disconnects during the run (AP power-cycle for 5 s, 30 s, and 5 min) and measure
the reconnect time and the publish gap for each. Write a post-mortem that proves
your firmware hit the grade-boundary metric: **median publish-delivery ratio ≥
99.0% over 24 hours, worst-case publish gap ≤ 60 s during a forced disconnect.**

The goal of this challenge is not "make it publish." Your mini-project already
publishes. The goal is to prove it *keeps* publishing — through AP power-cycles,
through broker-side disconnects, through the inevitable 3 a.m. DHCP-lease renewal
that drops your IPv4 for 400 ms — for a full day without you touching it. That
durability is the entire reason embedded networking is harder than desktop
networking: nobody is sitting at the keyboard to hit retry.

Allocate ~2 hours of active work plus 24 hours of wall-clock soak. Start the soak
Sunday evening so it finishes Monday evening. The deliverable is `SOAK-REPORT.md`
plus the two raw log files in your repository fork.

## Setup

You have completed:

- The mini-project builds, flashes, and publishes a reading every 10 s. You have
  watched it run for at least 10 minutes and seen the cadence hold.
- `cc-listen.py` runs on your laptop, subscribes to `cc7/devices/<id>/telemetry`,
  and prints each reading with a host-side timestamp.
- The Pico W is powered from a USB *wall adapter*, not your laptop. A laptop that
  sleeps mid-soak takes the listener down with it; the device should be on
  independent power so that only the listener depends on the laptop.
- Your AP (or a dedicated travel router / phone hotspot you can power-cycle without
  taking the whole house offline) is on a switched power strip within arm's reach.

You need a device id you can reference. The mini-project derives it from the
RP2040's 64-bit board id (`pico_get_unique_board_id`); it prints the resulting
3-hex-byte suffix on boot as `device-id=a1b2c3`. Note it down — every topic and
every log line keys off it.

## Procedure

### Phase 1 — Instrument both sides (15 min)

The mini-project already prints, on every successful publish:

```text
[+12347 ms] PUB seq=1234 bytes=96 rc=0
```

`cc-listen.py` already prints, on every received message:

```text
[+12.349 s] RECV seq=1234 bytes=96 topic=cc7/devices/a1b2c3/telemetry
```

The shared key between the two logs is the **sequence number**, which the device
embeds in the JSON payload (`"seq":1234`) and increments on every publish *attempt*
(not every success — that distinction is what lets you tell a dropped publish from
a never-attempted one). Confirm both sides print `seq` before starting the soak;
if either does not, fix it now, because you cannot reconcile logs without it.

Redirect both to files with timestamps the OS adds, so you have a second clock to
fall back on if the device's `time_us_64` ever wraps or resets:

```bash
# Device side (the UART/CDC console)
python3 -m serial.tools.miniterm /dev/cu.usbmodem* 115200 \
  | ts '[%Y-%m-%d %H:%M:%.S]' > device.log

# Laptop side (the subscriber)
python3 cc-listen.py --id a1b2c3 | ts '[%Y-%m-%d %H:%M:%.S]' > listen.log
```

(`ts` is from `moreutils`: `brew install moreutils` / `apt install moreutils`. If
you do not have it, `cc-listen.py` already prints an ISO-8601 wall-clock stamp; use
that column instead.)

### Phase 2 — Start the soak (2 min)

1. Power the Pico W from the wall adapter. Watch `device.log` show the boot
   sequence: `cyw43_arch_init` → WiFi join → DNS → TCP → TLS handshake → MQTT
   CONNACK rc=0 → first publish, all within ~8 s of power-on.
2. Confirm `listen.log` shows the first `RECV` within ~1 s of the device's first
   `PUB`.
3. Note the wall-clock start time. The soak runs until the same time tomorrow.
4. Walk away. Do not touch the device except for the three scheduled disconnects.

### Phase 3 — The three forced disconnects

Run these at roughly +2 h, +8 h, and +16 h into the soak (the exact times do not
matter; spacing them out does, so a single bad AP moment does not contaminate two
tests). For each, log the wall-clock time you flip the switch and the time you flip
it back.

| Test | AP off for | What you are measuring                                          |
|------|-----------:|----------------------------------------------------------------|
| D1   | 5 s        | Fast recovery: does the device reconnect on the first backoff? |
| D2   | 30 s       | Mid backoff: does it climb the ladder and recover ≤ 60 s?      |
| D3   | 5 min      | Deep backoff: does it cap at 60 s and *stay* retrying, not give up? |

For each test, after the AP comes back, watch `device.log` for the recovery trace:

```text
[+7203145 ms] link DOWN — entering backoff (attempt=1, sleep=1187 ms)
[+7204332 ms] WIFI_DOWN: reconnecting to "my-ap"...
[+7205901 ms] connected; ip=192.168.1.42
[+7206090 ms] DNS test.mosquitto.org -> 5.196.78.28
[+7207402 ms] TLS handshake ok (1312 ms)
[+7207501 ms] MQTT CONNACK rc=0
[+7207698 ms] PUB seq=1247 bytes=96 rc=0   <-- first publish after recovery
```

Compute, for each test, `recovery_time = (timestamp of first PUB after AP-on) −
(timestamp of AP-on)`. For D1 this should be a few seconds; for D3, expect the
device to be mid-60-s-backoff-sleep when the AP returns, so recovery can take up
to one full 60 s backoff interval plus the ~3 s rejoin plus the ~1.3 s TLS
handshake — still inside the 60 s publish-gap budget *if* you measure the gap as
"time between consecutive successful publishes," because the backoff sleep started
*before* the AP came back. If a single publish gap exceeds 60 s, that is a finding;
investigate whether your backoff cap is set above 60 s or your jitter is pushing
an interval past the cap.

### Phase 4 — Reconcile the logs (30 min)

Write a short reconciliation script (or extend `cc-listen.py`'s analysis mode) that:

1. Parses every `PUB seq=N` from `device.log` into a set `sent`.
2. Parses every `RECV seq=N` from `listen.log` into a set `received`.
3. Computes `delivery_ratio = len(sent ∩ received) / len(sent)`.
4. Lists `missing = sent − received` (publishes the device sent but the laptop
   never received) and `phantom = received − sent` (receives with no matching
   send — should be empty; if not, you have a sequence-number bug or two devices
   on the same topic).
5. Computes the publish-gap distribution: sort the `RECV` timestamps, diff
   consecutive ones, report the max gap and the count of gaps > 12 s (a gap > 12 s
   means at least one 10-s-cadence publish was missed or delayed).

Expected over 24 h at 10 s cadence: ~8640 attempted publishes. A 99.0% ratio
allows ~86 missing. Your three forced disconnects alone cost: D1 ≈ 1 publish, D2 ≈
4–5 publishes, D3 ≈ 30 publishes (5 min at 10 s). That is ~35 of your 86-publish
budget spent deliberately; the remaining ~51 is your headroom for everything you
did not plan — DHCP renewals, broker-side rate-limit blips, the AP's nightly
channel hop. If your unplanned losses exceed ~50, dig into *which* publishes were
lost and *why* (the `missing` set's sequence numbers will cluster around specific
wall-clock times; cross-reference `device.log` at those times).

### Phase 5 — Write the post-mortem

`SOAK-REPORT.md` (1500–2500 words) with these sections:

1. **Run summary** — start/end wall-clock, total attempted publishes, total
   received, the delivery ratio, pass/fail against the 99.0% bar.
2. **The three disconnects** — a table of D1/D2/D3 with AP-off duration, recovery
   time, publishes lost, and max publish gap. State whether each stayed ≤ 60 s.
3. **The missing publishes** — for every gap > 12 s, the sequence numbers lost and
   the root cause read off `device.log` (backoff, DHCP renewal, broker blip,
   unexplained).
4. **The backoff ladder in practice** — paste the actual sleep intervals the
   device logged across the run. Confirm they climb 1→2→4→8→16→…→60 s with jitter
   and reset to 1 s on each successful reconnect.
5. **What you would change** — having watched it for a day, what one change would
   most improve the delivery ratio? (Candidates: shorter MQTT keep-alive so dead
   connections are detected faster; QoS 1 with a small retransmit buffer; a
   second pinned CA so a mid-soak Let's Encrypt rotation does not kill you.)

## Deliverable

1. `SOAK-REPORT.md` — the post-mortem.
2. `device.log` and `listen.log` — the raw logs (gzip them; a day of 10-s logs is
   ~1 MB each uncompressed, trivial gzipped).
3. The reconciliation script you wrote in Phase 4.

Commit with a message like `week-11/challenge-01: 24h soak, 99.4% delivery, max
gap 58 s on D3`.

## Pass criteria

- The delivery ratio is computed from real logs (not asserted) and is ≥ 99.0%.
- All three forced disconnects recovered, each with a max publish gap ≤ 60 s.
- The `phantom` set is empty (no unexplained receives).
- Every gap > 12 s has a root cause attributed to it from the device log.
- The backoff ladder in the report matches the values in `wifi_common.h`
  (`NET_BACKOFF_INITIAL_MS = 1000`, `NET_BACKOFF_MAX_MS = 60000`, 25% jitter).

## Why this challenge matters

A sensor node that works for ten minutes on the bench and a sensor node that works
for a year in a ceiling are different products. The difference is entirely in the
failure-recovery paths — the code that runs only when something has already gone
wrong, which is therefore the code you exercise least and trust most. The soak is
how you exercise it on purpose, on a timescale long enough that the rare failures
become observable, while you still have the logs and the bench to diagnose them.

Every IoT product ships with a soak gate. Yours is 24 hours; a medical device's is
1000 hours; an automotive ECU's is measured in vehicle-years of accelerated-life
testing. The discipline is the same at every scale: instrument both ends, force
the failures you can, log everything, reconcile, and do not call it done until the
numbers say it is.

## References

- The mini-project README's "the 99% delivery metric" section.
- Week 11 Lecture 2 (lwIP raw API — the reconnect path and pbuf lifetime during
  teardown) and Lecture 3 (the TLS re-handshake on reconnect).
- AWS Architecture Blog, "Exponential Backoff And Jitter".
  <https://aws.amazon.com/blogs/architecture/exponential-backoff-and-jitter/>
- test.mosquitto.org broker notes. <https://test.mosquitto.org/>
- MQTT 5.0 §3.1.2.10 (Keep Alive, pp. 47–48) — why a dead TCP connection can go
  undetected for up to 1.5× the keep-alive interval, and how that shows up as a
  publish gap in your soak.
