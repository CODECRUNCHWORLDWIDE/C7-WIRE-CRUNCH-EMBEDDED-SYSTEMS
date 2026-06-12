# Lecture 2 — GATT, Services, Characteristics, and Security

> *Everything a BLE application reads or writes goes through one data structure: a flat array of attributes, each a tuple of (handle, type, permissions, value). That array is the GATT attribute table, and once you see it for what it is — not an object model, not a filesystem, just a sorted array you query by handle and by type — every GATT operation becomes obvious. A service is a marker in the array. A characteristic is two adjacent entries: a declaration and a value. Subscribing to notifications is writing 0x0001 to a third entry called the CCCD. This lecture builds the attribute table from first principles, walks the ATT wire protocol that reads and writes it, and then turns to the Security Manager: pairing, bonding, and the ECDH handshake that is the difference between a link a $10 sniffer can decrypt and one it cannot.*

This lecture has two halves that look unrelated but are not: GATT (how data is structured and exchanged) and security (who is allowed to exchange it). They meet at the characteristic's permissions — the moment you mark a characteristic "encryption required," GATT and the Security Manager become one conversation. Read the GATT half for the data model, the security half for the access model, and §9.5 for where they join.

## 1. The attribute: the only data structure that matters

Core Spec Vol 3 Part F §3.1 defines an **attribute** as four things:

- **Handle** — a 16-bit number, `0x0001` to `0xFFFF`, that uniquely identifies this attribute on this device. Handles are assigned in increasing order; gaps are allowed. The handle is the address you use to read or write the attribute.
- **Type** — a UUID (16-bit for SIG-assigned, 128-bit for custom) that says *what kind* of attribute this is. `0x2800` means "this is a primary service declaration." `0x2803` means "this is a characteristic declaration." `0x2A19` means "this is a Battery Level value."
- **Permissions** — read/write permission, plus whether reading or writing requires encryption, authentication, or authorization. Not transmitted on the air; enforced by the server.
- **Value** — the actual bytes. For a service declaration, the value is the service's UUID. For a Battery Level, the value is one byte, 0–100.

That is the entire GATT data model. A GATT server is a device that hosts an array of these attributes. A GATT client reads and writes them by handle. Everything else — services, characteristics, descriptors — is a *convention* about how to arrange attributes in the array.

## 2. Services, characteristics, descriptors: conventions over the array

GATT (Vol 3 Part G §3) layers structure on the flat attribute array:

A **service** (§3.1) is one attribute with type `0x2800` (Primary Service) or `0x2801` (Secondary). Its value is the service's UUID. The service "contains" all the attributes between its handle and the next service declaration — containment is purely positional. The Battery Service is one attribute: type `0x2800`, value `0x180F`.

A **characteristic** (§3.3) is *two* attributes that always appear together:

1. The **characteristic declaration**: type `0x2803`, whose value is a packed structure of `[properties: 1 byte][value handle: 2 bytes][value UUID: 2 or 16 bytes]`. The properties byte (Read/Write/Notify/Indicate/…) tells the client what it may do; the value handle points at the next attribute.
2. The **characteristic value**: at the handle named in the declaration, with the type being the characteristic's UUID. This holds the actual data. For Battery Level: type `0x2A19`, value one byte.

A **descriptor** (§3.3.3) is a further attribute attached to a characteristic. The one that matters this week is the **Client Characteristic Configuration Descriptor (CCCD)**, type `0x2902`. A client *writes* the CCCD to subscribe: `0x0001` enables notifications, `0x0002` enables indications, `0x0000` disables. The server keeps a per-client copy of each CCCD (bonded clients keep it across reconnections). Forgetting that a notify-capable characteristic *also* needs a CCCD attribute is the number-one reason "my notifications never arrive."

### A worked attribute table

A peripheral with a Generic Access service (mandatory) and a Battery Service:

```text
Handle  Type      Value                                    Meaning
0x0001  0x2800    0x1800                                   Primary Service: Generic Access
0x0002  0x2803    [Read][0x0003][0x2A00]                   Characteristic: Device Name (Read)
0x0003  0x2A00    "CC Sensor"                              -> Device Name value
0x0004  0x2803    [Read][0x0005][0x2A01]                   Characteristic: Appearance (Read)
0x0005  0x2A01    0x0540                                   -> Appearance value (Generic Sensor)
0x000E  0x2800    0x180F                                   Primary Service: Battery
0x000F  0x2803    [Read,Notify][0x0010][0x2A19]            Characteristic: Battery Level
0x0010  0x2A19    0x57                                     -> Battery Level value = 87%
0x0011  0x2902    0x0000                                   CCCD (client writes 0x0001 to subscribe)
```

