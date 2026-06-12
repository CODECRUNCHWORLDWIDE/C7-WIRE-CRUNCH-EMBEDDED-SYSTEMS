# Exercise Solutions — Week 14

Read these *after* you have attempted the exercises. The walkthroughs explain the expected output, the common pitfalls, and the bugs that everyone hits at least once.

The three exercises split by where they run:

- **Exercise 1** (`exercise-01-radio-decision-and-802154.c`) — host-side. Compile and run on your laptop. No hardware.
- **Exercise 2** (`exercise-02-sx1262-bringup.c`) — bench. A Pico wired to an SX1262 module.
- **Exercise 3** (`exercise-03-lorawan-uplink-mic.c`) — host-side. Compile and run on your laptop. No hardware.

The shared header is `lpwan_common.h`.

---

## Exercise 1 — Radio decision + 802.15.4 FCF

### Build and run

```bash
cc -std=c11 -Wall -Wextra -I. exercise-01-radio-decision-and-802154.c -o ex1
./ex1
```

### Expected output

```text
Part A — radio selection
  Vineyard soil sensor         -> LoRaWAN
  Smart light bulb             -> Thread
  Warehouse lighting           -> Zigbee
  Long-range HD video (bad)    -> NONE (design conflict)

Part B — 802.15.4 frame control fields
  Thread data frame      FCF=0x0869 type=Data           [SEC ACKREQ PANC]
  Zigbee beacon          FCF=0x8000 type=Beacon         []
  802.15.4 ACK           FCF=0x0002 type=Acknowledgment []
  MAC command            FCF=0x0023 type=MAC Command    [ACKREQ ]
```

### Walkthrough

**Part A** encodes the Lecture 1 heuristic as ordered rules. The order matters: the long-range-on-battery rule fires first, because LoRaWAN is the only radio that satisfies that constraint and nothing should override it. The "impossible brief" returns `RADIO_NONE` because the payload/duty-cycle budget cannot close even though the range/power constraints point at LoRaWAN — the function detects the conflict and refuses to recommend a radio that physically cannot serve the product.

The point of the exercise is not that the weights are sacred. Change them. Add a profile for an asset tracker (mobile, so ADR off, LoRaWAN with GPS), or a smart lock (Thread, battery, sleepy-end-device). The skill is turning a fuzzy "which radio?" into a function with named inputs you can reason about.

**Part B** parses the 16-bit Frame Control Field that opens every 802.15.4 frame. The low 3 bits select the frame type; the named bits (security, frame-pending, ack-request, PAN-ID-compression) flag the frame's properties. `0x0869` decodes as a Data frame with security enabled, ack requested, and PAN-ID compression — exactly the shape of a Thread MAC data frame you will see in a `pyspinel` or Wireshark capture. Recognising this field at a glance is the difference between reading a 15.4 capture and staring at hex.

### Common pitfalls

- **Treating the FCF as big-endian.** It is little-endian on the wire. If you read it MSB-first you will decode the wrong frame type. The exercise hands you the value already in host order; in a real capture, byte-swap.
- **Over-trusting the verdict.** `cc_choose_radio()` is a *starting point* for a conversation, not an oracle. The rule order is opinionated; in a real design review you would defend or revise it. The "NONE" result is the most valuable output — it tells you the brief is internally inconsistent.

---

## Exercise 2 — SX1262 bring-up

### Build and run

```cmake
add_executable(exercise2 exercise-02-sx1262-bringup.c)
target_link_libraries(exercise2 pico_stdlib hardware_spi hardware_gpio)
pico_add_extra_outputs(exercise2)
```

Wire the module per the header comment, flash `exercise2.uf2`, open the CDC console.

### Expected output

```text
=== Exercise 2: SX1262 bring-up ===
Configuring radio: 868.1 MHz, SF7, BW125, CR 4/5, +22 dBm.
GetStatus after config: 0x2A (chip mode 2, cmd status 5).
TX #0 (6 bytes): TX_DONE
TX #1 (6 bytes): TX_DONE
...
```

A second LoRa receiver (another SX1262, a LoRa gateway in raw mode, or an RTL-SDR running `gr-lora`) tuned to 868.1 MHz / SF7 / BW125 will see "CC7 #0", "CC7 #1", ... arrive every 5 seconds.

### Walkthrough

The status byte after configuration is your first sanity check. The `GetStatus` opcode (`0xC0`) returns a byte whose bits 6:4 are the chip mode (2 = STDBY_RC, 3 = STDBY_XOSC, 5 = TX, 6 = RX) and bits 3:1 are the last command status (1 = data available, 2 = command timeout, 5 = command-processed-OK is common). A status of `0x00` or `0xFF` means SPI is not talking to the chip at all — check wiring and the NSS polarity.

The TX path follows the datasheet's §9.2.1 ordering exactly: standby, regulator, RF-switch control, packet type, frequency, PA config, TX params, modulation, then per-packet buffer base, packet params, IRQ params, write buffer, SetTx, wait for DIO1.

### The five canonical SX126x bring-up bugs

