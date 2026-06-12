# Exercise Solutions — Week 13

This document walks each exercise's reference solution and lists the canonical bugs students hit. Read it after you have attempted each exercise — not before.

Exercises 1 and 3 are pure-byte host programs (no radio); you build and run them on your laptop. Exercise 2 is on-device BTstack firmware for the Pico W. Build all three before Wednesday.

---

## Exercise 1: Advertiser and Scanner (AD-structure builder/parser)

### What the exercise asks

Build a BLE advertising payload by appending AD structures, then parse a payload back into its structures — including correctly *refusing* a malformed payload whose length field overruns the buffer. No radio; this is the byte-framing you need before BTstack will accept your advertisement.

### Reference solution

The full source is in `exercise-01-advertiser-and-scanner.c`. The two load-bearing functions are `cc_ad_append` (builder) and `cc_ad_next` (parser). The framing rule is the whole exercise: an AD structure is `[length][type][data]`, and the `length` byte counts the type byte plus the data, *not itself*. So a 9-character name becomes `0x0A 0x09 <9 bytes>` — length `0x0A` = 10 = 1 (type) + 9 (data).

The parser's defensive check is the line that matters:

```c
if (pos + 1u + ad_len > buf_len) {
    return false;  /* malformed: length field overruns the payload. */
}
```

This is the BLE equivalent of the UF2 parser's `payloadSize > 476` check from Week 10: never trust a length field that came from the air.

### Expected output

```text
=== Exercise 1: BLE advertising payload builder/parser ===

built advertisement (18 bytes): 02 01 06 0a 09 43 43 20 53 65 6e 73 6f 72 03 03 1a 18
  [0] type=0x01 Flags                    data(1)=06  (GeneralDisc NoBR/EDR )
  [1] type=0x09 Complete Local Name      data(9)=43 43 20 53 65 6e 73 6f 72  ("CC Sensor")
  [2] type=0x03 Complete 16-bit UUIDs    data(2)=1a 18  (0x181a )
  -> 3 AD structures, 18 bytes consumed

overflow test: appending 30-byte name to a 18-byte payload -> rejected (good)

parsing a deliberately malformed payload:
malformed (7 bytes): 02 01 06 10 09 41 42
  [0] type=0x01 Flags                    data(1)=06  (GeneralDisc NoBR/EDR )
  -> 1 AD structures, 3 bytes consumed
```

Note `0x181a` is the Environmental Sensing service, written little-endian on the air as `1a 18` — the byte order that trips up everyone reading their first capture.

### The canonical bugs

1. **Off-by-one in the length byte.** Writing `length = data_len` instead of `length = 1 + data_len`. The result advertises, but every scanner mis-parses the *next* structure and usually drops the whole payload. Symptom: nRF Connect shows your device with no name and no services.
2. **Little-endian confusion on UUIDs.** The 16-bit UUID `0x181A` is on the air as `1A 18`. Storing it big-endian makes you advertise service `0x1A18` (nonexistent). Always `cc_write_le16`.
3. **Trusting the air's length field.** A naive parser does `memcpy(out, data, ad_len)` without checking `ad_len` fits. A hostile or corrupt advertisement then reads past your buffer. The reference parser checks first.
4. **Forgetting the 31-byte limit.** Legacy advertising is 31 bytes total. A long device name plus a flags structure plus a UUID list blows the budget; `cc_ad_append` returns 0, and you must either shorten the name (use `Shortened Local Name`) or move the overflow into the scan response. BTstack's `gap_advertisements_set_data` truncates silently if you do not.
5. **Putting the Flags structure in the scan response.** The Flags AD type is only valid in the advertising data, never in the scan response (Core Spec Vol 3 Part C §11.1.3). Some stacks reject it; some peers ignore your whole scan response. Keep Flags in the ADV payload.

---

## Exercise 2: GATT Client (Battery Service)

### What the exercise asks

Turn the Pico W into a central that scans, connects to a peripheral advertising the Battery Service (`0x180F`), discovers the service and the Battery Level characteristic (`0x2A19`), reads it once, and subscribes to notifications.

### Reference solution

The source is in `exercise-02-gatt-client-battery.c`. The whole exercise is an exercise in *async ordering*. BTstack's `gatt_client` API is event-driven: every operation kicks off and a later `GATT_EVENT_*_QUERY_RESULT` (zero or more) plus a terminating `GATT_EVENT_QUERY_COMPLETE` reports the outcome. The state machine enforces "do not start the next query until the previous one's `QUERY_COMPLETE` arrives":

