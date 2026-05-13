# Lecture 3 — Driver design patterns: transport vs protocol

> *A BMP280 driver should not know it is on SPI. A BMP280 driver should not know it is on I²C either. A BMP280 driver should know how to read pressure from a BMP280.*

## The split

Every sensor or display driver decomposes into two layers. Above the line: *protocol*. Below the line: *transport*.

```
+-----------------------------------------------+
|  Protocol layer (per-device)                  |
|    bmp280.c   mpu6050.c   ssd1306.c           |
|    "I know how a BMP280 is structured."       |
+-----------------------------------------------+
|  Transport layer (per-bus, per-peripheral)    |
|    i2c0_transport.c   spi0_transport.c        |
|    "I know how to push bytes on this wire."   |
+-----------------------------------------------+
|  Hardware                                     |
|    RP2040 I2C0 / SPI0 peripherals             |
+-----------------------------------------------+
```

A protocol-layer module is a *device driver* in the strict sense: it knows the chip's register map, its init sequence, its compensation arithmetic, its forced-mode timing. A transport-layer module is a *bus driver*: it knows how to push and pull bytes on a specific peripheral instance with specific pin routing and specific clock timing.

The contract between them is a *bus interface*. In C, that is a struct of function pointers. In C++ it is a class with virtual methods (or a CRTP template, see Week 4 of the syllabus). In Rust it is a trait (`embedded_hal::i2c::I2c`, `embedded_hal::spi::SpiDevice`). The names differ; the discipline does not.

## The `bus_t` interface

A minimal C bus interface for I²C-style register devices:

```c
/* bus.h */
#ifndef BUS_H
#define BUS_H

#include <stddef.h>
#include <stdint.h>

typedef struct bus_s bus_t;

struct bus_s {
    /* Write tx_len bytes from tx_buf to the device.
     * Returns tx_len on success, negative on error. */
    int (*write)(bus_t *self, const uint8_t *tx_buf, size_t tx_len);

    /* Write tx_len bytes, then (with RESTART or CSn held) read rx_len bytes.
     * For I2C: write-restart-read. For SPI: write tx_buf, then clock rx_len
     * dummy bytes and capture the responses.
     * Returns rx_len on success, negative on error. */
    int (*write_read)(bus_t *self,
                      const uint8_t *tx_buf, size_t tx_len,
                      uint8_t *rx_buf, size_t rx_len);

    /* Opaque context: peripheral pointer, slave address, CSn pin, etc. */
    void *ctx;
};

#endif
```

Three function pointers and a `void *ctx`. That is the whole interface. Every protocol-layer driver takes a `bus_t *` and never knows what is in `ctx`.

The `void *ctx` is the load-bearing trick. For an I²C transport, `ctx` holds the peripheral base address and the slave address. For an SPI transport, `ctx` holds the peripheral base address and the CSn GPIO number. The transport's `write` function casts `ctx` back to its concrete type and uses what it needs.

## A protocol-layer driver against `bus_t`

The BMP280 driver, in this idiom:

```c
/* bmp280.h */
#ifndef BMP280_H
#define BMP280_H

#include "bus.h"

typedef struct {
    bus_t *bus;
    /* Calibration coefficients, read once at init. */
    int32_t  t_fine;
    uint16_t dig_T1;
    int16_t  dig_T2, dig_T3;
    uint16_t dig_P1;
    int16_t  dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;
} bmp280_t;

int  bmp280_init(bmp280_t *dev, bus_t *bus);
int  bmp280_read_raw(bmp280_t *dev, int32_t *raw_t, int32_t *raw_p);
int  bmp280_compensate(bmp280_t *dev, int32_t raw_t, int32_t raw_p,
                       int32_t *t_centi_c, uint32_t *p_pa);

#endif
```

