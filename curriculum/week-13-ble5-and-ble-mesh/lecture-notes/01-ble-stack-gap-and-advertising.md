# Lecture 1 — The BLE Stack, GAP, and Advertising

> *Bluetooth Low Energy is two computers pretending to be one radio. The bottom half — the Link Layer and the PHY, the part that owns the antenna and counts microseconds — runs on a dedicated controller; on the Pico W that is the Infineon CYW43439, a combo chip soldered next to the RP2040. The top half — L2CAP, ATT, GATT, GAP, the Security Manager — runs as software on the RP2040, and the Pico SDK ships BTstack to be that software. Between them is HCI, a byte protocol of commands and events that the host sends down a SPI-flavored link and the controller answers. This lecture walks the whole stack top to bottom, then zooms into the bottom: the 40 channels, the four PHYs, the advertising packets that are the entire basis of beacons and of BLE Mesh, and the GAP roles that decide who advertises and who connects. By the end you will be able to take a Wireshark capture and point at every byte and name its layer.*

## 1. Why a Controller and a Host?

Open the Core Spec to Vol 1 Part A §1 ("Architecture"). The block diagram there splits the Bluetooth system into a **Controller** and a **Host**, joined by the **Host Controller Interface (HCI)**.

The split is not arbitrary — it is a hard real-time boundary. BLE has timing requirements measured in microseconds: the inter-frame space (the gap between a request and its response on the air) is exactly 150 µs (Core Spec Vol 6 Part B §4.2.1, the "T_IFS"); a connection event must start within a few microseconds of its scheduled anchor point or the peer declares the link unstable. No general-purpose MCU running an RTOS and a USB stack and a sensor loop can hit those deadlines reliably — an interrupt at the wrong moment blows the budget. So the timing-critical work lives in the controller's dedicated firmware, where nothing else competes for the CPU, and the controller exposes a *logical* interface: "advertise this payload," "connect to this address," "send this data on this connection." That logical interface is HCI.

On the Pico W:

- The **Controller** is the CYW43439's Bluetooth core. It owns the 2.4 GHz radio, runs the Link Layer state machine, schedules connection events, and handles the PHY. You do not program it directly; the SDK loads a firmware blob into it at boot.
- The **Host** is BTstack, compiled into your RP2040 firmware. It implements L2CAP, ATT, GATT, GAP, and the Security Manager. When you call `gap_advertisements_enable(1)`, BTstack composes the corresponding HCI command and sends it to the controller.
- **HCI** rides over the `cyw43` SPI-like bus (the same 4-wire link that carries Wi-Fi traffic — the two radios share the host link and the controller arbitrates between them). The SDK's `pico_cyw43_arch` library owns that transport.

This is the *same* architecture as a Linux laptop with a USB Bluetooth dongle: BlueZ is the host, the dongle is the controller, USB carries HCI. The only difference on the Pico W is that the "dongle" is soldered down and HCI rides SPI instead of USB. If you have ever run `btmon` on Linux to watch HCI traffic, you have already seen the interface you are about to drive from BTstack.

## 2. The full stack, layer by layer

From the bottom up (Core Spec Vol 1 Part A §1; each layer has its own Volume/Part):

| Layer | Where | Spec | What it does |
|-------|-------|------|--------------|
| **PHY** | Controller | Vol 6 Part A | GFSK modulation, 40 channels, the four PHYs (1M/2M/Coded) |
| **Link Layer** | Controller | Vol 6 Part B | advertising/data packets, access address, connection state machine, channel hopping, encryption |
| **HCI** | the boundary | Vol 4 Part E | commands (host→controller) and events (controller→host) |
| **L2CAP** | Host (BTstack) | Vol 3 Part A | multiplexing and fragmentation of upper-layer PDUs |
| **ATT** | Host | Vol 3 Part F | the attribute wire protocol: read/write/notify by handle |
| **GATT** | Host | Vol 3 Part G | services and characteristics built on ATT |
| **SMP** | Host | Vol 3 Part H | pairing, bonding, key distribution |
| **GAP** | Host | Vol 3 Part C | roles, discovery, connection procedures, advertising data format |

