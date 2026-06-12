# Resources — Week 13

Every link below is free unless explicitly marked otherwise. The Bluetooth Core Spec 5.4, the Bluetooth Mesh Protocol 1.1, the BTstack documentation, the Pico W datasheet/`connecting-to-the-internet-with-pico-w` guide, and the nRF Sniffer user guide are the five load-bearing references; if you read only these five, you will pass the quiz and the mini-project will work.

---

## Primary references — read this week

### 1. Bluetooth Core Specification 5.4

- **Source:** Bluetooth SIG, "Bluetooth Core Specification, Revision 5.4", 2023-01-31.
- **URL:** <https://www.bluetooth.com/specifications/specs/core-specification-5-4/> (free; registration not required to download the PDF).
- **Sections we use:**
  - Vol 1 Part A §1 — "Architecture". The one-page block diagram of the whole stack (Controller / HCI / Host). Read it Monday morning.
  - Vol 6 Part A §2–§3 — "Physical Layer". The 40 channels, GFSK modulation, the LE 1M/2M/Coded PHYs.
  - Vol 6 Part B §2–§4 — "Link Layer Specification". Advertising and data PDUs (§2.3), the connection state machine (§4.5), advertising procedures (§4.4.2), the channel-selection algorithms (§4.5.8).
  - Vol 3 Part C §9–§10 — "Generic Access Profile". Roles, discovery modes, connection procedures.
  - Vol 3 Part F §3 — "Attribute Protocol (ATT)". The attribute definition and the ATT PDUs.
  - Vol 3 Part G §2–§4 — "Generic Attribute Profile (GATT)". The profile hierarchy, service and characteristic definitions, the discovery and read/write procedures.
  - Vol 3 Part H §2 — "Security Manager Protocol". Pairing phases, association models, LE Secure Connections.
  - Vol 4 Part E — "Host Controller Interface". The HCI commands and events BTstack sends to the CYW43439.
- **Cite as:** "Core Spec 5.4 Vol X Part Y §Z".

### 2. Bluetooth Mesh Protocol 1.1

- **Source:** Bluetooth SIG, "Bluetooth Mesh Protocol, Revision 1.1", 2023-09-12 (this replaces the 2017 "Mesh Profile 1.0.1"; the layer model is the same, the document was reorganized and renamed).
- **URL:** <https://www.bluetooth.com/specifications/specs/mesh-protocol-1-1/> (free).
- **Sections we use:**
  - §3 — "Mesh networking". The layer stack (network, transport, access), addressing (§3.4), managed flooding, relay (§3.6), replay protection (§3.8).
  - §5 — "Foundation models". The Configuration Server/Client, the Health Server/Client.
  - §6 — "Provisioning". The PB-ADV bearer, the five provisioning phases, the ECDH key exchange, the OOB authentication, the Provisioning Data PDU.
- **Companion spec:** "Bluetooth Mesh Model 1.1" (same page) — the Generic OnOff and Sensor models we use. §3 (Generic models), §4 (Sensor models).
- **Cite as:** "Mesh Protocol 1.1 §X" and "Mesh Model 1.1 §X".

### 3. BTstack documentation and source

- **Source:** BlueKitchen GmbH, `bluekitchen/btstack` on GitHub, plus the hosted manual.
- **URL:** <https://bluekitchen-gmbh.com/btstack/> (manual), <https://github.com/bluekitchen/btstack> (source).
- **License:** Dual — free for non-commercial / open-source use; a paid commercial license is required to ship BTstack in a closed-source commercial product. The Pico SDK ships BTstack under this arrangement; for coursework you are squarely in the free tier.
- **Sections we use:**
  - "GATT Server" how-to — the `.gatt` file format and `compile_gatt.py`.
  - "GATT Client" how-to — the asynchronous service/characteristic discovery API.
  - "Run Loop" — BTstack's event model; why everything is a callback.
  - "Mesh" — the mesh stack API (`mesh_init`, model registration, the provisioning callbacks).
- **Source files worth reading:** `src/ble/att_server.c`, `src/ble/gatt_client.c`, `src/ble/sm.c` (the Security Manager), `src/mesh/`.
- **Cite as:** "BTstack manual, §title" or the source path.

### 4. Raspberry Pi Pico W documentation

- **Source:** Raspberry Pi Ltd.
- **URLs:**
  - "Raspberry Pi Pico W Datasheet" — <https://datasheets.raspberrypi.com/picow/pico-w-datasheet.pdf>. The CYW43439 wiring, the antenna, the power.
  - "Connecting to the Internet with Raspberry Pi Pico W" — <https://datasheets.raspberrypi.com/picow/connecting-to-the-internet-with-pico-w.pdf>. Despite the title, this is the canonical Pico W *Bluetooth* guide too: Chapter 4 ("Bluetooth") covers `pico_btstack_ble`, `btstack_config.h`, the cyw43 arch variants, and walks the SDK's BLE examples.
- **Cite as:** "Pico W datasheet §X" / "Pico W connectivity guide Ch. 4".

### 5. CYW43439 controller

