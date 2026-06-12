# Mini-Project — A BLE Mesh Occupancy Fleet with a Relayed Hop

## Brief

Build a three-node BLE Mesh network on Pico W boards. One node hosts a PIR occupancy sensor and publishes a Generic OnOff state when it detects motion; the other two subscribe to that state and drive an LED. The catch — and the whole point — is that the publisher and one subscriber are placed **out of direct radio range** of each other, so the message only arrives via the third node acting as a **relay**. You provision all three nodes with a host-side Python tool, place the boards so the relay is load-bearing, trip the PIR, and watch an LED two hops away light up. A sniffer confirms the relay re-broadcast the message with the TTL decremented.

End-to-end demonstration: power three Pico W boards (all unprovisioned, LEDs on). Run `cc-mesh-provision.py provision-all`; the three LEDs go out as each is provisioned. Run `cc-mesh-provision.py configure` and apply the pub/sub/relay configuration. Walk the publisher into the next room (the relay stays in the doorway). Wave your hand at the PIR. The far subscriber's LED lights within ~100 ms. Set the publisher's TTL to 1 and the far LED stays dark — proof the relay was carrying the message.

Allocate 6 hours for this mini-project.

## Deliverables

In `mini-project/` directory of your fork:

- `mesh_node.c` — the node firmware (publisher / subscriber / relay, selected at build time with `-DNODE_ROLE`).
- `gatt_sensor_peripheral.c` — the standalone GATT peripheral used in Step 2 to validate the GATT path before any mesh.
- `cc_sensor.gatt` — the GATT database for the Step-2 peripheral (`compile_gatt.py` input).
- `ble_common.h` — the shared header (the same file from `exercises/`).
- `btstack_config.h` — the BTstack feature-flag header (one for the GATT build, one for the mesh build; see the gotchas).
- `CMakeLists.txt` — builds `gatt_sensor_peripheral.uf2` plus the three mesh roles `mesh_publisher.uf2`, `mesh_subscriber.uf2`, `mesh_relay.uf2`.
- `cc-mesh-provision.py` — the host-side provisioner/configurator.
- `requirements.txt` — Python deps (`bleak`, `cryptography`).
- `Makefile` — convenience wrapper for build + provision + configure.

## The architecture

```text
        +-------------------+                    +-------------------+
        | PUBLISHER 0x0002  |                    | SUBSCRIBER 0x0003 |
        |  PIR on GP16      |   X   no direct    |  LED follows      |
        |  OnOff Server     |  <-X->  link       |  group 0xC000     |
        |  pub -> 0xC000    |       (out of range)| OnOff Client     |
        |  relay: OFF       |                    |  relay: OFF       |
        +---------+---------+                    +---------+---------+
                  |                                        ^
                  | ADV_NONCONN_IND (Mesh Message)         |
                  | DST=0xC000  TTL=7                       |
                  v                                        |
        +-------------------+   re-broadcast (TTL 7->6)     |
        | RELAY 0x0004      |-------------------------------+
        |  LED follows      |
        |  group 0xC000     |   (also a subscriber: lights its own LED)
        |  OnOff Client     |
        |  relay: ON        |
        +-------------------+

Host laptop:  cc-mesh-provision.py
   provision-all : ECDH provisioning -> each node gets unicast + NetKey
   configure     : AppKey bind, pub/sub addresses, relay flag
```

## Build prerequisites

You have:

- Pico SDK 1.5.1 or later with `PICO_SDK_PATH` set, built for `-DPICO_BOARD=pico_w`.
- `arm-none-eabi-gcc` 10.3 or later, CMake, `picotool`.
- **Three Pico W boards** (the relayed hop needs three nodes).
- A PIR sensor (HC-SR501 or similar, 3.3 V logic) wired to GP16 on the publisher board.
- Python 3.10+ with `bleak>=0.21` and `cryptography>=41.0` (the provisioner runs on a host with a Bluetooth adapter).
- A BLE sniffer (nRF Sniffer) to confirm the relay — strongly recommended.

## Step-by-step build

### Step 1 — Validate your toolchain on the SDK's BLE example

Before writing a line, build and flash the SDK's `picow_ble_temp_sensor` and confirm it advertises in nRF Connect. If the SDK's own BLE example does not build, fix that first — it is a toolchain problem, not a you problem, and it is far easier to diagnose against known-good code.

### Step 2 — Bring up the standalone GATT peripheral

Build `gatt_sensor_peripheral.c`. It needs `cc_sensor.gatt` compiled to `cc_sensor.h`:

```bash
cd mini-project
mkdir -p build && cd build
cmake -G "Unix Makefiles" .. -DPICO_BOARD=pico_w
make -j8 gatt_sensor_peripheral
```

The `CMakeLists.txt` calls `pico_btstack_make_gatt_header(gatt_sensor_peripheral cc_sensor.gatt)` so the generated `cc_sensor.h` is on the include path. Flash, then with nRF Connect:

1. Confirm "CC Occupancy" advertises the custom 128-bit service.
2. Connect; confirm the occupancy characteristic (READ | NOTIFY) and the LED-control characteristic (WRITE).
3. Subscribe to occupancy; wave at the PIR; confirm a notification arrives.
4. Write `0x01` to the LED-control characteristic; confirm the onboard LED lights.

This proves your GATT server, your PIR wiring, and your LED control all work. The mesh node reuses this exact occupancy-sensing and LED-driving code with a mesh model instead of a GATT characteristic.

### Step 3 — Build the three mesh roles

The mesh build needs a `btstack_config.h` with `ENABLE_MESH`, `ENABLE_MESH_ADV_BEARER`, `ENABLE_MESH_GATT_BEARER` (for PB-GATT provisioning), and — on the relay build only — `ENABLE_MESH_RELAY`. The `CMakeLists.txt` builds three targets from the one source:

```bash
make -j8 mesh_publisher mesh_subscriber mesh_relay
```

Each compiles `mesh_node.c` with a different `-DNODE_ROLE` and links `pico_btstack_mesh`. Outputs: `mesh_publisher.uf2`, `mesh_subscriber.uf2`, `mesh_relay.uf2` (~120 KB each).

### Step 4 — Flash and identify the boards

Flash `mesh_publisher.uf2` to the board with the PIR on GP16, `mesh_subscriber.uf2` to a second board, `mesh_relay.uf2` to the third. Label them physically. On boot each prints its role over USB CDC and lights its LED solid (the "unprovisioned, waiting" indicator):

```text
=== CC Mesh Node: role=PUBLISHER ===
[setup] role=PUBLISHER, Generic OnOff Server on element 0
```

### Step 5 — Provision the mesh

From the host:

```bash
python3 cc-mesh-provision.py provision-all
```

The tool scans for the three Unprovisioned Device beacons (it matches the `0xCC 0x7E 0x13` Device-UUID tag and reads the role from the last byte), assigns unicast addresses `0x0002`/`0x0003`/`0x0004`, and runs PB-GATT provisioning against each: ECDH P-256 key exchange, No-OOB authentication, and the encrypted Provisioning Data PDU carrying the shared NetKey and the unicast address. As each completes, that board's LED goes out and it prints:

```text
[mesh] PROVISIONED. We now have a unicast address and the NetKey.
```

The NetKey and AppKey are generated once and saved in `cc-mesh-db.json`; re-running provisions only nodes still unprovisioned.

### Step 6 — Configure pub/sub and the relay

```bash
python3 cc-mesh-provision.py configure
```

This prints (and records) the Configuration messages each node needs: AppKey add, Generic OnOff model app-bind, the publisher's publication address (`0xC000`), the subscribers' subscription to `0xC000`, and the relay flag (ON for the relay node, OFF for the others). Apply the configuration with nRF Mesh (the mobile app, which speaks the Configuration Client protocol) or the BTstack provisioner against each node's Configuration Server. Confirm with `cc-mesh-provision.py status` that all three show `cfg=True`.

### Step 7 — Place the boards so the relay matters

This is the physical heart of the project. Place:

- the **publisher** in one room,
- the **far subscriber** in another room, far enough and through enough wall that it does **not** hear the publisher directly (verify with a sniffer near the subscriber: you should *not* see the publisher's TTL-7 broadcast there),
- the **relay** in the doorway between them, in range of both.

If your space is too small to get out of range, attenuate: wrap the publisher's antenna area loosely in foil (do not short anything), or simply rely on distance plus the relay being closer to the far subscriber.

### Step 8 — Run the fleet

Wave at the PIR. The publisher prints and publishes:

```text
[pub] PIR edge -> OnOff 1; publishing OnOff Status to pub addr
```

The relay (in range) receives, lights its LED, and — because its relay feature is on — re-broadcasts the network PDU with TTL decremented to 6. The far subscriber receives the relayed copy and lights its LED:

```text
[RELAY] received OnOff Status = 1 (src=0x0002, ttl=6) -> LED ON
[SUBSCRIBER] received OnOff Status = 1 (src=0x0002, ttl=6) -> LED ON
```

Note the far subscriber's `ttl=6` (it heard the *relayed* copy), while the relay would have seen `ttl=7` from the publisher directly. That TTL drop is the proof the message took two hops.

### Step 9 — Prove the relay is load-bearing

Reconfigure the publisher's model publication TTL to **1** (via nRF Mesh's publication settings, or rebuild with the model's default TTL set to 1). Now the publisher's broadcast arrives at the relay with TTL 1, which the relay does **not** re-broadcast (the TTL-< 2 rule from Exercise 3 and Lecture 3). The far subscriber goes dark; the near relay still lights (it received the message, just did not forward it). Restore TTL 7 and the far LED comes back. This A/B test is the deliverable that proves you understand TTL and relaying.

## Pass criteria

- `gatt_sensor_peripheral` advertises, connects, notifies on PIR motion, and drives the LED on write (Step 2).
- All three mesh nodes provision successfully via `provision-all` (their LEDs go out).
- With the relay enabled and TTL 7, tripping the PIR lights **both** subscriber LEDs, and the far subscriber's log shows `ttl=6` (a relayed message).
- With the publisher's TTL set to 1, the far subscriber stays dark while the relay still lights — the A/B test from Step 9.
- A sniffer capture is committed showing the publisher's TTL-7 broadcast and, ~20 ms later, the relay's TTL-6 re-broadcast of the same SEQ.

## Common bringup gotchas

1. **Wrong `btstack_config.h` per target.** The GATT build needs `ENABLE_LE_PERIPHERAL`; the mesh builds need `ENABLE_MESH`/`ENABLE_MESH_ADV_BEARER`; the relay build additionally needs `ENABLE_MESH_RELAY` or the provisioner cannot turn relaying on. Keep separate config headers (or `#ifdef NODE_ROLE` inside one) and confirm the relay's Composition Data advertises the relay feature.
2. **The relay subscribes but does not relay.** Subscribing to `0xC000` makes the relay *consume* the message (light its LED); relaying is a *separate* network-layer feature controlled by the relay flag. A node can subscribe without relaying and vice-versa. If the far LED is dark but the relay's LED lights, you forgot to enable the relay flag.
3. **All three boards in range of each other.** If the publisher reaches the far subscriber directly, the far LED lights even with the relay off, and you have not actually demonstrated relaying. Verify with a sniffer near the subscriber that it does *not* hear the publisher's TTL-7 broadcast directly.
4. **Provisioning fails partway and the node is half-provisioned.** A node that received the NetKey but not its configuration is provisioned-but-unconfigured (LED off, but it does nothing). Re-run `configure`; if a node is wedged, factory-reset it by re-flashing the UF2 (which clears the persisted mesh state) and re-provision.
5. **The PIR's warm-up.** HC-SR501 PIRs need ~30–60 s after power-on to stabilize and will fire spuriously during warm-up. Give it a minute before concluding the publisher is misbehaving. Also tune the PIR's on-board sensitivity and time-delay pots.
6. **Sequence-number exhaustion across re-flashes.** If you re-flash a node without clearing its mesh state, its SEQ may reset below what subscribers have cached, and they will reject its messages as replays (Exercise 3's cache logic). A clean re-provision resets both sides.

## Bench session structure

- **Saturday 9 AM – 11 AM** — Steps 1–2: toolchain check, GATT peripheral bring-up, PIR and LED validated.
- **Saturday 11 AM – 1 PM** — Step 3–4: build and flash the three mesh roles; confirm each boots into the unprovisioned state.
- **Saturday 2 PM – 4 PM** — Steps 5–6: provision and configure the mesh; confirm all three `cfg=True`.
- **Saturday 4 PM – 6 PM** — Steps 7–9: place the boards, get the relayed LED to light, run the TTL A/B test, capture the sniffer trace.
- **Sunday morning** — write the README addendum with your sniffer capture and the TTL A/B result; commit; push.

## References

- All of Week 13's lecture notes.
- Bluetooth Mesh Protocol 1.1 §3 (network, relay, TTL), §5 (key derivation), §6 (provisioning). <https://www.bluetooth.com/specifications/specs/mesh-protocol-1-1/>
- Bluetooth Mesh Model 1.1 §3.2, §7.1 (Generic OnOff), §4 (Configuration messages).
- BTstack manual, "Mesh". <https://bluekitchen-gmbh.com/btstack/>
- Pico W connectivity guide, Ch. 4 (Bluetooth). <https://datasheets.raspberrypi.com/picow/connecting-to-the-internet-with-pico-w.pdf>
- nRF Mesh app (the Configuration Client for Step 6). <https://www.nordicsemi.com/Products/Development-tools/nRF-Mesh>