A client that wants the battery level does service discovery (find the `0x2800` whose value is `0x180F` → handles `0x000E`–`0x0011`), then characteristic discovery (find the `0x2803` for `0x2A19` → value handle `0x0010`), then reads handle `0x0010`. To get pushed updates it writes `0x0001` to handle `0x0011`. This is exactly the sequence Exercise 2's state machine walks.

## 3. The ATT wire protocol

ATT (Vol 3 Part F §3.4) is the request/response protocol that reads and writes the attribute table over the air. The PDUs you will see in a capture:

- **Exchange MTU Request/Response** — negotiate the maximum ATT payload. Default is 23 bytes (so a value fits in 23 − 1 opcode − 2 handle = 20 bytes per packet). Both sides propose; the smaller wins. Modern stacks negotiate up to 247 or 517. A larger MTU means a notification can carry more data in one packet instead of being fragmented.
- **Read By Group Type Request** — "find me all attributes of type X grouped by Y." Used for *primary service discovery*: `Read By Group Type, type=0x2800` returns every primary service and its handle range.
- **Read By Type Request** — "find me all attributes of type X." Used for *characteristic discovery* within a service: `Read By Type, type=0x2803` over a service's handle range returns its characteristics.
- **Find Information Request** — enumerate all handles and their types in a range; used to find descriptors like the CCCD.
- **Read Request / Read Response** — read one attribute by handle.
- **Read Blob Request** — read an attribute longer than the MTU, in chunks.
- **Write Request / Write Response** — write an attribute, acknowledged.
- **Write Command** — write without a response (faster, no confirmation). Used for high-rate control where an occasional dropped write is acceptable.
- **Handle Value Notification** — the *server* pushes an attribute value to a subscribed client. Fire-and-forget, no ATT-layer acknowledgement.
- **Handle Value Indication / Confirmation** — like a notification, but the client *must* reply with a Confirmation. Acknowledged, slower (one in flight at a time), used when the data must not be lost.

**Notify vs Indicate** is a real design choice. Notify is cheap and high-throughput but you have no proof the client got it; the link layer's CRC and retransmission guarantee delivery *if connected*, but if the client disconnected mid-notification, it is gone. Indicate is acknowledged end-to-end but you can only have one outstanding, capping the rate. A streaming sensor uses Notify; a "your door just unlocked" event uses Indicate.

## 3.5. Reading an attribute table from a capture

When you sniff a GATT discovery (homework problem 3), the attribute table reveals itself one ATT response at a time, and learning to reconstruct it from the wire is the skill that turns a capture into understanding. A primary-service-discovery response (`Read By Group Type Response`, opcode `0x11`) looks like a list of `(start handle, end handle, service UUID)` triples:

```text
0x11  06  0001 0005 1800   <- Generic Access, handles 0x0001-0x0005
      06  000E 0011 180F   <- Battery,        handles 0x000E-0x0011
```

The `06` is the length of each triple (2+2+2 for a 16-bit UUID). A characteristic-discovery response (`Read By Type Response`, opcode `0x09`) lists `(declaration handle, properties, value handle, char UUID)`:

```text
0x09  07  000F 12 0010 2A19   <- Battery Level: decl 0x000F, props 0x12, value 0x0010
```

`12` is the properties byte = Read | Notify. From these two responses alone you have reconstructed the relevant slice of the table: the Battery service spans `0x000E-0x0011`, its Battery Level value lives at `0x0010` and is readable+notifiable, and (since notify is present) a CCCD must sit at `0x0011`. A client now reads `0x0010` for the value and writes `0x0001` to `0x0011` to subscribe — and you predicted all of that from two PDUs. This is the GATT analog of reading a SPI transaction off a Saleae in Week 8: the protocol is fully legible on the wire once you know the framing, and a sniffer plus this framing is faster than any amount of `printf`.

## 4. BTstack's GATT server: the `.gatt` file

A brief orientation before the syntax: the workflow is that *you describe the table declaratively* and a *build-time tool serializes it*, rather than you constructing attributes at runtime. This is the same philosophy as a linker script or a device tree — a static description compiled into a binary blob — and it has the same benefit: the table is fixed and inspectable at build time, the Database Hash (§11) can be precomputed, and there is no runtime allocation or ordering bug to hit. The cost is that a truly dynamic table (services that appear and disappear at runtime) is awkward; for the fixed services of a sensor or actuator — which is almost everything — the declarative `.gatt` is exactly right.