```c
/* bmp280.c */
#include "bmp280.h"

#define BMP280_REG_ID       0xD0
#define BMP280_REG_CTRL_MEAS 0xF4
#define BMP280_REG_CONFIG    0xF5
#define BMP280_REG_PRESS_MSB 0xF7
#define BMP280_REG_CALIB00   0x88

#define BMP280_CHIP_ID       0x58

static int rd_reg(bmp280_t *dev, uint8_t reg, uint8_t *buf, size_t n) {
    return dev->bus->write_read(dev->bus, &reg, 1, buf, n);
}

static int wr_reg(bmp280_t *dev, uint8_t reg, uint8_t val) {
    uint8_t tx[2] = { reg, val };
    return dev->bus->write(dev->bus, tx, 2);
}

int bmp280_init(bmp280_t *dev, bus_t *bus) {
    dev->bus = bus;
    uint8_t id;
    if (rd_reg(dev, BMP280_REG_ID, &id, 1) < 0) return -1;
    if (id != BMP280_CHIP_ID)                    return -2;

    /* Read 24 bytes of calibration starting at 0x88. */
    uint8_t calib[24];
    if (rd_reg(dev, BMP280_REG_CALIB00, calib, 24) < 0) return -3;
    dev->dig_T1 = (uint16_t)((calib[1] << 8) | calib[0]);
    dev->dig_T2 = (int16_t)( (calib[3] << 8) | calib[2]);
    dev->dig_T3 = (int16_t)( (calib[5] << 8) | calib[4]);
    /* ... dig_P1..P9 ... */

    /* osrs_t = x1, osrs_p = x1, normal mode. */
    if (wr_reg(dev, BMP280_REG_CTRL_MEAS, 0x27) < 0) return -4;
    return 0;
}

int bmp280_read_raw(bmp280_t *dev, int32_t *raw_t, int32_t *raw_p) {
    uint8_t buf[6];
    if (rd_reg(dev, BMP280_REG_PRESS_MSB, buf, 6) < 0) return -1;
    *raw_p = ((int32_t)buf[0] << 12) | ((int32_t)buf[1] << 4) | (buf[2] >> 4);
    *raw_t = ((int32_t)buf[3] << 12) | ((int32_t)buf[4] << 4) | (buf[5] >> 4);
    return 0;
}
```

Notice what is *not* in `bmp280.c`:
- No `i2c_t *` references.
- No `spi_t *` references.
- No `GPIO_*` register pokes.
- No `0x40044000` literals (the I²C0 base) or `0x4003C000` (SPI0 base).
- No assumption about CSn pin, slave address, SCK frequency.

`bmp280.c` calls `dev->bus->write` and `dev->bus->write_read`. The transport does the rest. If you swap the BMP280 from I²C to SPI later (the chip supports both — datasheet §5, p. 30), you only change the `bus_t` instance you pass in. The protocol code is unchanged.

## An I²C transport implementation

```c
/* i2c_transport.h */
#ifndef I2C_TRANSPORT_H
#define I2C_TRANSPORT_H

#include "bus.h"

typedef struct {
    void *peri;          /* pointer to I²C peripheral registers */
    uint8_t address;     /* 7-bit slave address */
} i2c_transport_ctx_t;

void i2c_transport_make(bus_t *out, i2c_transport_ctx_t *ctx);

#endif
```

```c
/* i2c_transport.c */
#include "i2c_transport.h"
#include "rp2040_i2c.h"   /* the register definitions from Lecture 2 */

static int i2c_write(bus_t *self, const uint8_t *tx, size_t tx_len) {
    i2c_transport_ctx_t *ctx = (i2c_transport_ctx_t *)self->ctx;
    return i2c_write_blocking(ctx->peri, ctx->address, tx, tx_len, false);
}

static int i2c_write_read(bus_t *self,
                          const uint8_t *tx, size_t tx_len,
                          uint8_t *rx, size_t rx_len) {
    i2c_transport_ctx_t *ctx = (i2c_transport_ctx_t *)self->ctx;
    int n = i2c_write_blocking(ctx->peri, ctx->address, tx, tx_len, true);
    if (n < 0) return n;
    return i2c_read_blocking(ctx->peri, ctx->address, rx, rx_len, false);
}

void i2c_transport_make(bus_t *out, i2c_transport_ctx_t *ctx) {
    out->write = i2c_write;
    out->write_read = i2c_write_read;
    out->ctx = ctx;
}
```

