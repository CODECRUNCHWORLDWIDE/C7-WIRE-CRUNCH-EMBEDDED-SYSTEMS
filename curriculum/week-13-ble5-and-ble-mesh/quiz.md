# Quiz — Week 13

Ten questions. Closed-book on the spec citations (you should be able to name the Core Spec Volume/Part and the Mesh Protocol section); open-book on the C and BTstack syntax. ~45 minutes. Answer key is at the bottom; do not peek before completing.

---

## Question 1

On the Pico W, which half of the BLE stack runs on the CYW43439 and which half runs on the RP2040, and what protocol connects them?

(A) Everything runs on the RP2040; the CYW43439 is only an antenna.
(B) The Controller (PHY + Link Layer) runs on the CYW43439; the Host (L2CAP, ATT, GATT, GAP, SMP, via BTstack) runs on the RP2040; HCI connects them.
(C) The Host runs on the CYW43439; the Controller runs on the RP2040.
(D) Both halves run on the CYW43439; the RP2040 only sends `printf`.

## Question 2

A BLE advertising payload contains the bytes `02 01 06 0A 09 43 43 20 53 65 6E 73 6F 72`. Decode it into AD structures. What does each structure mean?

## Question 3

Why are the three primary advertising channels placed at 2402, 2426, and 2480 MHz specifically, rather than at three adjacent channels?

(A) To use the lowest-power frequencies.
(B) To sit in the gaps between the three non-overlapping Wi-Fi channels (1, 6, 11) so a busy Wi-Fi network does not wipe out all of BLE advertising at once.
(C) Because the FCC requires it.
(D) Arbitrary; the channel numbers carry no meaning.

## Question 4

In a GATT attribute table, a characteristic is represented by how many attributes, and what does each one hold?

## Question 5

A client wants to receive pushed updates of a characteristic. The characteristic's declaration shows the Notify property. What must the client write, to which attribute, to subscribe? What is the type (UUID) of that attribute?

(A) Write `0x0001` to the characteristic value handle.
(B) Write `0x0001` to the CCCD (descriptor type `0x2902`) that follows the characteristic.
(C) Write `0x2902` to the service declaration.
(D) Nothing; notifications are always on.

## Question 6

A product pairs two devices with LE Legacy "Just Works." A passive sniffer captures the pairing. Can it later decrypt the link? Why or why not? How does LE Secure Connections change the answer?

## Question 7

You switch a connection from the 1M PHY to the 2M PHY. What do you gain, what do you lose, and why?

(A) Gain double throughput and double range; lose nothing.
(B) Gain ~half the air-time (so less energy per packet) and up to double throughput; lose ~3 dB of receiver sensitivity (so ~30% less range), because doubling the symbol rate lets in more noise.
(C) Gain range; lose throughput.
(D) Nothing changes; the PHY is cosmetic.

## Question 8

BLE Mesh does not use connections between nodes. How does a message get from a publisher to a subscriber three hops away, and what stops the message from circulating forever?

## Question 9

In the mini-project, the publisher sends a Generic OnOff Status to group `0xC000` with TTL 7. The relay re-broadcasts it. With what TTL does the far subscriber see the message, and what does that TTL value prove?

(A) TTL 7; it proves the message arrived directly.
(B) TTL 6; it proves the message was relayed exactly once (the relay decremented 7 → 6).
(C) TTL 0; it proves the flood terminated.
(D) TTL 8; the relay incremented it.

## Question 10

Write the BTstack code that, after the controller reaches `HCI_STATE_WORKING`, configures and enables connectable undirected advertising with a 30 ms interval and a payload `adv_data` of length `adv_data_len`. Assume `gap_advertisements_set_params`, `gap_advertisements_set_data`, and `gap_advertisements_enable` are available.

---

## Answers

### Q1: B

The CYW43439 is the **Controller** — it owns the radio, the PHY, and the timing-critical Link Layer. The RP2040 runs the **Host** (L2CAP, ATT, GATT, GAP, SMP) as BTstack. **HCI** (Core Spec Vol 4 Part E) connects them, riding the `cyw43` SPI-like link. This is the same architecture as a Linux box with a USB Bluetooth dongle — BlueZ is the host, the dongle is the controller, USB carries HCI. The split exists because the 150 µs inter-frame timing cannot be met on a general-purpose MCU running an RTOS. Lecture 1 §1.

### Q2

Two AD structures (Core Spec Vol 3 Part C §11):

- `02 01 06` — length 2, type `0x01` (Flags), value `0x06` = LE General Discoverable (`0x02`) | BR/EDR Not Supported (`0x04`).
- `0A 09 43 43 20 53 65 6E 73 6F 72` — length 10, type `0x09` (Complete Local Name), value the ASCII "CC Sensor" (9 bytes).