You do not hand-build the attribute table in C. BTstack's `compile_gatt.py` (in the SDK at `lib/btstack/tool/`) reads a `.gatt` text file and emits a `profile_data` byte array — the serialized attribute table — plus `#define`d handle constants. The SDK's CMake helper `pico_btstack_make_gatt_header(target)` runs it for you.

A `.gatt` file for a sensor with a custom service:

```text
// cc_sensor.gatt
PRIMARY_SERVICE GAP_SERVICE
CHARACTERISTIC GAP_DEVICE_NAME READ
   "CC Sensor"

PRIMARY_SERVICE GATT_SERVICE
CHARACTERISTIC GATT_DATABASE_HASH READ

// A custom service: a 128-bit UUID generated once with `uuidgen`.
PRIMARY_SERVICE A1B2C3D4-0000-1000-8000-00805F9B34FB
CHARACTERISTIC   A1B2C3D4-0001-1000-8000-00805F9B34FB READ | NOTIFY | DYNAMIC
CHARACTERISTIC   A1B2C3D4-0002-1000-8000-00805F9B34FB WRITE | DYNAMIC
```

`READ`/`WRITE`/`NOTIFY` set the properties byte. `DYNAMIC` means the value is not a static constant in the table — BTstack calls *your* read/write callback when a client touches it, which is how you serve live sensor data. `compile_gatt.py` adds the CCCD automatically for any `NOTIFY`/`INDICATE` characteristic, so you do not declare it explicitly. The generated header gives you `ATT_CHARACTERISTIC_A1B2C3D4_0001_..._VALUE_HANDLE`, which you pass to `att_server_notify(con_handle, handle, data, len)` to push a notification.

The server's read/write callback for `DYNAMIC` attributes:

```c
static uint16_t att_read_callback(hci_con_handle_t con, uint16_t att_handle,
                                  uint16_t offset, uint8_t *buffer,
                                  uint16_t buffer_size) {
    if (att_handle == ATT_CHARACTERISTIC_A1B2C3D4_0001_..._VALUE_HANDLE) {
        return att_read_callback_handle_blob(
            (const uint8_t *)&g_sensor_value, sizeof(g_sensor_value),
            offset, buffer, buffer_size);
    }
    return 0;
}

static int att_write_callback(hci_con_handle_t con, uint16_t att_handle,
                              uint16_t transaction_mode, uint16_t offset,
                              uint8_t *buffer, uint16_t buffer_size) {
    if (att_handle == ATT_CHARACTERISTIC_A1B2C3D4_0002_..._VALUE_HANDLE) {
        if (buffer_size >= 1) g_command = buffer[0];
        return 0;
    }
    /* The CCCD write handler: a client subscribed/unsubscribed. */
    if (att_handle == ATT_CHARACTERISTIC_A1B2C3D4_0001_..._CLIENT_CONFIGURATION_HANDLE) {
        g_notifications_enabled = little_endian_read_16(buffer, 0) != 0;
    }
    return 0;
}
```

When the sensor reads a new value and `g_notifications_enabled`, you push it:

```c
att_server_notify(g_con_handle,
                  ATT_CHARACTERISTIC_A1B2C3D4_0001_..._VALUE_HANDLE,
                  (const uint8_t *)&g_sensor_value, sizeof(g_sensor_value));
```

That is the entire GATT-server programming model: write a `.gatt`, register read/write callbacks, call `att_server_notify` on change. The mini-project's `gatt_sensor_peripheral.c` is exactly this.

## 5. Security: the threat and the layers

BLE security answers three questions:

1. **Encryption** — can a sniffer read the data on the air? (Confidentiality.)
2. **Authentication** — is the device I am talking to the one I think it is, not a man-in-the-middle? (MITM protection.)
3. **Privacy** — can a tracker follow my device by its address over time? (Anti-tracking.)

The **Security Manager Protocol (SMP)**, Vol 3 Part H, handles the first two via *pairing* and *bonding*; privacy is handled by address randomization, which we touch on at the end.

**Pairing** establishes encryption keys for the current session. **Bonding** stores those keys so the next connection skips pairing and encrypts immediately. You pair once; you are bonded forever (until one side forgets the keys).

A practical note before the mechanics: most of the time you do not *implement* security, you *configure* it. You tell BTstack your device's I/O capabilities and your authentication requirements, you mark the sensitive characteristics with the right permission, and the Security Manager runs the handshake. The work is choosing the right policy — how sensitive is this data, what I/O does this device have, do I need MITM protection — not writing crypto. The sections that follow explain the mechanics so you can choose well and read a capture, but in your code the security surface is `sm_set_io_capabilities`, `sm_set_authentication_requirements`, and a handful of event handlers. Getting the *policy* wrong (shipping Just Works for a door lock) is the real risk; the mechanics are handled below the API.

