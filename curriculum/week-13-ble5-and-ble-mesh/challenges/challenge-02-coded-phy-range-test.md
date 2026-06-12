# Challenge 2 — Coded-PHY Range Test

## Brief

Measure, on real hardware across a real parking lot, how far a Pico W can talk on each of the three PHYs — LE 1M, LE 2M, and LE Coded S=8 — and plot RSSI and packet loss against distance. The goal is to replace the textbook claim "Coded PHY roughly quadruples range" with *your own* numbers from *your own* board, and to feel the trade-off between range, throughput, and air-time in your hands.

You should spend ~3 hours on this challenge (including the walking). The deliverable is a markdown document `RANGE-WRITEUP.md` with plots and a CSV of the raw measurements.

## Setup

You have completed:

- Lecture 3 (you can explain why 2M loses ~3 dB of sensitivity and why Coded S=8 trades 8× air-time for range).
- A way to confirm the controller's Coded-PHY support: the runtime `LE Read Local Supported Features` check from Lecture 3. **Run it first** — if your SDK's CYW43439 firmware blob does not expose Coded PHY, this challenge becomes a 1M-vs-2M comparison and you note that in the writeup. Do not skip the check; assuming Coded works and getting silent failures is the trap.

You need **two Pico W boards**: one CENTRAL that connects and measures RSSI, one PERIPHERAL that advertises and accepts the connection. Plus a tape measure or a phone's GPS/pedometer, and an open outdoor space (a parking lot, a field, a long hallway) where you can get 100+ metres of line-of-sight.

## Procedure

### Phase 1 — Build the test pair

The **peripheral**: a connectable GATT server that, once connected, sends a Notification every 100 ms carrying a 16-bit incrementing counter on a `NOTIFY` characteristic. Build it to start on 1M and accept a PHY switch.

The **central**: connects, subscribes to the counter, and for each PHY:

1. Issues `LE Set PHY` to switch to the target PHY; confirms via `HCI_SUBEVENT_LE_PHY_UPDATE_COMPLETE`.
2. Counts received notifications over a 10-second window (expected: 100 at 100 ms interval).
3. Reads RSSI with `HCI Read RSSI` (BTstack: `gap_read_rssi(con_handle)`, result in `GAP_EVENT_RSSI_MEASUREMENT`).
4. Reports over USB CDC: `PHY=<1M|2M|CODED> dist=<m> rssi=<dBm> received=<n>/100 loss=<pct>`.

The central drives the whole sweep from a single button press so you are not fumbling with a terminal in the parking lot.

```c
/* Central: cycle PHYs and measure. Pseudocode of the per-PHY step. */
static void measure_phy(uint8_t phy_code) {
    hci_send_cmd(&hci_le_set_phy, con_handle, 0, phy_code, phy_code,
                 phy_code == 3 ? 0x02 /* prefer S=8 coding */ : 0x00);
    /* wait for HCI_SUBEVENT_LE_PHY_UPDATE_COMPLETE */
    g_window_count = 0;
    start_timer_10s();
    /* notifications increment g_window_count in the GATT notification handler */
    /* on timer expiry: gap_read_rssi(con_handle); print the row. */
}
```

The Coded-PHY coding (S=2 vs S=8) is selected by the `phy_options` parameter of `LE Set PHY` (`0x02` = prefer S=8). Confirm in the update-complete event that you actually got Coded.

### Phase 2 — Walk the lot

1. Start at 1 m. Run the three-PHY sweep. Record the three rows.
2. Move to 5 m, 10 m, 20 m, 40 m, 60 m, 80 m, 100 m (or until even Coded drops out). At each distance, keep line-of-sight, hold both boards at the same height (radios are sensitive to ground reflection), and run the sweep.
3. Log every row to a CSV: `distance_m,phy,rssi_dbm,received,loss_pct`.

Expect: 2M drops out first (shortest range), 1M next, Coded S=8 last and by a wide margin. The exact distances depend on your antenna orientation, the lot's reflections, and 2.4 GHz traffic from nearby Wi-Fi — which is the point: textbook ranges are line-of-sight ideals; your numbers are real.