Twenty lines. The transport adapts the I²C peripheral to the `bus_t` contract. When the protocol layer calls `dev->bus->write_read`, the I²C transport translates it to "write tx with RESTART, then read rx with STOP," which is the standard register-read pattern.

## An SPI transport implementation

```c
/* spi_transport.h */
#ifndef SPI_TRANSPORT_H
#define SPI_TRANSPORT_H

#include "bus.h"

typedef struct {
    void *peri;          /* SPI peripheral registers */
    uint8_t cs_pin;      /* GP number for CSn */
} spi_transport_ctx_t;

void spi_transport_make(bus_t *out, spi_transport_ctx_t *ctx);

#endif
```

```c
/* spi_transport.c */
#include "spi_transport.h"
#include "rp2040_spi.h"

static int spi_write(bus_t *self, const uint8_t *tx, size_t tx_len) {
    spi_transport_ctx_t *ctx = (spi_transport_ctx_t *)self->ctx;
    cs_low(ctx->cs_pin);
    for (size_t i = 0; i < tx_len; i++) {
        (void)spi_xfer(ctx->peri, tx[i]);
    }
    cs_high(ctx->cs_pin);
    return (int)tx_len;
}

static int spi_write_read(bus_t *self,
                          const uint8_t *tx, size_t tx_len,
                          uint8_t *rx, size_t rx_len) {
    spi_transport_ctx_t *ctx = (spi_transport_ctx_t *)self->ctx;
    cs_low(ctx->cs_pin);
    for (size_t i = 0; i < tx_len; i++) {
        (void)spi_xfer(ctx->peri, tx[i]);
    }
    for (size_t i = 0; i < rx_len; i++) {
        rx[i] = spi_xfer(ctx->peri, 0xFF);   /* clock dummy bytes */
    }
    cs_high(ctx->cs_pin);
    return (int)rx_len;
}

void spi_transport_make(bus_t *out, spi_transport_ctx_t *ctx) {
    out->write = spi_write;
    out->write_read = spi_write_read;
    out->ctx = ctx;
}
```

Twenty-five lines, structurally identical. The differences from the I²C transport are exactly those that SPI imposes: chip-select around the transaction, dummy clocks on reads.

## Wiring it up in `main`

```c
#include "rp2040_i2c.h"
#include "rp2040_spi.h"
#include "i2c_transport.h"
#include "spi_transport.h"
#include "bmp280.h"
#include "ssd1306.h"

int main(void) {
    i2c0_init_100khz();
    spi0_init_1mhz();

    /* BMP280 on I²C at address 0x76. */
    i2c_transport_ctx_t bmp_ctx = { .peri = I2C0, .address = 0x76 };
    bus_t bmp_bus;
    i2c_transport_make(&bmp_bus, &bmp_ctx);

    /* SSD1306 on SPI with CSn on GP17. */
    spi_transport_ctx_t oled_ctx = { .peri = SPI0, .cs_pin = 17 };
    bus_t oled_bus;
    spi_transport_make(&oled_bus, &oled_ctx);

    bmp280_t  bmp;
    ssd1306_t oled;
    if (bmp280_init(&bmp, &bmp_bus)   < 0) hang();
    if (ssd1306_init(&oled, &oled_bus) < 0) hang();

    for (;;) {
        int32_t raw_t, raw_p, t_centi_c;
        uint32_t p_pa;
        bmp280_read_raw(&bmp, &raw_t, &raw_p);
        bmp280_compensate(&bmp, raw_t, raw_p, &t_centi_c, &p_pa);
        ssd1306_printf(&oled, "T=%d.%02d C  P=%lu Pa",
                       t_centi_c / 100, t_centi_c % 100, p_pa);
        delay_ms(250);
    }
}
```

