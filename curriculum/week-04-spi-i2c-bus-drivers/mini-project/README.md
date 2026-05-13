# Week 4 — Mini-project: the Pi Pico W multi-bus sensor display

> *One MCU, two buses, four chips, one display. A `FAULT-MODEL.md` card that names every way this can fail and the first three steps to diagnose each.*

## The artifact

By Sunday 23:59 local time you produce a public GitHub repo containing a Pi Pico W firmware that:

1. Reads a **BMP280** (temperature + pressure) over `I2C0` at address `0x76`.
2. Reads an **MPU-6050** (accelerometer + gyroscope, six axes) over `I2C0` at address `0x68`.
3. Reads a **second SSD1306** OLED — used as an I²C target — over `I2C0` at address `0x3C`. This second display shows summary text only; its purpose is to exercise the third device on the I²C bus.
4. Drives a **first SSD1306** OLED over `SPI0` (primary display) showing all three sensor readings refreshed at 4 Hz.
5. Is built on top of your hand-written `bus_t` abstraction (from Exercise 3) so that none of the four device drivers know whether they are on SPI or I²C.
6. Ships with a one-page `FAULT-MODEL.md` enumerating every failure mode for both buses and the first three diagnostic steps for each.
7. Includes at least three Saleae captures committed as `.sal` files: one I²C transaction with the BMP280, one I²C transaction with the MPU-6050, and one SPI write to the SSD1306.

The build remains SDK-free (no `pico-sdk` source in the tree) — same discipline as Week 3.

## System diagram

```
                          +--------------------------+
                          |        Pi Pico W         |
                          |                          |
   I²C bus (100 kHz)      |  GP4 -> SDA, GP5 -> SCL  |    SPI bus (1 MHz)
   pull-ups 4.7 kΩ        |                          |
        ---+              |  GP16 -> MISO (unused)   |              +----
           |              |  GP17 -> CSn (software)  |              |
   +-------+--+   +-------+--+    GP18 -> SCK        +-------+      +-+--+
   |  BMP280  |   | MPU-6050 |    GP19 -> MOSI               |     [   ]
   |  @ 0x76  |   |  @ 0x68  |    GP20 -> D/C̄ (software)    |     [SSD1306]
   +----------+   +----------+    GP21 -> RST (software)     |     [  SPI]
        |              |          +--------------------------+      +----+
        |              |               |    |    |    |
   +----+--+      +----+--+
   |   SSD1306  (I²C)  |
   |   @ 0x3C          |
   +-------------------+
```

Four devices on the I²C bus. The first SSD1306 is on SPI (primary display). The second SSD1306 is on I²C (summary display).

## Spec — required behavior

| Item | Spec |
|------|------|
| Boot-up output | The primary SSD1306 (SPI) shows `crunch-wire w04` for 500 ms, then transitions to live data |
| Refresh rate | All three sensor reads + both display updates complete within 250 ms (4 Hz refresh) |
| Primary display layout | Line 0: `T=25.08C P=1006hPa`. Line 2: `a=(x,y,z) g` (accelerometer in g). Line 4: `g=(x,y,z) dps` (gyroscope in degrees per second). Line 6: `bus: OK` or `bus: ERR <hex>` |
| Secondary display | A 5-column summary line such as `T25 P1006 ok` |
| Sensor accuracy | BMP280 temperature within ±0.5 °C of a reference thermometer; pressure within ±2 hPa of local METAR or another barometer |
| Abstraction | The four device-driver source files (`bmp280.c`, `mpu6050.c`, `ssd1306_spi.c`, `ssd1306_i2c.c`) compile and link without including any peripheral-register header. Verified by `grep` in the README |
| Saleae traces | `traces/i2c-bmp280.sal`, `traces/i2c-mpu6050.sal`, `traces/spi-ssd1306.sal` committed |
| FAULT-MODEL.md | One page, on the repo root, conforming to the template below |
| Build | `make` produces `build/w04-mini.elf` and `build/w04-mini.uf2`; flashing the `.uf2` to a BOOTSEL Pico boots the firmware |
| Runtime resilience | If the I²C bus stalls (you may simulate with a paperclip across SDA-to-GND), the firmware detects it within 10 ms, runs the nine-clock recovery (Challenge 1 procedure), and resumes |

## Repo layout

```
w04-mini/
├── Makefile
├── README.md
├── FAULT-MODEL.md            # the load-bearing artifact
├── pico.ld                   # your linker script from Week 3
├── startup.c                 # your startup from Week 3
├── boot2_w25q080.S           # the SDK's boot2 (borrowed; only file you may borrow)
├── src/
│   ├── main.c
│   ├── rp2040_i2c.h / .c
│   ├── rp2040_spi.h / .c
│   ├── rp2040_resets.h / .c
│   ├── rp2040_io_bank.h / .c
│   ├── bus.h
│   ├── i2c_transport.h / .c
│   ├── spi_transport.h / .c
│   ├── bmp280.h / .c
│   ├── mpu6050.h / .c
│   ├── ssd1306_spi.h / .c
│   ├── ssd1306_i2c.h / .c
│   ├── font5x8.h
│   └── uart.h / .c           # for UART logging (reused from Week 3)
├── tests/
│   ├── test_bmp280.c
│   ├── test_mpu6050.c
│   └── Makefile
└── traces/
    ├── i2c-bmp280.sal
    ├── i2c-mpu6050.sal
    └── spi-ssd1306.sal
```