Note the shape of this table: the higher you go, the closer to your application, and the more of it BTstack handles for you. You will spend nearly all of this week at the GAP and GATT rows (advertising, services, characteristics) and the SMP row (pairing); the rows below — L2CAP, ATT, the Link Layer, the PHY — are machinery you read about so you can debug, not machinery you program. This is the same layering discipline as the rest of embedded work: you operate at the highest layer that solves your problem and drop down only when something below breaks. A malformed advertisement is a GAP problem you fix at the GAP row; a connection that drops every few seconds might be an L2CAP or Link-Layer timing problem you diagnose by dropping to the HCI trace. Knowing which row a symptom lives on is half of debugging BLE.

GAP and GATT are *profiles*, not protocols — they define behavior and data formats on top of the protocols below them. GAP says "to be discoverable, advertise with these flags and this interval"; GATT says "a service is a group of characteristics, each a value with properties." Everything an application touches is GAP (discovery, connection) and GATT (data); ATT, L2CAP, the Link Layer, and the PHY are the machinery underneath that BTstack and the controller run for you.

We cover ATT and GATT in Lecture 2 and SMP in Lecture 2's second half. This lecture is the bottom three layers (PHY, Link Layer, HCI) plus GAP's discovery and advertising story.

## 3. The radio: 40 channels, four PHYs

BLE uses the 2.4 GHz ISM band, the same crowded spectrum as Wi-Fi, Zigbee, classic Bluetooth, and microwave ovens. It divides the band into **40 RF channels**, 2 MHz apart, from 2402 MHz (channel index 0) to 2480 MHz (channel index 39) — Core Spec Vol 6 Part A §1.4.1.

Three of these are the **primary advertising channels**: indices 37, 38, 39, at 2402, 2426, and 2480 MHz. They were chosen to sit in the gaps between the three non-overlapping Wi-Fi channels (1, 6, 11) so that a busy Wi-Fi network does not wipe out all of BLE's advertising at once. The other 37 channels (indices 0–36) are **data channels**, used inside connections (and, with extended advertising, for secondary advertising payloads).

The modulation is **GFSK** (Gaussian Frequency-Shift Keying) — a one-bit-per-symbol scheme where a 1 and a 0 are two slightly different frequencies. The *symbol rate* is where the four **PHYs** differ (Core Spec Vol 6 Part A §2–§3):

- **LE 1M** (the original, Bluetooth 4.0): 1 megasymbol per second = 1 Mbit/s raw. Every BLE device supports it; it is the mandatory baseline.
- **LE 2M** (Bluetooth 5.0): 2 Msym/s = 2 Mbit/s raw. Doubling the symbol rate halves the time a packet spends on the air, which roughly halves the energy per packet — a big deal for a coin-cell sensor. The cost is ~3 dB of receiver sensitivity, so the range shrinks by maybe 30%. Use it when the two devices are close and you care about battery or throughput.
- **LE Coded, S=2** (Bluetooth 5.0 Long Range): 500 kbit/s effective. Each bit is sent with a forward-error-correcting code (2 symbols per bit), trading data rate for ~2× the range.
- **LE Coded, S=8**: 125 kbit/s effective (8 symbols per bit), ~4× the range — hundreds of metres line-of-sight. This is the "I need to reach the far end of the warehouse" PHY.

A connection starts on 1M and can switch PHY with the `LE Set PHY` HCI command after connecting. The CYW43439 supports 1M and 2M for sure; Coded-PHY support depends on the controller firmware blob, which is why Lecture 3 has you *query* the controller's supported features at runtime rather than assume.

## 3.5. Why the access address and the CRC matter

Two fields the Link Layer prepends to every packet are worth understanding because they explain how a receiver tells "my packet" from "noise" in a band full of everyone else's traffic. The **access address** is a 32-bit value at the start of every Link Layer packet. For advertising, it is the fixed `0x8E89BED6` (every BLE device on Earth uses it for advertising, which is how any scanner can hear any advertiser). For a connection, it is a *random* 32-bit value chosen in the `CONNECT_IND` — so two nearby connections, even on the same data channel at the same instant, do not confuse each other's packets, because each receiver only accepts packets bearing *its* connection's access address. The value is chosen with constraints (not too many consecutive identical bits, not too close to the advertising address) so the receiver's correlator locks reliably.