`bmp280_init` is given a `bus_t` and the chip works. `ssd1306_init` is given a different `bus_t` and that chip works. The two drivers never know which bus they are on, and `main` is the only place where the wiring is bound.

## Why this matters in production

1. **Bus swaps.** Halfway through a project the hardware team moves the BMP280 from I²C0 to I²C1 because the routing is cleaner. With the transport/protocol split, you change one line in `main` (`peri = I2C1`) and rebuild. Without the split, you grep through `bmp280.c` looking for `0x40044000` and pray you find every reference.
2. **Bring-up of a second sensor.** When you add the MPU-6050, you write a new `mpu6050.c` that calls `bus->write_read`, hand it a new `bus_t` with `address = 0x68`, and done. No new transport code.
3. **Host testing.** You write a `mock_bus.c` that records every call and returns canned data. Compile `bmp280.c` for the host and run it under Unity / cmocka / Unity-fixture. You can unit-test the compensation arithmetic without an MCU on the bench. This is *the* discipline that lets you write a driver without the hardware in front of you.
4. **Future you porting to Rust.** `bus_t` maps cleanly onto `embedded_hal::i2c::I2c`. Your C protocol layer translates almost line-for-line into Rust because the abstraction shape is the same.

## What goes in the protocol layer (and what does not)

