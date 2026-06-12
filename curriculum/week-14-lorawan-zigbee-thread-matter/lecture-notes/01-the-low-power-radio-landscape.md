# Lecture 1 — The Low-Power Radio Landscape: LoRaWAN, Zigbee, Thread, Matter

> *There is no "best" wireless radio for IoT, the same way there is no "best" vehicle. A motorcycle, a cargo ship, and a forklift are all correct answers to different sentences. The mistake junior engineers make is picking the radio they already know — usually whatever was in the last project — and then bending the product around it. The mistake senior engineers make is over-engineering: reaching for Thread and Matter because they are the shiny new thing, when the product is a battery soil sensor that talks once an hour to a gateway 8 km away and a $5 LoRa module would have closed the link with a year of battery to spare. This lecture is a decision framework. By the end of it you should be able to look at a one-paragraph product brief and name the right radio — and, just as important, name the radios that are wrong and say why.*

This week is different from most of C7. Weeks 7 through 11 were "here is a peripheral, here is its datasheet, bring it up." This week is "here are four wireless ecosystems, each with its own physical layer, MAC, network stack, security model, and certification regime, and your job is to *choose*." The bring-up is real — you will transmit a LoRaWAN uplink to The Things Network from a Pico-plus-SX1262 on the bench, and you will stand up a conceptual Matter-over-Thread device — but the load-bearing skill this week is judgement, not register-poking.

The four technologies, in one sentence each:

- **LoRa / LoRaWAN** — kilometres of range, tens of bytes per message, minutes-to-hours between messages, years on a coin cell. A star-of-stars topology: end-devices, gateways, a network server. The radio you pick when *distance on a battery* is the dominant constraint.
- **Zigbee** — a mature 2.4 GHz mesh for dense, mostly-mains-powered local networks (lighting, building automation). 802.15.4 PHY, Zigbee PRO network layer, application "clusters." The radio you pick when you need a *local mesh of many nodes* and you do not need IP and you do not need a consumer smart-home ecosystem.
- **Thread** — also 802.15.4, also a mesh, but the network layer is 6LoWPAN + IPv6. Every Thread node is an IPv6 host; the mesh is self-healing and routerless from the app's point of view. The radio you pick when you want a *low-power mesh that speaks IP* and integrates with the modern smart-home stack.
- **Matter** — *not a radio.* Matter is an application layer (a data model, a commissioning flow, a security model) that runs *over* Thread or over Wi-Fi or over Ethernet. It is the CSA's (Connectivity Standards Alliance, formerly the Zigbee Alliance) attempt to end the "which ecosystem does this gadget support?" fragmentation. When people say "Matter device" for a low-power sensor, they almost always mean "Matter-over-Thread."

Hold those four sentences in your head. The rest of this lecture justifies them.

## 1. The physical layer is the thing that constrains you

Every other choice — MAC, mesh, IP, app layer — is software you can change. The physical layer is silicon and physics, and it sets the ceiling on range, data rate, and power. So we start there.

### 1.1 LoRa modulation — chirp spread spectrum

LoRa (the modulation, owned by Semtech, distinct from LoRaWAN the MAC) is a chirp-spread-spectrum scheme. A "chirp" is a tone that sweeps linearly across the channel bandwidth; a symbol is a chirp that starts at a frequency offset encoding `SF` bits. Two parameters dominate:

- **Spreading factor (SF7–SF12).** Each step up in SF doubles the chirp duration, halving the data rate but adding ~2.5 dB of processing gain. SF7 at 125 kHz bandwidth is ~5.5 kbit/s and reaches a kilometre or two in a town; SF12 at 125 kHz is ~250 bit/s and reaches 10–15 km line-of-sight, or a few km through buildings. This is the single most important LoRa knob and it is the one LoRaWAN's adaptive data rate (ADR) turns automatically.
- **Bandwidth (125 / 250 / 500 kHz).** Wider bandwidth means more data rate and worse sensitivity. EU868 mostly uses 125 kHz; US915 uses 125 kHz for uplink and 500 kHz for downlink.

The headline number that makes LoRa special: **receiver sensitivity below the noise floor.** A LoRa receiver at SF12/BW125 can demodulate a signal at roughly −137 dBm — below thermal noise — because the processing gain of the spread spectrum pulls the signal out. No 802.15.4 radio comes close. That sensitivity is the entire reason LoRa reaches kilometres on milliwatts. The cost is airtime: an SF12 packet of 20 bytes takes over a second on the air, and in the EU's 868 MHz band you are limited to a **1% duty cycle** per channel by regulation (ETSI EN 300 220), so a node transmitting SF12 can legally talk for ~36 seconds per hour. That duty-cycle ceiling, not the radio, is what caps LoRaWAN's traffic.

