# Homework — Week 14

Six problems. Estimated total: ~6 hours over the week. Submit as commits to your fork under `homework/week-14/`. Each problem names its deliverable path.

---

## Problem 1 — Airtime calculator and a duty-cycle table (45 min)

Write `homework/week-14/airtime.py` — a Python 3 script (standard library only) that implements the Semtech AN1200.13 LoRa airtime formula (the same one in Lecture 2 §1.4 and `main.c`). Then produce `homework/week-14/airtime-table.md` with a table of airtime (ms) and the minimum legal EU868 interval (airtime / 0.01) for:

- SF7, SF9, SF12
- BW125
- payloads of 8, 16, 32, 51 bytes

State, for each SF, the largest payload you can send once per minute while staying under 1% duty cycle. Then answer in prose: a node must report every 60 s and the link only closes at SF11 — what is the maximum payload size the duty cycle allows, and what does that force you to do to your data format?

---

## Problem 2 — Extend the radio-decision function (1 hour)

Open `exercises/exercise-01-radio-decision-and-802154.c`. Extend `cc_choose_radio()` to handle four new profiles, and add them to `main`:

1. A **smart door lock**: battery, 8 m to the hub, must work with Apple Home, sends 12 bytes on each lock/unlock. (Expected: Thread/Matter, sleepy end device.)
2. An **asset tracker** on shipping containers: battery, GPS, reports position (10 bytes) every 10 minutes, may be anywhere in a port (km-scale), mobile. (Expected: LoRaWAN, ADR off because mobile.)
3. A **commercial HVAC sensor network**: 120 mains-powered nodes in one building, 40 m max, no IP requirement, extending an existing Zigbee install. (Expected: Zigbee.)
4. A **video doorbell**: mains, 30 m to the AP, streams video, must work with the big ecosystems. (Expected: Wi-Fi, with Matter-over-Wi-Fi.)

You will need to add a `needs_video`/bandwidth signal to `product_profile_t`. Document in `homework/week-14/radio-decision.md`: the new rules you added, the order you placed them in, and one profile where you *disagree* with the function's verdict and would override it in a design review (there is always at least one — find it and argue it).

---

## Problem 3 — Decode a real (or synthetic) frame end to end (1 hour)

Using `mini-project/cc-ttn-decode.py`:

1. Build a frame with the mini-project's `lorawan.c` host self-test (or by hand) for a known payload, DevAddr, and keys.
2. Run `cc-ttn-decode.py` on it and confirm MIC VALID + correct plaintext.
3. Now corrupt one byte of the DevAddr in the `--devaddr` argument (reverse two bytes, simulating the endianness bug) and run it again. Confirm the tool reports MIC INVALID and the DevAddr-mismatch warning.
4. Corrupt one byte of the ciphertext instead (with the right DevAddr) and observe that the MIC is *still computed over the message including the corrupted byte* — explain why a single flipped ciphertext byte also fails the MIC.

Deliver `homework/week-14/frame-decode.md` with the four runs' output and your explanations. This builds the debugging reflex the mini-project depends on.

---

## Problem 4 — The LoRaWAN OTAA join, on paper (1.5 hours)

The mini-project uses ABP. Design (do not implement) the OTAA join. Deliver `homework/week-14/otaa-join.md` covering:

1. **The JoinRequest frame**: MHDR | JoinEUI(8 LE) | DevEUI(8 LE) | DevNonce(2) | MIC(4). What key signs its MIC? (AppKey.)
2. **The JoinAccept frame**: what fields the server returns (AppNonce/JoinNonce, NetID, DevAddr, DLSettings, RxDelay, optional CFList) and how the device decrypts it (note the unusual choice: JoinAccept is *AES-encrypted in the decrypt direction* so the device uses AES-encrypt to recover it — explain).
3. **Session-key derivation**: how NwkSKey and AppSKey are derived from AppKey, AppNonce/JoinNonce, NetID, and DevNonce (the two AES-ECB derivations, §6.2.2).
4. **Why OTAA beats ABP**: rotating session keys per join, fresh DevAddr, dynamic channel plan from the CFList, no frame-counter persistence headache.
5. **The DevNonce replay protection**: why the server must remember DevNonces and reject reuse (LoRaWAN 1.0.4 made DevNonce a monotonic counter for exactly this reason).

