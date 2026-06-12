# Challenge 1 — Sniff and Crack a Pairing

## Brief

Sniff a BLE link that pairs with **LE Legacy** pairing, recover its encryption keys with `crackle`, and read the supposedly-encrypted traffic. Then re-pair the same two devices with **LE Secure Connections** and watch the identical attack fail. The goal is not the crack; the goal is the visceral, first-hand demonstration of *why* the `SM_AUTHREQ_SECURE_CONNECTION` flag from Lecture 2 is the line between a link a $10 dongle can read and one it cannot.

You should spend ~2.5 hours on this challenge. The deliverable is a markdown document `PAIRING-WRITEUP.md` plus the two sniffer captures (`legacy.pcapng`, `sc.pcapng`).

## Setup

You have completed:

- Lecture 2 (you can describe the three pairing phases and the difference between Legacy STK derivation and Secure Connections ECDH).
- Exercise 2 (you have a working GATT client/server flow on the Pico W).
- A BLE sniffer: an nRF52840 dongle with the nRF Sniffer firmware and the Wireshark extcap plugin, *or* a Linux host with a Bluetooth adapter and `btmon` (`btmon` captures HCI on the host, which is enough to see the pairing on the *local* side — for the over-the-air attack you want the nRF Sniffer).
- `crackle` built from source (`git clone https://github.com/mikeryan/crackle && cd crackle && make`).

You will build two firmware variants of a tiny GATT peripheral that exposes one writable characteristic requiring encryption, differing only in the SMP configuration.

## Procedure

### Phase 1 — Build the Legacy-pairing peripheral

Start from the SDK's `picow_ble_temp_sensor` or your Exercise 2 server. Add one characteristic with `READ | WRITE | ENCRYPTION_KEY_SIZE_16` so the write requires an encrypted link, and configure SMP for **Legacy** pairing, Just Works:

```c
sm_init();
sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);   /* -> Just Works */
sm_set_authentication_requirements(SM_AUTHREQ_BONDING);     /* NO SC flag! */
```

Omitting `SM_AUTHREQ_SECURE_CONNECTION` lets the peer negotiate Legacy. `NO_INPUT_NO_OUTPUT` forces Just Works (TK = 0). Build, flash, confirm it advertises.

### Phase 2 — Capture the pairing over the air

1. Start the nRF Sniffer in Wireshark, set it to follow your peripheral's address (the sniffer must catch the `CONNECT_IND` to follow the connection — start the capture *before* you connect).
2. From your phone's nRF Connect (or a second Pico W central), connect to the peripheral and trigger pairing by writing the encrypted characteristic. The phone will pair (Just Works needs no user input).
3. Stop the capture once the link is encrypted and you have written the characteristic at least once. Save as `legacy.pcapng`.

In the capture, identify and screenshot:

- The `SMP Pairing Request` and `Pairing Response` — note `IO Capability = NoInputNoOutput`, `AuthReq` with the SC bit **clear**.
- The `Pairing Confirm` / `Pairing Random` exchange.
- The `LL_ENC_REQ` / `LL_ENC_RSP` link-layer encryption start.
- The point where ATT traffic becomes opaque (encrypted).

### Phase 3 — Crack it

Export the capture to a format `crackle` reads (`.pcap`), then run:

```bash
crackle -i legacy.pcap -o decrypted.pcap
```

`crackle` finds the pairing exchange, brute-forces the TK (trivial for Just Works, TK=0), derives the STK and LTK, and writes `decrypted.pcap` with the previously-encrypted ATT traffic in cleartext. Open `decrypted.pcap` in Wireshark and find your characteristic write — the value you wrote, now readable.

Document:

- The exact `crackle` output (it prints the recovered TK, STK, and LTK).
- The decrypted ATT write, with the value visible.
- How long the crack took (it is milliseconds).

### Phase 4 — Build the Secure-Connections peripheral

Change exactly two lines:

```c
sm_set_io_capabilities(IO_CAPABILITY_DISPLAY_YES_NO);        /* Numeric Comparison */
sm_set_authentication_requirements(
    SM_AUTHREQ_SECURE_CONNECTION | SM_AUTHREQ_MITM_PROTECTION |
    SM_AUTHREQ_BONDING);
```

Handle `SM_EVENT_NUMERIC_COMPARISON_REQUEST` by printing the code and auto-confirming (or confirming on a button press). Rebuild, flash.