### Phase 3 — Plot and analyze

Produce two plots (matplotlib, gnuplot, or a spreadsheet):

1. **RSSI vs distance**, three curves (1M/2M/Coded). All three should fall off similarly with distance (RSSI is a function of path loss, not PHY) — the difference is the *sensitivity floor* at which each PHY stops decoding.
2. **Packet loss vs distance**, three curves. This is where the PHYs separate: 2M's loss climbs first, then 1M, then Coded.

In the writeup, answer:

1. **At what distance did each PHY hit 50% packet loss?** This is your empirical "range" for each PHY. Compute the ratios (Coded-range / 1M-range, 1M-range / 2M-range) and compare to the textbook ~4× and ~1.3×.
2. **Why does RSSI vs distance look the same for all three PHYs but loss does not?** RSSI measures received signal power, which depends on path loss (distance, reflections), not on how the bits are coded. Loss depends on whether the receiver can *decode* at that signal level, which is the PHY's sensitivity — and Coded's FEC lets it decode several dB lower.
3. **What did Coded S=8 cost you?** At 100 ms notification interval the throughput is identical (you are nowhere near the rate limit), but measure the *air-time* per packet (Lecture 3: ~8× the 1M air-time). For a high-rate application, Coded would saturate the channel far sooner. State the trade explicitly.
4. **Where would each PHY be the right product choice?** 2M for a wrist wearable to a phone; 1M for a general-purpose sensor; Coded S=8 for a perimeter sensor at the edge of a property.

## Deliverable

`RANGE-WRITEUP.md` (1200–2000 words) plus `range.csv` and the two plots (committed as PNGs). Required sections:

1. **Controller capability check** — the `LE Read Local Supported Features` result; whether your CYW43439 exposed Coded PHY.
2. **Method** — the test setup, the distances, the environment (note nearby Wi-Fi, weather, line-of-sight obstructions).
3. **Raw data** — the CSV, plus the two plots.
4. **Analysis** — the four questions from Phase 3.
5. **Conclusion** — your empirical range ratios vs the textbook, and the product-choice guidance.

Commit with a message like `week-13/challenge-02: 1M/2M/Coded range sweep across the lot`.

## Pass criteria

- The controller-capability check is documented (and if Coded is unsupported, the writeup adapts honestly to a 1M-vs-2M comparison).
- At least 6 distance points are measured for each available PHY.
- Both plots are present and correctly labeled (axes, units, legend).
- The analysis correctly distinguishes RSSI (path loss) from packet loss (sensitivity), and states the air-time cost of Coded.
- The empirical range ratios are computed and compared to the textbook values, with an honest accounting of why they differ.

## Why this challenge matters

PHY selection is one of the few BLE decisions you cannot defer to a library — you have to *choose* 1M, 2M, or Coded based on your product's range, power, and rate budget, and the textbook numbers are ideals you will rarely see. Walking a parking lot with two boards and watching 2M die at 30 m while Coded reaches 120 m turns an abstract spec table into engineering intuition you will carry into every wireless product you build. It is the wireless analog of pushing a UART's baud rate until it breaks on the logic analyzer in Week 7 — you do not really know the limit until you find it on the bench.

## References

- Core Spec 5.4 Vol 6 Part A §3 (the LE PHYs and their sensitivity). <https://www.bluetooth.com/specifications/specs/core-specification-5-4/>
- Core Spec 5.4 Vol 6 Part B §4.6 (LE feature bits, including Coded PHY).
- Core Spec 5.4 Vol 4 Part E §7.8.49 (`LE Set PHY` command and `phy_options`).
- Pico W datasheet §2 (RF characteristics, antenna). <https://datasheets.raspberrypi.com/picow/pico-w-datasheet.pdf>
- BTstack manual, "GAP" (`gap_read_rssi`, the PHY APIs). <https://bluekitchen-gmbh.com/btstack/>
