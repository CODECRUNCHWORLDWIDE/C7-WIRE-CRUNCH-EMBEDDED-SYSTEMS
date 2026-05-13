# Week 4 — Homework

Six practice problems. Do them by Sunday. Each one is self-graded against the references; bring questions to Friday studio.

---

## H1 — Read the SDK's `i2c.c`

Open `pico-sdk/src/rp2_common/hardware_i2c/i2c.c` (linked in `resources.md`). Read `i2c_write_blocking_internal` end-to-end (~80 lines).

Write a one-paragraph annotation of the function — what it does, what corner cases it handles that your `i2c0_write_read` in Exercise 2 does not (look for: timeout handling, abort recovery, partial-completion semantics).

Bring the annotation to studio. Compare with your version of `i2c0_write_read`; identify one corner case you should add and one you can defer.

---

## H2 — Compute fast-mode timing

For `clk_peri = 125 MHz`, target fast-mode I²C at 400 kHz.

Per RP2040 datasheet §4.3.5 (p. 442):

```
Fscl = clk_peri / (HCNT + LCNT + SPKLEN + 7)
```

(The `+7` and `+SPKLEN` account for synchronization and spike-filter cycles. See the datasheet for the precise breakdown.)

UM10204 Table 10 (p. 38) requires for fast mode:
- `tLOW` ≥ 1.3 µs
- `tHIGH` ≥ 0.6 µs

Compute `HCNT` and `LCNT` values that satisfy both UM10204 minimums *and* the RP2040 timing equation for exactly 400 kHz. (Hint: the SDK uses `IC_FS_SPKLEN = 1`. Multiple solutions exist; pick one and show your arithmetic.)

---

## H3 — SSD1306 vs I²C address byte

The SSD1306 datasheet §8.1.5.1 (p. 20) describes I²C mode. The first byte of every I²C data transaction to the SSD1306 is a control byte with two fields:
- `Co` (bit 7): continuation bit.
- `D/C̄` (bit 6): data / command.

Write the four possible control bytes (for the four `(Co, D/C̄)` combinations) as hex values, and explain in one sentence what each combination means at the protocol level. Why does this layout let the SSD1306 mux command-bytes and data-bytes on a single I²C stream without a separate D/C̄ pin?

---

## H4 — Compensation correctness check

Take the BMP280 compensation reference values from datasheet Appendix 8.1 (p. 46):

```
adc_T = 519888
adc_P = 415148
dig_T1 = 27504; dig_T2 = 26435; dig_T3 = -1000
dig_P1 = 36477; dig_P2 = -10685; dig_P3 = 3024
dig_P4 = 2855; dig_P5 = 140; dig_P6 = -7
dig_P7 = 15500; dig_P8 = -14600; dig_P9 = 6000
```

Run the 32-bit compensation arithmetic by hand or in Python. Compute `T` (in 0.01 °C units) and `P` (in Q24.8 Pa). Cross-check against the datasheet's stated expected outputs: `T = 2508` (= 25.08 °C), `P = 25767233` (Q24.8 → divide by 256 → 100653.25 Pa → 1006.53 hPa).

If your numbers do not match, the bug is in your transcription — not in the datasheet.

---

## H5 — Sketch a Saleae expectation

Draw, on paper or in a vector tool, the expected Saleae trace for the following I²C transaction:

> Master at standard 100 kHz writes register `0xF4 = 0x27` to BMP280 at address `0x76`, then immediately reads 6 bytes from register `0xF7`.

Your sketch must include:
- The full bit pattern of every byte on SDA.
- The 9-clock-per-byte structure on SCL.
- The START, RESTART, ACK, NACK, STOP conditions in the right places.
- An estimate of the wall-clock duration of the whole transaction (back of envelope).

Bring the sketch to studio. Compare against an actual Saleae capture from Exercise 2.

---

## H6 — Read a Zephyr driver

Open the Zephyr BME280 driver at <https://github.com/zephyrproject-rtos/zephyr/blob/main/drivers/sensor/bosch/bme280/bme280.c>.

Identify the function-pointer table (or device-tree binding) that holds the bus operations. Write down the equivalent in *your* code from Exercise 3 — what corresponds to `dev->bus->write_read`?

In one paragraph, compare Zephyr's approach to ours. Where does Zephyr go further? (Hint: device-tree, runtime device enumeration, the `init_priority` mechanism.) Where does ours stay simpler? Document one design decision Zephyr made that you would adopt in your code and one that you would not.

---

## Submission

Push your homework answers as `homework/h1.md` … `homework/h6.md` in the Week-4 repo. Bring questions to Friday studio.
