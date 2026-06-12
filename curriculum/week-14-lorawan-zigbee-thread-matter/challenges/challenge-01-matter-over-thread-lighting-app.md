# Challenge 1 — Matter-over-Thread, the Real Thing

## Brief

Bring up a real Matter-over-Thread light on an nRF52840 using the open-source `connectedhomeip` SDK and OpenThread, commission it from a Thread Border Router, control it from Apple Home or Google Home or the `chip-tool` CLI, and sniff the 802.15.4 traffic with `pyspinel`. Then write the post-mortem comparing the experience to the LoRaWAN bring-up you did in the mini-project.

This is the week's "real silicon" Matter work. The lectures treat Matter-over-Thread conceptually because the Pico has no 802.15.4 radio; this challenge moves to hardware that does, so you see the genuine article rather than a simulation. Allocate ~4 hours.

## Why this is on different hardware

C7's rule is "real silicon or honest simulation, never a fake." The Pico + Pico SDK cannot run Thread — there is no 802.15.4 radio on the RP2040 and bolting a separate 15.4 transceiver onto the Pico to fake a Thread node would teach you a wiring exercise, not Thread. So this challenge uses the canonical Thread/Matter development part:

- **An nRF52840** (the Nordic nRF52840-DK, or a Dongle, or any nRF52840 board) — a Cortex-M4F with a built-in 802.15.4 + BLE radio. This is the reference target for OpenThread and connectedhomeip.
- **A Thread Border Router** — either an existing one in your home (Apple HomePod mini, Apple TV 4K, a recent Google Nest Hub) or a Raspberry Pi running the OpenThread Border Router (`ot-br-posix`) with an nRF52840 dongle as its radio co-processor (RCP).
- **The free toolchains** — `connectedhomeip` (Apache-2.0), OpenThread (BSD-3-Clause), `chip-tool` (built from the SDK), `pyspinel` for sniffing. All free and open.

If you do not have an nRF52840, you can run the entire flow against the connectedhomeip **`all-clusters-app` on a Linux host** plus the OpenThread simulation (`ot-cli-ftd` built with `OT_SIMULATION`), which gives you a real Thread mesh and a real Matter device on your laptop with no radio. That is still an honest build — the stack is the production code, only the radio is simulated — and it is the recommended path if you are hardware-short. State which path you took in the writeup.

## What you build

### Phase 1 — Build and flash the lighting-app

```bash
git clone --recurse-submodules https://github.com/project-chip/connectedhomeip.git
cd connectedhomeip
source scripts/activate.sh          # bootstraps the Matter build environment

cd examples/lighting-app/nrfconnect
west build -b nrf52840dk_nrf52840   # Zephyr-based nRF build
west flash
```

(If you take the simulation path: build `examples/lighting-app/linux` and the OpenThread simulated FTD instead.)

The lighting-app is a Matter On/Off + Level Control + Color Control light. Out of the box it forms or joins a Thread network on commissioning and drives an LED in response to On/Off/Level commands.

### Phase 2 — Stand up the Border Router

If you do not already have a hub-based TBR, build one on a Raspberry Pi:

```bash
git clone https://github.com/openthread/ot-br-posix.git
cd ot-br-posix
./script/bootstrap
INFRA_IF_NAME=wlan0 ./script/setup
# plug in an nRF52840 dongle flashed with the OpenThread RCP firmware
ot-ctl dataset init new
ot-ctl dataset commit active
ot-ctl ifconfig up
ot-ctl thread start
ot-ctl state          # should print "leader"
```

Capture the active dataset (`ot-ctl dataset active -x`) — you hand this to the device during commissioning so it joins *your* Thread network.

### Phase 3 — Commission the device

Using `chip-tool` (built from the SDK):

```bash
# pairing over BLE-then-Thread: the device advertises a commissionable BLE
# service; chip-tool connects, authenticates with the setup code, hands over
# the Thread dataset, and the device joins the mesh.
chip-tool pairing ble-thread 1 hex:<thread-dataset-hex> 20202021 3840
#                            ^node ^the dataset from Phase 2  ^passcode ^discriminator
```

