# Mini-Project — A Wi-Fi / MQTT 5 / TLS Sensor Node

## Brief

Build a complete sensor node on the Pico W: it joins WPA2/WPA3 Wi-Fi on boot,
resolves `test.mosquitto.org`, opens a TLS 1.2 connection to port 8883 with the
broker's certificate verified against a pinned ISRG Root X1, CONNECTs as an MQTT 5
client with a unique-id-derived client id, SUBSCRIBEs to its command channel, and
PUBLISHes a 96-byte JSON telemetry reading every 10 s at QoS 0 — and recovers
cleanly, with exponential backoff and jitter, from every failure you can synthesize
on the bench.

End-to-end on the bench: plug the Pico W into a USB power supply (no laptop
tether), and within ~8 s of cold boot it has joined Wi-Fi, handshaken TLS, and
published its first reading. It then holds the 10 s cadence for the week. On
`cc-listen.py` running on your laptop, you watch the readings stream in. The
grade-boundary metric you must hit: **median publish-delivery ratio ≥ 99.0% over
24 hours, worst-case publish gap ≤ 60 s during a forced disconnect** — which is
exactly what Challenge 1 measures.

Allocate ~6 hours of build/bring-up across Wednesday–Saturday, then the 24-hour
soak (Challenge 1) starts Sunday evening.

## Deliverables

In the `mini-project/` directory of your fork:

- `main.c` — the top-level retry-backoff state machine, the lwIP raw-API TCP
  transport, the mbedTLS 1.2 session glue, the heap-free JSON encoder, the sensor
  mock, and the publish loop. The five-state machine from `wifi_common.h`
  (`NET_STATE_BOOT/WIFI_DOWN/WIFI_UP_BROKER_DOWN/CONNECTED/BACKOFF`) lives here.
- `mqtt_client.c` / `mqtt_client.h` — the MQTT 5 client: CONNECT/CONNACK,
  PUBLISH (both directions, QoS 0), SUBSCRIBE/SUBACK, PINGREQ/PINGRESP,
  DISCONNECT. The encode/decode core is exactly Exercise 3's, grown up.
- `mbedtls_config.h` — the trimmed TLS config: one cipher suite
  (ECDHE-RSA-AES128-GCM-SHA256), one curve (secp256r1), TLS 1.2 only, ~95 KB of
  `.text` versus ~250 KB stock.
- `ca_bundle.h` + the generated `ca_root_pem.h` — the pinned trust anchor.
- `lwipopts.h` — the lwIP 2.2.0 options for the `_lwip_poll` variant.
- `CMakeLists.txt` + `pico_sdk_import.cmake` — the build, producing
  `sensor-node.uf2`. The CMake step fetches and embeds the authoritative
  ISRG Root X1 PEM (checksum-verified) so the pinned cert is never hand-transcribed.
- `cc-listen.py` — the laptop-side subscriber and soak analyzer.
- `requirements.txt` — `paho-mqtt>=1.6`.

## Architecture

```text
+-------------------+        +--------------------------------------------+
| Laptop            |        | Pico W (RP2040 + CYW43439)                 |
|                   |        |                                            |
| cc-listen.py      |        |  main.c  (state machine)                   |
|  paho-mqtt        | <--+   |    NET_STATE_WIFI_DOWN ---> wifi_up()       |
|  TLS:8883         |    |   |    NET_STATE_WIFI_UP_BROKER_DOWN            |
|  SUB telemetry    |    |   |        |  resolve_broker (DNS)             |
|  prints + ratio   |    |   |        |  tcp_open (lwIP raw API)          |
+-------------------+    |   |        |  tls_handshake (mbedTLS 1.2)      |
                         |   |        |  mqtt_connect / mqtt_subscribe    |
        test.mosquitto.org  |    NET_STATE_CONNECTED                      |
        broker  :8883 (TLS) +<---|    PUB telemetry q10s + PINGREQ        |
                            |   |    NET_STATE_BACKOFF (1->2->...->60 s)  |
                            |   |        exponential + 25% jitter          |
                            |   |                                          |
                            |   |  mqtt_client.c  (MQTT 5 wire format)     |
                            |   |  bio_send/bio_recv  (mbedTLS <-> lwIP)   |
                            |   +--------------------------------------------+
                            |
        every PUBLISH on the device is logged [+ms] PUB seq=N; every RECV on the
        laptop is logged [+s] RECV seq=N; cc-listen.py analyze reconciles them.
```

The layering, top to bottom: the MQTT client speaks to an abstract transport;
the transport is `mbedtls_ssl_write/read`; mbedTLS's BIO callbacks
(`bio_send`/`bio_recv`) speak to lwIP's raw TCP API; lwIP speaks to the CYW43
driver; the CYW43 speaks 802.11 to your AP. Each seam is a place a failure can
originate, and each is logged with a timestamp so a UART trace alone tells you the
failure point.