### Phase 5 — Capture and fail to crack

Repeat Phase 2 with the new firmware; save `sc.pcapng`. In the capture, identify:

- The `Pairing Request`/`Response` now showing `AuthReq` with the **SC bit set**.
- The `Pairing Public Key` PDUs in both directions (64 bytes each — the P-256 public keys). These are new; Legacy has no public-key exchange.
- The `DHKey Check` PDUs.

Now run `crackle -i sc.pcap -o /dev/null` and document its failure. `crackle` reports that it cannot recover the keys — there is no TK to brute-force; the LTK comes from an ECDH shared secret that never crossed the air. The encrypted ATT traffic stays opaque.

### Phase 6 — Explain it

In your writeup, answer:

1. **Why does Just Works Legacy fall to a passive sniffer?** Walk the STK derivation: STK = `s1(TK, Srand, Mrand)` with TK = 0, and both `Srand`/`Mrand` cross the air in the Pairing Random PDUs. Cite Core Spec Vol 3 Part H §2.3.5.5 and the Mike Ryan WOOT'13 paper.
2. **Why does Secure Connections resist it?** The LTK derives from the ECDH shared secret (DHKey); a passive sniffer sees both P-256 public keys but cannot compute the shared secret (the discrete-log/ECDH hardness). Cite Vol 3 Part H §2.3.5.6.
3. **What does Numeric Comparison add beyond Just Works SC?** SC Just Works still defeats *passive* sniffing but not an *active* MITM. Numeric Comparison binds the two public keys into a number both users confirm, defeating a relay attacker. Cite Vol 3 Part H §2.3.5.6.4.
4. **What would still leak?** Even with SC, an attacker present *during* the very first pairing could attempt an active MITM (mitigated by Numeric Comparison/Passkey/OOB, not by Just Works). And anything the device advertises in cleartext (its name, its presence, an unencrypted characteristic) is always readable. Encryption protects the bonded session, not the advertisement.

## Deliverable

`PAIRING-WRITEUP.md` (1500–2500 words) plus `legacy.pcapng` and `sc.pcapng`. Required sections:

1. **Summary** — what you cracked, what you couldn't, in one paragraph.
2. **Legacy capture** — annotated screenshots of the pairing PDUs and the encrypted ATT write.
3. **The crack** — verbatim `crackle` output, the recovered keys, the decrypted write.
4. **Secure Connections capture** — annotated screenshots showing the public-key exchange and the SC AuthReq bit.
5. **The failed crack** — verbatim `crackle` failure output.
6. **Explanation** — the four questions from Phase 6, with Core Spec citations.
7. **References** — inline citations to the Core Spec sections and the WOOT'13 paper.

Commit with a message like `week-13/challenge-01: legacy pairing cracked, SC resists`.

## Pass criteria

- Both captures are committed and show the pairing exchange clearly.
- The `crackle` success (Legacy) and failure (SC) outputs are captured verbatim.
- The decrypted Legacy ATT write is shown with its value readable.
- The writeup correctly explains the STK-derivation weakness and the ECDH defense with Core Spec citations.
- The writeup distinguishes passive-sniffing protection (any SC) from active-MITM protection (Numeric Comparison / Passkey / OOB).

## Why this challenge matters

"Use Secure Connections" is advice you will hear and ignore until you watch a $10 dongle decrypt a Just-Works link in front of you. After this challenge you will never ship `SM_AUTHREQ_BONDING` without `SM_AUTHREQ_SECURE_CONNECTION` again, and you will be able to explain to a skeptical product manager — with a packet capture, not a hand-wave — exactly what the difference buys. Every shipped BLE product makes this choice; most of the broken ones made it wrong.

## References

- Core Spec 5.4 Vol 3 Part H §2.3.5.5 (LE Legacy pairing), §2.3.5.6 (LE Secure Connections). <https://www.bluetooth.com/specifications/specs/core-specification-5-4/>
- Mike Ryan, "Bluetooth: With Low Energy Comes Low Security", USENIX WOOT 2013. <https://www.usenix.org/conference/woot13/workshop-program/presentation/ryan>
- `crackle`. <https://github.com/mikeryan/crackle>
- nRF Sniffer for Bluetooth LE user guide. <https://www.nordicsemi.com/Products/Development-tools/nRF-Sniffer-for-Bluetooth-LE>
- BTstack manual, "Security Manager". <https://bluekitchen-gmbh.com/btstack/>