## 6. The pairing handshake, phase by phase

Pairing (Vol 3 Part H §2.3) runs in three phases:

**Phase 1 — Feature exchange.** Each side sends a Pairing Request/Response carrying its **I/O capabilities** (does it have a display? a keyboard? neither?), whether it supports **OOB** data, whether it wants **bonding**, the MITM-protection flag, the Secure Connections flag, and which keys it wants distributed. The I/O capabilities and the OOB/MITM flags feed a lookup table (Vol 3 Part H §2.3.5.1, Table 2.8) that selects the *association model* for phase 2.

**Phase 2 — Key agreement.** This is where Legacy and Secure Connections diverge:

- **LE Legacy pairing** (Bluetooth 4.0–4.1) derives a Short-Term Key (STK) from a Temporary Key (TK) plus two random nonces. In the most common case ("Just Works"), the TK is *zero*. A passive sniffer that captures the pairing exchange can reconstruct the STK and then the Long-Term Key. This is not a theoretical weakness — `crackle` does it in milliseconds, and you will watch it in Challenge 1.
- **LE Secure Connections** (Bluetooth 4.2+, Vol 3 Part H §2.3.5.6) does **ECDH on the NIST P-256 curve**. Each side generates an ephemeral key pair, exchanges public keys, and computes a shared secret (the DHKey) that a passive sniffer *cannot* recover even with the full exchange — that is the whole point of Diffie-Hellman. From the DHKey both sides derive the Long-Term Key. A sniffer sees the public keys but cannot compute the shared secret.