Cite LoRaWAN L2 1.0.4 §6 throughout.

---

## Problem 5 — Matter data model for a device of your choice (1 hour)

Pick a device type that is *not* a light (a contact sensor, a thermostat, a window covering, a smart plug with energy metering). In `homework/week-14/matter-device.md`:

1. Identify the Matter **device type** and its **mandatory clusters** (look them up in the Matter Device Library / Application Cluster spec).
2. For each cluster, list the key attributes and commands.
3. Sketch the endpoint tree (Endpoint 0 root + your device endpoint(s)).
4. Identify which transport you would run it over (Thread vs Wi-Fi) and justify with the power/bandwidth argument.
5. Write the ~15 lines of pseudo-callback (the connectedhomeip shape from Lecture 3) that would drive your device's hardware in response to a command or attribute write.

The point is to see that "implement a Matter device" is mostly "map the standard clusters to your hardware," not "write a protocol."

---

## Problem 6 — Compare three LPWAN options for one product (45 min)

A utility wants a water-meter that reports consumption once an hour from inside a basement meter pit (deep RF disadvantage), battery life target 10 years. Compare **LoRaWAN**, **NB-IoT**, and **Sigfox** (you will need to read beyond this week's set — NB-IoT and Sigfox are LPWANs we did not build, but a senior engineer must know they exist). In `homework/week-14/water-meter.md`:

1. A comparison table: range/penetration, battery life, payload limits, network availability (public vs private), per-device recurring cost, certification.
2. For the basement-pit penetration problem specifically, which has the best link budget and why.
3. Your recommendation with the trade-off stated plainly, and the one piece of information you would need from the customer to be sure.

This problem deliberately reaches outside the week's radios — the skill is knowing the *landscape*, not just the four we built.

---

## Submission

```bash
git add homework/week-14/
git commit -m "week 14 homework: airtime, radio decision, frame decode, OTAA join, matter data model, water meter"
git tag week-14-homework
git push origin main --tags
```

The teaching team reviews homework asynchronously; expect feedback within ~5 business days.

---

## Grading rubric (100 points)

| Problem | Points | What earns full marks |
|--------:|-------:|-----------------------|
| 1 | 15 | Correct airtime numbers (match the AN1200.13 formula within rounding); the duty-cycle minimum intervals are right; the SF11/60 s payload limit is computed and its consequence for the data format is stated concretely. |
| 2 | 20 | Four new rules added in a defensible order; all four expected verdicts produced; the `product_profile_t` extension is clean; the "I disagree" override is argued with real reasoning, not hand-waving. |
| 3 | 15 | All four runs shown; MIC VALID then INVALID demonstrated; the DevAddr-endianness and single-byte-corruption explanations are correct (the MIC covers the whole message, so any byte change fails it). |
| 4 | 20 | All five sections present; the JoinAccept "encrypt-to-decrypt" oddity is explained correctly; the two session-key derivations are right; the DevNonce replay point is made; §6 cited. |
| 5 | 15 | A non-light device type with correct mandatory clusters; the endpoint tree is right; the transport choice is justified on power/bandwidth grounds; the pseudo-callback is plausible. |
| 6 | 15 | The three-way table is accurate; the basement-pit link-budget argument is sound; the recommendation states the trade-off and names the missing customer input. |

A submission scoring ≥80 demonstrates the week's core skill: choosing the right radio for a product with explicit reasoning, and understanding the LoRaWAN frame well enough to debug it.