- **Source:** Infineon Technologies (formerly Cypress).
- **URL:** <https://www.infineon.com/cms/en/product/wireless-connectivity/airoc-wi-fi-plus-bluetooth-combos/wi-fi-4-802.11n/cyw43439/> (datasheet behind a free registration in some regions; the Pico W datasheet reproduces the relevant parameters).
- **What we use it for:** confirming which BLE 5 features the controller exposes — 2M PHY and Extended Advertising are supported; LE Coded PHY support depends on the controller firmware blob the SDK bundles, which is why Lecture 3 has you query `LE Read Local Supported Features` at runtime rather than trusting the datasheet.
- **Cite as:** "CYW43439 datasheet" / "Pico W datasheet §2 (RF)".

---

## Secondary references — consult as needed

### nRF Sniffer for Bluetooth LE

- **Source:** Nordic Semiconductor.
- **URL:** <https://www.nordicsemi.com/Products/Development-tools/nRF-Sniffer-for-Bluetooth-LE>
- **License:** Free firmware + free Wireshark plugin. Runs on a ~$10 nRF52840 dongle.
- **What it is:** the standard BLE sniffer. Flash the firmware to an nRF52840 dongle, install the Wireshark extcap plugin, and you get live BLE capture in Wireshark with full AD-structure, ATT, and (with the LTK) decrypted-link decoding. The user guide explains how to "follow" a device through a connection.
- **Why we use it:** this is the week's logic analyzer. Every exercise has a "capture it" step.

### crackle

