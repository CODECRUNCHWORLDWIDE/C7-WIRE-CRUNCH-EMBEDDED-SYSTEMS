# Resources — Week 14

Every link below is free unless explicitly marked otherwise. The five load-bearing references are the Semtech SX1261/2 datasheet, the LoRaWAN L2 1.0.4 specification, RFC 4493 (AES-CMAC), the Matter Core specification, and the Thread specification. If you read only the first three you will pass the quiz and the mini-project will work; the last two carry the conceptual half.

---

## Primary references — read this week

### 1. Semtech SX1261/2 datasheet

- **Source:** Semtech Corporation, "SX1261/2 Long Range, Low Power, sub-GHz RF Transceiver", rev. 2.1.
- **URL:** <https://www.semtech.com/products/wireless-rf/lora-connect/sx1262> (datasheet PDF on the product page).
- **Sections we use:**
  - §6.1 — "Modem Configuration". Spreading factor, bandwidth, coding rate, the SF/airtime trade.
  - §8.3 — the SPI interface and the BUSY-line timing.
  - §9.2.1 — the recommended TX setup sequence (the ordering our `sx1262_config` follows).
  - §13 — "Commands". The full opcode catalogue: SetStandby, SetPacketType, SetRfFrequency, SetPaConfig, SetTxParams, SetModulationParams, SetPacketParams, SetTx, GetStatus, GetIrqStatus, etc.
- **Cite as:** "Semtech SX1261/2 datasheet §X.Y".

### 2. LoRaWAN L2 1.0.4 Specification

- **Source:** LoRa Alliance, "LoRaWAN L2 1.0.4 Specification" (TS001-1.0.4), 2020.
- **URL:** <https://resources.lora-alliance.org/technical-specifications/ts001-1-0-4-lorawan-l2-1-0-4-specification> (free, registration).
- **Sections we use:**
  - §4 — "MAC Message Formats". PHYPayload, MHDR, FHDR, FPort, FRMPayload.
  - §4.3.3 — "MAC Frame Payload Encryption (FRMPayload)". The AES-CTR-like cipher.
  - §4.4 — "Message Integrity Code (MIC)". The B0 block and the AES-CMAC computation.
  - §6 — "End-Device Activation". OTAA (JoinRequest/JoinAccept, session-key derivation) and ABP.
- **Cite as:** "LoRaWAN L2 1.0.4 §X".

### 3. LoRaWAN Regional Parameters RP002-1.0.4

- **Source:** LoRa Alliance, "LoRaWAN Regional Parameters RP002-1.0.4".
- **URL:** <https://resources.lora-alliance.org/technical-specifications/rp002-1-0-4-regional-parameters>
- **Sections we use:** the EU868 channel plan (default channels 868.1/868.3/868.5, 1% duty cycle, RX2 = 869.525 MHz DR0) and the US915 plan (64+8 channels, dwell-time limit, sub-band pinning).
- **Cite as:** "RP002-1.0.4, EU868".

### 4. RFC 4493 — The AES-CMAC Algorithm

- **Source:** Song, Poovendran, Lee, Iwata, IETF, June 2006.
- **URL:** <https://www.rfc-editor.org/rfc/rfc4493>
- **Sections we use:** §2.3 (subkey generation K1/K2), §2.4 (the MAC algorithm), §4 (the test vectors Exercise 3 and `lorawan.c` check against).
- **Cite as:** "RFC 4493 §X".

### 5. Matter Core Specification + Thread Specification

- **Matter:** CSA, "Matter Core Specification 1.3" (and 1.4). <https://csa-iot.org/all-solutions/matter/> (specs free with registration; the `connectedhomeip` SDK is the open implementation). Sections: §4 (Secure Channel — PASE/CASE), §6 (Commissioning), §8 (Interaction Model), §13 (Data Model). Cite as "Matter Core §X".
- **Thread:** Thread Group, "Thread 1.3 Specification". <https://www.threadgroup.org/support#specifications> (free with registration). Sections: §3 (Network Layer), §5 (MLE). Cite as "Thread 1.3 §X".

---

## Secondary references — consult as needed

### Semtech AN1200.13 — LoRa airtime

- **Source:** Semtech, "LoRa Modem Designer's Guide" / "LoRa Modulation Basics" (AN1200.xx series).
- **URL:** <https://www.semtech.com/uploads/documents/LoraDesignGuide_STD.pdf>
- **What we use it for:** the closed-form airtime equation in Lecture 2 §1.4 and in `airtime.py` (Homework 1) and `main.c`. The 1% duty-cycle accounting depends on it.

### ETSI EN 300 220 — sub-GHz SRD regulation (EU868)

- **URL:** <https://www.etsi.org/standards> (search EN 300 220).
- **What we use it for:** the 1% per-sub-band duty-cycle limit that caps LoRaWAN traffic in EU868. The reason the node holds a transmission rather than chattering.

### IEEE 802.15.4-2020

- **Source:** IEEE, "IEEE Standard for Low-Rate Wireless Networks".
- **URL:** <https://standards.ieee.org/ieee/802.15.4/7029/> (the standard is paywalled; the frame formats are widely summarised online and in the Thread/Zigbee specs).
- **Sections we use:** §7.2.2.1 (the Frame Control Field, parsed in Exercise 1) and §10 (the 2.4 GHz O-QPSK PHY).

