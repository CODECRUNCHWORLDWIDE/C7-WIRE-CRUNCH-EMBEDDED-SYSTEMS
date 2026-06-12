# Quiz — Week 14

Ten questions. Closed-book on the radio-selection reasoning and the LoRaWAN frame layout; open-book on the C and spec citations. ~45 minutes. Answer key at the bottom; do not peek before completing.

---

## Question 1

A product brief says: "battery-powered soil sensor, 6 km from the nearest gateway, sends 8 bytes every 30 minutes, 1,000 units across a farm." Which radio, and why is each of the other three wrong?

## Question 2

Zigbee and Thread both run on IEEE 802.15.4. Given that they share a physical layer, the choice between them is decided by:

(A) Range — Thread reaches farther.
(B) The network/application layer and ecosystem — Thread carries IPv6 and is what Matter standardised on; Zigbee is the mature mesh with its own application profiles.
(C) Data rate — Zigbee is faster.
(D) Frequency band — they use different bands.

## Question 3

What is Matter, precisely? Pick the most accurate statement.

(A) A 2.4 GHz radio that competes with Zigbee.
(B) A replacement for Thread.
(C) An application layer (data model + commissioning + security) that runs over Thread, Wi-Fi, or Ethernet.
(D) A LoRaWAN profile for smart homes.

## Question 4

A LoRaWAN uplink's MIC is computed with AES-128-CMAC over a "B0" block prepended to the message, then truncated. Which key, and how many bytes of the CMAC become the MIC?

(A) AppSKey; last 4 bytes.
(B) NwkSKey; first 4 bytes.
(C) AppKey; all 16 bytes.
(D) NwkSKey; last 8 bytes.

## Question 5

You register an ABP device on TTN, flash it, and it transmits — but nothing appears in the console. You capture the PHYPayload and run `cc-ttn-decode.py`, which reports `MIC INVALID`. List the four suspects, in the order the tool suggests checking them, and explain why DevAddr is first.

## Question 6

The same node sends an identical 3-byte payload twice in a row (same temperature, same battery). Will the two PHYPayloads on the wire be byte-identical? Justify.

(A) Yes — identical input gives identical output.
(B) No — the frame counter changes, which changes both the encrypted FRMPayload and the MIC.
(C) No — only the MIC changes; the FRMPayload is identical.
(D) Yes, unless ADR is enabled.

## Question 7

A LoRaWAN Class-A device transmits an uplink and you queue a downlink command from the network server. When can the device receive that downlink?

(A) Immediately, at any time.
(B) Only in the RX1 and RX2 receive windows that open 1 s and 2 s after the end of the device's next uplink.
(C) Only if it is a Class C device.
(D) On a scheduled ping slot synchronised to a beacon.

## Question 8

At SF12/BW125, a 20-byte LoRa payload has roughly 1.1–1.3 s of airtime. Under EU868's 1% per-sub-band duty cycle, what is the *minimum legal interval* between such transmissions on one channel, approximately?

(A) ~13 ms.
(B) ~1.2 s.
(C) ~120 s.
(D) No limit; EU868 has no duty cycle.

## Question 9

In the SX1262 driver, every command is preceded by a wait on the BUSY line. What goes wrong if you omit that wait, and why is the bug so hard to find?

## Question 10

Write the C that builds the B0 block used for a LoRaWAN uplink MIC, given a `lorawan_session_t *s` and a message length `msg_len`. Assume `cc_write_le32` from `lpwan_common.h` is available. (Sketch the 16 bytes.)

---

## Answers

### Q1: LoRaWAN, Class A.

Six kilometres on a battery with tiny infrequent payloads is the exact shape LoRaWAN was built for, and one gateway hears all 1,000 sensors. **Zigbee** and **Thread** are wrong because their 802.15.4 PHY reaches ~100 m per hop and there are no intermediate nodes to mesh across open farmland — meshing is not a substitute for range when the field is empty. **Wi-Fi** is wrong because it has neither the coverage nor the multi-year battery budget. This is the case where the answer is *forced*: only LoRaWAN closes the link.

### Q2: B

They share the 802.15.4 PHY, so the choice is never about range, data rate, or band (all the same). It is about the network/application layer: Thread carries IPv6 and is the low-power mesh Matter standardised on; Zigbee is the mature mesh with its own ZCL application profiles and a deep installed base. (Range is roughly equal; if anything depends on antenna and power, not the protocol.)

