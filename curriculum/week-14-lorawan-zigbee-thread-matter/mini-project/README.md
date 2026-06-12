# Mini-Project — A LoRaWAN Class-A Telemetry Node to The Things Network

## Brief

Build a complete LoRaWAN Class-A end-device on the RP2040 + SX1262 that transmits an encrypted, MIC-protected telemetry uplink to The Things Network. The node reads the RP2040's on-die temperature sensor and its supply voltage, encodes them into a 3-byte payload, encrypts the payload with AppSKey, signs the frame with NwkSKey, drives the SX1262 through the TX sequence, and transmits on EU868 — and the packet appears in your TTN console's live-data view.

End-to-end demonstration: power the node, watch the CDC console print each uplink with its PHYPayload and airtime, and watch the same frames materialise in the TTN console with correct RSSI/SNR and a decoded temperature reading. The moment the keys, the byte order, the MIC, and the frequency word are all correct simultaneously, the device appears in the console — that synchronisation is the whole point of the project.

Allocate ~6 hours.

## Why LoRaWAN is the hands-on half of this week

LoRaWAN is the one radio in this week's set you can bring up *completely* on the bench with a $15 module, a free network server, and zero certification: The Things Network gives you a public network server and (in most cities) public gateway coverage, so you do not even need your own gateway. Thread/Matter needs an 802.15.4 radio the Pico does not have, so that half is conceptual-plus-real-code (the lectures and Challenge 1). This mini-project makes the LoRaWAN half genuinely real: a packet leaves your bench and shows up on the internet.

## Deliverables

In `mini-project/`:

- `main.c` — the node: sensor read, payload build, uplink loop, CDC logging.
- `lorawan.c` / `lorawan.h` — the LoRaWAN MAC: AES-128, AES-CMAC (MIC), the AES-CTR-like FRMPayload encryption, the uplink builder.
- `sx1262.c` / `sx1262.h` — the SX1262 SPI driver (the Exercise 2 code, productised).
- `lpwan_common.h` — the shared header (the same file from `exercises/`).
- `secrets.h.example` — the template for your TTN keys (you copy it to `secrets.h` and fill in your DevAddr / NwkSKey / AppSKey; `secrets.h` is gitignored so you never commit keys).
- `CMakeLists.txt` — the Pico SDK build producing `lorawan-node.uf2`.
- `cc-ttn-decode.py` — a host-side tool that decodes a captured PHYPayload hex string offline (verifies the MIC, decrypts the FRMPayload) so you can debug a frame without the network.

## Architecture

```text
+------------------------+        +----------------------+        +----------------+
| RP2040 Pico            |        |  SX1262 LoRa module  |        | TTN (internet) |
|                        |  SPI0  |                      |  RF    |                |
|  main.c                +------->|  868.1 MHz SF7 BW125 +~~~~~~~>| public gateway |
|   read temp + vbat     |        |  +22 dBm             |        |   -> network   |
|   lorawan_build_uplink |        |                      |        |      server    |
|     - encrypt FRMPay   |        |                      |        |   -> your app  |
|       (AppSKey, CTR)   |        |                      |        |   -> live data |
|     - MIC (NwkSKey,    |        |                      |        |      console    |
|       CMAC)            |        |                      |        |                |
|   sx1262 SetTx         |        |                      |        |                |
|   CDC log              |        |                      |        |                |
+------------------------+        +----------------------+        +----------------+
```

## Build prerequisites

- Pico SDK 1.5.1+ with `PICO_SDK_PATH` set; `arm-none-eabi-gcc`; CMake; `picotool`.
- An SX1262 module wired to the Pico per `sx1262.h` (the Exercise 2 pin map).
- A The Things Network account (free) and an application with one registered ABP device (so you have a DevAddr, NwkSKey, AppSKey).
- Python 3.10+ for `cc-ttn-decode.py` (uses only the standard library).
- Public TTN gateway coverage in your area, or your own gateway. Check the TTN coverage map.

## Step-by-step

### Step 1 — Register an ABP device on TTN

In the TTN console: create an application, add an end device, choose "enter end device specifics manually," select the EU868 frequency plan and LoRaWAN 1.0.x, and choose **ABP activation**. TTN generates a DevAddr, NwkSKey, and AppSKey. Under the device's settings, **disable the frame-counter checks** (so a reboot that resets your counter does not lock the device out — a development convenience; never do this in production).