- **Source:** Mike Ryan, `mikeryan/crackle` on GitHub.
- **URL:** <https://github.com/mikeryan/crackle>
- **License:** BSD-2-Clause.
- **What it is:** a tool that recovers the keys of a **BLE Legacy**-paired link from a sniffer capture of the pairing exchange. It brute-forces the 6-digit Temporary Key (or exploits Just Works' TK=0) in milliseconds, then derives the STK and LTK. It does **not** work against LE Secure Connections — that is the point of Challenge 1: you crack a Legacy pairing, then re-pair with Secure Connections and watch `crackle` fail.
- **Why we use it:** the most concrete possible demonstration of why LE Secure Connections matters.

### nRF Connect (mobile and desktop)

- **Source:** Nordic Semiconductor.
- **URLs:** Android <https://play.google.com/store/apps/details?id=no.nordicsemi.android.mcp>, iOS App Store, desktop <https://www.nordicsemi.com/Products/Development-tools/nRF-Connect-for-Desktop>.
- **License:** Free.
- **What we use it for:** the no-extra-hardware fallback. The mobile app scans for advertisements, connects to your GATT server, lists services/characteristics, reads/writes/subscribes, and bonds — it is the fastest way to confirm your peripheral works before you involve a second Pico. The desktop app's Bluetooth Low Energy view does similar over an nRF52840 dongle.

### Bluetooth SIG Assigned Numbers

- **Source:** Bluetooth SIG.
- **URL:** <https://www.bluetooth.com/specifications/assigned-numbers/>
- **What it is:** the registry of 16-bit UUIDs (`0x180F` Battery Service, `0x2A19` Battery Level, `0x2902` CCCD, …), Company Identifiers (used in manufacturer-specific advertising data), and AD types. When you see a 16-bit UUID in a capture, this is where you look it up.
- **Cite as:** "Assigned Numbers, GATT Services / Characteristics / Descriptors".

### Apple "Bluetooth Design Guidelines" and Core Bluetooth

- **Source:** Apple Inc.
- **URLs:** "Accessory Design Guidelines for Apple Devices" (the Bluetooth chapter constrains advertising interval, connection parameters, and the preferred-connection-parameters characteristic that iOS honors), <https://developer.apple.com/accessories/Accessory-Design-Guidelines.pdf>; Core Bluetooth framework docs, <https://developer.apple.com/documentation/corebluetooth>.
- **What we use it for:** if you want your peripheral to talk to an iPhone, iOS imposes connection-parameter rules (min interval 15 ms, etc., §"Connection Parameters") and will reject some advertising configurations. The homework's "make it work with an iPhone" extension cites this.

### Pico SDK BLE examples

- **Source:** Raspberry Pi Ltd., `raspberrypi/pico-examples`.
- **URL:** <https://github.com/raspberrypi/pico-examples/tree/master/pico_w/bt>
- **License:** BSD-3-Clause.
- **Files we read:**
  - `standalone/` and `freertos/` `picow_ble_temp_sensor` — a GATT server peripheral exposing a temperature characteristic. The template for our `gatt_sensor_peripheral.c`.
  - `standalone/server_with_wifi/` — combining BLE and Wi-Fi on the one CYW43439, relevant to the mesh-to-MQTT bridge homework.
  - The `.gatt` files and the generated headers — read these to understand `compile_gatt.py`'s output.
- **What we use it for:** the mini-project's peripheral is a modification of `picow_ble_temp_sensor`; build that first and confirm it advertises.

---

## Tertiary references — context and depth

### "Getting Started with Bluetooth Low Energy" — Townsend, Cufí, Akiba, Davidson

- **Source:** O'Reilly, 2014. ISBN 978-1-4919-4951-1.
- **Chapters we use:** Ch. 2 ("Protocol Basics" — GAP and GATT explained without the spec's density), Ch. 4 ("GATT"). Predates BLE 5 (no 2M/Coded/Extended), so use it for the timeless GAP/GATT mental model and the Core Spec for the BLE 5 details. Library copy; the first edition's text is also widely mirrored online for free.
- **Why:** the gentlest correct introduction to the attribute table. Read Ch. 2–4 before the Core Spec if GATT is new to you.

### Bluetooth SIG "Bluetooth Mesh Networking — An Introduction for Developers"

- **Source:** Bluetooth SIG, free white paper.
- **URL:** <https://www.bluetooth.com/bluetooth-resources/bluetooth-mesh-networking-an-introduction-for-developers/>
- **What it covers:** managed flooding, the model layer, provisioning, and the publish/subscribe pattern, at a developer-overview level. ~30 pages, free. Read this before the Mesh Protocol spec; it is to the spec what the Adafruit explainers were to the UF2 README in Week 10.

### "LE Secure Connections" — Bluetooth SIG technical overview

- **Source:** Bluetooth SIG developer blog and the Core Spec Vol 3 Part H §2.3.5.6.
- **URL:** <https://www.bluetooth.com/blog/bluetooth-pairing-part-4/> (the "Bluetooth Pairing" 4-part blog series is the clearest free explanation of the pairing models and the ECDH handshake).
- **What we use it for:** the security lecture's narrative of why Legacy pairing is broken and Secure Connections is not. The 4-part series walks Just Works, Passkey, Numeric Comparison, and OOB with diagrams.

### Mike Ryan, "Bluetooth: With Low Energy Comes Low Security" (USENIX WOOT 2013)

- **Source:** USENIX Workshop on Offensive Technologies, 2013.
- **URL:** <https://www.usenix.org/conference/woot13/workshop-program/presentation/ryan>
- **What it is:** the paper that introduced `crackle` and demonstrated the Legacy-pairing break. Free PDF. Read it before Challenge 1 — it is the original source for the attack you will reproduce.

---

## Free Apple/Adafruit/SparkFun teaching materials

### Adafruit Learn — "Introduction to Bluetooth Low Energy"

- **URL:** <https://learn.adafruit.com/introduction-to-bluetooth-low-energy>
- **What it covers:** GAP, GATT, services and characteristics, advertising, from the maker's perspective with diagrams. The friendliest free BLE intro on the internet. 20 minutes.
- **Why we link it:** read it before the Core Spec if you have never touched BLE.

### SparkFun — "Bluetooth Basics"

- **URL:** <https://learn.sparkfun.com/tutorials/bluetooth-basics>
- **What it covers:** Classic vs LE, the radio basics, profiles. A context-setter for Monday.

---

## Tooling — what to install

You should have these on your path by Tuesday evening:

- **Pico SDK** at v1.5.1 or later (BTstack and `pico_cyw43_arch` are included), with `PICO_SDK_PATH` set and the `pico_w` board selected (`-DPICO_BOARD=pico_w`).
- **BTstack's `compile_gatt.py`** — ships in the SDK at `lib/btstack/tool/compile_gatt.py`; the SDK's CMake helper `pico_btstack_make_gatt_header()` invokes it for you.
- **A BLE sniffer** — an nRF52840 dongle with the nRF Sniffer firmware, *or* `btmon`/`bluetoothctl` on a Linux host with a Bluetooth adapter.
- **`crackle`** — built from source per its README (Challenge 1 only).
- **nRF Connect** — the mobile app, on whatever phone you have. Free.
- **Python 3.10+** with a virtualenv containing `pyserial>=3.5`, `cryptography>=41.0` (for the ECDH in the provisioner), `bleak>=0.21` (a cross-platform BLE client library, for the homework's host-side GATT client).
- **Wireshark** 4.x with the nRF Sniffer extcap plugin installed.

A `requirements.txt` in `mini-project/` pins the Python versions.

---

## Reading time budget

| Reference                                            | Time     | When               |
|------------------------------------------------------|----------|--------------------|
| Core Spec Vol 1 Part A §1 + Vol 6 Part B §2 (skim)   | 45 min   | Monday morning     |
| Adafruit BLE intro                                   | 20 min   | Monday afternoon   |
| Core Spec Vol 3 Part G §2–§4 (GATT)                  | 60 min   | Tuesday morning    |
| Core Spec Vol 3 Part H §2 (SMP)                      | 45 min   | Wednesday morning  |
| Bluetooth Pairing blog series (parts 1–4)           | 30 min   | Wednesday          |
| Mesh "Introduction for Developers" white paper       | 45 min   | Thursday morning   |
| Mesh Protocol 1.1 §3 + §6 (skim)                    | 45 min   | Thursday afternoon |
| BTstack GATT-server + mesh how-tos                   | 30 min   | spread             |
| Total                                                | ~5 hours | spread across the week |

If you do the readings on the day they support, the lectures and exercises will track cleanly. If you defer all the reading to Friday, you will be provisioning a mesh you do not understand — measurably slower and dramatically more frustrating.