```text
TC_W4_SCAN_RESULT
  --(adv has 0x180F)--> gap_connect --> TC_W4_CONNECT
  --(LE connection complete)--> discover_primary_services_by_uuid16 --> TC_W4_SERVICE_RESULT
  --(SERVICE_QUERY_RESULT)--> stash the service
  --(QUERY_COMPLETE)--> discover_characteristics_for_service_by_uuid16 --> TC_W4_CHARACTERISTIC_RESULT
  --(CHARACTERISTIC_QUERY_RESULT)--> stash the char
  --(QUERY_COMPLETE)--> read_value_of_characteristic --> TC_W4_READ_RESULT
  --(VALUE_QUERY_RESULT)--> print battery %
  --(QUERY_COMPLETE)--> write CCCD = 0x0001 --> TC_W4_ENABLE_NOTIFICATIONS_COMPLETE
  --(QUERY_COMPLETE)--> TC_W4_NOTIFICATION
  --(NOTIFICATION)--> print each update
```

You discover *by UUID* (`discover_primary_services_by_uuid16`) rather than discovering everything and filtering — it is one ATT round-trip instead of a full table walk, and it is what a battery-conscious central does.

### Expected output (over the USB CDC serial console)

```text
[gc] HCI up. Scanning for a Battery Service peripheral...
[gc] found 28:CD:C1:0A:11:22, connecting...
[gc] connected (handle 0x0040). Discovering services...
[gc] Battery Service: handles 0x000e-0x0011
[gc] Battery Level char: value handle 0x0010, properties 0x12
[gc] Battery Level = 87%
[gc] notifications enabled. Listening for updates...
[gc] notification: Battery Level = 86%
```

Properties `0x12` = `0x10` (Notify) | `0x02` (Read) — a battery level you can both read and subscribe to. Test against another Pico W running the SDK's `picow_ble_temp_sensor` modified to expose a battery service, or against any phone/peripheral that advertises `0x180F`.

### The canonical bugs

1. **Starting the next query before `QUERY_COMPLETE`.** Calling `gatt_client_discover_characteristics...` inside the `SERVICE_QUERY_RESULT` handler instead of waiting for `QUERY_COMPLETE`. BTstack returns `GATT_CLIENT_IN_WRONG_STATE` and your discovery silently stalls. The state machine exists to prevent exactly this.
2. **Reading `value_handle` as zero.** If no characteristic matched, `battery_level_char.value_handle` stays 0 and a read against handle 0 errors. Always check for 0 after `QUERY_COMPLETE`, as the reference does.
3. **Subscribing without registering the notification listener.** You must call `gatt_client_listen_for_characteristic_value_updates` *and* write the CCCD. Writing the CCCD alone enables notifications on the peer, but your handler never sees them because you did not register to listen. Both calls, in that order.
4. **Forgetting `ENABLE_LE_CENTRAL` / `ENABLE_GATT_CLIENT` in `btstack_config.h`.** The code compiles but `gatt_client_init()` or `gap_connect()` is a no-op stub. Symptom: scanning works, connecting never completes. Check your config header.
5. **Using `cyw43_arch_init()` without polling/threadsafe arch.** With `pico_cyw43_arch_none` you must call `btstack_run_loop_execute()` and never return from `main`; the run loop services the cyw43 driver. If you instead spin your own `while(1){tight_loop_contents();}`, BTstack never gets serviced and nothing happens.

---

## Exercise 3: Mesh Network PDU

### What the exercise asks

Build, parse, validate, and relay a cleartext BLE Mesh Network PDU. Decrement TTL under the relay rule, run a per-source replay cache, and exercise the negative cases (group source, unassigned destination, TTL-1 relay, bad NetMIC). No radio and no crypto — BTstack's mesh stack does AES-CCM; you must understand the framing and the relay arithmetic.

### Reference solution

The source is in `exercise-03-mesh-network-pdu.c`. The two rules that are the whole point:

- **The relay rule** (`cc_mesh_relay_ttl`): a message arriving with TTL < 2 is *not* relayed. This is what bounds managed flooding — without it, a message loops forever. TTL 7 → 6 → 5 → … → 1, and at 1 the flood stops.
- **The replay/dedup cache** (`replay_check_and_update`): a node tracks the high-water `SEQ` per `SRC` and drops anything `<=` what it has seen. This deduplicates the flood (you hear the same message from several relays) *and* defeats replay attacks (an attacker cannot re-broadcast a captured "turn on" message — its SEQ is stale).