In the protocol layer:
- Register addresses (`#define BMP280_REG_ID 0xD0`).
- Init sequences (the 26-byte SSD1306 command stream).
- Compensation formulas (BMP280's 32-bit pressure math).
- Device-specific timing (BMP280 forced-mode wait time, ~7 ms at oversampling x1).

Not in the protocol layer:
- `RESETS_RESET` writes (transport's job).
- `IO_BANK0` function-select writes (transport's job).
- `clk_peri` queries (transport's job).
- GPIO pin numbers (transport's job — via `ctx`).
- DMA channel allocation (transport's job).

If a protocol-layer file imports a peripheral register header, the abstraction has leaked. Refactor.

## Error reporting: stay `errno`-free

UNIX-style `errno` is a thread-local global. On a freestanding target with no threads, `errno` is a single global, which is fine but obscures the call graph. We prefer *return-value* error reporting: every bus function returns a signed int, positive = bytes transferred, zero = empty success, negative = error code (encoded with semantic meaning).

```c
typedef enum {
    BUS_OK              =  0,
    BUS_ERR_NOACK       = -1,   /* slave did not ACK address (I²C) */
    BUS_ERR_TX_NACK     = -2,   /* slave NACK'd a data byte mid-write */
    BUS_ERR_ARB_LOST    = -3,   /* lost arbitration to another master */
    BUS_ERR_TIMEOUT     = -4,   /* polling-loop guard hit */
    BUS_ERR_BUS_STUCK   = -5,   /* SDA stuck low at idle */
    BUS_ERR_BAD_PARAM   = -6,
} bus_status_t;
```

The protocol layer maps these to its own error space (`BMP280_ERR_WRONG_ID`, etc.) — *do not* leak transport-level error codes through the protocol API. The user of `bmp280_init` should not need to know that the underlying transport is I²C.

## Threading & re-entrancy

Three rules:

1. **The transport is not re-entrant by default.** Two callers cannot hit the same `bus_t` concurrently. If you have multiple FreeRTOS tasks sharing an I²C bus, wrap the transport in a mutex (Week 9).
2. **The protocol layer is re-entrant per instance.** `bmp280_read_raw(&bmp0)` and `bmp280_read_raw(&bmp1)` on different `bmp280_t` instances are safe to run concurrently — *if* their `bus_t` instances are different or the transport is mutex-protected.
3. **ISR safety.** Neither transport nor protocol should be called from an ISR. The polling loops block; the FIFOs may overflow on a long ISR. Use a queue from the ISR to a worker task; the worker calls the protocol layer (Week 9).

## Test scaffolding: the mock bus

```c
/* mock_bus.c — for host-side unit testing */
typedef struct {
    uint8_t expected_tx[16];
    size_t  expected_tx_len;
    uint8_t canned_rx[16];
    size_t  canned_rx_len;
    int     call_count;
} mock_bus_ctx_t;

static int mock_write_read(bus_t *self,
                           const uint8_t *tx, size_t tx_len,
                           uint8_t *rx, size_t rx_len) {
    mock_bus_ctx_t *ctx = (mock_bus_ctx_t *)self->ctx;
    assert(tx_len == ctx->expected_tx_len);
    assert(memcmp(tx, ctx->expected_tx, tx_len) == 0);
    memcpy(rx, ctx->canned_rx, rx_len);
    ctx->call_count++;
    return (int)rx_len;
}

void mock_bus_make(bus_t *out, mock_bus_ctx_t *ctx) {
    out->write_read = mock_write_read;
    out->write = NULL;   /* not exercised */
    out->ctx = ctx;
}

/* Test: bmp280_init reads the chip id and proceeds. */
void test_bmp280_init_recognizes_chip(void) {
    mock_bus_ctx_t mock = { .expected_tx = {0xD0}, .expected_tx_len = 1,
                            .canned_rx   = {0x58}, .canned_rx_len   = 1 };
    bus_t bus;
    mock_bus_make(&bus, &mock);
    bmp280_t dev;
    int r = bmp280_init(&dev, &bus);
    /* The init also reads 24 bytes of calibration, so this test stops short
     * at the first call; production tests script the full sequence. */
    assert(r == 0 || r == -3);   /* OK or calibration-read fault, depending on canned_rx */
    assert(mock.call_count >= 1);
}
```

You can compile and run this on the host (`gcc -o test_bmp280 test_bmp280.c bmp280.c mock_bus.c`) and never plug in a Pi Pico W. The protocol-layer arithmetic is correct or incorrect on the host the same way it is on the MCU.

## The "thin enough" rule

A transport-layer function should be ≤ 50 lines. A protocol-layer driver should be ≤ 500 lines for a typical sensor (a complex LCD like the ILI9341 may push 1500). If your transport is 200 lines, you have leaked protocol concerns (e.g. you are encoding BMP280-specific register reads in the transport). If your protocol layer is 80 lines, you might be missing the compensation arithmetic — or you might have the right code and an honestly simple device, which is fine.

## What you take away

- Two layers: transport on the bottom (knows the wire), protocol on top (knows the device).
- The contract is `bus_t`: three function pointers + `void *ctx`.
- The protocol layer never imports peripheral headers. The transport layer never knows about devices.
- Error reporting is by signed return value. No `errno`. No leaking transport errors through protocol APIs.
- Mock the transport to test the protocol on the host. This is the discipline that lets you ship a driver without bench access.
- A test fixture for `bmp280_init` is 20 lines of host C. Use it.

This is the pattern you will repeat for every peripheral the rest of the course. Bus drivers, radio drivers, file system drivers — all the same shape. Get the shape right this week and the next 20 weeks are easier.

## References

- "Sensor Driver Architecture — Best Practices," Phillip Johnston, Embedded Artistry (2019).
  <https://embeddedartistry.com/blog/2019/01/14/sensor-driver-architecture-best-practices/>
- `embedded-hal` traits (Rust):
  <https://docs.rs/embedded-hal/latest/embedded_hal/i2c/trait.I2c.html>
- "API Design — Embedded C," Beningo Embedded Group (free article).
- Zephyr Project, `drivers/sensor/bme280/bme280.c` — production example of the transport/protocol split.
  <https://github.com/zephyrproject-rtos/zephyr/tree/main/drivers/sensor/bosch/bme280>