The **CRC** is a 24-bit cyclic redundancy check over the PDU, seeded by a per-connection CRC-init value (also in the `CONNECT_IND`). It catches bit errors from interference; a packet that fails CRC is dropped and, inside a connection, retransmitted. This is why a BLE link survives a noisy room: corrupted packets are detected and retried at the Link Layer, invisibly to your application, until they succeed or the supervision timeout fires. When you watch a sniffer and see the occasional packet marked "CRC error," that is the physics of 2.4 GHz, and the Link Layer's retransmission is what hides it from you. The access address plus the CRC are the two-line answer to "how does a tiny radio find and trust its own packets in a band shared with Wi-Fi, every other Bluetooth device, and a microwave oven?"

## 4. The Link Layer state machine

The Link Layer (Vol 6 Part B §1.1) has five states a device can be in:

- **Standby** — radio off, doing nothing.
- **Advertising** — broadcasting advertising PDUs on channels 37/38/39.
- **Scanning** — listening on the advertising channels for advertisements.
- **Initiating** — listening for a specific advertiser in order to connect to it.
- **Connection** — in an established connection, hopping across the data channels.

These five states and the transitions between them are the controller's job; BTstack drives them through HCI commands (`gap_start_scan` puts the controller into Scanning, `gap_connect` into Initiating, a successful `CONNECT_IND` into Connection). You never manipulate the Link Layer state machine directly — you issue a GAP intent and the controller runs the state machine — but knowing the five states is what lets you read a controller's behavior: "it is stuck in Initiating" means your connect request found no advertiser; "it left Advertising for Connection" means a central connected.

A device can be in more than one of these conceptually (a connected device can keep advertising to accept more connections), and the controller multiplexes them. The states map directly onto GAP roles, which is the next section.

## 5. GAP roles

GAP (Vol 3 Part C §2.2.2) defines four **roles**, and a device can play several at once:

- **Broadcaster** — only advertises, never accepts connections. Pure one-way. Beacons (iBeacon, Eddystone) and BLE Mesh advertising are broadcasters.
- **Observer** — only scans, never connects. The receiving half of a beacon system.
- **Peripheral** — advertises *connectably*, and accepts a connection. A heart-rate strap, a temperature sensor, our GATT server. Once connected, the peripheral is the **slave** (modern spec: "peripheral") of the connection.
- **Central** — scans and *initiates* connections. A phone, a gateway, our GATT client. The central is the **master** of the connection; it owns the timing.

The asymmetry matters: the central drives the connection's parameters (interval, latency, timeout) and the channel hopping; the peripheral can *request* parameter changes but the central decides. A coin-cell peripheral that wants to sleep asks the central for a long connection interval and high peripheral latency; whether it gets them is the central's call.

## 6. Advertising: the heart of connectionless BLE

An advertiser broadcasts an **advertising PDU** on each of the three primary channels in turn, once per **advertising event**, repeating every **advertising interval**. The interval is configurable from 20 ms (fast, power-hungry, found quickly) to 10.24 s (slow, frugal, found slowly), per Vol 6 Part B §4.4.2.2. A small random delay of 0–10 ms is added to each event so that two advertisers that happen to be in lockstep do not collide forever.

The **advertising PDU types** (Vol 6 Part B §2.3.1):

- **`ADV_IND`** — connectable, scannable, undirected. The default for a peripheral: "anyone may connect to me, anyone may scan me for more data, I am not targeting a specific peer."
- **`ADV_DIRECT_IND`** — connectable, directed at one specific peer address. Used for fast reconnection to a known central; carries no payload, just the target address.
- **`ADV_NONCONN_IND`** — non-connectable, non-scannable. Pure broadcast. **This is what beacons and BLE Mesh use** — the advertiser is shouting data into the void and does not want connections.
- **`ADV_SCAN_IND`** — scannable but not connectable. A broadcaster that will answer a `SCAN_REQ` with a `SCAN_RSP` (more data) but will not accept a connection.

A *scanner* doing an **active scan** sends a `SCAN_REQ` to an advertiser, which replies with a `SCAN_RSP` — a second 31-byte payload. This is how a peripheral can advertise 31 bytes of name+UUIDs in the `ADV_IND` and another 31 bytes (say, a longer name or manufacturer data) in the scan response, for 62 bytes total without extended advertising. A **passive scan** just listens and never transmits — lower power, but you only see the primary 31 bytes.

