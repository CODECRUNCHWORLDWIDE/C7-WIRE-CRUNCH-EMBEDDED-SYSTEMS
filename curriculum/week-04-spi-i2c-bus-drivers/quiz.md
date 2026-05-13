# Week 4 — Quiz

Ten questions, ~30 minutes. Reference manuals open — this is a datasheet-grade quiz, not a memorization test. Cite a page number on every answer that warrants one.

---

## Q1 — SPI mode

The SSD1306 (Solomon Systech rev 1.1, datasheet §8.1.5, p. 19) latches MOSI on the rising edge of SCK, with SCK idling low.

What are the values of CPOL and CPHA? Which SPI mode is this (0/1/2/3)? Which two bits of the RP2040's `SSPCR0` register encode them, and what hex value would you write to `SSPCR0[7:6]` for this configuration?

---

## Q2 — SCK frequency arithmetic

The RP2040's SPI clock is derived from `clk_peri` (assume 125 MHz post-SDK boot) by `Fsck = clk_peri / (CPSDVSR × (1 + SCR))`, where `CPSDVSR` is even and ≥ 2, `SCR` is an integer ≥ 0.

You need to drive an SSD1306 at 4 MHz. List all `(CPSDVSR, SCR)` pairs that yield `Fsck` within ±5% of 4 MHz with `CPSDVSR ≤ 254`. Which pair gives the smallest absolute error?

---

## Q3 — I²C address arithmetic

The BMP280's 7-bit address is `0x76` (SDO tied to GND, datasheet §5.2, p. 31). What 8-bit byte is shifted out by the master for a *write* to this device? What 8-bit byte is shifted out for a *read*? Show the bit pattern of each (8 bits, MSB first).

---

## Q4 — RESTART vs STOP+START

Under UM10204 §3.1.7 (p. 13), a master may issue a RESTART instead of STOP+START between two transactions to the same slave. Name one *electrical* difference between the two on the wire, and one *bus-arbitration* difference that matters when another master is present.

---

## Q5 — Clock stretching

A BMP280 in forced mode at oversampling `x16` for both T and P spends ~22 ms measuring (datasheet §3.8.2, p. 21). Suppose the master reads register `0xF7` while a measurement is in progress.

Will the bus clock stretch? At which bit position of the transaction (address byte, register-pointer byte, ACK after register-pointer, first data byte, etc.)? Cite the BMP280 datasheet page that justifies your answer.

---

## Q6 — IC_DATA_CMD encoding

You want to read 4 consecutive bytes from MPU-6050 register `0x3B` (ACCEL_XOUT_H). The MPU-6050 is at I²C address `0x68`. Using the RP2040's `IC_DATA_CMD` register (datasheet §4.3.16, p. 471), write the sequence of FIFO pushes (one line per push) including the `CMD`, `STOP`, and `RESTART` bits where appropriate. Indicate the bit field of each push as a hex value (bits 10:0).

---

## Q7 — Pull-up sizing

You are designing a board with the BMP280, MPU-6050, and SSD1306 (I²C variant) on a shared I²C bus running at fast mode 400 kHz. The estimated bus capacitance is 150 pF. The supply is 3.3 V.

Per UM10204 §7.2 (p. 41), what is the maximum acceptable pull-up resistor value? What is the minimum acceptable value (to satisfy `VOL` at `IOL = 3 mA`, UM10204 §7.1, p. 39)? Pick a single value within that window and justify.

---

## Q8 — Transport/protocol split

Below is a code snippet from a hypothetical BMP280 driver:

```c
int bmp280_read_pressure(bmp280_t *dev, uint32_t *p_pa) {
    I2C0->IC_TAR = 0x76;
    /* ... write register pointer, read 3 bytes ... */
}
```

Identify the abstraction leak. What concrete refactor would you propose to make this driver bus-agnostic? Cite the relevant lecture section.

---

## Q9 — Bus-stuck diagnostic

You scope the I²C bus at idle and observe:
- SDA: stuck at 0 V.
- SCL: stuck at 3.3 V.

The master is not transmitting. Three slaves are wired to the bus.

Per UM10204 §3.1.16 (p. 20), what is the recovery procedure? Write the procedure as a numbered list. After the procedure runs, what condition tells you whether the bus has recovered? If the procedure fails, what is the next step?

---

## Q10 — The MPU-6050 wake-up

Per the MPU-6050 register map (InvenSense rev 4.2, §4.28), register `0x6B` (`PWR_MGMT_1`) has bit 6 = `SLEEP`. The power-on reset value of `PWR_MGMT_1` is `0x40`.

What is the consequence of skipping the wake-up step in your driver? Specifically: if you read `ACCEL_XOUT_H` (register `0x3B`) immediately after power-on without writing `PWR_MGMT_1` first, what value do you read? Justify by citing the datasheet.

---

## Scoring

10 points per question; 70 to pass. Reviewer signs off; you can retake once per week if needed.

The questions that most often trip students:
- Q2 (the `CPSDVSR` must be *even* — students miss this and pick odd values).
- Q5 (the BMP280 stretches on the *data byte read*, not on the register-pointer phase; many students guess the wrong bit position).
- Q6 (the `RESTART` bit goes on the *first read* push, not on the *last write* push; the SDK source disagrees with naive intuition).
- Q9 (the recovery is on the master side; many students propose power-cycling the slave, which is also a valid step but not the *first* one per UM10204).