## The FAULT-MODEL.md card — template

Your `FAULT-MODEL.md` must follow this exact structure. The file is one page (≤ 80 lines markdown). Cite datasheet pages on every diagnostic step.

```markdown
# w04-mini — Fault Model

Last updated: YYYY-MM-DD. Reviewer: <name>.

## Buses on this board

- **I²C0** on GP4 (SDA) / GP5 (SCL), 100 kHz, 4.7 kΩ pull-ups to 3V3.
  Devices: BMP280 @ 0x76, MPU-6050 @ 0x68, SSD1306-I2C @ 0x3C.
- **SPI0** on GP18 (SCK) / GP19 (MOSI) / GP16 (MISO) / GP17 (CSn-soft),
  mode 0, ~1 MHz. Device: SSD1306-SPI.

## I²C0 failure modes

| #   | Symptom on scope            | Likely cause                  | First 3 diagnostic steps                                                       |
|-----|-----------------------------|-------------------------------|--------------------------------------------------------------------------------|
| I.1 | No transactions; SDA/SCL idle high | Peripheral not enabled or pins not muxed | (1) Confirm `IC_ENABLE = 1`. (2) Confirm `IO_BANK0 GPIO4_CTRL = 3`. (3) Confirm `RESETS_DONE.i2c0 = 1`. RP2040 §4.3.16 p. 463; §2.19 p. 244. |
| I.2 | Address byte sent; NACK on 9th SCL edge | Wrong address or device unpowered | (1) Scope VDD on each module. (2) `i2c_bus_scan` to enumerate. (3) Read BMP280 SDO pin level. UM10204 §3.1.6 p. 12. |
| I.3 | SDA stuck low at idle | Slave holding SDA after aborted transaction | (1) Confirm SDA at idle. (2) Run nine-clock recovery (Challenge 1). (3) If recovery fails, power-cycle slaves. UM10204 §3.1.16 p. 20. |
| I.4 | Rising edge on SDA/SCL > 1 µs | Pull-ups too weak for bus capacitance | (1) Measure rise time. (2) Drop pull-ups from 4.7k to 2.2k. (3) Reduce bus speed to 50 kHz. UM10204 §7.2 p. 41. |
| I.5 | SCL held low > 5 ms | Slave clock-stretching beyond expectation or faulted | (1) Identify which slave is the stretcher (yank each one in turn). (2) Read BMP280 `status.measuring` bit. (3) Soft-reset the slave (BMP280: write 0xB6 to 0xE0). BMP280 datasheet §4.3.2 p. 24. |
| I.6 | Every Nth transaction NACKs | Bus capacitance marginal, noise on long wires | (1) Shorten wires. (2) Add 0.1 µF cap on VDD of each slave. (3) Move slow slave to its own bus on I2C1. |
| I.7 | Arbitration loss reported (`IC_TX_ABRT_SOURCE.ARB_LOST`) | Another master on the bus, or noise interpreted as another master | (1) Verify no other I²C master on the wires. (2) Check ground continuity. (3) Add hardware filter cap on SDA. RP2040 §4.3.16 p. 487. |

## SPI0 failure modes

| #   | Symptom                            | Likely cause                  | First 3 diagnostic steps                                                |
|-----|-------------------------------------|-------------------------------|-------------------------------------------------------------------------|
| S.1 | Panel dark, no pixels                | Charge pump not enabled       | (1) Scope OLED VBAT pin — must be ~9 V. (2) Verify init seq sends `0x8D, 0x14`. (3) Confirm display-on `0xAF` sent last. SSD1306 §10 p. 28. |
| S.2 | Random pixels on panel               | GDDRAM uninitialized after init | (1) Call `ssd1306_clear` after init. (2) Confirm horizontal addressing mode `0x20, 0x00`. SSD1306 §8.4 p. 25. |
| S.3 | Bytes shifted by 1 bit position       | Wrong SPI mode (e.g. mode 3 instead of 0) | (1) Scope SCK polarity (idle level). (2) Scope MOSI relative to SCK rising edge. (3) Confirm `SSPCR0[7:6] = 00`. RP2040 §4.4.4 p. 528. |
| S.4 | CSn pulses per-byte instead of per-frame | CSn driven by SPI peripheral, not software | (1) Confirm GP17 FUNCSEL = 5 (SIO), not 1 (SPI). (2) Confirm CSn toggles in software around each transaction. |
| S.5 | Display works for ~100 ms then freezes | TX FIFO overflow on rapid pushes | (1) Confirm polled loop waits on `SSPSR.TNF` before each push. (2) Scope `SSPSR.BSY` between writes. RP2040 §4.4.4 p. 530. |
| S.6 | Display shows previous frame's content | Frame-buffer write completed but no `0x21/0x22` cursor reset between frames | (1) Reset cursor before each `puts`. (2) Verify horizontal address mode wraps automatically. SSD1306 §10 p. 30. |
| S.7 | Display upside-down or mirrored | Missing or wrong `0xA1` (seg remap) or `0xC8` (COM remap) | (1) Confirm init sequence includes both. (2) Try the alternates `0xA0` / `0xC0` to confirm the inversion. |

## Cross-bus failure modes

| #   | Symptom                              | Likely cause                          | First 3 diagnostic steps                                            |
|-----|---------------------------------------|---------------------------------------|---------------------------------------------------------------------|
| X.1 | Brown-out under simultaneous bursts   | 3V3 rail droops when SSD1306 charge pump kicks on while I²C is active | (1) Scope 3V3. (2) Add 10 µF on the rail. (3) Stagger startup. |
| X.2 | Saleae captures look fine, firmware misreads | Endian or sign-extension bug in raw-to-engineering conversion | (1) Print raw bytes alongside computed value. (2) Compare with Python reference. (3) Check signed vs unsigned casts in compensation code. |
| X.3 | Refresh rate falls below 4 Hz under load | Bus contention; SSD1306 framebuffer dominates I²C | (1) Profile `time_us` around each transaction. (2) Move I²C SSD1306 to a separate bus or to SPI. (3) Reduce frame rate. |

## Known unrecoverable failures

- A slave that holds SCL low after power-cycle (SCL stuck low on a recovered bus). Hardware fix only: MCU-controlled power switch on each slave (FET + GPIO).
- A PCB ground discontinuity between MCU and slave. Symptom: intermittent NACKs that correlate with hand pressure on the board. No software fix.
- A counterfeit BMP280 (some clones return ID `0x57` or `0x58` but have wrong calibration semantics). Validate by cross-checking pressure against a known-good unit.

---

End of FAULT-MODEL.md.
```

