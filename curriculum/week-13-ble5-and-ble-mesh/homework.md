# Homework — Week 13

Six problems. Estimated total time: ~6 hours over the week. Submit answers as commits to your fork of the course repo; each problem has a designated file path under `homework/week-13/`. The grading rubric is at the end.

---

## Problem 1 — Decode a real advertising capture (1 hour)

Capture 30 seconds of BLE advertising near you (nRF Connect's scanner log, or an nRF Sniffer Wireshark capture). Pick **three** different advertisers — ideally a phone, a wearable, and a beacon or appliance.

Write `homework/week-13/adv-decode.md` that, for each advertiser:

1. Lists every AD structure (length, type, data) byte-by-byte from the raw payload.
2. Names each AD type from the Assigned Numbers and decodes its value (Flags bits, the name, the service UUID list, manufacturer data with the Company Identifier resolved).
3. Identifies the advertising PDU type (`ADV_IND` / `ADV_NONCONN_IND` / `ADV_EXT_IND` / …) and what that implies (connectable? a beacon? extended?).
4. For any manufacturer-specific data, identifies the company from the first two bytes (Assigned Numbers, Company Identifiers) and, if it is an iBeacon or Eddystone, decodes the beacon fields.

Cross-check your hand decode against your Exercise 1 parser by feeding it the raw bytes. The deliverable is ~300 lines with the raw hex inlined.

---

## Problem 2 — Write a `.gatt` file and predict the attribute table (1 hour)

Design a GATT server for a **plant monitor** with: soil moisture (read + notify, `uint16`), light level (read + notify, `uint16`), a pump-control characteristic (write, `uint8`), and the standard Battery Service.

Write `homework/week-13/plant_monitor.gatt` (the BTstack `.gatt` source) and `homework/week-13/attribute-table.md` that:

1. Hand-predicts the full attribute table `compile_gatt.py` will generate: every handle, type/UUID, and value, including the CCCDs that get auto-added for the notify characteristics.
2. Marks which characteristics are `DYNAMIC` (served by a callback) vs static.
3. States, for each notify characteristic, which handle the client writes `0x0001` to and which handle the server passes to `att_server_notify`.

If you have a Pico W, build it and diff your predicted table against the generated `cc_*.h`. Cite Core Spec Vol 3 Part G §3 throughout.

---

## Problem 3 — Reproduce the GATT discovery sequence on the wire (1 hour)

Using your Exercise 2 GATT client (or nRF Connect) against any peripheral, capture the connection with an nRF Sniffer.

Write `homework/week-13/gatt-discovery.md` that walks the ATT exchange:

1. The `Exchange MTU Request`/`Response` and the negotiated MTU.
2. The `Read By Group Type Request` (type `0x2800`) for primary service discovery and the `Response` listing services and handle ranges.
3. The `Read By Type Request` (type `0x2803`) for characteristic discovery and the `Response`.
4. The `Read Request`/`Response` for a value, or the `Write Request` to a CCCD, and the subsequent `Handle Value Notification`s.

For each ATT PDU, give the opcode (from `ble_common.h`), the handle(s), and the value, annotated against Core Spec Vol 3 Part F §3.4. The deliverable is the annotated capture as ~250 lines of markdown with the PDU hex.

---

## Problem 4 — The I/O-capability matrix and association-model selection (30 minutes)

Core Spec Vol 3 Part H §2.3.5.1, Table 2.8 maps the two devices' I/O capabilities (plus the OOB and MITM flags) onto the four association models (Just Works, Passkey Entry, Numeric Comparison, OOB).

Write `homework/week-13/io-capability-matrix.md` that:

1. Reproduces the table for Secure Connections, with the initiator's capability on one axis and the responder's on the other, each cell naming the resulting association model.
2. For each of these device pairs, states the model and whether it gives MITM protection:
   - Pico W (DisplayYesNo) ↔ iPhone (DisplayYesNo)
   - Pico W (NoInputNoOutput) ↔ iPhone (DisplayYesNo)
   - Smart lock (Display only) ↔ phone (Keyboard + Display)
   - Two sensors (both NoInputNoOutput)
3. Explains why "both NoInputNoOutput" can only ever be Just Works, and what that means for the security of such a link.

Cite the Core Spec table directly.

---

## Problem 5 — Bridge the mesh occupancy state to MQTT (1.5 hours)

The mini-project stops at the mesh. A real fleet forwards occupancy to a building-management system over MQTT (which you brought up earlier in Phase III).

Design and partially implement a **gateway node** that subscribes to the mesh group `0xC000` and republishes the OnOff state to an MQTT topic on the Pico W's Wi-Fi radio.

Write `homework/week-13/gateway/` containing:

1. `gateway_design.md` — the architecture: how one CYW43439 runs the BLE mesh stack *and* the Wi-Fi/lwIP/MQTT stack at once (the SDK's `server_with_wifi` example shows the coexistence pattern), and the topic design (e.g. `building/floor3/room12/occupancy`).
2. `mesh_to_mqtt.c` — the gateway firmware: a Generic OnOff Client subscribed to `0xC000` whose status handler publishes `{"occupied": 0|1, "src": "0x0002", "ts": ...}` to the MQTT topic. You may stub the Wi-Fi credentials and broker address as `#define`s. The mesh-side handler is the load-bearing part; reuse `mesh_node.c`'s `on_off_status_handler` as the template.
3. A note on **coexistence**: the BLE and Wi-Fi share one radio; document the duty-cycling implication (you cannot scan mesh advertisements continuously while also doing TLS handshakes — the controller time-slices) and how the GATT-proxy/duty-cycle settings affect mesh message latency through the gateway.

Cite the mini-project, the Mesh Model 1.1 Generic OnOff section, and your earlier MQTT work. Full Wi-Fi bring-up is optional; the mesh→MQTT bridge logic is required.

---

## Problem 6 — Measure the cost of relaying (1 hour)

Using the mini-project mesh, quantify what relaying costs.

Write `homework/week-13/relay-cost.md` that measures and reports:

1. **Latency.** With a sniffer, measure the time from the publisher's broadcast to the relay's re-broadcast, and from the re-broadcast to the far subscriber's LED. Report the per-hop added latency (it is dominated by the relay's retransmit interval, default ~20 ms, plus processing).
2. **Air-time.** Compute the air-time of one Mesh Message advertisement on 1M, and multiply by the relay's retransmit count (default 3) to get the air-time *one relay* spends per message. Extrapolate to a 20-node all-relay network: how many times does one occupancy event hit the air?
3. **The all-relay anti-pattern.** Explain why enabling relay on every node in a dense mesh is wasteful (the flood multiplies) and why the usual rule is "mains-powered nodes relay, battery nodes do not." Cite Mesh Protocol 1.1 §3.6.
4. **TTL tuning.** Argue for the right default TTL for a single-room occupancy fleet (small) vs a multi-floor building fleet (larger), trading reach against flood size.

The deliverable is ~250 lines with your sniffer measurements and the air-time arithmetic.

---

## Submission

Commit all six problems to your fork under `homework/week-13/`. Tag the commit `week-13-homework`.

```bash
git add homework/week-13/
git commit -m "week 13 homework: adv decode, gatt table, discovery, IO matrix, mesh-to-mqtt, relay cost"
git tag week-13-homework
git push origin main --tags
```

The teaching team reviews homework asynchronously; expect feedback within ~5 business days.

---

## Grading rubric

Each problem is graded out of the points shown; total 100.

| Problem | Points | What earns full marks |
|---------|-------:|-----------------------|
| 1 — Advertising decode | 18 | All three advertisers fully decoded byte-by-byte; AD types and PDU types correctly named; manufacturer data resolved to a Company Identifier; cross-checked against the Exercise 1 parser. |
| 2 — `.gatt` + attribute table | 18 | A valid `.gatt`; the predicted attribute table is complete and correct (handles, types, values, auto-added CCCDs); DYNAMIC vs static marked; the notify handles identified. (−4 if the auto-added CCCDs are missing.) |
| 3 — Discovery on the wire | 16 | The full discovery sequence captured and annotated with opcodes, handles, and values against the Core Spec; MTU negotiation shown; at least one notification captured. |
| 4 — I/O-capability matrix | 12 | The Secure Connections matrix reproduced correctly; all four device pairs assigned the right model and MITM verdict; the NoInputNoOutput → Just Works reasoning correct. |
| 5 — Mesh→MQTT bridge | 22 | A coherent coexistence design; `mesh_to_mqtt.c` with a correct Generic OnOff Client status handler that forms the MQTT publish; a sound topic design; the radio-coexistence/duty-cycle implication explained. (Full Wi-Fi bring-up not required; the bridge logic and the design are.) |
| 6 — Relay cost | 14 | Real sniffer latency measurements; correct air-time arithmetic and the 20-node extrapolation; the all-relay anti-pattern and the mains-vs-battery rule explained; defensible TTL recommendations. |

**Deductions across all problems:** −2 per missing or wrong Core Spec / Mesh Protocol citation where one is required; −3 for any code that would not compile (Problem 5's `mesh_to_mqtt.c` must at least compile against the SDK, stubs allowed). **Bonus +5:** a working end-to-end mesh→MQTT bridge (Problem 5) with a committed broker capture showing the occupancy messages arriving.

---

## References for the homework set

- Core Spec 5.4 Vol 3 Part C §11 (advertising data), Part F §3 (ATT), Part G §3 (GATT), Part H §2.3.5.1 (I/O-capability matrix). <https://www.bluetooth.com/specifications/specs/core-specification-5-4/>
- Bluetooth SIG Assigned Numbers (AD types, Company Identifiers, UUIDs). <https://www.bluetooth.com/specifications/assigned-numbers/>
- Bluetooth Mesh Protocol 1.1 §3.6 (relay). <https://www.bluetooth.com/specifications/specs/mesh-protocol-1-1/>
- Bluetooth Mesh Model 1.1 §3.2, §7.1 (Generic OnOff).
- BTstack manual, "GATT Server", "Mesh". <https://bluekitchen-gmbh.com/btstack/>
- Pico SDK `pico-examples/pico_w/bt/standalone/server_with_wifi` (BLE + Wi-Fi coexistence).