Watch the device join the Thread mesh, get an operational certificate, and become controllable.

### Phase 4 — Control it

```bash
chip-tool onoff toggle 1 1          # node 1, endpoint 1: toggle the LED
chip-tool levelcontrol move-to-level 128 0 0 0 1 1   # set brightness to 128
```

Then commission the *same device into Apple Home or Google Home* using the QR code / setup code, and confirm **multi-admin**: the device is now controllable from both `chip-tool` and the consumer app simultaneously, each with its own fabric and operational certificate. This is the headline Matter capability — demonstrate it.

### Phase 5 — Sniff the 802.15.4 traffic

Flash a second nRF52840 (or use a dedicated sniffer board) with the `nRF Sniffer for 802.15.4` firmware and run `pyspinel`:

```bash
python3 -m pyspinel.sniffer -c 15 -u /dev/ttyACM0 | wireshark -k -i -
```

Capture the commissioning and a few On/Off toggles. Identify in the capture:

- The 802.15.4 MAC Data frames (parse the FCF — you wrote the parser in Exercise 1).
- The 6LoWPAN compressed IPv6 headers.
- The MLE link-establishment messages during join.
- The Matter (CoAP/UDP) interaction-model messages for the toggles (encrypted under the CASE session — you will see the framing but not the cleartext, which is the point).

## Deliverable

1. `matter-thread-build-log.md` — your build commands and any fixes you had to make (the connectedhomeip build *will* fight you; document the fight).
2. `802154-capture.pcapng` — the Wireshark/pyspinel capture of commissioning + toggles.
3. `MATTER-POSTMORTEM.md` (1500–2500 words) — the comparison and analysis below.

## The post-mortem must cover

1. **The commissioning flow, step by step.** BLE advertise → PASE (SPAKE2+) → operational credential delivery → Thread dataset delivery → mesh join → CASE session. Annotate which step you saw in the capture and which were over BLE (invisible to the 15.4 sniffer).
2. **Multi-admin demonstrated.** Show the device controlled from two fabrics at once. Explain how the two operational certificates coexist.
3. **What the SDK gave you vs what you wrote.** Count the lines you actually authored (the cluster callbacks driving the LED) vs the SDK's size. Reflect on the trade.
4. **LoRaWAN vs Matter-over-Thread, head to head.** Using your mini-project as the LoRaWAN reference, fill a comparison: bring-up effort, code you owned, range, power, payload, ecosystem integration, certification burden, debuggability. For each, say which technology you would pick for: (a) the vineyard sensor, (b) the smart bulb, (c) the warehouse lighting.
5. **The honest cost of Matter.** Firmware size, build complexity, the DAC/attestation supply-chain obligation, the two-program certification (Thread + Matter). When is that cost worth paying, and when is it over-engineering?

## Pass criteria

- The device commissions and toggles from `chip-tool` (or the simulated equivalent).
- Multi-admin is demonstrated (two controllers, one device) — or, on the simulation path, two `chip-tool` fabrics.
- The 802.15.4 capture is annotated with at least the FCF, a 6LoWPAN frame, and an MLE frame correctly identified.
- The post-mortem's LoRaWAN-vs-Matter comparison is concrete and defensible, grounded in *your* two builds, not generic marketing.

## References

- connectedhomeip SDK. <https://github.com/project-chip/connectedhomeip>, `examples/lighting-app`, `docs/guides`.
- OpenThread. <https://openthread.io>, the codelabs and the `ot-ctl` reference.
- OpenThread Border Router. <https://openthread.io/guides/border-router>.
- Matter 1.3/1.4 Core Specification — §4 (Secure Channel: PASE/CASE), §6 (Commissioning), §8 (Interaction Model).
- Thread 1.3 Specification — §3 (Network Layer), §5 (MLE).
- pyspinel + nRF 802.15.4 sniffer. <https://github.com/openthread/pyspinel>.