### The advertising payload format

The payload (both in `ADV_IND` and in `SCAN_RSP`) is a sequence of **AD structures**, defined in Vol 3 Part C §11. Each is:

```text
[length: 1 byte] [AD type: 1 byte] [data: (length - 1) bytes]
```

The `length` byte counts the AD-type byte plus the data, but not itself. This is exactly the framing you implemented in Exercise 1. The legacy payload is at most 31 bytes.

Common AD types (Bluetooth SIG Assigned Numbers):

- `0x01` **Flags** — discoverability and BR/EDR support. Almost every connectable peripheral includes `0x06` = General Discoverable + BR/EDR Not Supported.
- `0x09` **Complete Local Name** (or `0x08` Shortened) — the human-readable name.
- `0x03` **Complete List of 16-bit Service UUIDs** — "I host these services." A central scanning for the Battery Service filters on this (Exercise 2 does exactly that).
- `0x07` **Complete List of 128-bit Service UUIDs** — for custom services.
- `0xFF` **Manufacturer Specific Data** — opaque vendor data; the first two bytes are the SIG-assigned Company Identifier. iBeacon and Eddystone-UID ride here.
- `0x29`/`0x2A`/`0x2B` — the BLE Mesh AD types (PB-ADV, Mesh Message, Mesh Beacon). Mesh traffic is just `ADV_NONCONN_IND` advertisements with these AD types.

A worked payload for a connectable sensor (the one Exercise 1 builds):

```text
02 01 06          Flags = General Discoverable | BR/EDR Not Supported
0A 09 "CC Sensor" Complete Local Name (9 chars)
03 03 1A 18       Complete 16-bit UUIDs = 0x181A (Environmental Sensing)
```

18 bytes, well under 31. On the air, the UUID is little-endian (`1A 18`), which is the first thing that confuses people reading a capture.

## 7. Connections

When a central decides to connect, it stops scanning and sends a **`CONNECT_IND`** PDU to the advertiser (Vol 6 Part B §2.3.3.1). This single PDU carries everything both sides need to run the connection:

- **Access Address** — a random 32-bit value that identifies *this* connection. Advertising uses the fixed access address `0x8E89BED6`; a connection uses a fresh random one so two nearby connections do not interfere.
- **CRC init** — the seed for the per-connection CRC.
- **WinSize / WinOffset** — the timing window for the first connection event.
- **Interval** — the **connection interval**, 7.5 ms to 4 s in 1.25 ms units. Every interval, the central and peripheral exchange at least one packet (the "connection event"). This is the master power-vs-latency knob.
- **Latency** — the **peripheral latency**: how many connection events the peripheral may *skip* if it has nothing to send. A peripheral with interval 30 ms and latency 4 only has to wake every 150 ms when idle, but can still respond within 30 ms when the central sends something.
- **Timeout** — the **supervision timeout**: how long the link may go with zero successful packets before either side declares it dead. Must be longer than `interval × (1 + latency)`.
- **Channel Map** — which of the 37 data channels are in use (some may be blacklisted for interference).
- **Hop** — the hop increment for the channel-selection algorithm.

From the `CONNECT_IND`, both sides independently compute the **channel-hopping sequence** (Vol 6 Part B §4.5.8): each connection event uses a different data channel, computed from the channel map and the hop increment, so the connection spreads across the spectrum and dodges narrowband interference. This is *adaptive frequency hopping*, and it is why BLE survives in a Wi-Fi-saturated room.

The interval/latency/timeout triple is the engineering decision of every BLE product. A wireless mouse wants a 7.5 ms interval and zero latency (snappy). A temperature sensor wants a 1 s interval and high latency (frugal). Apple's Accessory Design Guidelines impose minimums (15 ms interval) that iOS enforces — if your peripheral asks for 7.5 ms, an iPhone silently bumps it to 15 ms.

## 8. The HCI commands behind it all

When you write BTstack code this week, you are composing HCI commands. A few you will see in a `btmon`/HCI capture:

- `LE Set Advertising Parameters` / `LE Set Advertising Data` / `LE Set Advertising Enable` — the three commands behind `gap_advertisements_*`.
- `LE Set Scan Parameters` / `LE Set Scan Enable` — behind `gap_start_scan`.
- `LE Create Connection` — behind `gap_connect`.
- `LE Connection Update` — request new interval/latency/timeout mid-connection.
- `LE Set PHY` — switch to 2M or Coded.
- `LE Read Local Supported Features` — ask the controller what it can do (we use this in Lecture 3 to test for Coded PHY).

