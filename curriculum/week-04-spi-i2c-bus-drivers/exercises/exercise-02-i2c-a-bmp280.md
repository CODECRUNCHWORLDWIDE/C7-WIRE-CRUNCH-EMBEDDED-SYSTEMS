# Exercise 2 — I²C a BMP280

> *Read the BMP280's `id` register at `0xD0` (must return `0x58`), then read raw temperature + pressure, then compensate to °C and Pa with the Bosch integer formulas.*

## Goal

Bring up the RP2040's I²C0 peripheral from registers up, wire a BMP280 module to GP4/GP5 with proper pull-ups, and read three things in increasing complexity: (1) the chip ID, (2) the 24-byte calibration block, (3) compensated temperature and pressure values that match a reference thermometer to within ±0.5 °C and a reference barometer to within ±2 hPa.

By the end you can:
- Configure I²C0 for standard-mode 100 kHz with 7-bit addressing.
- Send a write-then-read transaction with RESTART (one of the most common I²C patterns).
- Recognize the `IC_TX_ABRT_SOURCE` bits when the BMP280 NACKs.
- Implement the Bosch 32-bit pressure compensation arithmetic from the datasheet, line for line.

## Setup

### Parts

- 1× Pi Pico W
- 1× BMP280 breakout board (e.g. Adafruit 2651, Sparkfun SEN-13676, generic GY-BMP280)
- 1× breadboard
- 4× male-to-male jumper wires
- 2× 4.7 kΩ resistors (for I²C pull-ups)
- 1× Saleae Logic 8 or equivalent

### Wiring

| Pi Pico W   | BMP280 module     | Signal       |
|-------------|-------------------|--------------|
| GP4         | `SDA` (or `SDI`)  | SDA          |
| GP5         | `SCL` (or `SCK`)  | SCL          |
| 3V3 (pin 36)| `VIN` / `VCC`     | 3.3 V supply |
| GND (pin 38)| `GND`             | Ground       |

Pull-ups:
- 4.7 kΩ from GP4 (SDA) to 3.3 V.
- 4.7 kΩ from GP5 (SCL) to 3.3 V.

**Important:** Many BMP280 breakouts include on-board pull-ups (typically 10 kΩ). If yours does, you can skip the external ones, but verify by ohmmeter before relying on it.

Address selection: if your BMP280 module has an `SDO` pin or `ADDR` jumper, tie SDO to GND for address `0x76` (default in this exercise). Tie SDO to VDD for `0x77`.

Logic analyzer:
- Ch0: GP5 (SCL)
- Ch1: GP4 (SDA)

### Prerequisites

