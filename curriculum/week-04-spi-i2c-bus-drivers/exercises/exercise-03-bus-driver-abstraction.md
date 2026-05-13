# Exercise 3 — Bus driver abstraction

> *Refactor your Exercise 1 and Exercise 2 code so that `ssd1306.c` and `bmp280.c` share a single `bus_t` interface. Neither protocol driver may know which bus it is on. Prove it with a host-side unit test.*

## Goal

Lift the SSD1306 driver and the BMP280 driver above an explicit `bus_t` abstraction (see Lecture 3). The protocol drivers must compile cleanly with no references to peripheral registers, no `RESETS_` writes, no GPIO pokes. The transport drivers (`i2c0_transport`, `spi0_transport`) hold all that knowledge.

By the end you can:
- Hand the BMP280 driver a *mock* `bus_t` on the host (no MCU connected) and run the init sequence as a unit test.
- Swap a BMP280 from I²C to SPI by changing one `bus_t` instantiation in `main.c` — without touching `bmp280.c`.
- Defend the layer split in a code review, citing what each layer is and is not responsible for.

## Setup

### Parts

Same as Exercises 1 + 2. You also need a development host (macOS, Linux, or WSL) with `gcc` and `make` for the host-side unit test.

### Prerequisites

- Exercises 1 and 2 completed and on GitHub.
- Lecture 3 read.

## Reading

- **Lecture 3** of this week.
- The Bosch BMP280 driver: <https://github.com/boschsensortec/BMP280_driver/blob/master/bmp280.c> — pay attention to its `read` and `write` function-pointer fields. Bosch's "we don't know your transport" abstraction is the same shape as ours.
- The Zephyr BME280 driver: <https://github.com/zephyrproject-rtos/zephyr/blob/main/drivers/sensor/bosch/bme280/bme280.c> — production-grade example of the split.

## Steps

### 1. Define the `bus_t` interface

Create `bus.h`:

```c
#ifndef BUS_H
#define BUS_H

#include <stddef.h>
#include <stdint.h>

typedef struct bus_s bus_t;

struct bus_s {
    int  (*write)(bus_t *self, const uint8_t *tx, size_t tx_len);
    int  (*write_read)(bus_t *self,
                       const uint8_t *tx, size_t tx_len,
                       uint8_t *rx, size_t rx_len);
    void *ctx;
};

#endif
```

### 2. Implement `i2c_transport`

Create `i2c_transport.h` / `i2c_transport.c`:

```c
/* i2c_transport.h */
typedef struct {
    void   *peri;       /* i2c_t * — cast inside the transport */
    uint8_t address;    /* 7-bit slave address */
} i2c_transport_ctx_t;

void i2c_transport_make(bus_t *out, i2c_transport_ctx_t *ctx);
```

```c
/* i2c_transport.c */
#include "i2c_transport.h"
#include "rp2040_i2c.h"

extern int i2c0_write_read(uint8_t addr,
                           const uint8_t *tx, size_t tx_len,
                           uint8_t *rx, size_t rx_len);

static int i2c_t_write(bus_t *self, const uint8_t *tx, size_t tx_len) {
    i2c_transport_ctx_t *c = self->ctx;
    return i2c0_write_read(c->address, tx, tx_len, NULL, 0);
}

static int i2c_t_write_read(bus_t *self,
                            const uint8_t *tx, size_t tx_len,
                            uint8_t *rx, size_t rx_len) {
    i2c_transport_ctx_t *c = self->ctx;
    return i2c0_write_read(c->address, tx, tx_len, rx, rx_len);
}

void i2c_transport_make(bus_t *out, i2c_transport_ctx_t *ctx) {
    out->write = i2c_t_write;
    out->write_read = i2c_t_write_read;
    out->ctx = ctx;
}
```

### 3. Implement `spi_transport`