BTstack hides these behind its GAP API, but knowing the command underneath means that when an operation fails, you can read the HCI command-complete event's status byte and know *which* HCI command the controller rejected. That is the difference between "BTstack isn't working" and "the controller returned 0x12 Invalid HCI Command Parameters to my `LE Set Advertising Data` because my payload exceeded 31 bytes."

## 9. Putting it on the Pico W with BTstack

The minimum to advertise as a connectable peripheral:

```c
#include "btstack.h"
#include "pico/cyw43_arch.h"

static btstack_packet_callback_registration_t hci_event_cb;

static const uint8_t adv_data[] = {
    0x02, 0x01, 0x06,                          /* Flags */
    0x0A, 0x09, 'C','C',' ','S','e','n','s','o','r',  /* Complete Local Name */
    0x03, 0x03, 0x1A, 0x18,                    /* 16-bit UUID 0x181A */
};

static void packet_handler(uint8_t type, uint16_t ch, uint8_t *pkt, uint16_t sz) {
    (void) ch; (void) sz;
    if (type != HCI_EVENT_PACKET) return;
    if (hci_event_packet_get_type(pkt) != BTSTACK_EVENT_STATE) return;
    if (btstack_event_state_get_state(pkt) != HCI_STATE_WORKING) return;

    /* Controller is up: configure and enable advertising. */
    uint16_t adv_int_min = 0x0030;  /* 30 ms in 0.625 ms units = 48. */
    uint16_t adv_int_max = 0x0030;
    bd_addr_t null_addr = {0};
    gap_advertisements_set_params(adv_int_min, adv_int_max,
                                  0 /* ADV_IND */, 0, null_addr, 0x07, 0x00);
    gap_advertisements_set_data(sizeof(adv_data), (uint8_t *)adv_data);
    gap_advertisements_enable(1);
}

int main(void) {
    stdio_init_all();
    if (cyw43_arch_init()) return -1;
    l2cap_init();
    sm_init();
    hci_event_cb.callback = &packet_handler;
    hci_add_event_handler(&hci_event_cb);
    hci_power_control(HCI_POWER_ON);
    btstack_run_loop_execute();   /* never returns. */
}
```

`0x0030` = 48 in 0.625 ms units = 30 ms, the advertising interval. The advertising type `0` is `ADV_IND` (connectable undirected). After flashing this, your phone's nRF Connect will show "CC Sensor" advertising the Environmental Sensing service — and you will have closed the loop from Exercise 1's hand-built bytes to a real radio.

The `btstack_run_loop_execute()` call is load-bearing: with `pico_cyw43_arch_none`, BTstack's run loop is what services the cyw43 driver and dispatches HCI events to your handler. Never replace it with your own `while(1)`.

## 10. The anatomy of a connection event

Once connected, the entire conversation happens in **connection events**, one per connection interval. Walk one (Core Spec Vol 6 Part B §4.5.1):

1. At the **anchor point** (the precisely-scheduled start of the event), the central transmits a data packet on the current data channel. Even if it has nothing to send, it sends an empty PDU — the packet *is* the heartbeat that keeps the supervision timer from expiring.
2. Exactly **T_IFS = 150 µs** later, the peripheral responds. The 150 µs is not negotiable; it is the spec's fixed inter-frame space and it is why the timing lives in the controller, not on the RP2040.
3. The two may keep ping-ponging within the event if both set the **More Data (MD)** bit, draining a backlog of L2CAP fragments. When neither has more, the event ends and both radios sleep until the next anchor point.

This structure is why the **connection interval** dominates both latency and power. A 7.5 ms interval means up to 133 round-trips per second (snappy, power-hungry); a 1 s interval means one (frugal, laggy). **Peripheral latency** lets the peripheral skip anchor points when idle: with interval 30 ms and latency 4, the peripheral must show up at least every 5th event (150 ms) but can still answer immediately when the central sends data with the MD bit. The **supervision timeout** must exceed `interval × (1 + latency)` — if you set timeout 100 ms with interval 30 ms and latency 4 (effective 150 ms idle), the link drops spuriously. Getting this triple internally consistent is a real bring-up bug; BTstack's `gap_request_connection_parameter_update(con_handle, min, max, latency, timeout)` is how the peripheral asks the central to change it after connecting.