### Q3: C

Matter is an application layer — a data model (endpoints/clusters/attributes/commands), an interaction model, a commissioning flow, and a certificate-based security model — that runs *over* a transport: Thread, Wi-Fi, or Ethernet. It is not a radio (A), not a Thread replacement (B), and has nothing to do with LoRaWAN (D). "Matter device" for a low-power sensor almost always means "Matter-over-Thread."

### Q4: B

The MIC is AES-128-CMAC keyed with **NwkSKey** (the network session key), and the MIC is the **first 4 bytes** of the 16-byte CMAC. AppSKey encrypts the FRMPayload, not the MIC. RFC and LoRaWAN both take the leading bytes for truncation. LoRaWAN L2 1.0.4 §4.4.

### Q5

Order: **(1) DevAddr byte order, (2) NwkSKey bytes, (3) the frame counter, (4) the message length in B0[15].** DevAddr is first because it is the most common bug: the TTN console displays DevAddr big-endian but the wire format is little-endian, so copying the console value verbatim into the byte array reverses it, which both fails the DevAddr match and poisons the B0 block the MIC is computed over. The other three are real but rarer. Note that an invalid MIC, not a radio problem, is what `cc-ttn-decode` reports — it isolates a keys/encoding bug from an RF bug without the network in the loop.

### Q6: B

No. The frame counter (FCnt) increments between the two uplinks, and FCnt feeds into *both* the FRMPayload encryption block A_i (so the ciphertext differs) *and* the MIC's B0 block (so the MIC differs). Identical plaintext therefore produces two different PHYPayloads. (C) is wrong because the FRMPayload also changes. This is by design: the monotonic counter is LoRaWAN's replay defence.

### Q7: B

A Class-A device's receiver is off except during the two short windows after its own uplink: RX1 (same frequency, 1 s after end-of-uplink for EU868) and RX2 (869.525 MHz, DR0, 2 s after). The network server holds the queued downlink until the device next uplinks, then transmits in a window. This is *the* defining behaviour of Class A: command latency is bounded below by the uplink interval. (C) and (D) describe Class C and Class B respectively.

### Q8: C

Airtime / duty-cycle limit = 1.2 s / 0.01 = 120 s. So you may transmit that SF12 packet on one sub-band roughly once every two minutes. (A) is the symbol time, not the packet. (B) is the airtime itself. (D) is false — EU868 enforces 1% per sub-band (ETSI EN 300 220). This duty-cycle ceiling, not the radio, is what caps LoRaWAN traffic.

### Q9

If you omit the BUSY wait, you start the next SPI command while the chip is still digesting the previous one, and the chip **silently drops the new command**. Half your configuration does not take effect. The bug is hard to find because there is no error: `GetStatus` may even look fine, TX_DONE may fire, but the radio is misconfigured (wrong frequency, wrong modulation) and the symptom is "no receiver hears anything" or "intermittent reception" with no diagnostic. The only reliable way to catch it is a logic analyzer showing the command issued while BUSY is high — which is why "BUSY ignored" is the first of the five canonical SX126x bugs.

### Q10

```c
uint8_t b0[16 + LORAWAN_MAX_PHY_PAYLOAD];
memset(b0, 0, 16);
b0[0]  = 0x49u;                 /* fixed block-type byte           */
/* b0[1..4] = 0x00 (already zeroed)                                */
b0[5]  = LORAWAN_DIR_UPLINK;    /* 0x00 uplink, 0x01 downlink      */
memcpy(&b0[6], s->devaddr, 4);  /* DevAddr, little-endian          */
cc_write_le32(&b0[10], s->fcnt_up);  /* 32-bit FCnt, little-endian */
b0[14] = 0x00u;                 /* fixed                           */
b0[15] = (uint8_t)msg_len;      /* length of MHDR..FRMPayload      */
/* then: memcpy(&b0[16], msg, msg_len) and aes_cmac(nwk_skey, ...) */
```

The block is `0x49 | 0x00000000 | Dir | DevAddr(4 LE) | FCnt(4 LE) | 0x00 | len`. B0[15] is the length of the message the MIC covers, not the PHYPayload length. LoRaWAN L2 1.0.4 §4.4.

(End of quiz.)