### Expected output

```text
origin : IVI=0 NID=0x68 CTL=0 TTL=7 SEQ=291 SRC=0x0002(unicast) DST=0xc000(group) tlen=3
packed header (12 bytes): 68 07 00 01 23 00 02 c0 00 82 04 01
parsed : IVI=0 NID=0x68 CTL=0 TTL=7 SEQ=291 SRC=0x0002(unicast) DST=0xc000(group) tlen=3

validate: OK
replay (first time): OK
replay (same SEQ again): SEQ_REPLAY   <- the flood dedup that stops loops

relay node decrements TTL: OK
relayed: IVI=0 NID=0x68 CTL=0 TTL=6 ...

repeated relays until the flood dies:
  relayed, TTL now 5
  ...
  relayed, TTL now 1
  TTL=1: not relayed (flood terminates)

negative cases:
  SRC=group: SRC_NOT_UNICAST
  DST=unassigned: DST_UNASSIGNED
  TTL=1 relay attempt: NOT_RELAYABLE
  simulated bad NetMIC: NETMIC_MISMATCH
```

Notice the packed bytes: `68` is `IVI=0|NID=0x68`; `07` is `CTL=0|TTL=7`; `00 01 23` is SEQ=0x000123=291 *big-endian* (mesh packs SEQ/SRC/DST big-endian even though BLE advertising data is little-endian — a deliberate trap in the exercise). `82 04 01` is the Generic OnOff Status opcode `0x8204` plus state `0x01` (ON).

### The canonical bugs

1. **Little-endian SEQ/SRC/DST.** The rest of BLE is little-endian, so muscle memory writes SEQ little-endian. The Mesh network header is **big-endian** for SEQ, SRC, DST (Mesh Protocol 1.1 §3.4.4). Use `cc_write_be24` / `cc_write_be16`, not the `le` variants. Symptom: your hand-built PDU decodes with a swapped source address in Wireshark.
2. **Relaying at TTL 1.** Decrementing TTL=1 to 0 and re-broadcasting. The rule is "do not relay if TTL < 2"; relaying at TTL 1 produces a TTL-0 message that some stacks treat as "deliver locally only," and you have wasted air-time. The reference refuses.
3. **No replay cache, or a per-message (not per-source) cache.** Without the cache, the flood never converges — each relay re-broadcasts every copy it hears, exponentially. With a per-message cache that forgets, the same message re-floods on the next pass. The cache must be per-source high-water SEQ.
4. **Treating a group address as a valid source.** A message *to* a group is normal; a message *from* a group is malformed (Mesh Protocol 1.1 §3.4.3). The validator rejects `SRC` outside the unicast range.
5. **Conflating CTL with the transport opcode.** `CTL=1` marks a *control* message (friendship, heartbeat) with an 8-byte NetMIC; `CTL=0` is an access message with a 4-byte NetMIC. Getting CTL wrong changes the NetMIC size and the whole PDU length math. Our occupancy traffic is all `CTL=0`.

---

## A general note on "it works in nRF Connect but not Pico-to-Pico"

For all three exercises, the gap between "my phone sees it" and "my second Pico sees it" usually comes down to:

- **Scan duty cycle.** A phone scans aggressively; a power-conscious Pico central with a 30 ms scan window and 30 ms interval (50% duty) can miss an advertiser whose interval is poorly phased against yours. Widen the scan window while debugging, then tighten it for power.
- **Address type mismatch.** A peripheral advertising with a random static address vs a public address — if your central connects with the wrong `bd_addr_type_t`, the connection times out. The reference reads the type from the advertising report and passes it straight through.
- **The cyw43 controller firmware blob.** The BLE features the controller exposes (notably LE Coded PHY) depend on the firmware blob the SDK bundles; query `LE Read Local Supported Features` at runtime rather than trusting the datasheet. Lecture 3 shows the check.
- **`btstack_config.h` drift.** Each exercise needs different feature flags (`ENABLE_LE_PERIPHERAL` for a server, `ENABLE_LE_CENTRAL`+`ENABLE_GATT_CLIENT` for a client, `ENABLE_MESH` for the mini-project). Mixing them up gives you link errors or silent no-ops. Keep one config per target.

When in doubt, sniff. A 30-second nRF Sniffer capture answers "is it on the air?" definitively, and that one question resolves most of this week's stalls.