### OpenThread

- **Source:** Google, `openthread/openthread` on GitHub.
- **URL:** <https://openthread.io> and <https://github.com/openthread/openthread>
- **License:** BSD-3-Clause.
- **What we use it for:** the Thread stack in Challenge 1; the `ot-cli` shell; the codelabs; the simulation build (`OT_SIMULATION`) for the no-hardware path.

### OpenThread Border Router (ot-br-posix)

- **URL:** <https://openthread.io/guides/border-router> and <https://github.com/openthread/ot-br-posix>
- **What we use it for:** standing up a Thread Border Router on a Raspberry Pi + nRF52840 RCP in Challenge 1, for those without an Apple/Google hub TBR.

### connectedhomeip (the Matter SDK)

- **Source:** CSA + platform vendors, `project-chip/connectedhomeip` on GitHub.
- **URL:** <https://github.com/project-chip/connectedhomeip>
- **License:** Apache-2.0.
- **What we use it for:** the `examples/lighting-app` brought up in Challenge 1; `chip-tool` for commissioning and control; the cluster callbacks shown in Lecture 3.

### The Things Network

- **URL:** <https://www.thethingsnetwork.org> (community) and <https://www.thethingsindustries.com/docs/> (docs).
- **What we use it for:** the free public LoRaWAN network server and gateway coverage the mini-project transmits to; device registration (ABP/OTAA); payload formatters (Challenge 2); the live-data console. Check the coverage map at <https://www.thethingsnetwork.org/map>.

### pyspinel + nRF 802.15.4 sniffer

- **URL:** <https://github.com/openthread/pyspinel>
- **What we use it for:** sniffing the 802.15.4 traffic during Matter commissioning in Challenge 1, feeding Wireshark.

---

## Tertiary references — context and depth

### Semtech LoRaMAC-node (the reference LoRaWAN stack)

- **URL:** <https://github.com/Lora-net/LoRaMac-node>
- **License:** BSD-3-Clause / Revised BSD.
- **What we use it for:** the production-grade LoRaWAN end-device stack we point at for the OTAA join, the full channel-hopping, and the ADR control loop that the mini-project simplifies away. When you outgrow the teaching code, this is where you go.

### ChirpStack (open-source LoRaWAN network server)

- **URL:** <https://www.chirpstack.io>
- **What we use it for:** the self-hosted alternative to TTN if you stand up your own gateway and network server. Mentioned in the mini-project's "out of scope" notes.

### Matter device library and the data-model spec

- **URL:** the Matter "Application Cluster Specification" and "Device Library Specification" (with the Core spec, via the CSA).
- **What we use it for:** Homework Problem 5 (pick a non-light device type and identify its mandatory clusters).

### "LoRa and LoRaWAN: A Technical Overview" (Semtech white paper)

- **URL:** Semtech's LoRa developer portal, the technical overview PDFs.
- **What we use it for:** a readable, vendor-but-honest overview of the modulation and the network architecture. 30 minutes; good before Lecture 2.

---

## Hardware — what to buy

- **A Pico** (you have one).
- **An SX1262 module** for your region's band. EU868: Waveshare "SX1262 LoRa Node (HF)" or a RAK3172 breakout. US915: the LF/US variant of the same. ~$12–20. Make sure it is SX126x (not the older SX127x) and matches your regional frequency.
- **(Challenge 1, optional) an nRF52840** — the nRF52840-DK (~$50) or a Nordic/MakerDiary Dongle (~$10). Plus a second nRF52840 for sniffing, or use the simulation path.
- **(Challenge 1, optional) a Thread Border Router** — an existing Apple HomePod mini / Apple TV 4K / recent Google Nest Hub, or a Raspberry Pi + nRF52840 RCP running `ot-br-posix`.

No LoRa gateway purchase is required if you have TTN public coverage. If you do not, the cheapest path is a single-channel gateway (~$30, for development only — single-channel gateways are not spec-compliant and TTN discourages them) or an 8-channel indoor gateway (~$90, e.g. the RAK7268 or a Things Indoor Gateway).

---

## Reading time budget

| Reference                              | Time     | When             |
|----------------------------------------|----------|------------------|
| Lecture 1 (radio landscape)            | 45 min   | Monday morning   |
| Semtech SX1261/2 datasheet §13         | 45 min   | Tuesday morning  |
| LoRaWAN L2 1.0.4 §4 + §4.4             | 45 min   | Wednesday morning |
| RFC 4493 (skim + the test vectors)     | 20 min   | Wednesday        |
| RP002 EU868 channel plan               | 15 min   | Thursday morning |
| Thread 1.3 overview + Matter Core §4/§6 | 60 min   | Friday morning   |
| connectedhomeip lighting-app README    | 20 min   | Friday           |
| **Total**                              | ~4 hours | spread across the week |

Do the readings on the day they support and the lectures and exercises track cleanly. Defer them and you will be guessing at byte order in the LoRaWAN frame on Saturday — measurably slower and far more frustrating.
