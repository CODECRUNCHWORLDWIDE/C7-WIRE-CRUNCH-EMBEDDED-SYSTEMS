# Lecture 3 — BLE 5 Features and BLE Mesh

> *BLE 5 added range, throughput, and broadcast capacity without breaking backward compatibility: the 2M PHY halves your air-time, the Coded PHY quadruples your range, and extended advertising raises the advertising payload from 31 bytes to 1650 and adds deterministic periodic broadcast. BLE Mesh is a different animal entirely — it throws away connections, runs on non-connectable advertisements, and floods every message across a self-healing network of relays bounded by a TTL. There is no master, no routing table, no parent/child hierarchy; any node can relay, and the network deduplicates with a sequence-number cache. This lecture covers the BLE 5 features that matter for an embedded fleet, then builds the mesh from the bottom: the layer stack, addressing, provisioning's ECDH onboarding ceremony, the model layer that is the mesh's answer to GATT services, and the relay-and-TTL machinery you prototyped in Exercise 3.*

## 1. LE 2M PHY: half the air-time

The 2M PHY (Bluetooth 5.0, Core Spec Vol 6 Part A §3) doubles the symbol rate from 1 to 2 Msym/s. Two consequences:

- **Air-time halves.** A packet that took 376 µs on 1M takes ~188 µs on 2M. Less air-time means less energy per packet (the radio is the dominant power draw), so a battery sensor that wakes, sends, and sleeps gets meaningfully longer life. Less air-time also means more devices share the spectrum before it saturates.
- **Sensitivity drops ~3 dB.** Doubling the bandwidth lets in more noise, so the receiver needs a stronger signal. Range shrinks by roughly 30%. 2M is the "close and frugal" PHY: a wearable and the phone on the same wrist, not a sensor across a building.

You switch to 2M *after* connecting (the connection always starts on 1M):

```c
/* Request 2M in both directions; the controller negotiates with the peer. */
gap_request_connection_parameter_update(con_handle, ...); /* optional */
hci_send_cmd(&hci_le_set_phy, con_handle,
             0 /* all_phys */, 2 /* tx: 2M */, 2 /* rx: 2M */, 0);
/* Watch for HCI_SUBEVENT_LE_PHY_UPDATE_COMPLETE to confirm. */
```

A `HCI_SUBEVENT_LE_PHY_UPDATE_COMPLETE` event reports the negotiated PHY — both sides must support 2M or it stays on 1M. The CYW43439 supports 2M; you confirm the peer does by reading the result event, not by assuming.

## 2. LE Coded PHY: quadruple the range

The Coded PHY (Bluetooth 5.0 "LE Long Range", Vol 6 Part A §3.1) adds forward error correction so the receiver can recover packets at a much lower signal-to-noise ratio. Two coding schemes:

- **S=2** — 2 coded symbols per bit, 500 kbit/s effective, ~2× the 1M range.
- **S=8** — 8 coded symbols per bit, 125 kbit/s effective, ~4× the 1M range. Hundreds of metres line-of-sight.

The trade is brutal on air-time (S=8 spends 8× as long sending each bit) and therefore on power and on spectrum sharing. Coded PHY is for "I must reach the far sensor and I send rarely" — an outdoor environmental node, a parking-lot beacon. You will measure 1M vs 2M vs Coded-S8 range across a parking lot in Challenge 2.

**Caveat for the Pico W:** Coded PHY support depends on the CYW43439 controller firmware blob the SDK bundles, and it has varied across SDK releases. Do not trust the datasheet — *query the controller at runtime*:

```c
/* After HCI is up, read what the controller actually supports. */
hci_send_cmd(&hci_le_read_local_supported_features);
/* In the command-complete event, bit 'LE Coded PHY' of the feature mask
   (Core Spec Vol 6 Part B §4.6, feature bit 11) tells you definitively. */
```

If the bit is clear, your `LE Set PHY` to Coded will fail with `0x12 Invalid HCI Command Parameters`, and the right response is to fall back to 1M and tell the user, not to silently do nothing.

## 3. Extended and periodic advertising

Legacy advertising is capped at 31 bytes on three channels. **Extended Advertising** (Bluetooth 5.0, Vol 6 Part B §2.3.4 and §4.4.2) lifts both limits:

- An `ADV_EXT_IND` PDU on a *primary* channel (37/38/39) carries no payload — it is a *pointer* to an `AUX_ADV_IND` on a *secondary* (data) channel, where up to **1650 bytes** of payload live (chained across multiple `AUX_CHAIN_IND` PDUs).
- This moves the bulk of advertising traffic off the three congested primary channels onto the 37 data channels, reducing collisions.

**Periodic Advertising** (Vol 6 Part B §4.4.2.4) adds a *deterministic* broadcast: an advertiser transmits on a fixed schedule, and a scanner that receives the `AUX_ADV_IND`'s sync info can lock onto the train and receive every periodic packet without scanning continuously — low-power, connectionless, one-to-many. This is the basis of Auracast broadcast audio and of large-scale sensor broadcast. The CYW43439 supports extended and periodic advertising; BTstack exposes them through its `gap_extended_advertising_*` API.

For most embedded fleet work you will not need extended advertising — 31 bytes is plenty for a sensor's state — but you must recognize it in a capture (an `ADV_EXT_IND` with no data, pointing elsewhere) so you do not conclude "this advertiser sends empty packets."

## 3.5. When to reach for BLE 5 features (and when not to)

The three BLE 5 features are easy to over-apply. A decision guide for embedded fleets:

- **Reach for 2M** when the two devices are reliably close and you care about battery or you are bumping the throughput ceiling — a wearable streaming sensor data to a phone on the same body, or a sensor that sends large bursts and wants to minimize radio-on time. Do *not* reach for 2M for a device that needs maximum range; you are throwing away ~3 dB.
- **Reach for Coded S=8** when range is the binding constraint and the data rate is low — a perimeter sensor, an agricultural node, an asset tag that pings rarely from across a yard. Do *not* reach for Coded for anything chatty; the 8× air-time will saturate the channel and drain batteries faster than the range gain is worth.
- **Reach for Extended Advertising** when you need a connectionless broadcast payload larger than 31 bytes, or when you want periodic advertising's deterministic low-power broadcast (sensor data a fleet of observers reads without connecting). Do *not* reach for it for a normal connectable peripheral; legacy 31-byte advertising plus a GATT connection is simpler and universally supported, and some older centrals do not scan for extended advertisements.

The meta-point: BLE 5's features are *trades*, not free upgrades. Each one costs you something (range, air-time, or compatibility) to buy something else. The senior move is to know the cost and choose deliberately, not to flip every feature on because it is newer. For the mini-project's mesh, none of these features apply — mesh runs on legacy 1M advertising — and that is the correct default for a sensing fleet that values robustness and universal interop over peak performance.

## 4. BLE Mesh: a different mental model

Everything up to here has been BLE the way you have always thought about it: a device advertises, a central connects, they exchange GATT data. The rest of this lecture asks you to set that model down and pick up a genuinely different one. BLE Mesh shares only the radio with the BLE you know — the same 2.4 GHz channels, the same advertising packets — and reuses none of the connection machinery. It is closer in spirit to a gossip protocol than to a client-server link, and the first hour you spend with it, your instincts from connection-oriented BLE will actively mislead you. So read this section slowly; the mental-model switch is the hard part, and once it clicks the rest is detail.

Stop thinking about connections. **BLE Mesh** (Mesh Protocol 1.1) does not connect nodes to each other. Every mesh message is a **non-connectable advertisement** (`ADV_NONCONN_IND` with the Mesh Message AD type `0x2A`) broadcast on the three advertising channels. Every node that hears it processes it; every node configured as a **relay** re-broadcasts it. There is:

- **No master.** Every node is equal. The "provisioner" onboards new nodes but has no role in routing.
- **No routing table.** A message is *flooded*, not routed. There is no notion of "the path to node 7."
- **No connections between nodes.** (Mesh *can* use a GATT connection — PB-GATT and the GATT Proxy — but only as a bridge for a phone that cannot do raw advertising scanning; node-to-node traffic is always advertisements.)

This is **managed flooding**. It is robust — any node can relay, so the network self-heals around a dead node — and wasteful — one message floods the entire network. The "managed" part is two mechanisms that keep the flood from melting down: the **TTL** bounds how many hops a message travels, and a **sequence-number cache** deduplicates so a node does not re-relay a message it has already seen. You implemented both in Exercise 3.

## 4.5. Why connectionless changes everything

The decision to abandon connections is the single fact that explains every other oddity of BLE Mesh, so it is worth dwelling on. In connection-oriented BLE, two devices negotiate an access address, a hopping sequence, and an interval, and then talk privately on the data channels — a third device cannot even hear them without catching the `CONNECT_IND`. A mesh has none of that. Every message is a public broadcast on the three advertising channels that *every* node in earshot receives. The consequences cascade:

- **No pairwise state.** A connection has per-link state (the access address, the encryption keys, the connection parameters). A mesh node has no per-peer state at all — it has the network keys and a replay cache, and it processes every message the same way regardless of who sent it. This is why a mesh scales to hundreds of nodes where pairwise connections would drown in O(N²) link state.
- **No connection setup latency.** A connection costs a setup round-trip before the first byte. A mesh message just goes — broadcast it and it is on the air. The flip side is no flow control and no acknowledgement, which is why mesh layers its own retransmit (the relay-retransmit count) and its own end-to-end integrity (the TransMIC).
- **Receiver always listening.** A connectionless broadcast only works if receivers are listening when it is sent. Mesh nodes that are not Low-Power Nodes scan the advertising channels continuously (a high duty cycle), which is why an always-on mesh node is a poor fit for a coin cell — and why the Friend/Low-Power-Node mechanism exists to let battery nodes sleep and poll a mains-powered friend instead.
- **Security must be message-level, not link-level.** A connection encrypts the link; everything inside is private. A broadcast has no link to encrypt, so mesh encrypts each *message* (twice — NetKey and AppKey), and that per-message encryption is what makes the public broadcast private to the intended recipients while still relayable by intermediaries. The two-key design (§5) is a direct consequence of being connectionless.

Hold this connectionless fact in mind as the rest of the lecture unfolds: the layer stack, the addressing, the provisioning, the relay — every piece is shaped by "there are no connections, only broadcasts that some nodes choose to repeat."

## 5. The mesh layer stack

Mesh has its own layered architecture (Mesh Protocol 1.1 §3.1), distinct from the BLE host stack:

| Layer | What it does |
|-------|--------------|
| **Model** | application behavior: Generic OnOff, Sensor, Light Lightness. The mesh's "GATT services." |
| **Foundation Model** | Configuration and Health models — manage the node itself. |
| **Access** | formats application messages (opcode + parameters) for a model. |
| **Upper Transport** | application-layer encryption with the **AppKey** + a 32/64-bit Transport MIC. End-to-end between the source model and the destination model. |
| **Lower Transport** | segmentation and reassembly (a message bigger than one Network PDU is split). |
| **Network** | network-layer encryption with the **NetKey**, the TTL, the 24-bit sequence number, source/destination addresses, and header obfuscation. Hop-by-hop. |
| **Bearer** | the advertising bearer (`ADV_NONCONN_IND`) or the GATT bearer. |