## Build prerequisites

- Pico SDK 1.5.1 or later, `PICO_SDK_PATH` set. The SDK vendors lwIP 2.2.0 and
  mbedTLS under `lib/`; the `pico_cyw43_arch_lwip_poll` and `pico_mbedtls` targets
  are what we link.
- `arm-none-eabi-gcc` 10.3+ and CMake 3.13+.
- `xxd` (for the CA-header generation step; ships with vim).
- A **Pico W** (the CYW43 wireless variant; `-DPICO_BOARD=pico_w`).
- A Wi-Fi network you control (home AP or phone hotspot — not enterprise
  WPA2-EAP). Python 3.10+ with `paho-mqtt` for `cc-listen.py`.

Copy `pico_sdk_import.cmake` from `$PICO_SDK_PATH/external/` into this directory
if it is not already here.

## Step-by-step build

### Step 1 — Build

```bash
mkdir build && cd build
cmake -DPICO_BOARD=pico_w \
      -DWIFI_SSID="\"my-ap\"" \
      -DWIFI_PASSWORD="\"correct-horse-battery-staple\"" ..
make -j$(nproc)
```

The double-double-quote on the credentials is deliberate: the inner quotes survive
the shell and reach the C preprocessor as a string literal. Output:
`sensor-node.uf2` (~430 KB; comfortably under the 500 KB budget and the 752 KB
active bank from Week 10).

If the configure step prints "Downloading ISRG Root X1", it fetched and
checksum-verified the pinned root. If you are offline, drop the PEM at
`certs/isrgrootx1.pem` first and CMake will use that copy.

### Step 2 — Flash

Hold BOOTSEL, plug in, drop the UF2:

```bash
cp sensor-node.uf2 /Volumes/RPI-RP2      # macOS; Linux: /media/$USER/RPI-RP2
```

Or, if you have Week 10's signed bootloader installed, deploy over OTA:

```bash
python3 ../../week-10-bootloaders-and-firmware-updates/mini-project/cc-sign.py \
    sensor-node.bin sensor-node.ccf
python3 ../../week-10-bootloaders-and-firmware-updates/mini-project/cc-flash.py \
    sensor-node.ccf
```

Deploying over OTA is the production path and the prerequisite for surviving a CA
rotation (Challenge 2).

### Step 3 — Watch it come up

```bash
screen /dev/cu.usbmodem* 115200          # or minicom
```

Expected boot trace (timings on a typical home AP):

```text
[2003 ms] cc7 sensor node boot; device-id=a1b2c3 client-id=cc7-pico-a1b2c3
[3490 ms] WIFI_DOWN: connecting to "my-ap"...
[6712 ms] connected; ip=192.168.1.42
[6740 ms] DNS test.mosquitto.org -> 5.196.78.28
[8061 ms] TLS handshake ok (1287 ms)
[8159 ms] MQTT CONNACK rc=0
[8203 ms] subscribed to cc7/devices/a1b2c3/cmd
[8210 ms] PUB seq=0 bytes=58 rc=0
[18210 ms] PUB seq=1 bytes=58 rc=0
[28211 ms] PUB seq=2 bytes=58 rc=0
```

First publish within ~8 s of boot; cadence holds at 10 s thereafter.

### Step 4 — Subscribe from the laptop

```bash
pip install -r requirements.txt
python3 cc-listen.py --id a1b2c3
```

```text
# connected to test.mosquitto.org:8883 rc=Success
# subscribed to cc7/devices/a1b2c3/telemetry
[2026-06-12T14:03:21] [+    1.402 s] RECV seq=0 bytes=58 topic=cc7/devices/a1b2c3/telemetry temp_c=24.5 vbat_v=3.30 uptime_s=6
[2026-06-12T14:03:31] [+   11.408 s] RECV seq=1 bytes=58 topic=cc7/devices/a1b2c3/telemetry temp_c=24.5 vbat_v=3.30 uptime_s=16
```

### Step 5 — Drive the command channel

