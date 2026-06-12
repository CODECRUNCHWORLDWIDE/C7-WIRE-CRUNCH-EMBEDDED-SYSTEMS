# Week 14 — LoRaWAN, Zigbee, Thread, and Matter

> *This is the week you stop asking "how do I bring up this peripheral?" and start asking "which radio is this product even supposed to use?" There are four wireless ecosystems on the table — LoRa/LoRaWAN for kilometres-on-a-battery, Zigbee for the mature local mesh, Thread for the IPv6 low-power mesh, and Matter as the application layer that ties the smart-home half together — and they are not interchangeable. A motorcycle, a cargo ship, and a forklift are all correct answers to different sentences. The load-bearing skill this week is judgement: read a one-paragraph product brief and name the right radio, and the wrong ones, and why. The hands-on half is real — you will transmit an encrypted, MIC-protected LoRaWAN uplink from a Pico + SX1262 to The Things Network and watch it appear in the console — and the conceptual half (Thread + Matter) is taught honestly on the right silicon, not faked on a Pico that has no 802.15.4 radio.*

Welcome to Week 14 of C7. Last week you put a node on Wi-Fi and pushed telemetry through a TLS-secured MQTT broker — one device, one IP network, lots of bandwidth and lots of power. This week zooms out to the radios that win when Wi-Fi loses: when the device must reach kilometres on a coin cell (LoRaWAN), or live in a dense self-healing mesh of hundreds of nodes (Zigbee, Thread), or pair into Apple/Google/Amazon out of the box (Matter). You will build the LoRaWAN half end to end and you will read, reason about, and bring up the Thread/Matter half on the hardware it actually needs.

The week splits cleanly:

- **The decision** (Lecture 1, Exercise 1). A framework for choosing a radio from explicit inputs — range, duty cycle, payload, node count, IP requirement, power budget, ecosystem requirement — encoded as a function you can test your judgement against.
- **LoRa + LoRaWAN, hands-on** (Lecture 2, Exercises 2 & 3, the mini-project, Challenge 2). The SX1262 SPI driver, the LoRaWAN MAC, the AES-CMAC MIC verified against RFC 4493 vectors, and a complete Class-A node that appears in the TTN console.
- **Thread + Matter, conceptual-plus-real** (Lecture 3, Challenge 1). The 802.15.4 mesh, IPv6-over-6LoWPAN, the Matter data model and commissioning flow, and a real `connectedhomeip` lighting-app brought up on an nRF52840 (or the honest Linux + OpenThread simulation).

C7's rule holds: real silicon or honest simulation, never a fake. LoRa is fully hands-on because a $15 module plus the free The Things Network lets you close a real link with no certification. Matter-over-Thread is conceptual-plus-real because it genuinely needs an 802.15.4 radio the RP2040 does not have — so Challenge 1 moves to the nRF52840 and the production SDK rather than pretending a Pico is a Thread node.

> **Note on platform.** Weeks 1–11 are RP2040 + Pico SDK, and the hands-on LoRaWAN work this week stays there: a Pico drives a Semtech SX1262 LoRa module over SPI0. The Thread/Matter half necessarily uses the nRF52840 (built-in 802.15.4 + BLE) and the open-source OpenThread and `connectedhomeip` stacks — the canonical, free, open development path for that ecosystem. Every toolchain this week is free and open.

---

## Learning objectives

By the end of this week, you will be able to:

- **Choose** the right radio for a product from explicit constraints. Given range, duty cycle, payload size, node count, IP requirement, power budget, and smart-home requirement, name LoRaWAN, Zigbee, Thread, Wi-Fi, or BLE Mesh — and recognise the "design conflict" brief that no radio in the set can satisfy (the long-range + big-frequent-payload + battery trap that points at cellular instead). You encode this in `cc_choose_radio()` (Exercise 1).
- **Explain** LoRa modulation. Chirp spread spectrum; spreading factor (SF7–SF12) trading data rate for ~2.5 dB of link budget per step; bandwidth; the below-noise-floor receiver sensitivity that lets LoRa reach kilometres on milliwatts; and the airtime/duty-cycle ceiling (EU868 1% per sub-band) that actually caps LoRaWAN traffic. Cite the Semtech SX1261/2 datasheet and AN1200.13.
- **Drive** a Semtech SX1262 over SPI from the RP2040. The command/response protocol, the BUSY-line discipline (the #1 bug), the §9.2.1 TX setup sequence, the frequency word (`hz / FREQ_STEP`), the PA config, and waiting for TX_DONE on DIO1 (Exercise 2, `sx1262.c`).
- **Build** a LoRaWAN 1.0.x uplink byte by byte. MHDR | DevAddr (little-endian!) | FCtrl | FCnt | FPort | FRMPayload | MIC; the AES-CTR-like FRMPayload encryption with AppSKey; and the 4-byte MIC as AES-128-CMAC with NwkSKey over the B0 block. Verify the CMAC primitive against RFC 4493's test vectors *before* trusting the protocol (Exercise 3, `lorawan.c`).
- **Operate** The Things Network. Register an ABP device, manage the frame counter (and why a reset ABP node silently stops working), write a JavaScript payload decoder, and read RSSI/SNR from the live-data console (mini-project, Challenge 2).
- **Reason** about LoRaWAN device classes. Class A (uplink then two RX windows, the low-power default), Class B (beaconed ping slots), Class C (always-on receiver, mains only) — and why a Class-A device's downlink latency is bounded below by its uplink interval (Lecture 2, Challenge 2).
- **Explain** the 802.15.4 substrate shared by Zigbee and Thread. The 2.4 GHz PHY, the 16-bit Frame Control Field (you parse it in Exercise 1), the ~100 m per-hop range that meshing recovers coverage from but cannot substitute for true range, and why choosing Zigbee vs Thread is never a PHY decision.
- **Distinguish** Zigbee from Thread. Zigbee's coordinator/router/end-device roles and ZCL clusters vs Thread's 6LoWPAN + IPv6 + self-healing mesh with no permanent coordinator; when the mature incumbent (Zigbee) wins and when the IP-native forward path (Thread) wins (Lecture 1, Lecture 3).
- **Explain** Matter as an application layer over Thread or Wi-Fi. The endpoint/cluster/attribute/command data model, the BLE-bootstrap commissioning flow, PASE (SPAKE2+) and CASE (certificate) security, multi-admin fabrics, and the DAC attestation supply chain — and the honest cost: two certification programs plus a per-device crypto provisioning obligation (Lecture 3, Challenge 1).
- **Bring up** a real Matter-over-Thread device. The `connectedhomeip` lighting-app on an nRF52840, an OpenThread Border Router, commissioning from `chip-tool` and a consumer hub, multi-admin, and an 802.15.4 sniff with `pyspinel` (Challenge 1).
- **Ship** a complete LoRaWAN Class-A telemetry node. Pico + SX1262, encrypted MIC-protected uplinks to TTN, duty-cycle accounting, and an offline frame decoder (`cc-ttn-decode.py`) that cross-validates the device's crypto byte-for-byte (the mini-project).

---

## Prerequisites

You have shipped the Week 13 mini-project (the Wi-Fi/TLS/MQTT telemetry node). You can read a datasheet's command table and turn it into a SPI driver — that is the whole of Weeks 7–8. You are comfortable with the AES block cipher conceptually; this week you implement AES-128 and AES-CMAC from FIPS 197 and RFC 4493 and verify them against the standards' own test vectors, so a refresher on AES's round structure (SubBytes/ShiftRows/MixColumns/AddRoundKey) before Thursday will pay off.

You have, on the bench: a Pico, a Semtech **SX1262** LoRa module (Waveshare "SX1262 LoRa Node (HF)" for EU868, or the RAKwireless RAK3172 / a generic SX1262 breakout), jumper wires, and a USB cable. You have a free **The Things Network** account and you have checked the TTN coverage map for a public gateway in your area (or you have your own gateway). For Challenge 1 you have (or can simulate) an **nRF52840** and a Thread Border Router.

You have skimmed: the **Semtech SX1261/2 datasheet** §13 (the command catalogue), the **LoRaWAN L2 1.0.4** spec §4 (frame formats) and §4.4 (the MIC), and **RFC 4493** (AES-CMAC). For the conceptual half, skim the **Matter 1.3 Core Specification** §4 (secure channel) and the **Thread 1.3** overview. Links in `resources.md`.

---

## Topics covered

- **The radio landscape.** LoRa/LoRaWAN, Zigbee, Thread, Matter, Wi-Fi, BLE — what each is, what it costs, and the one-sentence summary of each. The decision framework and the five-rule heuristic. Cite Lecture 1.
- **LoRa modulation.** Chirp spread spectrum, SF and bandwidth, the link-budget-vs-airtime trade, the below-noise-floor sensitivity, and the EU868/US915 channel plans and their duty-cycle/dwell-time limits. Semtech datasheet + RP002.
- **The SX1262 driver.** SPI command framing, BUSY discipline, the TX setup sequence, the frequency word, PA config, IRQ mapping to DIO1, TX_DONE. `sx1262.c`, Exercise 2.
- **The LoRaWAN MAC.** PHYPayload layout, MHDR/FHDR/FPort/FRMPayload/MIC, the little-endian DevAddr trap, the FRMPayload AES-CTR cipher, the AES-CMAC MIC over B0, the frame counter as replay defence. `lorawan.c`, Exercise 3.
- **Device classes and receive windows.** Class A/B/C, RX1/RX2 timing, why Class-A downlink latency tracks the uplink interval. Challenge 2.
- **The Things Network.** ABP vs OTAA, frame-counter management, payload decoders, the console. Mini-project, Challenge 2.
- **802.15.4.** The shared PHY, the Frame Control Field, per-hop range and meshing. Exercise 1, Lecture 1/3.
- **Zigbee vs Thread.** Roles, clusters, IPv6, the single-coordinator vs no-coordinator robustness difference. Lecture 3.
- **Thread.** 6LoWPAN, IPv6/UDP, MLE, the Border Router, OpenThread, sleepy end devices. Lecture 3, Challenge 1.
- **Matter.** The data model, the interaction model, PASE/CASE, fabrics and multi-admin, device attestation, `connectedhomeip`, Matter-over-Thread vs Matter-over-Wi-Fi. Lecture 3, Challenge 1.
- **Certification.** Regulatory + per-ecosystem programs; the doubled burden of Matter-over-Thread and the DAC supply chain. Lecture 1 §4.

---

## Weekly schedule

| Day       | Focus                                                                          | Lectures | Exercises | Challenges | Quiz/Read | Homework | Mini-Project | Bench/Self-Study | Daily Total |
|-----------|--------------------------------------------------------------------------------|---------:|----------:|-----------:|----------:|---------:|-------------:|-----------------:|------------:|
| Monday    | The radio landscape; the decision framework; 802.15.4 FCF                       |   2h     |   1h      |    0h      |   0.5h    |   1h     |     0h       |       0.5h       |     5h      |
| Tuesday   | LoRa modulation; the SX1262 driver; bench bring-up                              |   2h     |   2h      |    0h      |   0.5h    |   1h     |     0h       |       0.5h       |     6h      |
| Wednesday | LoRaWAN MAC; AES-CMAC + MIC; the frame byte by byte                             |   2h     |   2h      |    0.5h    |   0.5h    |   1h     |     1h       |       0.5h       |     7.5h    |
| Thursday  | TTN bring-up; device classes; RX windows; duty cycle                           |   1h     |   1h      |    1h      |   0.5h    |   1h     |     1.5h     |       0.5h       |     7.5h    |
| Friday    | Thread + Matter; commissioning; the conceptual build; end-to-end uplink         |   1h     |   0h      |    2h      |   0.5h    |   1h     |     1h       |       0.5h       |     6h      |
| Saturday  | Mini-project deep work — LoRaWAN node + cc-ttn-decode + TTN console             |   0h     |   0h      |    0h      |   0h      |   0h     |     4h       |       0.5h       |     4.5h    |
| Sunday    | Quiz; review; polish the radio-decision write-up and the LoRaWAN frame notes    |   0h     |   0h      |    0h      |   0.5h    |   0h     |     0h       |       0h         |     0.5h    |
| **Total** |                                                                                | **8h**   | **6h**    |  **3.5h**  |  **3h**   |  **5h**  |   **9h**     |     **3h**       |   **37.5h** |

Self-paced cohorts compress to ~14 h/week. The load-bearing items: Lecture 1 (the decision framework — get this and the rest of the week is bookkeeping), Exercise 3 (the MIC verified against RFC 4493 — every "my node doesn't show up in TTN" bug reduces to this), and the mini-project (the moment a real packet leaves your bench and lands on the internet is the artifact of the week). Challenge 1 (the real Matter-over-Thread build) is the second-most-instructive artifact; commit the post-mortem.

---

## How to navigate this week

| File                                                                                          | What's inside                                                                                  |
|-----------------------------------------------------------------------------------------------|-----------------------------------------------------------------------------------------------|
| [README.md](./README.md)                                                                      | This overview                                                                                  |
| [resources.md](./resources.md)                                                                | Semtech SX126x datasheet, LoRaWAN L2 1.0.4, RP002, RFC 4493, Thread/Matter specs, OpenThread, connectedhomeip, TTN |
| [lecture-notes/01-the-low-power-radio-landscape.md](./lecture-notes/01-the-low-power-radio-landscape.md) | The four ecosystems, the PHY constraints, the decision framework, certification               |
| [lecture-notes/02-lorawan-mac-and-the-sx126x-driver.md](./lecture-notes/02-lorawan-mac-and-the-sx126x-driver.md) | The SX1262 driver, airtime/duty cycle, the LoRaWAN frame and MIC, channel plans, ADR |
| [lecture-notes/03-thread-matter-and-choosing-the-radio.md](./lecture-notes/03-thread-matter-and-choosing-the-radio.md) | Thread, Matter, commissioning, multi-admin, and the worked radio-selection examples |
| [exercises/exercise-01-radio-decision-and-802154.c](./exercises/exercise-01-radio-decision-and-802154.c) | Encode the radio-selection heuristic; parse an 802.15.4 frame control field (host)       |
| [exercises/exercise-02-sx1262-bringup.c](./exercises/exercise-02-sx1262-bringup.c)            | Bring up the SX1262 over SPI and transmit a raw LoRa packet (bench)                            |
| [exercises/exercise-03-lorawan-uplink-mic.c](./exercises/exercise-03-lorawan-uplink-mic.c)    | Build a LoRaWAN uplink and compute its MIC; verify AES-CMAC against RFC 4493 (host)            |
| [exercises/lpwan_common.h](./exercises/lpwan_common.h)                                         | Shared header: SX126x opcodes, LoRaWAN field layouts, 802.15.4 FCF bits, the decision struct   |
| [exercises/SOLUTIONS.md](./exercises/SOLUTIONS.md)                                             | Walkthroughs, expected output, the five canonical SX126x bring-up bugs                         |
| [challenges/challenge-01-matter-over-thread-lighting-app.md](./challenges/challenge-01-matter-over-thread-lighting-app.md) | Bring up a real Matter-over-Thread light on nRF52840; multi-admin; sniff 15.4   |
| [challenges/challenge-02-the-things-network-decoder-and-downlink.md](./challenges/challenge-02-the-things-network-decoder-and-downlink.md) | A TTN payload decoder, a downlink, and a Class-A round trip       |
| [quiz.md](./quiz.md)                                                                           | 10 questions; radio selection, LoRaWAN frame, MIC, classes, Thread/Matter                      |
| [homework.md](./homework.md)                                                                   | Six problems with a grading rubric                                                             |
| [mini-project/README.md](./mini-project/README.md)                                            | The week's capstone: a LoRaWAN Class-A telemetry node to TTN                                   |
| [mini-project/main.c](./mini-project/main.c)                                                  | The node: sensor read, uplink build, TX loop, duty-cycle accounting                           |
| [mini-project/lorawan.c](./mini-project/lorawan.c) / [lorawan.h](./mini-project/lorawan.h)    | The LoRaWAN MAC: AES, CMAC, FRMPayload cipher, build + verify/decrypt                          |
| [mini-project/sx1262.c](./mini-project/sx1262.c) / [sx1262.h](./mini-project/sx1262.h)        | The productised SX1262 driver                                                                  |
| [mini-project/cc-ttn-decode.py](./mini-project/cc-ttn-decode.py)                              | Host-side offline LoRaWAN frame decoder (verify MIC + decrypt)                                 |

---

## Reading order if you are short on time

1. **README.md** — this document; 20 minutes.
2. **lecture-notes/01-the-low-power-radio-landscape.md** — the decision framework; 45 minutes.
3. **lecture-notes/02-lorawan-mac-and-the-sx126x-driver.md** — the driver and the MAC; 45 minutes.
4. **lecture-notes/03-thread-matter-and-choosing-the-radio.md** — Thread, Matter, the worked examples; 45 minutes.
5. **exercises/SOLUTIONS.md** — read after attempting; 30 minutes.
6. **mini-project/README.md** — the capstone brief; 20 minutes.
7. **challenges/challenge-01-matter-over-thread-lighting-app.md** — read on Friday before the conceptual build; 15 minutes.

Core reading is ~3.5 hours; the exercises and mini-project are ~12 hours of bench time. Plan accordingly.

---

## What is intentionally out of scope this week

- **The LoRaWAN OTAA join.** The mini-project uses ABP so an uplink works without the JoinRequest/JoinAccept state machine. OTAA is the production-correct choice (rotating session keys, dynamic channel plan, no frame-counter persistence headache) and Homework Problem 4 designs it; we point at the Semtech LoRaMAC-node reference for the full implementation.
- **Multi-channel hopping and the full ADR control loop.** We pin one EU868 channel and a fixed SF. A production stack rotates channels pseudo-randomly per uplink and runs the ADR loop from `LinkADRReq` downlinks; Lecture 2 explains both and leaves them as extensions.
- **A from-scratch Zigbee or Thread stack.** Nobody writes these by hand; you bring up OpenThread and `connectedhomeip`, which are the certified production code. Challenge 1 configures them on real silicon. Writing a 6LoWPAN compressor or an MLE state machine is a multi-week project, not a week-14 exercise.
- **Matter on the Pico.** The RP2040 has no 802.15.4 radio. Faking a Thread node on a Pico with a bolted-on transceiver would teach a wiring exercise, not Thread. Challenge 1 uses the nRF52840 (or an honest Linux + OpenThread simulation), and we say so plainly.
- **Cellular LPWAN (NB-IoT, LTE-M) and Sigfox.** Out of the four-radio set this week, but a senior engineer must know they exist — Homework Problem 6 reaches outside the week deliberately to put them on the map.

---

## A note on judgement

Most of C7 rewards depth — knowing one peripheral to the register. This week rewards *breadth and judgement*: knowing four ecosystems well enough to choose, fast, under a one-paragraph brief, and to recognise the brief that is internally impossible before anyone has soldered a prototype. The single most valuable output of `cc_choose_radio()` is `RADIO_NONE` — the function refusing to recommend a radio that physically cannot serve the product. Train that reflex. The most expensive mistakes in IoT are not bad drivers; they are good drivers for the wrong radio, discovered six months and one tooling-up later. Pick right first.