The **two-key design** is the security heart of mesh. The **NetKey** secures the *network* — every node in the network has it, and it lets any node relay any message (a relay must decrypt the network layer to read the TTL and re-encrypt). The **AppKey** secures the *application* — only nodes that share an AppKey can read the message's payload. So a relay node can forward a "turn on the lights" message without being able to read it (it has the NetKey but not the lighting AppKey). This separation is what lets you trust a relay you do not trust with the data. Cite Mesh Protocol 1.1 §3.8 for the key hierarchy.

## 5.5. Segmentation: when a message is bigger than one advertisement

An advertising-bearer packet carries at most 31 bytes, of which the mesh network PDU header and MICs eat a chunk, leaving room for only a small access payload (a Generic OnOff Status fits easily; a Sensor model reading with several properties may not). When an access message exceeds what fits in one Network PDU, the **Lower Transport layer** (Mesh Protocol 1.1 §3.5.2) **segments** it: the message is split into numbered segments, each carried in its own Network PDU, and the receiver **reassembles** them. Segmentation also changes the acknowledgement story — a segmented message is acknowledged segment-by-segment with a Segment Acknowledgement (a control message), so the sender knows which segments to retransmit, whereas an unsegmented message is fire-and-forget.

This matters for two reasons. First, segmentation multiplies the air-time: a 3-segment message floods three times as many packets, each relayed, so a chatty large-payload model is far more expensive than a small one — another reason Generic OnOff (a single unsegmented byte) is cheaper than a rich Sensor reading. Second, the reassembly buffer is per-source state on the receiver, so a node must size its reassembly capacity for the largest message and the number of concurrent senders; running out drops messages. For the mini-project, every message is a single unsegmented Network PDU (the OnOff state is one byte), so segmentation never triggers — but you must recognize a segmented exchange in a sniffer (multiple Network PDUs with the same SeqZero and incrementing SegO, plus a Segment Ack) so you do not mistake it for several independent messages.

## 6. Mesh addressing

Three address types (Mesh Protocol 1.1 §3.4.2), all 16-bit:

- **Unicast** (`0x0001`–`0x7FFF`) — one **element** of one node. Assigned at provisioning time. A node with three elements gets three consecutive unicast addresses.
- **Group** (`0xC000`–`0xFEFF`) — a multicast address that models *subscribe* to. "All kitchen lights" is a group address; the lights subscribe to it and the switch publishes to it. The mini-project's subscribers all subscribe to one group.
- **Virtual** (`0x8000`–`0xBFFF`) — a 16-bit address derived by hashing a 128-bit Label UUID, giving a huge sparse namespace without coordinating 16-bit assignments. Less common; we use group addresses.

Plus fixed addresses: `0xFFFF` all-nodes, `0xFFFE` all-relays, `0xFFFD` all-friends, `0xFFFC` all-proxies. `ble_common.h` encodes all these ranges and the `mesh_addr_is_*` classifiers.

An **element** is the unit of addressing. A node is a physical device; an element is an independently-addressable entity within it. A two-gang light switch is one node with two elements (so each gang has its own unicast address and can be controlled independently). Each element hosts one or more **models**.

## 6.5. Elements and the unicast-address arithmetic

Addressing has one more wrinkle worth spelling out because it surprises people: a node does not get *an* address, it gets a *block* of consecutive unicast addresses, one per element. The provisioner assigns the node's *primary* unicast address and the node's elements occupy that address and the ones immediately after it. A node with three elements provisioned at `0x0005` occupies `0x0005`, `0x0006`, and `0x0007`; the next node the provisioner onboards must start at `0x0008` or higher, never overlapping.

This is why `cc-mesh-provision.py` tracks a `next_unicast` counter and increments it by the node's element count (our nodes are single-element, so it increments by one each time, assigning `0x0002`, `0x0003`, `0x0004`). Get the arithmetic wrong — assign two nodes overlapping addresses — and messages addressed to one element silently go to the wrong node, a maddening bug because nothing errors; the wrong LED just lights. The element-block model exists so that a multi-function device (a two-gang switch, a multi-channel dimmer) can address each function independently while provisioning as one node, and the provisioner's job is to hand out non-overlapping blocks. For the mini-project's single-element nodes it is trivial, but the principle — addresses are assigned in element-sized blocks — is the one to carry into a real deployment with multi-element fixtures.

## 7. Provisioning: onboarding a node securely

An unprovisioned node has no keys and no address — it is useless until provisioned. **Provisioning** (Mesh Protocol 1.1 §6) is the secure onboarding ceremony, and it is structurally similar to BLE pairing (ECDH + an OOB authentication value) because it solves the same problem: establish a shared secret with a device you have never met, over a channel an attacker can hear.

The bearer is **PB-ADV** (provisioning over advertisements, AD type `0x29`) or **PB-GATT** (over a temporary GATT connection, for phones). The phases (Mesh Protocol 1.1 §6.3):

1. **Beaconing.** The unprovisioned node broadcasts an *Unprovisioned Device beacon* (AD type `0x2B`) carrying its Device UUID. The provisioner scans for these.
2. **Invite / Capabilities.** The provisioner sends a Provisioning Invite; the node replies with its Capabilities (number of elements, supported OOB authentication methods, public-key type).
3. **Start / Public Key exchange.** Both sides agree on the method and exchange **ECDH P-256 public keys**. Each computes the shared **ECDHSecret** — a passive sniffer cannot.
4. **Authentication (Confirmation / Random).** Using an **OOB value** (a number displayed on the node, or printed on its label, or just zero for the unauthenticated case), both sides compute and exchange confirmation values that prove neither is a man-in-the-middle.
5. **Provisioning Data.** The provisioner sends the node, encrypted under a key derived from the ECDHSecret, the **NetKey**, the **key index**, the **IV index**, the **flags**, and the node's assigned **unicast address**.