A worked power estimate: a sensor with a 1 s interval, latency 0, sending a 20-byte notification once a minute. It wakes every second for an empty connection event (~1 ms of radio at, say, 8 mA) and once a minute does real work. Average current is dominated by the once-per-second wakeups: ~`1 ms × 8 mA / 1000 ms ≈ 8 µA` of radio duty plus sleep current. Lengthen the interval to 4 s (the maximum) and the radio duty drops 4×. This arithmetic is the whole reason BLE exists, and it is why the interval/latency choice is an engineering decision, not a default.

## 10.5. Scanning: passive, active, and the duty-cycle trade

The receiving side of advertising is **scanning**, and it has the same power-vs-latency tension as advertising. A scanner is defined by two numbers (Core Spec Vol 6 Part B §4.4.3): the **scan window** (how long it listens) and the **scan interval** (how often a window starts). The ratio is the **duty cycle**: a 30 ms window every 30 ms interval is 100% duty (the radio is always listening — fast discovery, high power); a 30 ms window every 300 ms interval is 10% duty (frugal, but it can miss advertisements that fall in the 270 ms gaps).

The subtlety that bites people: an advertiser and a scanner with poorly-phased timing can take a *long* time to rendezvous. If the advertiser advertises every 100 ms on each of three channels and the scanner listens 10% of the time on one channel at a time, the expected time-to-discovery is seconds, not milliseconds. The random 0–10 ms advertising delay (§6) exists precisely to break pathological lock-step where the scanner's window always lands in the advertiser's silent gap. When your central "can't find" a peripheral that nRF Connect sees instantly, the usual cause is your scan duty cycle is too low — widen the window while debugging (`gap_set_scan_parameters(1, 0x0030, 0x0030)` for 100% duty), then tighten it for production power.