The length byte counts the type byte plus the data, not itself: the name structure's length `0x0A` = 10 = 1 (type) + 9 (name). Lecture 1 §6, Exercise 1.

### Q3: B

The three primary advertising channels (indices 37/38/39) are at 2402, 2426, and 2480 MHz, chosen to sit in the gaps between the three non-overlapping Wi-Fi channels 1, 6, and 11. A Wi-Fi-saturated environment cannot wipe out all three advertising channels at once. Lecture 1 §3.

### Q4: Two attributes.

A characteristic is **two** attributes (Core Spec Vol 3 Part G §3.3):

1. The **characteristic declaration**, type `0x2803`, whose value is `[properties byte][value handle: 2 bytes][value UUID]`. The properties byte says Read/Write/Notify/Indicate; the value handle points at the next attribute.
2. The **characteristic value**, at the handle named in the declaration, with the type being the characteristic's UUID, holding the actual data.

(A notify-capable characteristic adds a third attribute, the CCCD `0x2902`, but the characteristic *itself* is the declaration + value pair.) Lecture 2 §2.

### Q5: B

The client writes `0x0001` (little-endian) to the **Client Characteristic Configuration Descriptor (CCCD)**, an attribute of type `0x2902` that follows the characteristic value (Core Spec Vol 3 Part G §3.3.3.3). `0x0001` enables notifications, `0x0002` enables indications, `0x0000` disables. Forgetting to write the CCCD is the number-one reason "my notifications never arrive." Lecture 2 §2, Exercise 2.

### Q6: Yes (Legacy); no (Secure Connections).

**LE Legacy Just Works** derives the Short-Term Key from a Temporary Key that is *zero* for Just Works, plus two random nonces (`Mrand`, `Srand`) that **cross the air in cleartext** during pairing. A passive sniffer captures the nonces, knows TK = 0, computes the STK and then the LTK, and decrypts the link. `crackle` does this in milliseconds (Core Spec Vol 3 Part H §2.3.5.5; Mike Ryan WOOT'13).

**LE Secure Connections** does ECDH on the P-256 curve: each side generates an ephemeral key pair, exchanges public keys, and computes a shared secret (DHKey) the sniffer **cannot** recover from the public keys alone (the ECDH hardness). The LTK derives from the DHKey, which never crossed the air, so the sniffer cannot decrypt. Demanding `SM_AUTHREQ_SECURE_CONNECTION` forces this. Lecture 2 §6, Challenge 1.

### Q7: B

The 2M PHY doubles the symbol rate to 2 Msym/s. **Gain:** packets spend ~half the air-time, which roughly halves the energy per packet (the radio dominates power) and up to doubles throughput. **Lose:** ~3 dB of receiver sensitivity (doubling the bandwidth admits more noise), shrinking range by ~30%. 2M is the "close and frugal" PHY. Lecture 3 §1.

### Q8: Managed flooding, bounded by TTL.

BLE Mesh uses **managed flooding** over non-connectable advertisements (`ADV_NONCONN_IND`, Mesh Message AD type `0x2A`). Every node that hears a message and is configured as a **relay** re-broadcasts it; the message floods across relays until it reaches the subscriber. There is no routing table and no master. Two mechanisms stop infinite circulation: the **TTL** (each relay decrements it; a message with TTL < 2 is not relayed, so the flood terminates after a bounded number of hops), and the **sequence-number cache** (each node tracks the highest SEQ per source and drops duplicates/replays). Lecture 3 §4 and §9, Exercise 3.

### Q9: B

The publisher sends with TTL 7. The relay decrements it to 6 and re-broadcasts. The far subscriber (out of direct range of the publisher) hears the relayed copy at **TTL 6**, proving the message was relayed exactly once. If the far subscriber had been in direct range it would have seen TTL 7. The TTL drop from 7 to 6 is the on-air evidence of a single hop. Lecture 3 §10, mini-project Step 8.

### Q10

```c
case BTSTACK_EVENT_STATE:
    if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) break;
    {
        uint16_t adv_int_min = 0x0030;  /* 48 * 0.625 ms = 30 ms. */
        uint16_t adv_int_max = 0x0030;
        bd_addr_t null_addr = {0};
        /* type 0 = ADV_IND (connectable undirected); channel map 0x07 = all
           three primary channels; filter policy 0x00 = allow any. */
        gap_advertisements_set_params(adv_int_min, adv_int_max, 0, 0,
                                      null_addr, 0x07, 0x00);
        gap_advertisements_set_data(adv_data_len, (uint8_t *)adv_data);
        gap_advertisements_enable(1);
    }
    break;
```

`0x0030` = 48 in 0.625 ms units = 30 ms. Type `0` is `ADV_IND`. Channel map `0x07` enables all three primary channels. Lecture 1 §9.

(End of quiz.)