Mirror structure. The `ctx` holds peripheral pointer + CSn pin number. Read-by-write reads dummy 0xFF bytes (the SSD1306 is write-only, so reads are not exercised — but you implement the function anyway because next week's flash driver will use it).

### 4. Refactor `bmp280.c`

Rewrite `bmp280.c` to take a `bus_t *bus` in its init and never call `i2c0_write_read` directly. Every register access goes through `dev->bus->write` or `dev->bus->write_read`. See Lecture 3 for the structure.

After refactoring, run:

```sh
grep -E '(i2c|spi|RESETS|IO_BANK|0x4004|0x4003)' bmp280.c
```

This must return **nothing**. If it returns anything, the abstraction has leaked.

### 5. Refactor `ssd1306.c`

Same: `ssd1306_init` takes a `bus_t *bus`. The CSn and D/C̄ control move into the SPI transport — the SSD1306 driver only knows `bus->write(buf, len)` to send commands or data.

The D/C̄ line is *not* part of standard SPI. You have two options:
- **(a)** Embed D/C̄ control in the SSD1306 transport, with a "this is a command" flag added to the SSD1306-specific subclass of `bus_t`. Less clean.
- **(b)** Have the SSD1306 driver call a separate `ssd1306_dc_pin` API that goes directly to the GPIO. Cleaner abstraction-wise but leaks GPIO knowledge into the protocol layer.
- **(c)** Use the SSD1306 over I²C instead, where D/C̄ is encoded in the first byte of each transaction (the `Co | D/C` byte; datasheet §8.1.5.1 p. 20). Cleanest.

For this exercise pick (b) and document the leak in `README.md`. The mini-project uses (c) for the second OLED.

### 6. Wire it up in `main.c`

```c
int main(void) {
    /* Hardware bring-up. */
    i2c0_init_100khz();
    spi0_init_1mhz();

    /* BMP280 on I²C at 0x76. */
    i2c_transport_ctx_t bmp_ctx = { .peri = I2C0, .address = 0x76 };
    bus_t bmp_bus;
    i2c_transport_make(&bmp_bus, &bmp_ctx);

    /* SSD1306 on SPI with CSn = GP17. */
    spi_transport_ctx_t oled_ctx = { .peri = SPI0, .cs_pin = 17 };
    bus_t oled_bus;
    spi_transport_make(&oled_bus, &oled_ctx);

    bmp280_t bmp;
    ssd1306_t oled;
    bmp280_init(&bmp, &bmp_bus);
    ssd1306_init(&oled, &oled_bus);

    for (;;) {
        int32_t t_centi;
        uint32_t p_pa;
        bmp280_read(&bmp, &t_centi, &p_pa);
        char line[32];
        snprintf(line, sizeof(line), "T=%ld.%02ld C", t_centi/100, t_centi%100);
        ssd1306_clear(&oled);
        ssd1306_set_cursor(&oled, 0, 0);
        ssd1306_puts(&oled, line);
        snprintf(line, sizeof(line), "P=%lu Pa", p_pa);
        ssd1306_set_cursor(&oled, 0, 2);
        ssd1306_puts(&oled, line);
        delay_ms(250);
    }
}
```

**Checkpoint:** Flash, observe live temperature/pressure on the OLED, refreshed at 4 Hz.

### 7. Write the host-side unit test

Create `tests/test_bmp280.c`:

```c
#include "bmp280.h"
#include "bus.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Scripted mock: a queue of expected (tx, rx) pairs. */
typedef struct {
    const uint8_t *expected_tx;
    size_t         expected_tx_len;
    const uint8_t *canned_rx;
    size_t         canned_rx_len;
} mock_step_t;

typedef struct {
    mock_step_t *steps;
    size_t       step_count;
    size_t       cursor;
} mock_ctx_t;

static int mock_write(bus_t *self, const uint8_t *tx, size_t tx_len) {
    mock_ctx_t *m = self->ctx;
    mock_step_t *s = &m->steps[m->cursor++];
    assert(tx_len == s->expected_tx_len);
    assert(memcmp(tx, s->expected_tx, tx_len) == 0);
    return (int)tx_len;
}

static int mock_write_read(bus_t *self,
                           const uint8_t *tx, size_t tx_len,
                           uint8_t *rx, size_t rx_len) {
    mock_ctx_t *m = self->ctx;
    mock_step_t *s = &m->steps[m->cursor++];
    assert(tx_len == s->expected_tx_len);
    assert(memcmp(tx, s->expected_tx, tx_len) == 0);
    assert(rx_len == s->canned_rx_len);
    memcpy(rx, s->canned_rx, rx_len);
    return (int)rx_len;
}

int main(void) {
    /* Script: bmp280_init reads id (expects 0x58),
     * then reads 24 bytes of calibration,
     * then writes ctrl_meas = 0x27. */
    static const uint8_t id_tx[] = {0xD0};
    static const uint8_t id_rx[] = {0x58};
    static const uint8_t cal_tx[] = {0x88};
    static const uint8_t cal_rx[24] = { /* known reference values */
        0xE6, 0x6E, 0x59, 0x67, 0x18, 0xFC,  /* dig_T1..T3 little-endian */
        0x6D, 0x90, 0xBC, 0xD6, 0xD0, 0x0B,
        0x10, 0x04, 0xC4, 0xFF, 0xF9, 0xFF,
        0x8C, 0x3C, 0xF8, 0xC6, 0x70, 0x17
    };
    static const uint8_t ctrl_tx[] = {0xF4, 0x27};

    static mock_step_t steps[] = {
        { id_tx,   1, id_rx,  1 },
        { cal_tx,  1, cal_rx, 24 },
        { ctrl_tx, 2, NULL,   0 },
    };
    mock_ctx_t mock = { .steps = steps, .step_count = 3, .cursor = 0 };

    bus_t bus = {
        .write = mock_write,
        .write_read = mock_write_read,
        .ctx = &mock
    };

    bmp280_t dev;
    int r = bmp280_init(&dev, &bus);
    assert(r == 0);
    assert(mock.cursor == 3);

    /* Now exercise the compensation arithmetic with known raw values. */
    int32_t t_centi;
    uint32_t p_pa;
    /* (Bosch's reference vector: adc_T = 0x80000, adc_P = 0x80000 → known result.) */
    bmp280_compensate(&dev, 0x80000, 0x80000, &t_centi, &p_pa);
    /* Assert plausibly close to a value computed by Python or Bosch's ref impl. */
    assert(t_centi > 2000 && t_centi < 3000);    /* ~20–30 °C */
    assert(p_pa > 90000 && p_pa < 110000);       /* ~900–1100 hPa */

    printf("test_bmp280: PASS\n");
    return 0;
}
```

Build and run on the host:

```sh
gcc -std=c11 -Wall -Wextra -I. tests/test_bmp280.c bmp280.c -o test_bmp280
./test_bmp280
```

**Checkpoint:** Prints `test_bmp280: PASS`. No MCU involved.

### 8. (Optional) Move the BMP280 to SPI

If you have a BMP280 module that exposes its SPI pins (CSB, SDI, SDO, SCK), wire it to SPI1 (GP10–GP13) and add a *second* transport binding in `main.c`:

```c
spi_transport_ctx_t bmp_spi_ctx = { .peri = SPI1, .cs_pin = 13 };
bus_t bmp_spi_bus;
spi_transport_make(&bmp_spi_bus, &bmp_spi_ctx);

bmp280_t bmp;
bmp280_init(&bmp, &bmp_spi_bus);    /* same driver, different bus */
```

The BMP280 over SPI uses a slightly different register-access convention (bit 7 of the address byte is R/W̄: 1 = read, 0 = write). You will need to extend `bmp280.c`'s `rd_reg`/`wr_reg` to encode this, *but* in a way that does not leak transport knowledge: do the encoding in the SPI variant of the BMP280 transport, or in a `bmp280_set_protocol()` mode setter. Optimal: encode the "high bit = read" convention inside the SPI transport's `ctx`.

This is the test of your abstraction. If you can swap I²C ↔ SPI by changing one line in `main.c`, the split is good.

## Artifact

Commit to `exercises/ex03/`:

1. `bus.h`
2. `i2c_transport.h` / `.c`
3. `spi_transport.h` / `.c`
4. `bmp280.h` / `.c` — refactored, no peripheral imports.
5. `ssd1306.h` / `.c` — refactored, no peripheral imports.
6. `main.c` — wires it together; renders T + P on the OLED.
7. `tests/test_bmp280.c` — host-side unit test.
8. `tests/Makefile` — builds and runs the host test.
9. `Makefile` extension for the on-target build.
10. `README.md` — describe the abstraction, list any leaks you deliberately accepted (e.g. the SSD1306 D/C̄ pin per step 5), and show the `grep` output proving `bmp280.c` is bus-agnostic.

## Common faults

| Symptom | Cause | First diagnostic |
|---|---|---|
| `bmp280.c` won't compile against the host test | `bmp280.c` still includes `rp2040_i2c.h` | Remove the include; the only header it needs is `bus.h` |
| Host test crashes in `mock_write` | Mock step count mismatch — driver made more calls than the script expects | Print `m->cursor` on each call to find which step failed |
| Compensation asserts trip | Calibration bytes in `cal_rx[]` don't produce a plausible result for the chosen raw values | Recompute the expected output with Python: `numpy` + the Bosch ref code |
| Target build links but BMP280 returns -1 from init | Wrong slave address in the I²C transport `ctx`. Confirm `0x76` (SDO=GND) vs `0x77` (SDO=VDDIO). | Scope SDA for the address byte |
| SSD1306 init succeeds but nothing renders | D/C̄ pin not toggled correctly because the abstraction hid it | Move D/C̄ control into the SPI transport's command-vs-data distinction, or use a side-channel API |

## Stretch goals

- Add a `bus->probe(addr)` method to the I²C transport. It sends an address-only transaction and returns whether the slave ACKs. Use it in `main.c` to enumerate the bus at boot.
- Wrap each `bus_t` in a FreeRTOS mutex (preview — Week 9). The mutex is acquired inside `write` and `write_read`; the protocol layer is unaware.
- Add timing instrumentation: every `bus_t->write_read` call records `time_us_before`, `time_us_after`, and the difference into a circular buffer. Print the histogram from `main` once a minute. This is the bench discipline that catches a slowing-down bus before it fails.

## Hand-in

Push and tag `w04-ex03` when:
- The on-target build flashes a working firmware that shows BMP280 readings on the SSD1306.
- The host-side unit test passes.
- `grep -E '(i2c|spi|RESETS|IO_BANK)' bmp280.c` returns empty.
- The reviewer signs off on the abstraction in code review.