The card is the deliverable that distinguishes shippable firmware from "it works on my bench." Every project in Phase II onwards ships with one. Get the format right this week.

## Grading

| Axis                                    | Weight |
|-----------------------------------------|--------|
| Firmware compiles, flashes, runs        | 25%    |
| Three Saleae captures committed          | 10%    |
| Abstraction discipline (grep verification) | 15%  |
| FAULT-MODEL.md present and conforming    | 20%    |
| Bus-recovery demonstrated under fault    | 15%    |
| README explanatory quality                | 10%    |
| Code review (reviewer's eye, one round) | 5%     |

Passing: 70%. The FAULT-MODEL.md is a hard prerequisite — without it, the project does not pass regardless of other axes.

## Suggested build sequence

1. **Day 1 (Saturday morning, 3 h):** wire all four devices on the breadboard. Confirm 3V3 and GND on every module. Confirm pull-ups on SDA/SCL.
2. **Day 1 (Saturday afternoon, 2 h):** copy your Exercise 3 code as the starting point. Add `mpu6050.c` (use Exercise 2's I²C transport).
3. **Day 1 (Saturday evening, 1 h):** add the second SSD1306 over I²C. Use the `ssd1306_i2c.c` style — D/C̄ is encoded in the first byte (`0x40` for data, `0x00` for command).
4. **Day 2 (Sunday morning, 2 h):** write `FAULT-MODEL.md` against the template. Run the recovery flow with a deliberate fault.
5. **Day 2 (Sunday afternoon, 1 h):** capture the three Saleae traces. Commit them. Tidy the README.
6. **Day 2 (Sunday evening):** push, tag `w04-mini`, request review.

## Stretch (not graded, for portfolio)

- **DMA-paced SPI for the primary display.** Move the SSD1306 framebuffer flush to a DMA channel paced by `DREQ_SPI0_TX`. Halve the CPU load. RP2040 §2.5 p. 87.
- **Forced-mode BMP280 sampling with sleep.** Switch the BMP280 from normal mode to forced mode (datasheet §3.6 p. 21). Sleep the MCU between samples. Average current drops 10×.
- **An MPU-6050 sensor-fusion filter.** Implement a complementary filter on the accel + gyro to compute a single roll/pitch estimate. Render it on the primary OLED. Preview of Week 16.
- **A `bus_t` mock + Unity unit-test suite.** Cover every protocol-layer driver with at least 5 host-side tests each. Run them in CI on every push. Preview of Week 12.

## Final note

The FAULT-MODEL.md is what your future self (or your on-call colleague) will read at 3 AM when the fleet is misbehaving. Write it as if you are addressing yourself in that moment: short, factual, with a path from symptom to diagnosis to fix.

When the LED comes up and the pressure reads 1013 hPa and the FAULT-MODEL is on the repo, this week is done.