1. **BUSY ignored.** You fire commands without waiting for BUSY to fall, so the chip drops half of them. Symptom: configuration "works" but TX never completes, or the frequency is wrong. Fix: `sx_wait_busy()` before *every* NSS assertion. This is the bug; if something is wrong, suspect this first.
2. **TCXO never started.** The module has a TCXO powered from DIO3, and you either forgot `SetDio3AsTcxoCtrl` or passed the wrong voltage code. Symptom: BUSY stuck high forever, every command hangs. The `sx_wait_busy()` 100 ms ceiling turns this from an infinite hang into a printable warning — which is why the ceiling exists.
3. **RF switch not driven.** Most modules route the antenna through an RF switch controlled by DIO2; without `SetDio2AsRfSwitchCtrl`, the PA is driving into an open and nothing radiates. Symptom: TX_DONE fires (the chip thinks it transmitted) but no receiver hears anything. Fix: `SetDio2AsRfSwitchCtrl(1)`.
4. **Wrong frequency word.** You passed Hz directly to `SetRfFrequency` instead of `hz / FREQ_STEP`. Symptom: transmitting hundreds of kHz off, intermittent or no reception. Fix: the `frf = hz / FREQ_STEP` arithmetic with the 32 MHz crystal step.
5. **PA config / deviceSel wrong.** The SX1261 and SX1262 have different PA configs; using the SX1261's on an SX1262 (or vice versa) gives wrong output power or damages nothing but radiates weakly. Symptom: very short range. Fix: `deviceSel = 0x00` for SX1262, `0x01` for SX1261, with the matching duty-cycle/hpMax bytes (datasheet §13.1.14, Table 13-21).

---

## Exercise 3 — LoRaWAN uplink + MIC

### Build and run

```bash
cc -std=c11 -Wall -Wextra -I. exercise-03-lorawan-uplink-mic.c -o ex3
./ex3
```

### Expected output

```text
AES-CMAC primitive self-test (RFC 4493):
  PASS: empty + 16-byte RFC 4493 vectors match.

Building three uplinks (empty FRMPayload, FPort 1):
FCnt=0:
  PHYPayload    40 12 34 56 78 00 00 00 01 0B 62 CE 54 (13 bytes)
    MIC         0B 62 CE 54 (4 bytes)
FCnt=1:
  PHYPayload    40 12 34 56 78 00 01 00 01 C4 FA 21 62 (13 bytes)
    MIC         C4 FA 21 62 (4 bytes)
FCnt=2:
  PHYPayload    40 12 34 56 78 00 02 00 01 9D AA 24 69 (13 bytes)
    MIC         9D AA 24 69 (4 bytes)
```

(The exact MIC bytes depend on the keys in the source; the point is that they *change with FCnt* and that the CMAC self-test passes.)

### Walkthrough

The exercise does three things, in the right order:

1. **Verifies the AES-128 + AES-CMAC primitive against RFC 4493's test vectors** before building anything on top. This is the load-bearing discipline of the whole week: you do not trust a crypto primitive you have not checked against the standard's own vectors. The empty-message vector (`bb1d6929...`) and the 16-byte vector (`070a16b4...`) both come straight from RFC 4493 §4.
2. **Builds the PHYPayload** byte by byte: MHDR (`0x40` = unconfirmed up, major R1), DevAddr (little-endian on the wire — note `12 34 56 78` is DevAddr `0x78563412`), FCtrl (`0x00`), FCnt (little-endian 16-bit), FPort (`0x01`), then the MIC.
3. **Computes the MIC** by AES-CMAC over the B0 block (`0x49 00 00 00 00 | dir | DevAddr | FCnt | 00 | len`) concatenated with the message, keyed with NwkSKey, truncated to 4 bytes.

### The lesson in the changing MIC

The three uplinks send an *identical* message (empty payload, FPort 1) yet produce three different MICs, because the frame counter feeds into B0. That is LoRaWAN's replay defence in action: the network server tracks the highest FCnt it has seen and rejects any frame at or below it. Reuse a counter and the server rejects the frame as a replay. This is also why an ABP device that reboots and resets its counter to 0 silently stops working — the server has already seen FCnt 0 through N and drops everything until the device climbs back above N.

### Common pitfalls

- **Byte order on DevAddr.** The single most common LoRaWAN bug. DevAddr and the EUIs are little-endian *on the wire* but the TTN console displays them big-endian. Copy the console value verbatim into the byte array and your MIC will be wrong. Reverse the bytes.
- **CMAC subkey generation.** The K1/K2 subkeys (RFC 4493 §2.3) require the constant `0x87` XOR after the left-shift *only when the high bit was set*. Get the conditional backwards and the empty-message vector fails immediately — which is exactly why the self-test runs first.
- **Forgetting the message length in B0[15].** The B0 block's last byte is the length of the message the MIC covers (MHDR..FRMPayload), not the length of B0 and not the PHYPayload length. Off by one here and the MIC is wrong with no other symptom.
- **Truncation direction.** The MIC is the *first* 4 bytes of the 16-byte CMAC, not the last. RFC and LoRaWAN both take the leading bytes.

---

## What you should be able to do now

After these three exercises you can: reason about radio selection numerically (Ex 1), drive a real LoRa transceiver from registers up over SPI (Ex 2), and build a standards-conformant LoRaWAN uplink with a correct MIC verified against RFC 4493 (Ex 3). The mini-project stitches Ex 2 and Ex 3 together into a node that appears in the The Things Network console.
