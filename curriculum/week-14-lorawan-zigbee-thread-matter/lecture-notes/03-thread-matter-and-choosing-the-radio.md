# Lecture 3 — Thread, Matter, and Choosing the Radio

> *LoRaWAN was a chip and a MAC you could hold in your head. Thread and Matter are an ecosystem you have to respect. The good news: where LoRaWAN makes you implement AES-CMAC by hand, Thread and Matter come with a mature open-source stack (OpenThread, connectedhomeip) that you configure rather than write. The bad news: "configure rather than write" hides an enormous amount of machinery — IPv6 over 802.15.4, a self-healing mesh, a certificate-based commissioning flow, a multi-fabric security model — and when it breaks, you are debugging a stack you did not write. This lecture explains what that machinery does, why Matter chose Thread, what a Matter device actually is in code, and then closes the week with the worked radio-selection examples that are the whole point.*

This is a **conceptual** build, deliberately. Matter-over-Thread needs an 802.15.4 radio the RP2040 does not have. The honest way to teach it — C7's way — is to read the real stack on the real target (Challenge 1 walks the `connectedhomeip` lighting example on an nRF52840) and reason carefully about the architecture, rather than fake a Thread mesh on a Pico that has no 802.15.4 radio. So this lecture is dense with "here is what the code does and what it costs," and the hands-on radio work this week stays with LoRa, where a $15 module and a free network server let you bring up the genuine article end to end.

## 1. Thread — IPv6 mesh on 802.15.4

### 1.1 The stack, layer by layer

Thread (Thread Group spec 1.3 / 1.4) is built in layers on the 802.15.4 PHY/MAC you met in Lecture 1:

- **802.15.4 PHY/MAC** — 2.4 GHz, 250 kbit/s, channels 11–26, the same radio as Zigbee. Frames are MAC Data frames with link-layer AES-CCM* security.
- **6LoWPAN (RFC 6282)** — IPv6 header compression. A full IPv6 header is 40 bytes; an 802.15.4 frame is at most 127 bytes. 6LoWPAN compresses the common-case header down to a handful of bytes (eliding link-local prefixes, deriving the interface identifier from the MAC address, compressing UDP ports). Without it, IPv6 would not fit.
- **IPv6 + UDP** — Thread devices are genuine IPv6 hosts. Each has a **link-local** address (`fe80::/10`), a **mesh-local** address (a unique-local `fd00::/8` prefix scoped to the Thread network), and, via a Border Router, a **global** address. Application traffic is UDP (CoAP, in Matter's case).
- **MLE (Mesh Link Establishment)** — how nodes discover neighbours, exchange link-quality info, and maintain the mesh. MLE messages carry the network parameters and the routing updates.
- **Distance-vector routing** — Routers exchange routing costs and forward packets hop-by-hop. The mesh self-heals: if a Router drops, traffic reroutes; the network keeps working.

### 1.2 Roles and the absence of a single point of failure

Thread node roles:

- **Leader** — *one per network*, elected dynamically. The Leader assigns Router IDs and holds the authoritative copy of the network data. Critically, **if the Leader fails, the network elects a new one** — there is no permanent coordinator whose death kills the mesh. This is Thread's headline robustness advantage over Zigbee, where the single coordinator is a permanent single point of failure.
- **Router** — mains-powered (always-on radio), forwards for others, can become Leader.
- **REED (Router-Eligible End Device)** — can be promoted to Router if the mesh needs more routers, but acts as an end device until then.
- **End Device (MED / FED)** — talks only to its parent Router; does not forward.
- **Sleepy End Device (SED)** — an End Device whose radio is off most of the time; it polls its parent for queued messages on a schedule. This is how a battery contact-sensor lives for years on a Thread mesh: it sleeps, wakes briefly to poll, sleeps again. The parent Router buffers downlinks for it.

### 1.3 The Border Router — why Thread suddenly matters

A **Thread Border Router (TBR)** has two radios: 802.15.4 (the Thread side) and Wi-Fi or Ethernet (the home-network side). It does three jobs: routes IPv6 between the Thread mesh and the LAN/internet, runs the **mDNS/SRP** service discovery that lets Matter controllers find Thread devices, and distributes the off-mesh routable prefix.

The reason Thread went from "interesting" to "inevitable" between 2021 and 2024 is that the border routers shipped *into homes by the million* inside products people already bought: Apple HomePod and HomePod mini, Apple TV 4K, many Google Nest Hubs and Nest Wifi points, Amazon Echo (4th gen and later). Your house probably already has a Thread Border Router. A new Thread device joins the mesh those existing hubs already run — no new gateway to buy. That is the network effect Matter needed, and it is why "Matter-over-Thread" became the default mental model for low-power Matter devices.

### 1.4 OpenThread

You do not write a Thread stack. **OpenThread** (Google, BSD-3-Clause, on GitHub) is the reference open-source implementation, certified and shipped in production by everyone in the ecosystem. You bring it up on an 802.15.4-capable MCU (nRF52840, EFR32MG, the SiLabs and Nordic parts), configure the dataset (channel, PAN ID, network key, mesh-local prefix), and it forms or joins the mesh. The `ot-cli` shell lets you drive it by hand:

```text
> dataset init new
> dataset commit active
> ifconfig up
> thread start
> state
leader            # this node became the Leader of a fresh network
> ipaddr
fd11:22::ff:fe00:fc00
fd11:22::ff:fe00:dc00
fe80::...          # link-local
```

That is a working Thread network. The application code above it speaks UDP/CoAP to other nodes by their IPv6 addresses. Challenge 1 has you bring this up on real hardware.

### 1.5 The Thread dataset — the network's identity

A Thread network is defined by its **Active Operational Dataset**, a TLV-encoded blob that every node on the network shares. The fields you will set or inspect:

- **Network Name** — a human label (e.g. `OpenThread-1234`).
- **Channel** — the 802.15.4 channel, 11–26.
- **PAN ID** — a 16-bit network identifier (the 802.15.4 PAN).
- **Extended PAN ID** — a 64-bit network identifier (disambiguates two PANs with the same 16-bit ID).
- **Network Key** — the 128-bit master key from which link-layer keys derive. This is the secret; whoever has it can join.
- **Mesh-Local Prefix** — the `fd00::/8` ULA prefix scoped to this network, from which mesh-local addresses are formed.
- **PSKc** — the commissioner credential, derived from a passphrase, used to authenticate an external commissioner.

When you commission a Matter device, the dataset is exactly what gets handed to it (inside the PASE-secured channel) so it joins *your* network rather than forming its own. You can dump it from `ot-ctl`:

```text
> dataset active -x
0e080000000000010000000300001235060004001fffe0...   # the TLV blob, hex
```

That hex string is what you pass to `chip-tool pairing ble-thread`. If two devices have different datasets, they are on different Thread networks and cannot see each other — a common "why won't they talk" bug.

### 1.6 6LoWPAN compression, concretely

A worked example of why 6LoWPAN matters. A full IPv6 + UDP header is 40 + 8 = 48 bytes. An 802.15.4 frame has at most 127 bytes total, and after the MAC header and the link-layer security overhead you have roughly 80–90 bytes of usable payload. Spending 48 of those on an uncompressed header leaves ~35 bytes for application data — unworkable.

6LoWPAN compresses the common case aggressively:

- The IPv6 **version, traffic class, and flow label** collapse to a few bits when they are default.
- The **source and destination addresses** are elided entirely when they are link-local and derivable from the 802.15.4 MAC addresses (the interface identifier is the MAC).
- The **hop limit** compresses to common values (1, 64, 255).
- The **UDP ports** compress to a nibble each when they fall in a well-known range, and the UDP checksum can be elided.

In the best case the 48-byte header shrinks to ~6 bytes, recovering ~42 bytes of payload — the difference between a usable network and a useless one. This is why Thread *requires* 6LoWPAN; it is not an optimisation, it is what makes IPv6-over-802.15.4 physically fit.

## 2. Matter — the application layer over Thread

### 2.1 What a Matter device *is*

A Matter device is a tree:

```
Node
 └── Endpoint 0   (the "root" — Basic Information, Network Commissioning, etc.)
 └── Endpoint 1   (a device type, e.g. "On/Off Light")
        ├── Cluster: On/Off          attrs: OnOff(bool)        cmds: On, Off, Toggle
        ├── Cluster: Level Control    attrs: CurrentLevel(u8)   cmds: MoveToLevel...
        └── Cluster: Identify         attrs: IdentifyTime(u16)  cmds: Identify
```

- An **Endpoint** is a logical device (a single bulb might have one; a power strip has one per outlet).
- A **Cluster** is a feature module with **attributes** (state you can read/write/subscribe) and **commands** (actions you can invoke). The cluster vocabulary is inherited from Zigbee's ZCL, deliberately — Matter is, in part, "Zigbee's data model done right and made mandatory to conform to."
- The **device type** (On/Off Light, Color Temperature Light, Contact Sensor, Thermostat, …) is a named set of mandatory clusters. A "Matter On/Off Light" *must* implement Identify, Groups, Scenes, On/Off; a controller can rely on that.

### 2.2 The interaction model

Controllers talk to devices with four interactions (Matter Core §8): **Read** an attribute, **Write** an attribute, **Invoke** a command, **Subscribe** to attribute changes (the device pushes updates when state changes — this is how a controller's UI stays live without polling). All four ride a secure session over UDP, using CoAP-like framing.

### 2.25 The interaction model, in detail

Matter's four interactions (Core §8) are worth seeing concretely, because they are what a controller actually sends:

- **Read** — "what is the value of attribute X on cluster Y at endpoint Z?" The device replies with the value. Example: read `OnOff::OnOff` on Endpoint 1 to learn whether the light is on.
- **Write** — "set attribute X to V." Used for writable attributes (e.g. a thermostat setpoint). Many attributes are read-only and must be changed via commands instead.
- **Invoke** — "run command C with arguments A." This is how most *actions* happen. Turning a light on is `Invoke On` on the On/Off cluster, not a write to `OnOff` — the command path lets the device run logic (ramp the LED, update related attributes) rather than just flipping a value.
- **Subscribe** — "tell me whenever attribute X changes, but no more often than min and at least every max seconds." The device then *pushes* reports. This is how a controller UI stays live: it subscribes to `OnOff` and `CurrentLevel`, and the moment a physical switch changes them, the report arrives. Subscriptions are why Matter feels responsive without polling.

The subscription model is the part that most distinguishes Matter from a request/response API: state changes flow *to* the controllers that care, with the device choosing when to report within the negotiated min/max window. A well-behaved device reports promptly on change and coalesces rapid changes to respect the min interval, protecting the mesh from a chatty attribute.

### 2.3 The security model — PASE, CASE, fabrics

This is the part that earns Matter its complexity, and it is genuinely good engineering:

- **Commissioning (PASE — Passcode-Authenticated Session Establishment).** When you set up a new device, you scan its QR code or 11-digit setup code. Your phone connects to the device over **BLE** (the device advertises a commissionable BLE service before it has any network). PASE uses **SPAKE2+** — a password-authenticated key exchange — so the passcode never crosses the air in the clear and a passive eavesdropper learns nothing. Over that secure channel, the commissioner gives the device its **operational credentials** (an operational certificate, a node ID, the fabric's root cert) and, for a Thread device, the **Thread network credentials** (the dataset: channel, PAN ID, network key). The device then joins the Thread mesh.
- **Operation (CASE — Certificate-Authenticated Session Establishment).** Once commissioned, controllers and the device authenticate each other with X.509 certificates (the operational certs issued during commissioning), establishing a session keyed with AES-CCM. No passwords at runtime; mutual certificate auth.
- **Fabrics and multi-admin.** A **fabric** is one administrative domain (one ecosystem — your Apple Home, say). A Matter device can belong to **multiple fabrics at once**: you can commission the same bulb into Apple Home *and* Google Home *and* Alexa, and all three control it independently, each with its own operational certificate and root of trust. This "multi-admin" capability is the user-visible payoff of the whole certificate architecture — it is why Matter ended the "which one ecosystem does this gadget support?" question.
- **Device Attestation.** During commissioning the device proves it is a genuine, certified product with a **Device Attestation Certificate (DAC)** — a per-device cert, baked in at manufacture, chaining up to a Product Attestation Authority (PAA) that the CSA recognises via the **Distributed Compliance Ledger (DCL)**. This is the anti-counterfeiting layer, and it is a real manufacturing-supply-chain obligation: you cannot ship a logo-bearing Matter device without provisioning a DAC into every unit.

### 2.4 connectedhomeip — the SDK you configure, not write

The open-source Matter SDK is **`connectedhomeip`** (CSA + the platform vendors, Apache-2.0, on GitHub). It is large and it is not light: building the `lighting-app` example for an nRF52840 produces a firmware in the hundreds of kilobytes, and the SDK pulls in OpenThread, mbedTLS, the Matter data model, the interaction model, and the commissioning machinery. The cluster behaviour you implement is a handful of callbacks:

```cpp
// Pseudocode shape of a Matter On/Off Light's cluster callback in connectedhomeip.
// The SDK dispatches Invoke/Write here; you drive the GPIO. (Real code lives in
// examples/lighting-app and the generated zap callbacks.)
void emberAfOnOffClusterServerAttributeChangedCallback(
        EndpointId endpoint, AttributeId attributeId) {
    if (attributeId == OnOff::Attributes::OnOff::Id) {
        bool on = false;
        OnOff::Attributes::OnOff::Get(endpoint, &on);
        gpio_set_level(LED_GPIO, on ? 1 : 0);   // the actual "do the thing"
    }
}
```

The lesson: with Matter you write the ~20 lines that turn the LED on and off; the SDK is the other several hundred thousand. That ratio is the deal. You trade "I understand every byte" (which you got with LoRaWAN) for "I conform to an ecosystem standard and pair with three brands of hub out of the box." Both trades are correct for different products, which is the entire theme of this week.

### 2.45 A small catalogue of Matter clusters and device types

To make the data model concrete, here is the vocabulary you meet first:

**Common clusters (appear across many device types):**
- **Identify** — make the device blink so the user can find it; attribute `IdentifyTime`, command `Identify`.
- **Groups** — bind the device into a group addressable as a unit.
- **Scenes** — store and recall attribute snapshots ("movie mode").
- **On/Off** — the simplest actuator; attribute `OnOff` (bool), commands `On`, `Off`, `Toggle`.
- **Level Control** — dimming; attribute `CurrentLevel` (u8), commands `MoveToLevel`, `Move`, `Step`.
- **Color Control** — hue/saturation/color-temperature for lights.
- **Basic Information** — vendor name, product name, serial, software version (on Endpoint 0).
- **Network Commissioning** — how the device joins Thread/Wi-Fi (on Endpoint 0).

**Sensor-side clusters (read-mostly):**
- **Temperature Measurement** — attribute `MeasuredValue` (int16, centidegrees).
- **Relative Humidity Measurement** — attribute `MeasuredValue`.
- **Occupancy Sensing** — attribute `Occupancy` (bitmap).
- **Boolean State** — a generic open/closed (contact sensor).

**A few device types and their mandatory clusters:**
- **On/Off Light** — Identify, Groups, Scenes, On/Off.
- **Dimmable Light** — the above plus Level Control.
- **Color Temperature Light** — plus Color Control.
- **Contact Sensor** — Identify, Boolean State.
- **Temperature Sensor** — Identify, Temperature Measurement.
- **Thermostat** — Identify, Thermostat (setpoints, mode), often Temperature Measurement.

The pattern: a device type is a *named contract* — implement these clusters and any Matter controller knows how to drive you. Homework Problem 5 has you pick a non-light device type and lay out its clusters. The skill is reading the device-type library and mapping the mandatory clusters to your hardware, not inventing a protocol.

### 2.5 Matter-over-Wi-Fi vs Matter-over-Thread

Matter runs over Thread *or* Wi-Fi (or Ethernet). The split:

- **Matter-over-Thread** — for low-power, battery, mesh devices: sensors, bulbs, locks, contact sensors. Thread's sleepy-end-device support gives multi-year battery life; the mesh gives whole-home coverage from low-power radios.
- **Matter-over-Wi-Fi** — for mains-powered, higher-bandwidth devices that benefit from the existing AP: cameras, some plugs, some appliances. No Thread Border Router required, but no multi-year battery either.

A device picks one transport at design time. The Matter application layer above is identical; only the transport differs. The commissioning flow (BLE bootstrap → hand over Thread *or* Wi-Fi credentials) is the same shape for both.

### 2.6 The commissioning flow, as a sequence

The single most user-visible piece of Matter is the commissioning flow. It is worth tracing as a sequence, because it is also the part that most often goes wrong on the bench:

```text
 Phone (commissioner)            New device                 Thread mesh / hub
        |                            |                              |
        |  scan QR / enter setup code|                              |
        |--------- BLE scan -------->| (device advertises a         |
        |<-------- BLE adv ----------|  commissionable BLE service) |
        |                            |                              |
        |---- BLE connect ---------->|                              |
        |==== PASE (SPAKE2+) =======>| (passcode never on the wire) |
        |<=== secure channel up =====|                              |
        |                            |                              |
        |-- Device Attestation req ->|                              |
        |<- DAC + attestation -------| (proves it's a genuine,      |
        |                            |  certified product)          |
        |                            |                              |
        |-- install operational ---->| (node ID, operational cert,  |
        |   credentials              |  fabric root cert)           |
        |-- install Thread dataset ->| (channel, PAN ID, net key)   |
        |                            |---- join Thread mesh ------->|
        |                            |<--- mesh-local IPv6 addr ----|
        |                            |                              |
        |==== CASE (cert auth) ======>| (operational session,        |
        |<=== operational session ===|  AES-CCM, over Thread/UDP)   |
        |---- read/invoke ----------->| (now controlling the device) |
```

Three things to notice. First, **the commissioning runs over BLE**, not over Thread — the device has no Thread credentials yet, so BLE is the bootstrap channel. This is why a Thread device needs a BLE radio too (the nRF52840 has both, which is why it is the canonical part). Second, **the passcode is never transmitted**: SPAKE2+ is a password-authenticated key exchange, so a passive sniffer on the BLE link learns nothing usable. Third, the **Thread credentials are handed over inside the PASE-secured channel**, so the network key never appears in the clear either.

When commissioning fails on the bench, it is almost always one of: the BLE advertisement not appearing (the device did not enter commissioning mode), the setup code mismatch (PASE fails), the device attestation failing (test certs not provisioned, or the commissioner enforcing attestation it should waive for a dev device), or the Thread dataset being wrong (the device joins a phantom network). Challenge 1 makes you walk all of these.

### 2.7 What Matter genuinely does not solve

Honesty about the limits, because the marketing oversells:

- **It does not retrofit old devices.** A 2019 Zigbee bulb is not a Matter device; you need a bridge. Matter solved *new-device* fragmentation, not the installed base (the bridge path closes that gap, with caveats).
- **It does not eliminate the cloud for everything.** Local control of the basic device type works without internet, but many vendors still route advanced features (firmware updates, history, automations) through their cloud. "Matter" on the box does not guarantee "fully local."
- **It added real engineering weight.** A Zigbee On/Off endpoint was a few hundred lines. The Matter equivalent drags in the whole SDK, OpenThread, mbedTLS, and the attestation machinery. The interoperability is worth it for a consumer product; it is dead weight for an internal device.
- **The device-type coverage grew over time.** Cameras, robot vacuums, and detailed energy devices arrived in Matter 1.2–1.4, later than the first lights and plugs. Check the current device library before promising a device type.
- **Certification is real money and real calendar time.** Two programs (Thread + Matter) plus regulatory, plus the per-unit DAC provisioning. Budget months, not weeks.

None of this makes Matter a bad choice — it makes it a *considered* choice. Pick it because the product lives or dies on ecosystem pairing, not because it is the newest acronym.

## 3. Zigbee vs Thread, decided

Lecture 1 established they share a PHY, so the choice is network-and-ecosystem. The decision:

- Pick **Thread** if the product is a *new* consumer smart-home device that should pair with Apple/Google/Amazon via **Matter**, or if you need IPv6 addressability, or if you want the no-single-coordinator robustness. This is the forward-looking default for new consumer IoT.
- Pick **Zigbee** if you are extending an *existing* Zigbee deployment, if you are in commercial/industrial lighting or building automation where Zigbee is entrenched and certified product is abundant and cheap, or if you have no IP/ecosystem requirement and want the most mature, lowest-cost mesh. Zigbee is not dead; it is the incumbent, and incumbency is worth a lot in a deployed building.
- And note the bridge: a **Zigbee-to-Matter bridge** lets an existing Zigbee fleet appear as Matter devices to new controllers, which is how the industry is migrating without re-flashing millions of installed bulbs.

## 4. The worked radio-selection examples

This is the payoff. For each brief, the answer and the *why* — and the wrong answers and why they are wrong. These are the cases behind Exercise 1's `cc_choose_radio()`.

### 4.1 Vineyard soil-moisture sensor

*Brief: 500 sensors across 40 hectares, each 2–8 km from the farm office, buried by the vines, on a single AA-cell budget for one season, reporting moisture and temperature (6 bytes) every 15 minutes.*

**Answer: LoRaWAN, Class A, ABP or OTAA.** Range (kilometres) on a battery with tiny infrequent payloads is the exact shape LoRaWAN was built for; one gateway at the office hears all 500 sensors. Wrong answers: Thread/Zigbee cannot reach 8 km and there are no intermediate nodes to mesh across an empty field; Wi-Fi has no coverage and no power budget; BLE is a non-starter at this range. This is the one case where the answer is *forced* — only one radio closes the link.

### 4.2 Smart light bulb for the consumer market

*Brief: a screw-in LED bulb sold at retail, must work with Apple Home, Google Home, and Alexa, mains-powered, 10 m from the hub, controlled in real time.*

**Answer: Matter-over-Thread** (or Matter-over-Wi-Fi for a higher-bandwidth fixture). The product requirement *is* "pairs with the three big ecosystems," and Matter is the only thing that delivers multi-admin pairing. Thread because it is the low-power mesh Matter standardises on and the border routers are already in customers' homes. Wrong answers: plain Zigbee gives you a fragmented per-ecosystem certification mess and no clean Apple Home path; LoRaWAN is absurd at 10 m for a chatty real-time control device (and the duty cycle forbids real-time control anyway); plain BLE does not give whole-home multi-admin control.

### 4.3 Warehouse lighting retrofit

*Brief: 400 fixtures in a 5,000 m² warehouse, mains-powered, occupancy + daylight control, 50 m max between nodes, no consumer-ecosystem requirement, lowest installed cost.*

**Answer: Zigbee.** Dense mesh, mains-powered nodes (so always-on routers everywhere — the mesh is rock-solid), no IP requirement, no Apple Home requirement, and a deep catalogue of cheap certified commercial lighting gear. Thread would also *work* technically, but you would pay for IPv6 and Matter machinery the product does not use, and the commercial-lighting ecosystem is Zigbee-native. Wrong answers: LoRaWAN's duty cycle forbids the sub-second control a lighting system needs; Wi-Fi at 400 nodes stresses the AP and costs more per node.

### 4.4 The impossible brief

*Brief: stream 200-byte updates every 5 seconds from a moving asset 5 km away, on a battery.*

**Answer: NONE — the brief is a design conflict.** LoRaWAN reaches 5 km on a battery but cannot carry 200 bytes every 5 seconds: that payload at the SF you need for 5 km is over a second of airtime, every 5 seconds is a 20%+ duty cycle, and EU868 caps you at 1%. No mesh radio reaches 5 km. Cellular (LTE-M/NB-IoT — outside this week's set) is the real answer, at a real power and cost penalty. The instructive move is to recognise the conflict *before* prototyping: range, payload size, frequency, and battery are four constraints that cannot all be maximised, and a brief that demands all four is telling you the product needs a different power source or a different architecture. `cc_choose_radio()` returns `RADIO_NONE` here on purpose — flagging the impossible brief is a feature, not a bug.

## 4.5 The cross-cutting question: own the network or rent it

One axis cuts across all the worked examples and rarely appears on a radio's spec sheet: do you *own* the network infrastructure or *rent* it?

- **Own it (LoRaWAN with your own gateways, Zigbee, Thread):** you deploy and run the gateways/coordinators/border-routers. No recurring per-device fees. You control coverage, uptime, and data residency. The capital cost is the gateways and the operational cost is keeping them alive. This wins for a campus, a farm, a factory, a building — a bounded space you control.
- **Rent it (LoRaWAN on a public operator, cellular NB-IoT/LTE-M, Sigfox):** someone else runs the network and you pay per device per month/year. No infrastructure to deploy or maintain, instant wide-area coverage, but recurring cost that scales with fleet size and a dependency on an operator's business continuity. This wins for devices scattered across a region with no infrastructure of your own.

The vineyard sensor (§4.1) is an "own it" case — one gateway at the farm office covers the whole vineyard, no recurring fees. A nationwide fleet of vending-machine telemetry units is a "rent it" case — you cannot deploy gateways nationwide, so you pay a cellular carrier. The same radio (LoRaWAN) can be either: TTN's public network (rent, free at small scale) or your own gateway (own). Decide the network-ownership model alongside the radio; it shapes the recurring-cost line of the business case more than any technical parameter.

## 4.6 Mobility — the constraint that breaks meshes

A subtle constraint that disqualifies whole categories: is the device *mobile*?

- A **mesh** (Zigbee, Thread) assumes a relatively stable topology — nodes discover neighbours and establish routes, and that machinery has overhead every time the topology changes. A device that moves through the mesh constantly re-parents and re-routes; a device that *leaves* the mesh's coverage is simply gone. Meshes are for fixed or slowly-moving deployments.
- **LoRaWAN** handles mobility gracefully *because it is a star* — the device does not associate with a specific gateway, so as it moves, different gateways pick it up with no handover protocol. This is why asset trackers use LoRaWAN (or cellular), not Thread. The one caveat: a mobile LoRaWAN device must *disable ADR*, because its link margin changes faster than the ADR control loop can track — the network would keep trying to optimise an SF that was right two minutes ago.
- **Cellular** is built for mobility (it is a phone network) and handles handover natively — the right answer for fast-moving, wide-area assets.

So "is it mobile?" is a fast disqualifier: mobile rules out the meshes and points at LoRaWAN (slow, sparse reporting) or cellular (faster, denser). Add this to your decision reflex alongside range and power.

## 5. Where to go deeper

- **LoRaWAN:** the LoRa Alliance L2 1.0.4 spec and RP002 regional parameters; the Semtech SX126x datasheet; The Things Network's free console and learn pages.
- **Thread:** the Thread 1.3 spec; OpenThread's docs and `ot-cli`; the codelabs Google publishes for nRF52840 + OpenThread.
- **Matter:** the Matter 1.3/1.4 Core Specification; the `connectedhomeip` SDK and its `examples/lighting-app`; the CSA's device-type library.
- **Zigbee:** the Zigbee PRO / R23 specification and the Zigbee Cluster Library (ZCL); a vendor SDK (Silicon Labs Simplicity Studio, Nordic's Zigbee stack) if you are extending a real deployment.
- **The 802.15.4 layer underneath:** IEEE 802.15.4-2020 §7 (MAC) and §10 (the 2.4 GHz PHY) — the substrate both meshes share, and the frame whose control field you parsed in Exercise 1.
- **6LoWPAN:** RFC 6282 (header compression) and RFC 4944 (the original adaptation layer) — why IPv6 fits inside a 127-byte 802.15.4 frame at all.
- **The commissioning crypto:** the SPAKE2+ draft (the PAKE behind PASE) and the Matter security model chapters — if you want to understand *why* the passcode never crosses the wire, not just that it does not.
- **Border-router internals:** `ot-br-posix` and the Thread "SRP / mDNS" service-discovery chapters — how a Thread device becomes findable from your phone and the wider IP network.
- **The decision itself:** revisit Exercise 1 and change the weights. Argue with them. The point of encoding the heuristic in code is that you can test your judgement against new briefs — and discover where your intuition and the rules disagree, which is exactly where the interesting engineering conversations happen.

### A last word on honesty in teaching

It would have been easy to bolt an 802.15.4 transceiver onto a Pico, write 200 lines of glue, and call it "a Thread node" — and you would have learned a wiring exercise and nothing about Thread. C7's rule earns its keep here: **real silicon or honest simulation, never a fake.** LoRaWAN is fully hands-on because the bench can support it honestly; Thread/Matter is conceptual-plus-real because the honest path is the nRF52840 and the production SDKs, and a Pico simply is not that hardware. When you hit a topic that your platform cannot do justice, the right move is to say so, move to the right platform for the deep-dive, and reason carefully on the platform you have — not to fake it. That discipline is itself a senior-engineer skill, and it is worth more than any one driver.

## 6. A debugging checklist for the conceptual build

When you do Challenge 1 and Matter-over-Thread misbehaves, work this list in order — it is the bench reflex for this stack:

- **No BLE advertisement?** The device is not in commissioning mode. Factory-reset it; check it has not already been commissioned into a fabric (a commissioned device stops advertising).
- **PASE fails?** The setup code (passcode + discriminator) is wrong, or the device's verifier was provisioned with a different passcode. Re-check the QR/code.
- **Attestation fails?** Test certs (the development PAA/DAC) are not provisioned, or your commissioner enforces production attestation. For a dev build, point the commissioner at the test PAA store.
- **Joins but no IPv6?** The Thread dataset handed over does not match the network the border router runs, so the device formed or joined a phantom network. Dump both datasets and diff them.
- **Has an address but commands fail?** CASE is not establishing — the operational certificates do not chain, or the controller and device disagree on the fabric. Re-commission cleanly.
- **Works from chip-tool but not the phone app?** The two fabrics are independent; commission into each separately. Multi-admin means multiple commissionings, not one shared by all.

Most "Matter is broken" sessions reduce to one of these six, in roughly this frequency order. The 802.15.4 sniff (pyspinel) helps for the join/route layer but is blind to the BLE commissioning and to the CASE-encrypted payloads — so reason about which layer you are debugging before you reach for the sniffer.

You now have the full arc: LoRa modulation and the SX1262 (Lecture 2), the LoRaWAN MAC and its MIC (Lecture 2, Exercise 3), Thread and Matter (this lecture, Challenge 1), and the decision framework that ties them together (Lecture 1, Exercise 1). The mini-project makes the LoRaWAN half real on the bench; the challenges make the Thread/Matter half real on the right silicon.