**Phase 3 — Key distribution.** With an encrypted link established, the devices exchange long-term keys: the **LTK** (for encrypting future connections — this is what bonding stores), the **IRK** (Identity Resolving Key, for resolving a peer's rotating private address — the privacy mechanism), and the **CSRK** (Connection Signature Resolving Key, for signed writes without encryption).

## 7. The four association models

Phase 2 of Secure Connections authenticates against a man-in-the-middle using one of four **association models**, chosen from the I/O-capability matrix:

- **Just Works** — no user interaction. The ECDH still defeats *passive* sniffing, but there is no protection against an *active* MITM (an attacker who relays the pairing). Used when at least one device has no I/O (a sensor with no display or buttons). Encrypted but unauthenticated.
- **Passkey Entry** — one device displays a 6-digit number; the user types it into the other. The passkey is mixed into the key confirmation, defeating a MITM. Used when one device has a display and the other a keyboard.
- **Numeric Comparison** (Secure Connections only) — both devices display the same 6-digit number derived from the public keys, and the user confirms they match. Defeats a MITM because an attacker relaying the pairing cannot make both numbers match. Used when both devices have a display and a yes/no input — e.g. a phone and a smart lock. This is what you will use in the mini-project.
- **Out Of Band (OOB)** — the authentication value is exchanged over a *different* channel (NFC tap, a QR code, a printed label scanned by the phone). The strongest, because the OOB channel is assumed to be MITM-free.

The decision is the I/O matrix's job, not yours — you declare each device's capabilities and the SIG-specified table picks the model. A Pico W with a `printf` console but no real display typically declares "DisplayYesNo" or "NoInputNoOutput," which lands you in Numeric Comparison or Just Works respectively.

## 8. Bonding, and why a bonded link is fast

After pairing with bonding, both sides store the LTK keyed by the peer's identity address. On the next connection, the central sends an `LE Start Encryption` (or the peripheral requests it) using the stored LTK; the link encrypts in one round-trip with no pairing ceremony. This is why your phone reconnects to your earbuds instantly the second time but made you confirm a number the first time.

BTstack stores bonding keys in a `le_device_db` — on the Pico W, you can back this with a flash sector so bonds survive a reboot. The mini-project bonds the occupancy node to a phone for configuration and persists the bond.

Enabling Secure Connections with Numeric Comparison in BTstack:

```c
sm_init();
sm_set_io_capabilities(IO_CAPABILITY_DISPLAY_YES_NO);
sm_set_authentication_requirements(
    SM_AUTHREQ_SECURE_CONNECTION | SM_AUTHREQ_MITM_PROTECTION |
    SM_AUTHREQ_BONDING);

/* In your SM event handler, confirm the numeric value: */
case SM_EVENT_NUMERIC_COMPARISON_REQUEST: {
    uint32_t code = sm_event_numeric_comparison_request_get_passkey(packet);
    printf("[sm] confirm code on both devices: %06"PRIu32"\n", code);
    /* In a real device, the user presses a button. We auto-confirm here: */
    sm_numeric_comparison_confirm(
        sm_event_numeric_comparison_request_get_handle(packet));
    break;
}
```

`SM_AUTHREQ_SECURE_CONNECTION` is the flag that demands ECDH P-256 rather than Legacy. Omit it and BTstack will happily fall back to Legacy pairing if the peer requests it — and `crackle` wins. Demand it, and Challenge 1's attack fails.

One persistence detail: bonding is only useful if the keys survive a reboot. BTstack stores them in a `le_device_db`, and on the Pico W you back that database with a flash sector (the SDK's `pico_flash_bank` or a TLV store in a reserved flash region) so a bonded peer reconnects encrypted after a power cycle without re-pairing. If you leave the device-db in RAM, every reboot forgets all bonds and the user must re-pair — a common "why does it keep asking me to pair" complaint. The mini-project persists the bond so the configuration phone reconnects silently.

## 9. Privacy: resolvable private addresses

A device advertising the same Bluetooth address forever is trackable: a network of beacons can follow you by your earbuds' address. The privacy feature (Vol 6 Part B §1.3.2.2) lets a device advertise a **Resolvable Private Address (RPA)** that it rotates every ~15 minutes. The RPA is generated from the device's **IRK** (exchanged during bonding); a *bonded* peer holding the IRK can resolve the RPA back to the real identity, but an unbonded tracker cannot. This is how an Apple Watch stays paired to your phone while being untrackable by strangers. We enable privacy in the bonding step and explain the IRK, but the full resolution dance and the controller's address-resolution offload are out of scope this week.

## 9.5. Permissions and the encryption requirement

A characteristic's **permissions** are enforced by the server and never travel on the air (Core Spec Vol 3 Part F §3.2.5). They are richer than "read/write" — each access can require *encryption*, *authentication*, or *authorization*:

- **Open** — anyone connected may read/write. Fine for a public sensor value.
- **Encryption required** — the link must be encrypted (the peer must have paired/bonded) before the access succeeds. An unencrypted read returns the ATT error `0x05 Insufficient Authentication` (or `0x0F Insufficient Encryption`), which prompts a well-behaved client to pair and retry.
- **Authentication required** — the link must be encrypted *with an authenticated key* (MITM-protected pairing: Passkey, Numeric Comparison, or OOB — not Just Works). This is the level for a "unlock the door" characteristic: Just Works is not enough.
- **Authorization required** — the application must approve the specific access (a callback returns yes/no), layered on top of encryption.

In the mini-project's `cc_sensor.gatt`, the LED-control characteristic is declared `WRITE | ENCRYPTION_KEY_SIZE_16`, so a random scanner that connects cannot toggle the LED — it must first pair. When a client writes without encryption, BTstack's ATT layer returns `Insufficient Encryption` *before* your write callback runs, and the client's stack typically initiates pairing automatically. This is the right default for any characteristic that *does* something (actuates, configures, unlocks); a read-only sensor value can stay open. The permission is a property of the attribute in the table, set by the `.gatt` keyword, and the server enforces it on every access — there is no way for a client to bypass it from the air, because the check happens on the server side after decryption.

The interplay with security (§5–§8) is the whole point: declaring a characteristic "authentication required" is what *forces* the LE-Secure-Connections-with-MITM pairing from the security section. The permission and the pairing model are two halves of one decision — "how sensitive is this data" picks the permission, which picks the minimum pairing strength, which picks the association model.

## 10. The MTU, fragmentation, and why your notification is 20 bytes

The default ATT MTU is **23 bytes** (Core Spec Vol 3 Part F §3.4.2). Subtract the 1-byte opcode and the 2-byte handle and a notification carries exactly **20 bytes** of value. This is the single most surprising number for a new BLE developer: you declare a 64-byte characteristic, you `att_server_notify` 64 bytes, and the client receives 20. The fix is the **Exchange MTU** procedure — both sides propose their maximum (BTstack defaults to 247 on the Pico W, controller permitting), the smaller wins, and now a notification carries up to MTU − 3 bytes. But the MTU negotiation must happen *before* you rely on large notifications, and some peers (older Android) never negotiate up, so a robust characteristic either fits in 20 bytes or uses **Read Blob** / chained notifications to send more.

There is a deeper layering point here. ATT runs over **L2CAP** (Vol 3 Part A), and L2CAP *can* fragment a large ATT PDU across several Link Layer packets — but the ATT MTU caps the *logical* PDU size regardless. So "MTU" is an ATT-layer concept (how big one ATT operation can be), while the Link Layer's separate "Data Length Extension" (DLE, up to 251-byte LL payloads) is a *transport* optimization underneath. They are independent: a large MTU without DLE means your big ATT PDU rides many small LL packets (more overhead); both together is the high-throughput configuration. For this week's sensor data, 20 bytes per notification is plenty and you can ignore both — but know the numbers so a future "why is my data truncated at 20 bytes" is a five-second answer.

## 10.5. The standard profiles you should know exist

The SIG has standardized dozens of GATT profiles — fixed combinations of services and characteristics — so that a generic client (your phone) can talk to any compliant device of a class without per-device code. A few worth recognizing, because you will see them in captures and reuse their UUIDs:

- **Battery Service (`0x180F`)** with **Battery Level (`0x2A19`)** — a single byte, 0–100. Exercise 2 reads exactly this. Nearly every battery-powered peripheral exposes it; iOS and Android render it natively.
- **Device Information Service (`0x180A`)** — manufacturer name, model number, firmware revision, serial number. Read-only strings. A phone shows these in the device details.
- **Environmental Sensing Service (`0x181A`)** — temperature (`0x2A6E`), humidity, pressure, with a descriptor describing the measurement (sampling interval, units). The SDK's `picow_ble_temp_sensor` exposes the temperature characteristic.
- **Human Interface Device (HID over GATT)** — keyboards, mice, game controllers. A complex profile (report maps, multiple characteristics) but the reason a BLE keyboard "just works" with any OS.
- **Heart Rate Service (`0x180D`)** — the canonical GATT tutorial profile: a heart-rate measurement characteristic that notifies. Every BLE tutorial in existence implements it.

The lesson is *reuse the standard UUIDs when your data fits a standard profile*. If your device measures temperature, expose it via the Environmental Sensing Service's temperature characteristic, not a custom UUID — then a generic dashboard reads it without knowing anything about your product. Invent a custom service only for data that genuinely has no standard home. The mini-project's occupancy state *could* arguably be a standard characteristic, but occupancy/presence is awkwardly covered in GATT (it is cleaner in the mesh Sensor model's "Presence Detected" property), so we use a custom service for the GATT peripheral and the proper mesh model for the mesh — a realistic "standard where it fits, custom where it does not" split.

## 11. The GATT Caching problem and the Database Hash

A power-conscious central does not want to re-discover the whole attribute table on every reconnection — service discovery is several round-trips. **GATT Caching** (Vol 3 Part G §2.5.2, Bluetooth 5.1) lets the client cache the table across connections and detect when it changed. The mechanism is the **Database Hash** characteristic (`0x2B2A`) in the GATT service: a 128-bit AES-CMAC over the attribute table's structure. The client reads it once, caches the table; on reconnect it reads the hash again — if it matches, the cache is valid and discovery is skipped; if it changed (you shipped a firmware update that added a characteristic), the client re-discovers. BTstack's `compile_gatt.py` computes the Database Hash at build time and serves it from a static attribute, which is why the `cc_sensor.gatt` in the mini-project includes a `GATT_DATABASE_HASH` characteristic. Omitting it means iOS re-discovers every connection — slower reconnection and more battery on both sides.

## 11.5. The GATT client: discovery is several round-trips, all asynchronous

The client side (Exercise 2) is where the async event model is least forgiving, so it is worth spelling out the round-trips. To read one characteristic from a freshly-connected peripheral, a client does, *in sequence*:

1. **Exchange MTU** — one round-trip. Negotiate a larger ATT MTU so later reads carry more.
2. **Primary service discovery** — `Read By Group Type Request(0x2800)` over `0x0001..0xFFFF`, possibly several round-trips if there are many services (each response carries as many as fit in the MTU; the client repeats from the next handle until the server returns `Attribute Not Found`). To find one known service, `discover_primary_services_by_uuid16` filters server-side and returns just that service in fewer round-trips.
3. **Characteristic discovery** — `Read By Type Request(0x2803)` over the service's handle range, again one-or-more round-trips.
4. **Descriptor discovery** (if you need the CCCD handle) — `Find Information Request` over the characteristic's range.
5. **Read / Write / Subscribe** — the actual operation.

That is potentially a dozen packets over several connection intervals before the first byte of data. At a 30 ms interval, full discovery can take 300–500 ms — which is exactly why GATT Caching (§11) exists, and why a battery-conscious client caches the table and skips discovery on reconnect.

In BTstack each step is a function call that *returns immediately* and a later event that reports completion. The iron rule, restated because it is the week's most common bug: **you may not start step N+1 until step N's `GATT_EVENT_QUERY_COMPLETE` arrives.** Calling `gatt_client_discover_characteristics...` from inside the `SERVICE_QUERY_RESULT` handler (before `QUERY_COMPLETE`) returns `GATT_CLIENT_IN_WRONG_STATE` and the operation silently never happens. Exercise 2's explicit `TC_W4_*` state machine exists solely to make this ordering impossible to get wrong; copy the pattern into any GATT client you write.

A second client subtlety is the **notification listener registration**. Subscribing is *two* operations that are easy to conflate: you write `0x0001` to the CCCD (which tells the *server* to start sending), and you register a `gatt_client_notification_t` listener (which tells *BTstack* to route the arriving notifications to your handler). Do only the first and the server sends notifications into the void; your handler never fires. Do both, in order, and the updates flow. Exercise 2 does both inside the `TC_W4_READ_RESULT` → `QUERY_COMPLETE` transition.

## 12. Worked example: serving and notifying a sensor value

Tying the GATT-server model together, here is the full lifecycle of one occupancy notification, the exact path the mini-project's peripheral runs:

1. **Build time.** `compile_gatt.py` reads `cc_sensor.gatt`, sees `CHARACTERISTIC ... READ | NOTIFY | DYNAMIC`, and emits the attribute table: a declaration (`0x2803`), a value attribute (marked dynamic, so reads route to your callback), and an auto-added CCCD (`0x2902`). It `#define`s `ATT_CHARACTERISTIC_..._VALUE_HANDLE` and `..._CLIENT_CONFIGURATION_HANDLE`.
2. **Connect.** The client connects; BTstack fires `HCI_SUBEVENT_LE_CONNECTION_COMPLETE`; you stash the `con_handle`.
3. **Discover.** The client runs `Read By Group Type` (services) and `Read By Type` (characteristics), which BTstack answers from `profile_data` without involving your code — the table is static structure.
4. **Subscribe.** The client writes `0x0001` to the CCCD handle. BTstack calls your `att_write_callback` with `att_handle == ..._CLIENT_CONFIGURATION_HANDLE`; you set `g_notifications_on = 1`.
5. **Sensor event.** Your PIR poll sees motion, updates `g_occupancy_state`, and — because `g_notifications_on` — calls `att_server_notify(con_handle, ..._VALUE_HANDLE, &state, 1)`.
6. **On the air.** BTstack composes an ATT `Handle Value Notification` (opcode `0x1B`, the value handle, the byte) and hands it to the controller, which sends it in the next connection event. The client's notification handler fires.

Every GATT server you ever write is this loop: a `.gatt`, a write callback that tracks the CCCD, and an `att_server_notify` on change. The complexity is all in the spec's framing, which BTstack and `compile_gatt.py` handle; your code is the sensor logic plus six lines of glue.

A subtlety worth flagging: the CCCD is **per-client**. If two centrals connect to your peripheral and only one subscribes, you must notify only that one — `att_server_notify(con_handle, ...)` takes the connection handle precisely so you can target the subscribed client. The server keeps a CCCD value per (client, characteristic) pair, and for a *bonded* client BTstack persists that subscription across reconnections (the client subscribed once, reconnects, and notifications resume without re-writing the CCCD). For an unbonded client the CCCD resets on disconnect. This is why a "notify on change to whoever is subscribed" server tracks subscriptions per connection handle, not a single global flag — the single-flag shortcut in the mini-project's peripheral works only because it serves one client at a time, and a multi-client server must track the set.

## 12.5. Reliability: when notifications are not enough

Notifications are unacknowledged at the ATT layer, and the consequences are subtle. The Link Layer *does* retransmit lost packets within a connection (every data packet is acknowledged at the LL level and retried until acked or the supervision timeout fires), so a notification is reliably delivered *as long as the connection holds*. What is not protected is the gap around a disconnect: if the peripheral fires a notification at the same instant the link drops, that notification is gone, and neither side knows it was lost. For a streaming sensor (occupancy, temperature) this is fine — the next notification supersedes it. For an event that must not be lost (a door unlocked, an alarm tripped), it is not.

Three escalating remedies:

1. **Indications.** Use `att_server_indicate` instead of `att_server_notify`. The client must reply with a Confirmation; BTstack will not let you send a second indication until the first is confirmed. Slower (one in flight), but you *know* it arrived. The cost is throughput — you cannot stream at indication rates.
2. **Application-level sequence numbers.** Put a counter in the characteristic value; the client detects a gap and reads the current value to catch up. This is how you get notification throughput with indication-grade integrity: notify fast, and let the client reconcile via the sequence number on reconnect.
3. **A persistent state characteristic.** Keep the authoritative state in a readable characteristic the client reads on every reconnect. Notifications become an optimization (fast updates while connected) rather than the source of truth (which is the read-on-connect). This is the right pattern for occupancy: the readable occupancy value is the truth; the notification is just a low-latency nudge.

The mini-project's mesh side has the analogous concern, solved differently: mesh publications are unacknowledged floods, so a Generic OnOff Server publishes *periodically* (a heartbeat) in addition to on-change, and a Generic OnOff Client can *Get* the current state — the same "the readable/pollable state is the truth, the push is the optimization" pattern, one layer up. Recognizing this pattern — authoritative pollable state plus opportunistic push — across both GATT and mesh is the transferable lesson.

## 12.7. Custom services and 128-bit UUIDs

The SIG-assigned 16-bit UUIDs (`0x180F` Battery, `0x181A` Environmental Sensing) cover standard profiles, but your own product's data needs a **custom service**, which means a **128-bit UUID**. You generate a base UUID once (`uuidgen` on the command line produces one) and derive your service and characteristic UUIDs from it by varying a couple of bytes — the convention `compile_gatt.py` and most stacks follow is to keep a 128-bit base and assign the 16-bit-like suffix per attribute. The mini-project's `cc_sensor.gatt` does exactly this: a base `A1B2C3D4-0000-...` with `-0001-` for occupancy and `-0002-` for LED control.

The cost of 128-bit UUIDs is air-time and table size: a custom service advertised in the AD payload eats 16 bytes (vs 2 for a 16-bit UUID), so it crowds the 31-byte legacy advertising budget — often you advertise an *incomplete* 128-bit UUID list (or none) and let the client discover the full UUID after connecting. The benefit is a globally-unique namespace you own: no SIG registration, no collision risk, and a client written for your product filters on your UUID and ignores everything else. The decision is the same as elsewhere in the stack: standard 16-bit UUIDs for interoperable standard data (battery, heart rate), 128-bit custom UUIDs for your proprietary data. A real product usually has both — the standard Device Information and Battery services plus a custom service for its actual function.

One trap: byte order. A 128-bit UUID in the advertising AD structure and in the attribute table is stored **little-endian** (least-significant byte first), so the human-readable `A1B2C3D4-0000-1000-8000-00805F9B34FB` is laid out in memory as `FB 34 9B 5F 80 00 00 80 00 10 00 00 D4 C3 B2 A1` — reversed. The mini-project's `adv_data[]` shows this reversal explicitly, and mis-ordering it is why "my custom service does not show up in nRF Connect" is a common Tuesday bug. The 16-bit UUIDs have the same little-endian-on-the-air rule (`0x181A` → `1A 18`), just less visibly.

## 13. Summary

GATT is a flat attribute table — (handle, type, permissions, value) — and services, characteristics, and descriptors are conventions about how to arrange entries in it. ATT is the request/response protocol that reads and writes that table by handle; notifications and indications are the server pushing values to a subscribed client. BTstack generates the table from a `.gatt` file and calls your callbacks for dynamic attributes. Security splits into pairing (per-session keys) and bonding (stored keys); LE Legacy pairing is broken by a passive sniffer, LE Secure Connections uses ECDH P-256 that is not, and the four association models (Just Works, Passkey, Numeric Comparison, OOB) add MITM protection according to each device's I/O capabilities. Demand `SM_AUTHREQ_SECURE_CONNECTION` and a sniffer cannot read your link.

Next lecture: BLE 5's 2M and Coded PHYs and extended advertising, then BLE Mesh — a completely different beast that abandons connections entirely in favor of flooding, with its own provisioning, addressing, models, and the relay-and-TTL machinery you prototyped in Exercise 3.

## References for this lecture

- Core Spec 5.4 Vol 3 Part F §3 (Attribute Protocol: the attribute, the ATT PDUs). <https://www.bluetooth.com/specifications/specs/core-specification-5-4/>
- Core Spec 5.4 Vol 3 Part G §2–§4 (GATT: profile hierarchy, service/characteristic/descriptor definitions, discovery and read/write procedures).
- Core Spec 5.4 Vol 3 Part H §2 (Security Manager: pairing phases, association models, LE Secure Connections, key distribution).
- Core Spec 5.4 Vol 6 Part B §1.3.2.2 (Resolvable Private Addresses).
- Bluetooth SIG Assigned Numbers (service/characteristic/descriptor UUIDs). <https://www.bluetooth.com/specifications/assigned-numbers/>
- BTstack manual, "GATT Server", "Security Manager". <https://bluekitchen-gmbh.com/btstack/>
- "Bluetooth Pairing" 4-part blog series, Bluetooth SIG. <https://www.bluetooth.com/blog/bluetooth-pairing-part-4/>
- Mike Ryan, "Bluetooth: With Low Energy Comes Low Security", USENIX WOOT 2013 (the `crackle` paper).