- Week 3 mini-project on GitHub.
- Exercise 1 of this week completed (or skipped — Exercise 2 does not depend on Ex.1's code).

## Reading

- **RP2040 datasheet** §4.3.16 (I²C register list), pp. 463–502. Focus on `IC_CON`, `IC_TAR`, `IC_DATA_CMD`, `IC_STATUS`, `IC_TX_ABRT_SOURCE`, `IC_ENABLE`.
- **RP2040 datasheet** §4.3.5 (Timing parameters), p. 442 — the `HCNT`/`LCNT` formula.
- **RP2040 datasheet** §2.19 (IO_BANK0), p. 244, Table 269 — GP4/GP5 map to I²C0 on FUNCSEL=3.
- **RP2040 datasheet** §2.7 (RESETS), p. 105 — bit 3 of `RESETS_RESET` is I2C0.
- **BMP280 datasheet** (Bosch rev 1.20), §3.11 (Calibration data), p. 23.
- **BMP280 datasheet** §4 (Memory map), pp. 24–28.
- **BMP280 datasheet** §3.3.3 / Appendix 8.1 — the integer compensation reference code.
- **UM10204** §3.1 (Protocol), pp. 9–14.
- **Lecture 2** of this week.

## Steps

### 1. Add I²C register definitions

Create `rp2040_i2c.h`:

```c
#ifndef RP2040_I2C_H
#define RP2040_I2C_H

#include <stdint.h>

#define I2C0_BASE  0x40044000u
#define I2C1_BASE  0x40048000u

typedef struct {
    volatile uint32_t IC_CON;          /* 0x00 */
    volatile uint32_t IC_TAR;          /* 0x04 */
    volatile uint32_t IC_SAR;          /* 0x08 */
    volatile uint32_t reserved_0c;     /* 0x0C */
    volatile uint32_t IC_DATA_CMD;     /* 0x10 */
    volatile uint32_t IC_SS_SCL_HCNT;  /* 0x14 */
    volatile uint32_t IC_SS_SCL_LCNT;  /* 0x18 */
    volatile uint32_t IC_FS_SCL_HCNT;  /* 0x1C */
    volatile uint32_t IC_FS_SCL_LCNT;  /* 0x20 */
    volatile uint32_t reserved_24[2];
    volatile uint32_t IC_INTR_STAT;    /* 0x2C */
    volatile uint32_t IC_INTR_MASK;    /* 0x30 */
    volatile uint32_t IC_RAW_INTR_STAT;/* 0x34 */
    volatile uint32_t reserved_38[6];
    volatile uint32_t IC_CLR_INTR;     /* 0x40 */
    volatile uint32_t reserved_44[3];
    volatile uint32_t IC_CLR_TX_ABRT;  /* 0x54 */
    volatile uint32_t reserved_58[4];
    volatile uint32_t IC_ENABLE;       /* 0x6C */
    volatile uint32_t IC_STATUS;       /* 0x70 */
    volatile uint32_t IC_TXFLR;        /* 0x74 */
    volatile uint32_t IC_RXFLR;        /* 0x78 */
    volatile uint32_t reserved_7c;
    volatile uint32_t IC_TX_ABRT_SOURCE;/* 0x80 */
    /* ... omitted ... */
} i2c_t;

#define I2C0  ((i2c_t *)I2C0_BASE)
#define I2C1  ((i2c_t *)I2C1_BASE)

#define IC_CON_MASTER_MODE      (1u << 0)
#define IC_CON_SPEED_STD        (1u << 1)
#define IC_CON_SPEED_FAST       (2u << 1)
#define IC_CON_RESTART_EN       (1u << 5)
#define IC_CON_SLAVE_DISABLE    (1u << 6)

#define IC_DATA_CMD_CMD_READ    (1u << 8)
#define IC_DATA_CMD_STOP        (1u << 9)
#define IC_DATA_CMD_RESTART     (1u << 10)

#define IC_STATUS_TFNF          (1u << 1)
#define IC_STATUS_RFNE          (1u << 3)

#define IC_RAW_INTR_TX_ABRT     (1u << 6)

#endif
```

### 2. Write `i2c0_init_100khz`

```c
void i2c0_init_100khz(void) {
    resets_clear(1u << 3);
    while ((resets_done() & (1u << 3)) == 0) { }

    io_bank_funcsel(4, 3);   /* GP4 -> I2C0_SDA */
    io_bank_funcsel(5, 3);   /* GP5 -> I2C0_SCL */

    I2C0->IC_ENABLE = 0;

    I2C0->IC_CON = IC_CON_MASTER_MODE
                 | IC_CON_SPEED_STD
                 | IC_CON_RESTART_EN
                 | IC_CON_SLAVE_DISABLE;

    /* 100 kHz at clk_peri = 125 MHz; see Lecture 2 / SDK source. */
    I2C0->IC_SS_SCL_HCNT = 650;
    I2C0->IC_SS_SCL_LCNT = 730;

    I2C0->IC_ENABLE = 1;
}
```

**Checkpoint:** Scope SDA and SCL at idle. Both should be at 3.3 V (high). If either is low, your pull-ups are missing or wired wrong.

### 3. Write `i2c0_write_read`

This is the transport-layer kernel. Both BMP280 reads and the MPU-6050 reads next week use this exact function.

```c
int i2c0_write_read(uint8_t addr,
                    const uint8_t *tx, size_t tx_len,
                    uint8_t *rx, size_t rx_len) {
    I2C0->IC_ENABLE = 0;
    I2C0->IC_TAR = addr;
    I2C0->IC_ENABLE = 1;

    for (size_t i = 0; i < tx_len; i++) {
        uint32_t cmd = tx[i];
        if (i == tx_len - 1 && rx_len == 0) cmd |= IC_DATA_CMD_STOP;
        while (!(I2C0->IC_STATUS & IC_STATUS_TFNF)) { }
        I2C0->IC_DATA_CMD = cmd;
    }

    for (size_t i = 0; i < rx_len; i++) {
        uint32_t cmd = IC_DATA_CMD_CMD_READ;
        if (i == 0)            cmd |= IC_DATA_CMD_RESTART;
        if (i == rx_len - 1)   cmd |= IC_DATA_CMD_STOP;
        while (!(I2C0->IC_STATUS & IC_STATUS_TFNF)) { }
        I2C0->IC_DATA_CMD = cmd;
    }

    for (size_t i = 0; i < rx_len; i++) {
        while (!(I2C0->IC_STATUS & IC_STATUS_RFNE)) {
            if (I2C0->IC_RAW_INTR_STAT & IC_RAW_INTR_TX_ABRT) {
                uint32_t src = I2C0->IC_TX_ABRT_SOURCE;
                (void)I2C0->IC_CLR_TX_ABRT;
                return -(int)src;
            }
        }
        rx[i] = (uint8_t)(I2C0->IC_DATA_CMD & 0xFF);
    }
    return (int)rx_len;
}
```

### 4. Read the BMP280 `id`

```c
int main(void) {
    i2c0_init_100khz();

    uint8_t reg = 0xD0;
    uint8_t id;
    int r = i2c0_write_read(0x76, &reg, 1, &id, 1);
    if (r < 0) {
        uart_printf("i2c error: %d\n", r);
        hang();
    }
    uart_printf("BMP280 id = 0x%02X (expected 0x58)\n", id);
}
```

**Checkpoint:** UART prints `BMP280 id = 0x58`. Capture the I²C transaction on Saleae — you should see one START, address `0x76` + write bit (so `0xEC` on the wire MSB-first), ACK, register `0xD0`, ACK, RESTART, address `0x76` + read bit (`0xED`), ACK, response byte `0x58`, NACK from master, STOP.

### 5. Read the 24-byte calibration block

The BMP280 stores 12 little-endian 16-bit calibration coefficients at registers `0x88`–`0x9F`. Read them in one transaction:

```c
uint8_t calib[24];
uint8_t reg = 0x88;
i2c0_write_read(0x76, &reg, 1, calib, 24);

uint16_t dig_T1 = (calib[1] << 8) | calib[0];
int16_t  dig_T2 = (calib[3] << 8) | calib[2];
int16_t  dig_T3 = (calib[5] << 8) | calib[4];
uint16_t dig_P1 = (calib[7] << 8) | calib[6];
int16_t  dig_P2 = (calib[9] << 8) | calib[8];
int16_t  dig_P3 = (calib[11] << 8) | calib[10];
int16_t  dig_P4 = (calib[13] << 8) | calib[12];
int16_t  dig_P5 = (calib[15] << 8) | calib[14];
int16_t  dig_P6 = (calib[17] << 8) | calib[16];
int16_t  dig_P7 = (calib[19] << 8) | calib[18];
int16_t  dig_P8 = (calib[21] << 8) | calib[20];
int16_t  dig_P9 = (calib[23] << 8) | calib[22];
```

**Checkpoint:** Print the values. They are factory-burned and will be different per chip, but each should be plausibly nonzero. Typical orders of magnitude: `dig_T1 ≈ 27000`, `dig_T2 ≈ 26000`, `dig_T3` small (might be negative).

### 6. Configure the BMP280 for normal-mode sampling

Write `ctrl_meas` at `0xF4` to `0x27` (oversampling x1 for both T and P, normal mode):

```c
uint8_t tx[2] = { 0xF4, 0x27 };
i2c0_write_read(0x76, tx, 2, NULL, 0);   /* write-only, no read */
```

### 7. Read the raw temperature and pressure

```c
uint8_t buf[6];
uint8_t reg = 0xF7;
i2c0_write_read(0x76, &reg, 1, buf, 6);

int32_t adc_P = ((int32_t)buf[0] << 12) | ((int32_t)buf[1] << 4) | (buf[2] >> 4);
int32_t adc_T = ((int32_t)buf[3] << 12) | ((int32_t)buf[4] << 4) | (buf[5] >> 4);
```

**Checkpoint:** `adc_T` should be in the range 400000–600000 at room temperature. `adc_P` is around 300000–500000 at sea level. The values are unsigned 20-bit fields; the formulas below convert them.

### 8. Implement the integer compensation arithmetic

Verbatim from BMP280 datasheet Appendix 8.1, p. 45:

```c
static int32_t t_fine;

static int32_t bmp280_compensate_T(int32_t adc_T) {
    int32_t var1, var2, T;
    var1 = ((((adc_T >> 3) - ((int32_t)dig_T1 << 1))) * ((int32_t)dig_T2)) >> 11;
    var2 = (((((adc_T >> 4) - ((int32_t)dig_T1)) *
              ((adc_T >> 4) - ((int32_t)dig_T1))) >> 12) *
            ((int32_t)dig_T3)) >> 14;
    t_fine = var1 + var2;
    T = (t_fine * 5 + 128) >> 8;
    return T;   /* °C × 100 */
}

static uint32_t bmp280_compensate_P(int32_t adc_P) {
    int64_t var1, var2, p;
    var1 = ((int64_t)t_fine) - 128000;
    var2 = var1 * var1 * (int64_t)dig_P6;
    var2 = var2 + ((var1 * (int64_t)dig_P5) << 17);
    var2 = var2 + (((int64_t)dig_P4) << 35);
    var1 = ((var1 * var1 * (int64_t)dig_P3) >> 8) +
           ((var1 * (int64_t)dig_P2) << 12);
    var1 = (((((int64_t)1) << 47) + var1)) * ((int64_t)dig_P1) >> 33;
    if (var1 == 0) return 0;
    p = 1048576 - adc_P;
    p = (((p << 31) - var2) * 3125) / var1;
    var1 = (((int64_t)dig_P9) * (p >> 13) * (p >> 13)) >> 25;
    var2 = (((int64_t)dig_P8) * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (((int64_t)dig_P7) << 4);
    return (uint32_t)p;   /* Q24.8 Pa */
}
```

The pressure result is in Q24.8 fixed point: divide by 256 to get Pa, divide by 25600 to get hPa.

**Checkpoint:** Print temperature and pressure. At sea level on a calm day you should see ~25 °C and ~1013 hPa. If your office is at altitude, expect ~10 hPa per 100 m of elevation lower than 1013.

### 9. Capture the Saleae trace

Capture the full sequence: chip ID read, calibration read, ctrl_meas write, data read. Decode with the I²C protocol analyzer. Save as `traces/ex02-bmp280-bringup.sal`.

## Artifact

Commit to `exercises/ex02/`:

1. `rp2040_i2c.h`
2. `i2c.c` — `i2c0_init_100khz`, `i2c0_write_read`.
3. `bmp280.c` — `bmp280_init`, `bmp280_read_raw`, `bmp280_compensate_T`, `bmp280_compensate_P`.
4. `main.c` — reads + prints every 1 s over UART.
5. `Makefile` extension.
6. `traces/ex02-bmp280-bringup.sal`.
7. `README.md` — one paragraph summary; include the printed `id` value and a sample temperature/pressure pair.

## Common faults

| Symptom | Cause | First diagnostic |
|---|---|---|
| `i2c_write_read` returns `-(1)` (= `ABRT_7B_ADDR_NOACK`) | Wrong slave address — module has SDO tied to VDD (use `0x77`) | Scope SDA on the 9th SCL pulse of the address byte; SDA high = no ACK |
| Returns `-(8)` (`ABRT_TXDATA_NOACK`) | Slave ACK'd address but NACK'd a register byte — wrong register or module faulted | Cross-check datasheet §4, p. 24 |
| `id` reads `0xFF` consistently | Bus stuck or no slave response on a missing pull-up | Scope both lines at idle; both should be at 3.3 V |
| `id` reads `0x60` instead of `0x58` | You have a BME280 (the BMP280's cousin with humidity), not a BMP280 | Use the BME280 driver or the BMP280 + humidity-disabled config; see Bosch BST-BME280-DS002 |
| SDA stuck low at boot | Bus left in mid-transaction by previous firmware; not recovered | Run the nine-clock recovery from Lecture 2 in your init |
| Compensation produces negative pressure | `t_fine` not set before pressure compensation | Always call `bmp280_compensate_T` *first*, even if you only want pressure |
| Compensation produces ~24 hPa | Forgot to divide the Q24.8 result by 256 | Divide `p` by 256 for Pa, by 25600 for hPa |
| Bus runs but every 5th transaction NACKs | Pull-up too weak; rising edge marginal | Drop pull-up to 2.2 kΩ; or slow bus to 50 kHz |

## Stretch goals

- Replace standard mode with fast mode (400 kHz). Use `IC_FS_SCL_HCNT`/`LCNT`. Measure the BMP280 throughput improvement.
- Add the BMP280 IIR filter (config register `0xF5`, bits 4:2). Set to coefficient 4 and observe smoothing of pressure readings.
- Implement the `forced` measurement mode instead of `normal`. Force one measurement, wait for the `measuring` bit in `status` to clear, read the result, sleep. Average power drops by ~10× at 1 Hz polling.
- Add a second I²C device on the same bus — an MPU-6050 at `0x68`. Read the `WHO_AM_I` at `0x75` (must return `0x68`) and confirm both devices respond on the same wire. (This is exactly what the mini-project requires, so doing it here gets you ahead.)

## Hand-in

When the UART prints stable temperature within ±0.5 °C of an external thermometer and pressure within ±2 hPa of an external barometer (or the published value from a nearby weather station for your local altitude), push and tag `w04-ex02`.