### Step 2 — Fill in your secrets

```bash
cp secrets.h.example secrets.h
# edit secrets.h: paste DevAddr / NwkSKey / AppSKey from the TTN console.
# IMPORTANT: the console shows these big-endian (MSB-first); our wire format
# needs DevAddr little-endian. The header comment tells you exactly which
# fields to byte-reverse. Getting this wrong is the #1 bug — see Lecture 2.
```

### Step 3 — Build and flash

```bash
mkdir build && cd build
cmake -DPICO_BOARD=pico ..
make -j
picotool load lorawan-node.uf2 -x   # or drag-and-drop in BOOTSEL mode
```

### Step 4 — Watch the uplinks

Open the CDC console (`screen /dev/cu.usbmodem* 115200`). You should see:

```text
=== LoRaWAN Class-A node ===
DevAddr 26011F88  EU868  SF7 BW125 CR4/5  +14 dBm
uplink #0 FCnt=0: temp=24.31C vbat=92%  payload=0x09 7F 5C
  PHYPayload (16 B): 40 88 1F 01 26 00 00 00 01 <enc3> <mic4>
  airtime 56.6 ms  next in 30 s (duty 0.19%)
uplink #1 FCnt=1: ...
```

### Step 5 — Confirm on TTN

In the TTN console's **Live data** tab for your application, each uplink appears within a second or two of transmission, with the gateway's RSSI and SNR. Add the payload decoder from Challenge 2 to see the temperature as a named field.

### Step 6 — Debug offline with cc-ttn-decode.py

If a frame is rejected, copy its PHYPayload hex from the console and decode it on your laptop:

```bash
python3 cc-ttn-decode.py \
  --devaddr 26011F88 \
  --nwkskey <hex32> --appskey <hex32> \
  --phy 40881F0126000000010 97F5C...
```

The tool re-derives the MIC and decrypts the FRMPayload, telling you whether the MIC is valid and what the plaintext was — so you can tell a key/byte-order bug from a radio bug without the network in the loop.

## The payload format

3 bytes on FPort 1:

| Offset | Field       | Type   | Encoding                       |
|-------:|-------------|--------|--------------------------------|
| 0–1    | temperature | int16  | centidegrees C, big-endian     |
| 2      | battery     | uint8  | percent (0–100)                |

The decoder in Challenge 2 turns these into `temperature_c` and `battery_pct`.

## Duty-cycle discipline

The node computes the airtime of each uplink (Lecture 2 §1.4) and enforces a minimum inter-uplink interval so it stays under EU868's 1% per-sub-band limit. At SF7/BW125 a 16-byte PHYPayload is ~57 ms; a 30-second interval is 0.19% duty cycle — comfortably legal. The node refuses to transmit early if doing so would breach the budget; it logs the hold. This is not optional politeness — it is regulatory compliance, and a node that ignores it will (rightly) get a deployment shut down.

## Pass criteria

- The node transmits a well-formed LoRaWAN uplink that **appears in the TTN console** with a valid MIC (TTN drops invalid-MIC frames silently, so appearance *is* the MIC check).
- The FRMPayload decodes to a sensible temperature and battery value.
- The CDC log shows the airtime and the duty-cycle accounting, and the node respects the inter-uplink minimum.
- `cc-ttn-decode.py` round-trips: it verifies the MIC and recovers the plaintext of a captured frame.
- The frame counter increments per uplink and the node does not reuse a counter.

## What is intentionally out of scope

- **The OTAA join.** We use ABP so we can send an uplink without the JoinRequest/JoinAccept state machine. OTAA is the production-correct choice (rotating session keys, dynamic channel plan); we point at the Semtech LoRaMAC-node reference for the full join. Homework Problem 5 has you design the join.
- **The full multi-channel hopping and ADR control loop.** We pin one EU868 channel and a fixed SF. A production stack rotates channels pseudo-randomly per uplink and runs the ADR control loop from `LinkADRReq` downlinks. We discuss both in Lecture 2 and leave them as extensions.
- **Class B / Class C.** We are Class A — uplink, then two short RX windows. Challenge 2 adds the RX windows for a downlink; Class B's beaconed ping slots and Class C's always-on receiver are out of scope.
- **A private gateway.** We rely on TTN's public network. If you have no public coverage, the resources file points at the cheapest single-channel and full eight-channel gateways to stand up your own.