**Passive vs active** scanning (the first argument to `gap_set_scan_parameters`): a passive scanner only listens; an active scanner sends a `SCAN_REQ` to each connectable/scannable advertiser and collects the `SCAN_RSP`, doubling the data it can read (62 bytes total) at the cost of transmitting (more power, and it reveals the scanner's presence). Exercise 2's client uses an active scan so it can read service UUIDs the advertiser put in either the ADV or the scan response.

## 10.7. Addresses: public, random static, and private

Every BLE device has a 48-bit address, and which *kind* it uses matters for both discovery and privacy (Core Spec Vol 6 Part B §1.3):

- **Public address** — a globally-unique IEEE-assigned MAC, like an Ethernet address. Permanent, traceable. Used by devices that do not care about being followed (a desktop dongle).
- **Random static address** — a random 48-bit value (top two bits set to `11`) chosen once and kept until the next power cycle (or permanently, if stored). Not IEEE-registered, so it costs no MAC allocation, but it is still a stable identifier while it lasts. Many cheap peripherals use this. The Pico W defaults to a random static address derived from its unique chip ID.
- **Resolvable Private Address (RPA)** — rotates every ~15 minutes, generated from the IRK, resolvable only by a bonded peer (the privacy feature, §9 of Lecture 2). Untrackable by strangers.
- **Non-resolvable private address** — a purely random address that rotates and that *nobody* can resolve; used when even the bonded peer should not correlate sessions.

The address *type* is carried alongside the address in every advertising report and every connection request, and getting it wrong is a real bug: if a central tries to connect to a peripheral advertising a random address but passes the public-address type to `gap_connect`, the connection request targets a device that is not there and times out. Exercise 2's client reads the address type from the advertising report (`gap_event_advertising_report_get_address_type`) and passes it straight through to `gap_connect` — never assume public. When your "connect" silently times out against a peripheral your scan clearly saw, a type mismatch is the first thing to check.

## 11. Beacons: connectionless broadcast in practice

A **beacon** is a `ADV_NONCONN_IND` broadcaster that packs its identity into the 31-byte manufacturer-specific AD structure. The two dominant formats live entirely inside AD type `0xFF`:

**iBeacon** (Apple) — the manufacturer data is: Company ID `0x004C` (Apple, little-endian `4C 00`), then `02 15` (iBeacon type + length), then a 16-byte Proximity UUID, a 2-byte Major, a 2-byte Minor, and a 1-byte measured-power calibration value. A receiver computes coarse distance from RSSI vs the measured power. The whole beacon is one `ADV_NONCONN_IND` repeated every ~100 ms.

```text
02 01 06                                  Flags
1A FF 4C 00 02 15                         len=26, ManufData, Apple, iBeacon, 21 bytes
   E2 C5 6D B5 DF FB 48 D2 B0 60 D0 F5    Proximity UUID (16 bytes)
   A7 10 96 E0
   00 01                                  Major
   00 02                                  Minor
   C5                                     measured power (-59 dBm)
```

**Eddystone** (Google, now community) — uses *service data* (AD type `0x16`) for the Eddystone service `0xFEAA`, with frame types UID, URL, and TLM (telemetry). The URL frame compresses `https://` and common TLDs into single bytes so a full short URL fits in 31 bytes — the basis of "physical web" tap-to-open links.

Beacons matter to this week because BLE Mesh is, at the radio level, *also* a `ADV_NONCONN_IND` broadcaster — a mesh node shouting Mesh Message AD structures (type `0x2A`) is doing exactly what a beacon does, just with a different AD type and a relay layer on top. If you can read an iBeacon in a capture, you can read a mesh advertisement; only the AD type and the payload format differ.

## 12. A worked HCI trace

When you call `gap_advertisements_enable(1)`, BTstack emits a sequence of HCI commands and the controller answers with command-complete events. In a `btmon` (or BTstack's packet-logger) trace you see:

```text
> HCI Command: LE Set Advertising Parameters
    Min interval: 30.000 ms   Max interval: 30.000 ms
    Type: ADV_IND   Own address type: Public   Channel map: 37,38,39
< HCI Event: Command Complete (LE Set Advertising Parameters)  Status: Success
> HCI Command: LE Set Advertising Data
    Length: 18   Data: 020106 0A0943432053656E736F72 03031A18
< HCI Event: Command Complete (LE Set Advertising Data)  Status: Success
> HCI Command: LE Set Advertising Enable  Enable: 0x01
< HCI Event: Command Complete (LE Set Advertising Enable)  Status: Success
```

Three commands, three success events, and the device is on the air. When something fails — a 32-byte payload, an invalid interval — the relevant command-complete carries a non-zero status (`0x12 Invalid HCI Command Parameters` for the oversized payload), and *that* is the byte that tells you which of your GAP calls the controller rejected. Learning to read this trace turns "BTstack isn't advertising" into "the controller rejected my `LE Set Advertising Data` because length 32 exceeds 31" — a five-second diagnosis instead of an hour of guessing. Enable BTstack's HCI dump (`hci_dump_init`) during bring-up; turn it off for production.

## 12.5. The BTstack run loop and the three cyw43 arch variants

Everything you write this week is a callback hanging off **BTstack's run loop**, and the run loop is provided by one of three `pico_cyw43_arch` variants — picking the wrong one is a common first-day stall:

- **`pico_cyw43_arch_none`** — BTstack's own run loop drives everything; you call `btstack_run_loop_execute()` and it never returns. All your code runs in callbacks (HCI events, GATT events, BTstack timers). This is the lightest and what every exercise uses. No FreeRTOS, no second thread; just the event model.
- **`pico_cyw43_arch_threadsafe_background`** — the cyw43 driver is serviced from an interrupt/alarm in the background, so you can run a normal `while(1)` loop *and* use BTstack, provided you guard BTstack calls with `cyw43_arch_lockout`-style locking. Heavier; for when you have other foreground work.
- **`pico_cyw43_arch_*` with FreeRTOS** — BTstack runs as a FreeRTOS task. Use this when you are integrating BLE into the FreeRTOS sensor hub from Week 9.

The run loop is also how you schedule periodic work without a busy-wait. A BTstack timer:

```c
static btstack_timer_source_t poll_timer;
static void poll_handler(btstack_timer_source_t *ts) {
    /* do periodic work — sample a sensor, kick a state machine */
    btstack_run_loop_set_timer(ts, 200);   /* re-arm for 200 ms */
    btstack_run_loop_add_timer(ts);
}
/* in main, after hci_power_control: */
btstack_run_loop_set_timer_handler(&poll_timer, &poll_handler);
btstack_run_loop_set_timer(&poll_timer, 200);
btstack_run_loop_add_timer(&poll_timer);
```

This is the pattern the mini-project's PIR poll uses. The cardinal rule with `arch_none`: **never block inside a callback**. A callback that spins for 50 ms starves the run loop, the cyw43 driver does not get serviced, HCI events back up, and the connection drops on a supervision timeout. Do a little work and return; if you need to wait, set a timer and return. This is the same ISR-discipline lesson from Week 11 (an ISR may not block) applied to the BLE event loop.

## 12.7. The 2.4 GHz neighborhood: coexistence on the Pico W

A last practical point that bites every Pico W project: the CYW43439 has **one radio** shared between Wi-Fi and Bluetooth. They cannot both transmit at the same instant; the controller's coexistence logic time-slices between them. For a pure-BLE project this is invisible — BLE has the radio to itself. But the moment you also bring up Wi-Fi (the gateway homework, the mesh-to-MQTT bridge), BLE scanning and Wi-Fi packets contend, and you will see BLE advertising intervals jitter and connection events occasionally slip when Wi-Fi is busy. The SDK's `server_with_wifi` example exists to show the coexistence pattern; the practical implication is that a device doing both must budget for higher BLE latency than a BLE-only device, and a latency-critical BLE link (a mouse) should not share its chip with a heavy Wi-Fi load.

The broader 2.4 GHz neighborhood is just as crowded *off* the chip. Your BLE shares the band with every Wi-Fi access point, every other Bluetooth device, every Zigbee/Thread network, and every microwave oven within ~40 m. BLE's defenses are the three-channel advertising spread (dodging Wi-Fi 1/6/11) and adaptive frequency hopping inside connections (§7), but in a genuinely saturated environment — a conference hall, an apartment building — you *will* see packet loss, longer discovery times, and the occasional dropped connection. This is not a bug in your code; it is the physics of an unlicensed band. When you debug "my advertiser is flaky," rule out RF congestion (try a quieter location or a sniffer's channel-occupancy view) before you suspect your firmware. The same lesson the Saleae taught in Week 7 — the bus is noisier than the datasheet's clean timing diagram — applies to the air, only more so, because you do not own the medium.

## 13. Summary

The Controller/Host split is the mental model: timing-critical Link Layer and PHY in the CYW43439, everything an app touches (L2CAP up through GAP/GATT/SMP) in BTstack on the RP2040, HCI between them. The radio is 40 channels, three for advertising, with four PHYs trading rate for range and power. Advertising is the basis of connectionless BLE — beacons and Mesh are `ADV_NONCONN_IND` broadcasts — and the AD-structure framing you built in Exercise 1 is exactly what goes on the air. A connection is bootstrapped by a single `CONNECT_IND` that carries the access address, the interval/latency/timeout triple, and the hop parameters, after which both sides hop the data channels in lockstep.

Next lecture: GATT — the attribute table, services, characteristics, descriptors, notifications vs indications — and then the Security Manager: pairing, bonding, and why LE Secure Connections is the line between a link a sniffer can read and one it cannot.

## References for this lecture

- Core Spec 5.4 Vol 1 Part A §1 ("Architecture"). <https://www.bluetooth.com/specifications/specs/core-specification-5-4/>
- Core Spec 5.4 Vol 6 Part A §1–§3 (PHY, channels, the LE PHYs).
- Core Spec 5.4 Vol 6 Part B §2.3 (advertising/connection PDUs), §4.4.2 (advertising), §4.5 (connections), §4.5.8 (channel selection).
- Core Spec 5.4 Vol 3 Part C §2.2.2 (GAP roles), §11 (advertising data format).
- Core Spec 5.4 Vol 4 Part E (HCI commands and events).
- Bluetooth SIG Assigned Numbers (AD types, Company Identifiers). <https://www.bluetooth.com/specifications/assigned-numbers/>
- BTstack manual, "GAP" and "Run Loop". <https://bluekitchen-gmbh.com/btstack/>
- Pico W connectivity guide, Ch. 4 ("Bluetooth"). <https://datasheets.raspberrypi.com/picow/connecting-to-the-internet-with-pico-w.pdf>