The transceiver we use is the **Semtech SX1262** (datasheet rev. 2.1). It is the current-generation LoRa chip: +22 dBm output, −148 dBm sensitivity floor, sub-GHz (150–960 MHz so it covers EU868 and US915), driven over SPI with a command/response protocol. Its predecessor, the SX1276/SX1278 (the "SX127x" family), is still everywhere in cheap modules and tutorials; it works but it is register-mapped rather than command-mapped and burns more receive current. For new designs in 2026, pick SX126x. The Pico has no radio; we hang an SX1262 module off SPI0 and drive it (Exercise 2 and the mini-project).

### 1.2 The 802.15.4 PHY — the shared substrate of Zigbee and Thread

Zigbee and Thread both ride **IEEE 802.15.4** (the 2.4 GHz, O-QPSK, 250 kbit/s variant; 802.15.4-2020 §10). Sixteen channels, numbered 11–26, each 2 MHz wide, spaced 5 MHz apart across the 2.4 GHz ISM band. The same chip — an nRF52840, an EFR32, a CC1352 — can run either stack; the silicon does not care. What differs is everything above the MAC.

Key 802.15.4 facts that bound both Zigbee and Thread:

- **Range per hop** is ~10–100 m indoors, a few hundred metres outdoors line-of-sight. This is two-to-three orders of magnitude less than LoRa. Both stacks recover *coverage* through *meshing* — packets hop node-to-node — but a mesh is not a substitute for range when there are no nodes in between. You cannot mesh across an empty field.
- **Data rate** is 250 kbit/s on the air, realistically ~50–100 kbit/s of application throughput after MAC overhead, ACKs, and CSMA backoff. That is plenty for sensor telemetry and lighting control and far more than LoRaWAN, but it is not Wi-Fi.
- **MAC frame** starts with a 16-bit Frame Control Field (you parse it in Exercise 1; 802.15.4-2020 §7.2.2.1). The low three bits are the frame type (Beacon, Data, Ack, MAC Command). Both Zigbee and Thread frames are 802.15.4 Data frames at the MAC layer; the difference is the payload they carry.

The takeaway: choosing between Zigbee and Thread is **never** a PHY decision. It is a network-and-ecosystem decision, because they share a PHY.

### 1.3 Wi-Fi and BLE, for contrast

We covered Wi-Fi (Week 13, ESP32 + lwIP + MQTT) and BLE/BLE-Mesh (Week 13 in the alternate track) already, but they belong on the same axes:

- **Wi-Fi** — high throughput, IP-native, ubiquitous infrastructure, but power-hungry (an associated Wi-Fi node is hard to run for years on a battery; the radio's receive current and the association overhead dominate). The right radio when there is mains power and an existing AP and you need bandwidth. Matter runs over Wi-Fi for mains-powered, high-bandwidth devices (cameras, some plugs).
- **BLE** — short range (10–30 m), phone-native, excellent for commissioning and wearables, modest mesh via BLE Mesh. The right radio when a phone is the other end, or for the *commissioning* leg of a Thread/Matter device (Matter uses BLE to bootstrap Thread credentials — more on this in Lecture 3).

## 2. The MAC and network layers

### 2.1 LoRaWAN — star-of-stars, three identities, two root keys

LoRaWAN (the LoRa Alliance's MAC, spec "LoRaWAN L2 1.0.4" or the newer 1.1) turns point-to-point LoRa into a network. The topology is **star-of-stars**:

- **End-devices** broadcast uplinks. They do not associate with a specific gateway; they just transmit, and any gateway in range hears them.
- **Gateways** are dumb forwarders. A gateway receives a LoRa packet, wraps it in UDP with metadata (RSSI, SNR, timestamp), and forwards it to a network server over the backhaul (Ethernet/cellular). A gateway has no per-device state.
- **The network server** deduplicates (multiple gateways heard the same uplink), checks the MIC, decrypts, and routes the application payload to the application server. It also schedules downlinks.

An end-device has three identities and a set of keys:

- **DevEUI** — a globally unique 64-bit device identifier (like a MAC address), burned in at manufacture.
- **JoinEUI** (formerly AppEUI) — identifies the join server.
- **DevAddr** — a 32-bit *network* address, assigned during the join. This is what appears in every uplink's frame header.
- **AppKey** (OTAA) — the single root key. During the over-the-air-activation join, the device and join server derive two session keys from it: **NwkSKey** (network session key, used for the MIC) and **AppSKey** (application session key, used to encrypt the payload). LoRaWAN 1.1 splits the network key further, but 1.0.x — what TTN and most of the world still run — has these two.

There are two activation modes:

- **OTAA (over-the-air activation)** — the device sends a JoinRequest, the server replies with a JoinAccept, session keys are derived. This is the recommended mode: keys rotate per session, the device picks up the network's channel plan dynamically.
- **ABP (activation by personalization)** — the DevAddr and both session keys are hard-coded into the device at manufacture. Simpler to bring up (no join handshake), worse security (keys never rotate), and a frame-counter management headache. We use **ABP in the mini-project** because it lets us send an uplink without implementing the join state machine — the right pedagogical simplification, the wrong production choice.

**LoRaWAN device classes** (the spec's §4.2.1 MType plus the class behaviour):

- **Class A** — the baseline, the lowest power, *mandatory for all devices*. The device transmits an uplink whenever it wants, then opens two short receive windows (RX1, RX2) a fixed delay later. Outside those windows the radio is off. The network can only send a downlink in a receive window, which means downlinks are latency-bound by how often the device uplinks. A Class A device that uplinks once an hour can only receive a command once an hour. This is the right class for almost every battery sensor.
- **Class B** — adds scheduled receive "ping slots" synchronised to a gateway beacon, so the network can reach the device on a schedule (say every 2 seconds) without the device having to uplink first. Costs more power (the beacon sync) but bounds downlink latency. Used for things that need occasional commands — actuators, valves.
- **Class C** — the receiver is *always on* except while transmitting. Lowest downlink latency, highest power; only viable on mains power. Used for mains-powered actuators where you need to send a command immediately.

We build a **Class A uplink** in the mini-project. Class A is 95% of LoRaWAN deployments.

### 2.2 The MIC — why your uplink silently vanishes

Every LoRaWAN uplink ends in a 4-byte **Message Integrity Code** (MIC), an AES-128-CMAC truncated to 4 bytes, keyed by NwkSKey, computed over a "B0" block prepended to the message (spec §4.4). The B0 block folds in the direction (uplink/downlink), the DevAddr, and the frame counter — so the MIC is unique per frame even if the payload repeats. You implement this in Exercise 3 against the RFC 4493 AES-CMAC test vectors.

The reason to belabour the MIC: **a wrong MIC produces no error.** The network server checks the MIC, finds it invalid, and drops the frame. No NACK, no downlink, nothing in the application server. The single most common "my LoRaWAN node doesn't work" bug is a MIC computed over the wrong bytes, or with the keys in the wrong byte order (LoRaWAN is little-endian on the wire for DevAddr and the EUIs, which trips up everyone at least once), or with a stale frame counter. The frame counter, by the way, is the replay defence: the server tracks the highest FCnt it has seen and rejects any frame with an FCnt at or below it. Reset your device without persisting FCnt and the server rejects everything until the counter catches up — another classic ABP footgun.

### 2.3 Zigbee — the application-layer incumbent

Zigbee PRO (the 2015 / R23 stacks) sits on 802.15.4 and adds:

- A **network layer** with mesh routing (AODV-derived), a 16-bit short address per node, and three node roles: **coordinator** (one per network, forms the PAN), **router** (mains-powered, relays, always on), and **end device** (can sleep, talks only to its parent router).
- An **application layer** organised around **clusters** — standardised attribute/command sets. The "On/Off cluster," the "Level Control cluster," the "Color Control cluster," etc. A Zigbee light bulb implements the On/Off and Level clusters; a switch implements the client side. The Zigbee Cluster Library (ZCL) is the catalogue.
- **Security** via a network key (shared by all nodes) plus optional link keys. The Trust Center (usually the coordinator) distributes keys.

Zigbee's strengths: maturity (two decades of deployments), a vast ecosystem of certified lighting and HVAC products, low cost, mesh self-healing. Its historical weakness: **interoperability between vendors was never as good as the marketing claimed** — "Zigbee certified" did not guarantee two vendors' devices worked together, because profiles and clusters were implemented inconsistently. That fragmentation is precisely the problem Matter was created to solve.

### 2.4 Thread — 802.15.4 with IPv6

Thread (spec maintained by the Thread Group; current 1.3 / 1.4) is the other 802.15.4 mesh. The defining difference from Zigbee: **every Thread node is an IPv6 host.** The stack is:

- **6LoWPAN** (RFC 6282) — header compression that squeezes IPv6 into 802.15.4's 127-byte frames.
- **IPv6 + UDP** — Thread devices speak real IPv6. They have link-local and mesh-local addresses; routable global addresses come via a Border Router.
- **MLE (Mesh Link Establishment)** and a distance-vector routing protocol — the mesh self-organises. Roles: **Router**, **Router-Eligible End Device (REED)**, **End Device (FED/MED)**, **Sleepy End Device (SED)**. Crucially, **there is no single coordinator whose failure kills the network** — Thread elects a Leader dynamically and the role can migrate. This is a real robustness advantage over Zigbee's single coordinator.
- **A Thread Border Router (TBR)** bridges the Thread mesh to your home IP network (Wi-Fi/Ethernet), giving Thread devices a path to the internet and to your phone. Apple's HomePod, many Google Nest devices, and Amazon Echo (4th gen) are Thread Border Routers — which is *why* Thread suddenly matters: the border routers are already in millions of homes.

Thread does **not** define an application layer. It gives you IPv6 and UDP and stops. That is by design: the application layer is **Matter's** job. Thread is plumbing; Matter is what flows through it.

### 2.5 Matter — the application layer that ends the ecosystem wars

Matter (CSA, current 1.3 / 1.4) is an application protocol, not a radio. It runs over Thread, Wi-Fi, or Ethernet. It defines:

- A **data model** — devices are collections of *endpoints*, each endpoint has *clusters* (yes, the Zigbee word, deliberately — Matter inherited ZCL's structure), each cluster has *attributes* and *commands*. An On/Off Light device type has an On/Off cluster. This is conceptually close to Zigbee's model, modernised and made mandatory-to-conform.
- An **interaction model** — read/write attributes, invoke commands, subscribe to attribute changes, over a secure session.
- A **secure channel** — PASE (Password-Authenticated Session Establishment) for commissioning, CASE (Certificate-Authenticated Session Establishment) for operation, both built on standard primitives (SPAKE2+, X.509 certs, AES-CCM).
- A **commissioning flow** — the magic that made Matter a consumer story. You scan a QR code (or NFC), your phone connects to the new device over **BLE**, authenticates with the code, hands the device your Thread network credentials (or Wi-Fi credentials), and the device joins. The same flow works for any Matter device from any vendor into any ecosystem (Apple Home, Google Home, Amazon Alexa, Samsung SmartThings) — **multi-admin**, meaning one physical device can be controlled by several ecosystems at once.

Matter is built on the open-source **`connectedhomeip`** SDK (Apache-2.0, on GitHub). It is a large, complex codebase — Matter is genuinely heavy, far heavier than a Zigbee cluster implementation — and that weight is the honest cost of the interoperability it buys.

What Matter does *not* solve: it does not magically make a 2019 Zigbee bulb a Matter device (you need a bridge); it does not eliminate cloud dependencies for every feature (some vendors still phone home); it does not cover every device type yet (cameras, robot vacuums, and energy devices arrived in later revisions); and it added real engineering complexity to what used to be a 200-line Zigbee endpoint. Matter is an industry consolidation moment, and like all consolidation it trades simplicity for reach.

## 3. The decision framework

Here is the table to internalise. The columns are the constraints; the rows are the radios; the cells are "good / acceptable / no."

| Constraint                         | LoRaWAN        | Zigbee         | Thread         | Wi-Fi          | BLE Mesh       |
|------------------------------------|----------------|----------------|----------------|----------------|----------------|
| Range (single hop)                 | 2–15 km        | 10–100 m       | 10–100 m       | 30–100 m       | 10–30 m        |
| Data rate (app)                    | 0.25–5 kbit/s  | ~50 kbit/s     | ~50 kbit/s     | 10+ Mbit/s     | ~10 kbit/s     |
| Years on a coin cell               | yes            | end-devices    | sleepy ED      | no             | low-power node |
| Speaks IP                          | no             | no             | yes (IPv6)     | yes            | no             |
| Smart-home ecosystem               | no             | partial        | yes (Matter)   | yes (Matter)   | vendor mesh    |
| Mesh                               | no (star)      | yes            | yes            | no             | yes            |
| Max payload per msg                | ~51–242 B      | ~80 B          | full IPv6 MTU  | huge           | ~11 B/segment  |
| Typical duty/latency               | minutes–hours  | sub-second     | sub-second     | sub-second     | sub-second     |

And the heuristic, in order — take the first row that matches:

1. **Need kilometres of range on a battery, talking infrequently?** → **LoRaWAN.** Nothing else closes the link. (Soil sensors, asset trackers, smart meters, parking sensors, environmental monitoring.)
2. **Need to integrate with consumer smart-home hubs (Apple/Google/Amazon) and/or be IP-addressable, low power?** → **Thread + Matter.** (Light bulbs, contact sensors, thermostats, plugs.)
3. **Need a dense local mesh of many mostly-mains nodes, no IP, no consumer ecosystem, and you value a mature/cheap part?** → **Zigbee.** (Commercial lighting retrofits, building automation, industrial sensing networks already on Zigbee.)
4. **Need bandwidth and there is mains power and Wi-Fi?** → **Wi-Fi** (with Matter if it is a smart-home product). (Cameras, video doorbells, some plugs.)
5. **Phone-proximity interaction, few nodes, short range?** → **BLE / BLE Mesh.** (Wearables, beacons, point-of-sale, commissioning.)

You will run this exact logic in Exercise 1 — it is encoded in `cc_choose_radio()`. Four product profiles go in; LoRaWAN, Thread, Zigbee, and "design conflict" come out. The "design conflict" case (long range + big frequent payload + battery) is the most instructive: it is the brief that *cannot be satisfied by any radio*, and recognising that early — before you have built a prototype — is worth more than any amount of register-level cleverness.

## 4. The energy budget — where "years on a coin cell" comes from

"Battery life" is the constraint that sells low-power radios, and it is worth doing the arithmetic instead of trusting the marketing. A coin cell (CR2032) holds roughly 220 mAh. A AA alkaline holds ~2500 mAh; a AA lithium (Energizer L91) ~3000 mAh. Battery life is total capacity divided by *average* current, and average current is dominated by two things: how much current the radio draws while transmitting/receiving, and how long it spends doing it versus sleeping.

Work a LoRaWAN node. An SX1262 transmitting at +14 dBm draws ~45 mA; at +22 dBm, ~118 mA. A 13-byte SF7 uplink is ~46 ms of TX. So one uplink at +14 dBm costs `45 mA × 0.046 s = 2.07 mC ≈ 0.575 µAh`. The MCU wake, sensor read, and crypto add maybe another `10 mA × 50 ms ≈ 0.14 µAh`. Sleep current for an RP2040 in deep sleep with the SX1262 in cold sleep is ~10 µA (the RP2040's own sleep current is the dominant term here — a dedicated low-power MCU like an STM32L0 gets this under 2 µA, which is why production LoRaWAN nodes do not use an RP2040). Over a 15-minute interval, sleep costs `10 µA × 0.25 h = 2.5 µAh`, dwarfing the ~0.7 µAh of the uplink itself.

So at one uplink per 15 minutes, the node burns ~3.2 µAh per cycle, ~12.8 µAh/hour, ~0.31 mAh/day. A 3000 mAh AA-lithium pair lasts `3000 / 0.31 ≈ 9700 days ≈ 26 years` on paper — except self-discharge (a few percent per year) and the MCU sleep current become the real limit, and you would size for ~5–10 years to be honest. The lesson: at low duty cycles, **sleep current dominates, not transmit current**, which is why the choice of MCU matters as much as the choice of radio, and why the RP2040 — a wonderful teaching part — is the wrong production part for a multi-year battery node. We use it here because the *radio* concepts transfer; in a product you would pair the SX1262 with an ultra-low-power MCU.

The same arithmetic explains why Wi-Fi cannot do multi-year battery: an associated Wi-Fi node's receive current is tens of milliamps *continuously* (it must stay associated to receive), and even with aggressive power-save modes the average current is one to two orders of magnitude above a sleepy LoRaWAN or Thread node. Thread's sleepy-end-device mode wins here precisely because the radio is off between polls — the same trick LoRaWAN's Class A uses.

## 5. The wider LPWAN landscape — Sigfox, NB-IoT, LTE-M

LoRaWAN is one of several Low-Power Wide-Area Networks, and a senior engineer must know its neighbours even though we only build LoRaWAN this week:

- **Sigfox** — an ultra-narrowband LPWAN with a *managed network* (you pay Sigfox/its operators per device per year). Tiny payloads (12 bytes uplink, 8 bytes downlink), very few messages per day (140 uplinks/day in the EU regulatory cap), but extreme range and the lowest device cost. The catch: there is one network operator, coverage is patchy and shrinking in some regions, and the business model (recurring per-device fees, single vendor) makes many engineers nervous. Use it only where its coverage is solid and the per-device economics work.
- **NB-IoT (Narrowband IoT)** — a cellular LPWAN (3GPP) running in licensed spectrum on existing carrier infrastructure. Better building penetration than LoRaWAN (licensed spectrum, carrier-grade towers), no gateway to deploy (the carrier's network is the gateway), but a SIM and a data plan per device and higher device cost and power than LoRaWAN. The right call for a basement water meter where penetration matters and a carrier already covers the site (Homework Problem 6 explores exactly this).
- **LTE-M (Cat-M1)** — also cellular, higher bandwidth than NB-IoT, supports mobility and voice, used for asset trackers and wearables that need more than NB-IoT's trickle. More power than NB-IoT.

The decision among LPWANs comes down to: do you want to *own* the network (LoRaWAN — deploy your own gateways, no recurring per-device fees, unlicensed spectrum with duty-cycle limits) or *rent* it (NB-IoT/LTE-M/Sigfox — carrier or operator owns it, recurring fees, licensed spectrum, better penetration, no infrastructure to deploy)? For a campus, a farm, or a factory you control, LoRaWAN's "own it" model usually wins. For devices scattered across a country with no infrastructure of your own, cellular's "rent it" model wins despite the recurring cost. This own-vs-rent axis is the one that does not show up in the radio spec sheets and matters most in the budget.

## 6. Certification — the thing nobody warns you about

A radio choice is also a certification choice, and certification is months and money:

- **Regulatory** (everywhere): FCC Part 15 (US), ETSI EN 300 220 (EU sub-GHz) / EN 300 328 (EU 2.4 GHz), plus regional equivalents. This applies to *any* radio you ship; it is about emissions, not protocol. Budget for a test-lab pass.
- **LoRaWAN Certified** (LoRa Alliance) — optional but expected for devices that claim interoperability with public networks. Tests the MAC conformance.
- **Zigbee Certified** (CSA) — tests cluster conformance and interop.
- **Thread Certified** (Thread Group) — required to use the Thread logo and to be a "good citizen" on a mesh.
- **Matter Certified** (CSA) — required to use the Matter logo and to appear in Apple/Google/Amazon ecosystems with the official badge. This is the expensive one: it layers on top of the transport certification (so a Matter-over-Thread device needs Thread certification *and* Matter certification) and involves the CSA's Distributed Compliance Ledger (DCL), per-device attestation certificates (DACs) signed by a CSA-approved PAA, and test-house conformance runs.

The point for this lecture: when you choose Matter-over-Thread, you are signing up for **two** certification programs plus regulatory, and a per-device cryptographic attestation supply chain. That is the real cost behind "just works with Apple Home." It is worth it for a consumer product that lives or dies on ecosystem support; it is absurd overhead for an internal industrial sensor that talks to your own gateway. Match the certification burden to the product.

## 5. What we build this week, and why these choices

- **Exercise 2 + mini-project: a real LoRaWAN Class-A uplink** on Pico + SX1262 to The Things Network. This is hands-on because LoRaWAN is the one radio in this set you can fully bring up on the bench with a $15 module, a free network server (TTN), and no certification — and because the SX126x SPI command protocol is exactly the kind of datasheet-driven driver work C7 trains.
- **Lecture 3 + Challenge 1: a conceptual Matter-over-Thread build.** Matter-over-Thread genuinely needs an 802.15.4 radio the Pico does not have (you would use an nRF52840 + the `connectedhomeip` SDK + an OpenThread Border Router). Rather than fake it on the wrong silicon, we treat it as a *conceptual* build: we read the commissioning flow, the data model, and the security handshake; we reason about what the code does and what it costs; and Challenge 1 walks the `connectedhomeip` "lighting-app" example on the real target so you see the genuine article. C7's rule is "real silicon or honest simulation, never a fake" — so LoRa is hands-on and Matter is conceptual-plus-read-the-real-code, and we say so plainly.

## 7. The constraints, one at a time

The decision table compresses seven constraints into a grid. It helps to take each constraint on its own and ask "which radios does this one *eliminate*?" — because the fastest way to a decision is by elimination, not by scoring.

**Range.**
- Under ~30 m: everything is in play, including BLE.
- 30–300 m: the meshes (Zigbee, Thread) and Wi-Fi; LoRaWAN is overkill but works.
- 300 m – 1 km: meshes only if you have intermediate nodes to hop through; otherwise LoRaWAN.
- Over 1 km: LoRaWAN or cellular. The meshes are out unless the space is densely populated with nodes the whole way.

**Power source.**
- Mains everywhere: any radio; Wi-Fi and Class-C LoRaWAN become viable.
- Battery, rechargeable, days between charges: BLE, Wi-Fi with aggressive sleep, Thread routers.
- Battery, multi-year, coin cell or primary AA: LoRaWAN Class A, Thread sleepy end device, Zigbee end device. Wi-Fi is eliminated.

**Reporting frequency / latency.**
- Sub-second command latency needed: meshes or Wi-Fi (always-on or near-it). LoRaWAN Class A is eliminated (downlink only after an uplink).
- Seconds-to-minutes acceptable: all radios.
- Minutes-to-hours, sparse: LoRaWAN's sweet spot; the duty cycle is no constraint here.

**Payload size.**
- A few bytes to tens of bytes: any radio.
- Hundreds of bytes occasionally: meshes, Wi-Fi; LoRaWAN only if infrequent (duty cycle).
- Kilobytes or streaming: Wi-Fi or cellular. Everything low-power is eliminated.

**Node count / density.**
- A handful: any radio.
- Tens to hundreds in one space: the meshes (Zigbee, Thread) shine; this is what they are for.
- Thousands over a wide area: LoRaWAN (star scales to thousands per gateway) or cellular.

**IP / ecosystem requirement.**
- Must be internet-addressable (IPv6): Thread or Wi-Fi.
- Must pair with Apple/Google/Amazon: Matter, so Thread or Wi-Fi underneath.
- Neither: Zigbee and LoRaWAN are back in play.

Run a brief through these six filters and usually only one or two radios survive. The grid in §3 is the same logic pre-computed; doing it by elimination builds the intuition the function `cc_choose_radio()` encodes.

## 8. A glossary you will need all week

The acronyms come thick this week. Keep this list open:

- **LoRa** — the chirp-spread-spectrum *modulation*, owned by Semtech. The physical layer.
- **LoRaWAN** — the *MAC and network protocol* (LoRa Alliance) that runs on LoRa modulation. Star-of-stars topology.
- **SX126x** — the Semtech transceiver family (SX1261, SX1262, SX1268) we drive over SPI. The current generation; the older SX127x is register-mapped and higher-current.
- **TTN** — The Things Network, a free, community, public LoRaWAN network server with public gateway coverage.
- **DevEUI / JoinEUI / DevAddr** — the device's globally unique ID / the join server's ID / the 32-bit network address.
- **AppKey / NwkSKey / AppSKey** — the OTAA root key / the network session key (MIC) / the application session key (payload encryption).
- **OTAA / ABP** — over-the-air activation (join handshake, rotating keys) / activation by personalization (hard-coded keys).
- **MIC** — Message Integrity Code, the 4-byte AES-CMAC tag that gates every frame.
- **SF / BW / CR** — spreading factor / bandwidth / coding rate, the three LoRa modulation knobs.
- **ADR** — Adaptive Data Rate, the network-driven control loop that tunes a node's SF and power.
- **802.15.4** — the IEEE PHY/MAC shared by Zigbee and Thread (2.4 GHz, 250 kbit/s).
- **6LoWPAN** — IPv6 header compression that fits IPv6 into 802.15.4's 127-byte frames.
- **Thread** — the 802.15.4 + 6LoWPAN + IPv6 self-healing mesh.
- **TBR** — Thread Border Router, the bridge between a Thread mesh and the home IP network.
- **OpenThread** — Google's open-source Thread stack, the production reference.
- **Matter** — the application layer (data model + commissioning + security) over Thread / Wi-Fi / Ethernet.
- **CSA** — Connectivity Standards Alliance (formerly the Zigbee Alliance), Matter's steward.
- **PASE / CASE** — the password-authenticated (commissioning) and certificate-authenticated (operation) session-establishment protocols in Matter.
- **DAC / PAA / DCL** — Device Attestation Certificate / Product Attestation Authority / Distributed Compliance Ledger, Matter's anti-counterfeiting chain.
- **connectedhomeip** — the open-source Matter SDK.
- **ZCL** — Zigbee Cluster Library, the cluster/attribute/command model Matter inherited.

## 8.5 The five anti-patterns that sink IoT radio choices

Senior engineers have watched these mistakes wreck projects. Recognise them:

**1. "We'll use what we used last time."** The last project's radio is a fact about the last project, not this one. A team that shipped a Zigbee lighting product reaches for Zigbee on a long-range agricultural sensor and discovers, six months in, that no mesh reaches across a field. The fix is to start every product from the constraints, not from the parts bin.

**2. "Mesh will solve the range problem."** Meshing recovers *coverage* in a populated space; it cannot manufacture *range* across empty space. There must be nodes to hop through. A mesh across a parking lot with three nodes is three nodes that cannot hear each other. If the link distance exceeds one hop and there are no intermediate nodes, the mesh is irrelevant — you need a long-range radio.

**3. "We'll just turn the power up."** Regulatory power limits are hard ceilings (FCC/ETSI), and even at the limit, doubling power buys 3 dB — a small fraction of the 37 dB gap between a mesh radio and LoRa. Power is not the knob; sensitivity and modulation are. You cannot out-power a fundamental link-budget deficit.

**4. "Matter because it's the future."** Matter is the right answer for consumer smart-home products that must pair with the big ecosystems. It is expensive over-engineering for an internal industrial sensor that talks to your own gateway — two certification programs, a per-device attestation supply chain, and a heavyweight SDK, all to solve a problem (multi-vendor consumer interop) the product does not have. Choose Matter for its actual benefit, not its novelty.

**5. "We'll figure out the network later."** The own-vs-rent decision (your gateways vs a carrier/operator) shapes the recurring-cost line of the business case more than any technical parameter, and it is hard to reverse after deployment. A fleet built on a rented network that doubles its per-device fee is a fleet with a margin problem and no easy exit. Decide the network-ownership model when you decide the radio.

The thread through all five: the expensive IoT mistakes are not bad firmware, they are good firmware for the wrong radio or the wrong network model, discovered late. The cheapest possible time to catch them is at the one-paragraph-brief stage — which is exactly the skill this week trains.

## 8.7 Three more briefs, decided fast

Practice the elimination reflex on three more one-liners. For each, the surviving radio and the constraint that did the eliminating:

**"A pet tracker on a collar, nationwide, reports location every 5 minutes, recharged weekly."**
- Range: nationwide → eliminates the meshes and Wi-Fi.
- Mobility: it moves → eliminates the meshes again, confirms a star/cellular.
- Power: recharged weekly, not multi-year → cellular's higher current is acceptable.
- Verdict: **cellular (LTE-M)**, with **LoRaWAN on a public/roaming network** as a lower-power, lower-data alternative if location-every-5-minutes at low resolution suffices. The nationwide + mobile combination is what forces a star or cellular topology.

**"A greenhouse with 60 climate sensors and 20 vent actuators, one building, sub-second control, mains for actuators, battery for sensors."**
- Range: one building, <50 m → any radio works on range.
- Node count: 80 nodes, dense → meshes shine.
- Latency: sub-second control → eliminates LoRaWAN Class A.
- IP/ecosystem: none stated → Zigbee and Thread both fine; Zigbee if cost-driven and no IP needed, Thread if you want IP/Matter headroom.
- Verdict: **Zigbee or Thread**, leaning Zigbee for a closed industrial system on cost, Thread if the operator wants Matter integration later.

**"A flood sensor in 500 storm drains across a city, reports water level hourly, 10-year battery, no mains anywhere."**
- Range: city-wide, drains are far apart → eliminates the meshes (no nodes between drains).
- Power: 10-year battery, no mains → eliminates Wi-Fi.
- Frequency: hourly, tiny payload → LoRaWAN's sweet spot.
- Penetration: underground in a drain → favours sub-GHz; LoRaWAN's link budget helps, but consider NB-IoT if the link does not close from below ground.
- Verdict: **LoRaWAN** if the link budget closes from inside the drain (test it!), **NB-IoT** as the penetration fallback. This is the case where you *measure* the link budget before committing, because "underground" can eat 20–30 dB.

Notice that in all three, two or three constraints eliminate most of the field and the survivor is obvious. That is the whole method: eliminate fast on the hard constraints (range, mobility, power), then break ties on the soft ones (cost, ecosystem, IP).

## 9. The one slide to remember

If you forget everything else this week, keep these five lines:

1. **Need kilometres on a battery?** LoRaWAN. Nothing else closes the link.
2. **Need to pair with Apple/Google/Amazon, low power?** Matter-over-Thread.
3. **Dense local mesh, mains, no IP, no consumer ecosystem, lowest cost?** Zigbee.
4. **Bandwidth, mains, an AP already there?** Wi-Fi (with Matter if smart-home).
5. **Range + big frequent payload + battery, all at once?** Impossible in this set — that brief needs cellular or a different power source. Say so out loud.

The fifth line is the one that earns its keep. Recognising an internally inconsistent brief before anyone builds a prototype is the highest-leverage thing you do this week.

## 10. What this week does not cover (and where it lives)

To set expectations:

- **The full LoRaWAN OTAA join** — designed in Homework Problem 4; the production implementation is the Semtech LoRaMAC-node reference.
- **Multi-channel hopping and the ADR control loop** — discussed in Lecture 2; the mini-project pins one channel and a fixed SF for clarity.
- **Writing a Zigbee or Thread stack from scratch** — nobody does this; you bring up OpenThread and connectedhomeip, the certified production code. Challenge 1.
- **Cellular LPWAN (NB-IoT, LTE-M) and Sigfox** — covered in this lecture as part of the landscape and revisited in Homework Problem 6, but not built; they need a cellular modem and a SIM, outside this week's bench.
- **BLE and BLE Mesh** — Week 13's topic; referenced here only as the short-range option and as Matter's commissioning bootstrap.
- **Antenna design and RF layout** — a whole discipline of its own; we use pre-matched modules with chip antennas or u.FL connectors and treat the RF front end as a black box. A product would do real antenna tuning and a chamber pass.

The week is deliberately broad-then-deep: broad on the decision (all the radios), deep on the one you can fully build on the bench (LoRaWAN). That shape mirrors how the skill is actually used — you choose among many, then go deep on the one you chose.

## 11. Before you move on

A quick self-check. You should be able to, without notes:

- Name the four ecosystems and the one-sentence summary of each.
- State, for a one-paragraph brief, the surviving radio and the constraint that eliminated the others.
- Explain why LoRa reaches kilometres where 802.15.4 reaches metres (the sensitivity / link-budget gap).
- Explain why Zigbee-vs-Thread is never a PHY decision.
- Explain what Matter is (an application layer) and what it runs over (Thread / Wi-Fi / Ethernet).
- Recognise the "design conflict" brief and say why no radio in the set serves it.
- State the own-vs-rent network axis and the mobility-breaks-meshes rule.

If any of those is fuzzy, re-read the relevant section before Lecture 2 — the LoRaWAN deep dive assumes you have the landscape in your head.

Read Lecture 2 next for the LoRaWAN MAC and the SX126x driver in depth; Lecture 3 for Thread, Matter, and the choose-the-radio worked examples.