From any MQTT client on the broker (or `cc-listen.py`'s broker):

```bash
mosquitto_pub -h test.mosquitto.org -p 8883 --capath /etc/ssl/certs \
    -t cc7/devices/a1b2c3/cmd -m "set-interval 5"
# device log: [123456 ms] CMD recv: "set-interval 5"
#             [123456 ms] interval set to 5 s

mosquitto_pub -h test.mosquitto.org -p 8883 --capath /etc/ssl/certs \
    -t cc7/devices/a1b2c3/cmd -m "reboot"
# device log: [...] reboot command honored  (watchdog reset; re-boots in ~8 s)
```

### Step 6 — Force a disconnect and watch the backoff

Power-cycle your AP. The device drops to backoff and climbs the ladder:

```text
[63012 ms] link DOWN — entering backoff
[63013 ms] backoff sleep=1000 ms
[64015 ms] WIFI_DOWN: connecting to "my-ap"...   (AP still off -> 30 s timeout)
[94030 ms] wifi connect failed: -1
[94031 ms] backoff sleep=2000 ms
...
[AP back] connected; ip=192.168.1.42
[...] TLS handshake ok (1304 ms)
[...] MQTT CONNACK rc=0      <-- backoff resets to 1000 ms on success
[...] PUB seq=37 bytes=58 rc=0
```

This is the Challenge 1 behavior; rehearse it before the soak.

## Pass criteria

- `sensor-node.uf2` is < 500 KB at `-Os`.
- Cold boot to first publish ≤ 8 s on a typical home AP.
- The 10 s cadence holds within ±200 ms over a 10-minute observation
  (read the inter-`RECV` deltas in `cc-listen.py`).
- TLS verifies the broker cert against the pinned root; a deliberately wrong root
  (Challenge 2) is rejected with `MBEDTLS_ERR_X509_CERT_VERIFY_FAILED` and the
  device backs off cleanly rather than crashing.
- A forced AP power-cycle recovers within the backoff envelope (≤ 60 s publish
  gap) and the backoff resets to 1 s on reconnect.
- The 24-hour soak (Challenge 1) reaches ≥ 99.0% delivery as computed by
  `cc-listen.py analyze`.

## Common bring-up gotchas

1. **`cyw43_arch_init` hangs.** You built without `-DPICO_BOARD=pico_w`; there is
   no CYW43 driver in the binary. The onboard LED is wired to the CYW43, so
   `gpio_put(25,…)` does nothing on a Pico W — that is the tell.
2. **TLS handshake fails at `-0x2700`.** Certificate verify failed. Either the
   pinned root is wrong (check `ca_root_pem.h` was generated from
   `isrgrootx1.pem`) or the broker rotated its chain (Challenge 2's runbook).
   Bump `mbedtls_debug_set_threshold(3)` to see the exact verify step.
3. **TLS handshake stalls forever at WANT_READ.** Your `bio_recv` is not being fed
   because `cyw43_arch_poll()` is not running inside the handshake loop. The
   `_lwip_poll` variant only services the stack when you poll it; the handshake
   loop in `tls_handshake()` polls on every WANT_READ — keep it that way.
4. **The ring buffer overflows under load.** `tcp_recv_cb` drops bytes when the
   4 KB ring is full, which corrupts the TLS stream and fails the next record MAC.
   At our payload sizes the ring never fills in steady state; if it does, you are
   stalling the drain (a long blocking call in the main loop) — find it.
5. **Publishes succeed but the laptop sees nothing.** `test.mosquitto.org` lets
   anyone publish and subscribe; if two students use the same device id, your
   topics collide. The id is derived from the RP2040 unique board id, so this only
   happens if you hardcoded it — don't.
6. **`mbedtls_ssl_write` returns WANT_WRITE constantly.** The lwIP send buffer is
   full because `tcp_output` is not flushing; confirm `TCP_SND_BUF` in `lwipopts.h`
   is at least a few MSS and that `bio_send` calls `tcp_output` after `tcp_write`.

## Bench session structure

- **Wednesday** — get Exercise 1 (Wi-Fi join) and Exercise 3 (MQTT encode) solid;
  they are the load-bearing pieces of `main.c` and `mqtt_client.c`.
- **Thursday** — bring up TLS: build `mbedtls_config.h`, generate the CA header,
  get `tls_handshake` to verify the broker cert. This is the longest single
  bring-up; budget 2 hours.
- **Friday** — wire the MQTT client over TLS; get a CONNACK rc=0 and a first
  PUBLISH the laptop receives.
- **Saturday** — the retry-backoff state machine and the publish loop; force
  disconnects and watch recovery. End the day with a clean 10-minute run.
- **Sunday evening** — start the 24-hour soak (Challenge 1).

## References

- All of Week 11's lecture notes (CYW43, lwIP raw API, MQTT 5 wire format + TLS).
- pico-sdk networking docs.
  <https://www.raspberrypi.com/documentation/pico-sdk/networking.html>
- lwIP 2.2.0 raw API. <https://www.nongnu.org/lwip/2_2_x/raw_api.html>
- MQTT 5.0 OASIS Standard.
  <https://docs.oasis-open.org/mqtt/mqtt/v5.0/mqtt-v5.0.html>
- mbedTLS configuration + X.509 verification guides.
  <https://mbed-tls.readthedocs.io/>
- test.mosquitto.org. <https://test.mosquitto.org/>
- Week 10's signed-OTA bootloader (the deployment path for this firmware).