After this, the node is a *provisioned node* — it has the NetKey and an address — but its models are still unconfigured. The provisioner (now acting as a **Configuration Client**) talks to the node's mandatory **Configuration Server** model to: add an **AppKey**, bind the AppKey to the node's models, set each model's **publish address** and **subscribe addresses**, and set the node's **feature flags** (relay on/off, proxy, friend, low-power). Only after configuration does the node do anything useful.

Your `cc-mesh-provision.py` walks the PB-ADV provisioning and then the configuration, handing each of the three nodes a unicast address, the shared NetKey and AppKey, and the right publish/subscribe addresses and relay flags.

## 7.5. The Configuration Server: the node's control surface

Every node has a mandatory **Configuration Server** model on its primary element (Mesh Protocol 1.1 §4.4), and it is worth understanding because it is *how* a node becomes useful after provisioning and *how* you debug a misconfigured mesh. Provisioning gives a node a NetKey and an address; the Configuration Server is the interface through which a Configuration Client (your phone's nRF Mesh app, or the BTstack provisioner) then tells the node everything else:

- **Config AppKey Add** — install an AppKey, indexed, bound to a NetKey. A node can hold several AppKeys (one per application sharing the network).
- **Config Model App Bind** — bind an AppKey to a specific model on a specific element, so that model can encrypt/decrypt application traffic. Our Generic OnOff models must be bound to the AppKey or they cannot read each other's messages.
- **Config Model Publication Set** — set a model's publish address, TTL, period, and retransmit. This is what points the publisher's OnOff Server at group `0xC000`.
- **Config Model Subscription Add** — add a group/virtual address to a model's subscription list. This is what makes the subscribers listen to `0xC000`.
- **Config Relay Set / Config Proxy Set / Config Friend Set** — enable or disable the node's features. This is what turns the relay node's relay feature on.
- **Config Node Reset** — factory-reset the node back to unprovisioned.

Every one of these is a message *to* the node's Configuration Server, encrypted under a special **device key (DevKey)** unique to that node (derived during provisioning and known only to the node and the provisioner). The DevKey is why configuration is private to the provisioner: even another node on the same network, holding the NetKey, cannot reconfigure a peer, because it lacks that peer's DevKey. When `cc-mesh-provision.py configure` prints the plan, each line is one of these Configuration messages; applying them (via nRF Mesh) is sending those messages to each node's Configuration Server. When a node "provisioned but does nothing," the Configuration Server holds the answer: read its model bindings and subscription lists and you will find the AppKey bind or the subscription you forgot.

## 8. Models: the mesh's application layer

A **model** (Mesh Model 1.1) is a standardized set of states, messages, and behaviors — the mesh equivalent of a GATT service. Models come in **server** (owns the state) and **client** (controls/observes the state) flavors:

- **Configuration Server / Client** — mandatory; manage the node (keys, addresses, features). Used during setup.
- **Health Server / Client** — mandatory; report faults and "identify" (blink to find a node).
- **Generic OnOff Server / Client** — a boolean. A light's on/off, a switch, **or an occupancy detector's detected/clear** — which is how we use it. Opcodes: `Generic OnOff Get` (`0x8201`), `Set` (`0x8202`), `Set Unacknowledged` (`0x8203`), `Status` (`0x8204`).
- **Sensor Server / Client** — typed sensor readings with units and properties.
- **Light Lightness / CTL / HSL** — the full lighting model family.

**Publish and Subscribe** is the mesh's communication pattern, and it is *configured*, not coded. A model's **publish address** is where it sends its state on change (or on a periodic schedule); a model's **subscribe addresses** are the group/virtual addresses whose messages it accepts. In the mini-project:

- The occupancy node's **Generic OnOff Server** has its publish address set to the group `0xC000`.
- Each subscriber node's **Generic OnOff Client** subscribes to `0xC000`.

When the PIR fires, the server publishes a `Generic OnOff Status` (state = ON) to `0xC000`; every node subscribed to `0xC000` receives it and lights its LED. Nobody addressed anybody by unicast; the group address decoupled the publisher from the subscribers. That decoupling is why mesh scales — adding a fourth light is "subscribe it to `0xC000`," no change to the publisher.

## 8.5. States, bound states, and the Sensor model

A model is built around **states** (Mesh Model 1.1 §2). The Generic OnOff model's state is a single boolean, but richer models have richer states and — crucially — **bound states** that change together. A Light Lightness model and a Generic OnOff model on the same element are bound: setting OnOff to 0 forces Lightness to 0; setting Lightness above 0 forces OnOff to 1. This binding is defined by the spec, not by your code, so a generic on/off switch can control a dimmable light it knows nothing about — the light's binding does the right thing. This is the mesh's interoperability story: a switch and a bulb from different vendors work together because both implement the same standardized models and state bindings.

For occupancy you *could* use a richer model than Generic OnOff. The **Sensor Server** model (Mesh Model 1.1 §4) carries typed readings: each sensor property has a 16-bit Property ID (from the SIG's GATT-characteristic-derived list), a value formatted per that property's specification, and optional cadence (publish-on-change-threshold) settings. An occupancy sensor's natural property is "Presence Detected" (a boolean property) or "People Count" (a count). The Sensor model is the right choice for a deployment that wants typed, self-describing data a generic dashboard can render without per-device code. We use Generic OnOff in the mini-project because it is the simplest model that demonstrates publish/subscribe and relaying end-to-end — but in the homework's gateway problem, mapping a Sensor model reading to an MQTT JSON payload is the more realistic exercise, and you should understand that Generic OnOff is the teaching simplification, not the production choice.

The **publication** mechanism (Mesh Model 1.1 §3.4) is also richer than "send on change." A model's publication can be **periodic** (every N seconds, with N set as a Publish Period during configuration) and can carry a **retransmit** count/interval to fight the advertising bearer's packet loss. An occupancy sensor typically publishes on change *and* periodically (a heartbeat so subscribers can detect a dead sensor by its silence), with the period set by the configurator, not hardcoded. BTstack's generic-on-off server publishes when you call `mesh_access_state_changed` *and* on the configured period; the mini-project's publisher relies on the change-triggered path.

## 8.7. Publish/subscribe vs unicast: why the group address matters

It is worth contrasting the two ways a mesh message can be addressed, because the choice shapes how a fleet scales. A model can publish to a **unicast** address (one specific element) or to a **group** address (everyone subscribed). The mini-project uses a group (`0xC000`), and the reason is decoupling: the occupancy publisher does not know — and does not need to know — which nodes care about its state. It publishes to `0xC000`; whoever subscribed gets it. Add a fourth light next month? Subscribe it to `0xC000` and it works, with zero change to the publisher. Remove a subscriber? The publisher never notices. The group address is an indirection that lets the set of subscribers change without touching the publisher, exactly as an MQTT topic decouples publishers from subscribers in the broker world you have already seen.

Unicast addressing is the opposite: tight coupling, used when a controller wants to address one specific device ("turn on *that* light"). A wall switch wired to one fixture publishes to that fixture's unicast address. The trade is flexibility vs precision: group addressing scales the fleet effortlessly but broadcasts to everyone subscribed; unicast is surgical but requires the sender to know the recipient's address. Most real meshes use both — group addresses for "all the lights in this room respond to this switch," unicast for "the commissioning tool reconfigures this one node." Recognizing which addressing a message uses (the DST in the network header: `0xC000`-range is a group, `0x0001`–`0x7FFF` is a unicast) is the first thing you read off a mesh capture, and it tells you immediately whether you are looking at fleet-wide pub/sub traffic or a point-to-point command.

## 9. Relay and TTL: bounding the flood

This is the heart of "managed flooding" — the mechanism that keeps a flood from melting the network — so it earns the most careful treatment, and it is exactly what you prototyped in Exercise 3.

Each Network PDU carries a 7-bit **TTL** (0–127). The originating node sets it (BTstack's default is 7); each relay decrements it by one before re-broadcasting; a message with TTL 0 or 1 is delivered locally but **not** relayed (Mesh Protocol 1.1 §3.4.6.3). This is the arithmetic from Exercise 3. TTL is what makes a flood terminate: without it, a message circulates forever across the relays.

The **relay feature** is a per-node flag set during configuration. A node with relay *off* receives messages addressed to it but never re-broadcasts; a node with relay *on* re-broadcasts every message it hears (subject to the TTL and the dedup cache). Tuning which nodes relay is a real deployment decision: too few relays and the network is a star with dead spots; all relays and the network is a full flood that wastes air-time and battery. Mains-powered nodes (lights) relay; battery nodes (sensors) usually do not.

A node also tunes its **relay retransmit count** and **interval** — how many times and how often it re-broadcasts each message, to overcome the ~2–5% packet loss of a single advertising broadcast. Three retransmits at 20 ms is typical.

The **sequence-number cache** (the replay/dedup cache from Exercise 3) does double duty: it deduplicates the flood (a node hears the same message from several relays and processes it once) *and* it defeats replay attacks (an attacker who records a "turn on" message and re-broadcasts it later is rejected, because its (SRC, SEQ) is stale). The cache is keyed by source unicast address; each entry is the highest SEQ seen from that source, paired with the current IV index (Mesh Protocol 1.1 §3.8.8). Running out of SEQ space (24 bits ≈ 16 million messages) triggers the **IV Update procedure** to roll the IV index and reset the space — a production concern we name but do not implement.

## 10. The occupancy fleet, end to end

Putting it together for the mini-project (full brief in `mini-project/README.md`):

1. Three Pico W nodes flash the same `mesh_node.c`, built three times with `-DNODE_ROLE=PUBLISHER`, `=SUBSCRIBER`, `=RELAY` (the relay is also a subscriber so it lights its own LED).
2. `cc-mesh-provision.py` provisions all three: unicast addresses `0x0002`/`0x0003`/`0x0004`, a shared NetKey and AppKey, the publisher's Generic OnOff Server publishing to `0xC000`, the two subscribers' clients subscribed to `0xC000`, and the relay node's relay feature **on** (the publisher's and far subscriber's relay features **off**).
3. Physically place the publisher and the far subscriber out of direct radio range (different rooms, or attenuate with distance/walls), with the relay between them.
4. Trip the PIR. The publisher sends `Generic OnOff Status(ON)` to `0xC000` with TTL 7. The near subscriber (in range) gets it directly. The far subscriber is out of range — but the relay heard it, decremented TTL to 6, and re-broadcast it; the far subscriber gets the relayed copy and lights up.
5. A sniffer confirms: you see the publisher's broadcast (TTL 7) and, ~20 ms later, the relay's re-broadcast (TTL 6) of the same SEQ. Set the publisher's TTL to 1 and the far LED goes dark — direct proof that the relay was load-bearing.

That relayed LED two hops from the PIR is the artifact of the week. It proves you understand flooding, TTL, provisioning, and the publish/subscribe model — the whole mesh, working on the same Pico W you have used since Week 1.

## 9.5. Composition Data: how a provisioner learns what a node is

After provisioning but before configuration, the provisioner asks "what are you?" by reading the node's **Composition Data Page 0** (Mesh Protocol 1.1 §4.2.1) from its Configuration Server. This is the node's self-description:

```text
CID  (2 bytes)  Company Identifier (Bluetooth SIG-assigned)
PID  (2 bytes)  Product Identifier (vendor-defined)
VID  (2 bytes)  Version Identifier
CRPL (2 bytes)  the size of the Replay Protection List
Features (2 bytes)  a bitfield: Relay | Proxy | Friend | Low Power
Elements[]  for each element:
    Location (2 bytes)
    NumS (1 byte)  count of SIG models
    NumV (1 byte)  count of vendor models
    SIG model IDs (2 bytes each)
    Vendor model IDs (4 bytes each)
```

The **Features** bitfield is the load-bearing field for our mini-project: it advertises whether the node *can* relay (bit 0), proxy (bit 1), be a friend (bit 2), or be a low-power node (bit 3). The relay build of `mesh_node.c` (compiled with `ENABLE_MESH_RELAY` in its `btstack_config.h`) sets the Relay bit; the publisher and far-subscriber builds clear it. The provisioner reads this, sees which nodes are relay-capable, and only enables the relay feature where the hardware reports it can — you cannot turn relaying on for a node that did not compile it in. The Elements list tells the provisioner each element's models (Generic OnOff Server `0x1000` on the publisher, Client `0x1001` on the subscribers) so it knows which model to bind the AppKey to and which to set publication/subscription on. Composition Data is, in effect, the mesh's answer to GATT service discovery: a structured "here is everything I can do" the configurator reads before it configures.

## 9.6. The replay-protection list and IV index, in depth

Exercise 3 gave you a toy per-source SEQ cache; production mesh formalizes it as the **Replay Protection List (RPL)** (Mesh Protocol 1.1 §3.8.8). Each entry is `(SRC, last-seen SEQ, IV index)`; a node must store enough entries to cover every source it might hear (the CRPL field in Composition Data advertises this capacity). When a message arrives, the node looks up its SRC: if the message's `(IV index, SEQ)` is not strictly greater than the stored value, the message is a replay or a duplicate and is dropped *before* it is delivered to any model or relayed. This is what makes BLE Mesh resistant to the trivial "record the unlock message, replay it later" attack that plagues naive RF systems — the recorded message's SEQ is stale the instant it is recorded.

The 24-bit SEQ space (≈16.7 million) sounds large, but a chatty node publishing several times a second exhausts it in months, and a node *must not* reuse a SEQ under the same IV index (reuse breaks the AES-CCM nonce uniqueness, which catastrophically weakens the encryption). The **IV Update procedure** (Mesh Protocol 1.1 §3.10.5) rolls the network-wide IV index before exhaustion, resetting every node's usable SEQ space. The IV index is part of the AES-CCM nonce and is delivered at provisioning time and refreshed via Secure Network Beacons. We run a static mesh that never approaches SEQ exhaustion, so we provision IV index 0 and leave it — but the RPL is still live in every node (it is what dedups the flood), and you must size it for the number of sources, which is why the publisher's, subscriber's, and relay's `btstack_config.h` sets a sufficient `MESH_NODE_MAX_REPLAY_LIST_SIZE`.

## 10.5. The two-MIC encryption, walked byte by byte

The exercise gave you the cleartext network header; here is what the crypto layer does to it before it hits the air, because you will need to read it in a sniffer. A mesh access message is encrypted **twice** (Mesh Protocol 1.1 §3.8):

1. **Upper Transport encryption (AppKey).** The access payload (opcode + parameters, e.g. `82 04 01` for OnOff Status ON) is encrypted with AES-CCM under the **AppKey**, producing the ciphertext plus a **TransMIC** (4 bytes for an access message). This is *end-to-end*: only nodes holding the AppKey can read it. The AES-CCM nonce mixes in the SEQ, SRC, DST, and IV index, so the same plaintext encrypts differently every message — and a replayed message fails the MIC check at the right SEQ.
2. **Network encryption (NetKey).** The lower-transport PDU (the upper-transport ciphertext + TransMIC, possibly segmented) plus the DST address are encrypted with AES-CCM under the **EncryptionKey** derived from the NetKey (the k2 derivation your provisioner implements), producing a **NetMIC** (4 bytes for an access message, 8 for a control message). Then the first two header bytes (IVI/NID, CTL/TTL, and the SEQ/SRC) are **obfuscated** with a keystream from the **PrivacyKey** (also from k2) so a passive observer cannot even correlate messages by source address.

So a node relaying a message it cannot read does this: de-obfuscate the header with the PrivacyKey (it has the NetKey, so it can), verify and decrypt the network layer to recover the TTL and DST, check the replay cache, decrement the TTL, re-encrypt the network layer, re-obfuscate, and re-broadcast — all **without ever touching the AppKey**, so it never sees the `82 04 01` payload. That is the precise mechanism behind Lecture-3's claim that you can trust a relay you do not trust with the data: the NetKey/AppKey split makes "forward it" and "read it" two different capabilities. In a sniffer, a message you have the NetKey for shows the de-obfuscated header (you can see SRC, TTL, SEQ) but an opaque payload unless you also have the AppKey — exactly mirroring what a relay sees.

The `derive_nid_encryption_privacy` (k2) and `app_key_id` (k4) functions in `cc-mesh-provision.py` compute the NID, EncryptionKey, PrivacyKey, and AID that this whole scheme keys on; run the provisioner's self-test (`SOLUTIONS`-style) and you will see the 7-bit NID and 6-bit AID that appear in the network and transport headers of every message your mesh sends.

## 10.6. A worked provisioning trace

When `cc-mesh-provision.py provision-all` runs against one node, the PB-GATT exchange in a sniffer (or the tool's verbose log) reads:

```text
-> Provisioning Invite        (attention 5 s; node blinks to identify)
<- Provisioning Capabilities  (1 element, No-OOB available, P-256 public key)
-> Provisioning Start         (algo P-256, No-OOB auth)
-> Provisioning Public Key    (provisioner's 64-byte X||Y)
<- Provisioning Public Key    (device's 64-byte X||Y)   [both compute ECDHSecret]
-> Provisioning Confirmation   (AES-CMAC over provisioner random + auth value)
<- Provisioning Confirmation   (device's confirmation)
-> Provisioning Random         (provisioner reveals its random)
<- Provisioning Random         (device reveals its random; both verify confirms)
-> Provisioning Data           (AES-CCM: NetKey||KeyIdx||Flags||IVIndex||Unicast)
<- Provisioning Complete
```

Every line maps to a function in the provisioner: `_prov_invite`, the capabilities parse, `_prov_start_no_oob`, `_prov_public_key` + `ecdh_shared_secret`, `_prov_confirmation`, `_prov_random`, then `build_provisioning_data` + `encrypt_provisioning_data` + `_prov_data`. The structural similarity to BLE pairing (Lecture 2) is not a coincidence — both are ECDH + an OOB-authenticated confirmation, because both solve "agree on a secret with a stranger over a channel an attacker can hear." Once the Provisioning Data PDU lands, the node has the NetKey and a unicast address and is a member of the network; configuration (AppKey, pub/sub, relay flag) follows over the now-encrypted mesh.

## 10.7. The GATT bearer, the proxy, and why phones need it

There is a wrinkle in "mesh runs on advertisements": a phone cannot continuously scan raw mesh advertisements while also doing everything else a phone does, and iOS in particular does not expose the raw advertising-bearer APIs an app would need. So mesh defines a second bearer, the **GATT bearer** (Mesh Protocol 1.1 §6.5), and a node feature called **Proxy**. A proxy node exposes a GATT service (the Mesh Proxy Service `0x1828`) with two characteristics — Data In and Data Out — over which a phone, using ordinary GATT, tunnels mesh PDUs. The proxy node receives those PDUs over GATT, injects them into the advertising-bearer flood (and vice versa), bridging the phone into the mesh without the phone ever touching the advertising bearer.

This is also how **PB-GATT** provisioning works (the bearer our `cc-mesh-provision.py` uses): the unprovisioned node exposes the Mesh Provisioning Service `0x1827`, the provisioner connects over GATT and runs the provisioning PDUs through the Data In/Out characteristics — which is exactly the `_pb_gatt_write_pdu` / `_pb_gatt_read_pdu` pair in the provisioner. PB-ADV (provisioning straight over advertisements) is the alternative used by a provisioner that *can* drive the advertising bearer (a dedicated gateway, an nRF dongle). On a laptop with `bleak`, GATT is the path of least resistance, so the mini-project provisions over PB-GATT.

The trade-off is duty-cycling. A proxy node holding a GATT connection to a phone time-slices its radio between servicing that connection and scanning the mesh advertising bearer, which adds latency to mesh messages passing through it. The homework's gateway problem hits this directly: a single CYW43439 doing BLE mesh *and* Wi-Fi MQTT is time-slicing three ways, and you must reason about the latency budget. For the mini-project's three always-on nodes provisioned once and then left alone, the proxy is only used during the provisioning ceremony; steady-state occupancy traffic is pure advertising bearer.

## 10.8. Why managed flooding instead of routing

It is worth stating plainly why BLE Mesh chose flooding over the routing meshes (Zigbee, Thread) it competes with. Routing meshes maintain a routing table — each node knows the next hop toward each destination — which is efficient (a message travels one path, not a flood) but fragile (the table must be discovered, maintained, and repaired when a node dies, and the repair is a protocol unto itself). Flooding has no table: a message simply floods, and the network self-heals around a dead relay automatically because there was never a path to repair. The cost is air-time — one message hits the air many times — which is acceptable for the low-rate control traffic mesh targets (a light switch, an occupancy sensor) and unacceptable for high-rate data (which is why you would not stream video over BLE Mesh).

The engineering lesson generalizes beyond BLE: flooding trades bandwidth for robustness and simplicity. When messages are small and infrequent and the network must survive node death with zero configuration, flooding wins. When bandwidth is precious and the topology is stable, routing wins. BLE Mesh bet that lighting and sensing fleets value robustness and zero-config self-healing over bandwidth, and for that domain the bet was right — which is why it is a real alternative to Zigbee for building-scale lighting and sensing, the use case the mini-project models in miniature.

## 11. Out of scope but worth naming

- **Key Refresh and IV Update.** A production mesh rotates the NetKey periodically (Mesh Protocol 1.1 §3.10.4) and increments the IV index before the SEQ space exhausts (§3.10.5). We run a static mesh; these are a production course's job.
- **Friendship and Low Power nodes.** A battery node can be a **Low Power Node** that sleeps and polls a mains-powered **Friend** node which caches messages for it (Mesh Protocol 1.1 §3.6.4). This is how mesh supports battery sensors despite the always-listening flood. We run all three nodes always-on.
- **Proxy.** The **GATT Proxy** lets a phone (which cannot scan raw mesh advertisements while doing other work) talk to the mesh over a GATT connection (Mesh Protocol 1.1 §6.6). We provision with PB-ADV from a Pico-attached host, so we do not need the proxy.

## 11.5. Provisioning security on the bench vs in the field

The mini-project provisions with **No-OOB** authentication (the AuthValue is 16 zero bytes), which means the ECDH defeats a *passive* sniffer of the provisioning but not an *active* man-in-the-middle present during the ceremony. That is the same Just-Works trade you saw in BLE pairing, and it is acceptable on a bench where you control the RF environment. In the field it is not: an attacker who is present at provisioning time can MITM the ECDH and own the node.

The field-grade fix is **OOB authentication**: each node ships with a static OOB value (a number printed on a label, or output-OOB where the node blinks a number you count, or input-OOB where you type a number into the node), and that value is mixed into the provisioning confirmation so a MITM cannot complete the ceremony without it. A production occupancy fleet would print a per-device OOB code on each sensor and have the installer scan or type it during commissioning. We use No-OOB to keep the provisioner readable and the bench setup fast, and we name the gap loudly — the same honesty Week 10 applied to its "the bootloader's public key is in flash, not OTP" caveat. Provisioning is the mesh's root of trust; weakening it (as we do for teaching) weakens everything above it, and you should know exactly what you traded away.

A second field concern is the **provisioner itself**. Whoever provisions a node hands it the NetKey — so the provisioner is a high-value target, and in production it runs on a hardened device (a commissioning app with the keys in a secure enclave), not a laptop with `cc-mesh-db.json` in cleartext. Our `cc-mesh-db.json` stores the NetKey and AppKey in plaintext JSON, which is fine for a bench and catastrophic for a deployment. Treat it the way Week 10 treated the Ed25519 private key: `chmod 600`, never commit it, and understand that a leak compromises the whole network.

## 11.7. Debugging a mesh: what a sniffer shows and what it does not

Mesh debugging deserves its own note because it is the hardest part of the week. A sniffer on the advertising channels shows you every Mesh Message PDU (`ADV_NONCONN_IND` with AD type `0x2A`) on the air. With the NetKey loaded into Wireshark's mesh decryption settings, you see the de-obfuscated network header — SRC, DST, TTL, SEQ — for every message, which is enough to answer the questions that matter:

- **"Is the publisher sending?"** Look for a Mesh Message with SRC = the publisher's unicast and DST = `0xC000`. If it is not there, the publisher's model publication is misconfigured (no publish address bound) — a Configuration problem, not a radio problem.
- **"Is the relay relaying?"** Look for a *second* Mesh Message, same SEQ, same SRC, TTL one lower, ~20 ms after the first. If the first is there but the second is not, the relay feature is off (a Config Relay Set you skipped) or the relay is out of range of the publisher.
- **"Why is the far subscriber dark?"** If the relayed (TTL-6) message is on the air near the subscriber but the LED stays off, the subscriber's model subscription to `0xC000` is missing or the AppKey is not bound — again Configuration, visible as the subscriber simply ignoring a message it should consume.

What the sniffer does *not* show without the AppKey is the access payload — you see that *a* message went from SRC to `0xC000` with TTL 6, but not that it was a Generic OnOff Status ON, unless you also load the AppKey. This is the NetKey/AppKey split made tangible: with only the NetKey (the relay's view) you can trace the flood and the TTL but not read the command; with the AppKey too (the subscriber's view) you read everything. Load both into Wireshark for full visibility, and you have the mesh equivalent of the fully-decoded SPI trace from Week 8 — every message legible, the flood and the relay and the TTL all on screen. The single highest-leverage habit of the mini-project is to keep this capture running while you bring up the fleet; almost every stall reduces to "the message is/ isn't on the air" or "the message is on the air but the node ignores it," and the sniffer answers both in seconds.

## 12. Summary

The week's two big ideas sit side by side here: BLE 5 made connection-oriented BLE faster, longer-range, and broadcast-capable; BLE Mesh built a whole different network on the same radio by abandoning connections for flooding.

BLE 5 gave you three knobs: 2M (half the air-time, less range), Coded (quadruple the range, far less throughput, query the controller before using it), and extended/periodic advertising (big connectionless broadcast). BLE Mesh is a separate world built on non-connectable advertisements: managed flooding with no master and no routing, a two-key (NetKey/AppKey) security model that lets untrusted relays forward encrypted payloads, unicast/group/virtual addressing, an ECDH provisioning ceremony, a model layer that mirrors GATT services, and a TTL-plus-sequence-cache mechanism that bounds the flood and defeats replay. The mini-project's relayed occupancy LED is all of it working at once.

## References for this lecture

- Core Spec 5.4 Vol 6 Part A §3 (LE 2M and Coded PHYs). <https://www.bluetooth.com/specifications/specs/core-specification-5-4/>
- Core Spec 5.4 Vol 6 Part B §2.3.4 (extended advertising PDUs), §4.4.2.4 (periodic advertising), §4.6 (LE features).
- Bluetooth Mesh Protocol 1.1 §3 (network architecture, addressing, relay, replay protection), §5 (foundation models), §6 (provisioning). <https://www.bluetooth.com/specifications/specs/mesh-protocol-1-1/>
- Bluetooth Mesh Model 1.1 §3 (Generic models), §7.1 (Generic OnOff messages).
- Bluetooth SIG, "Bluetooth Mesh Networking — An Introduction for Developers". <https://www.bluetooth.com/bluetooth-resources/bluetooth-mesh-networking-an-introduction-for-developers/>
- BTstack manual, "Mesh". <https://bluekitchen-gmbh.com/btstack/>
- Pico W connectivity guide, Ch. 4. <https://datasheets.raspberrypi.com/picow/connecting-to-the-internet-with-pico-w.pdf>
