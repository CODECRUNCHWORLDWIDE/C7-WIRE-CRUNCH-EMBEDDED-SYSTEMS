# Exercise 1 — SPI an OLED

> *Drive an SSD1306 128×64 monochrome OLED over SPI0 on a Pi Pico W. By the end of this exercise the panel renders the string `"crunch-wire w04"`.*

## Goal

Bring up the RP2040's SPI0 peripheral from registers up, wire an SSD1306 4-wire SPI module, send the 26-byte init sequence, and write text to the GDDRAM frame buffer. No `hardware_spi` API from the SDK; everything is your own register pokes or your Week 3 GPIO/RESETS helpers.

By the end you can:
- Configure SPI0 for mode 0 at 1 MHz with 8-bit frames.
- Drive CSn from software (because the PL022's hardware CSn pulses per-frame, which breaks SSD1306 multi-byte commands — see Lecture 1).
- Push the 26-byte init sequence and watch the OLED light up.
- Render a fixed-width font glyph for each of 15 characters of `"crunch-wire w04"`.

## Setup

### Parts

- 1× Pi Pico W
- 1× SSD1306 OLED breakout board, 128×64, **4-wire SPI** variant (NOT the I²C variant). Common sources: Adafruit 938, Sparkfun LCD-13003, generic AliExpress modules.
- 1× breadboard
- 6× male-to-male jumper wires
- 1× Saleae Logic 8 or equivalent logic analyzer (4 channels minimum)

### Wiring

| Pi Pico W   | OLED module    | Signal       |
|-------------|----------------|--------------|
| GP18        | `SCK` or `CLK` | SCK          |
| GP19        | `MOSI` or `SDA`| MOSI         |
| GP17        | `CS` or `CSn`  | Chip select (software-driven) |
| GP20        | `DC` or `D/C`  | Data/command (software-driven) |
| GP21        | `RST` or `RES` | Reset (software-driven) |
| 3V3 (pin 36)| `VCC` / `VDD`  | 3.3 V supply |
| GND (pin 38)| `GND`          | Ground       |

Logic analyzer:
- Ch0: GP18 (SCK)
- Ch1: GP19 (MOSI)
- Ch2: GP17 (CSn)
- Ch3: GP20 (D/C)

You may skip the `MISO` pin entirely — the SSD1306 in 4-wire SPI mode is write-only.

### Prerequisites

- Week 3 mini-project on GitHub. You have a working `Makefile`, a working linker script, a working `startup.c` with the vector table. This exercise adds source files to that build.
- The Pi Pico SDK *not* on your include path. Everything in this exercise is your own code or the Bosch / Solomon datasheet talking.

## Reading

Before you start, read:

- **RP2040 datasheet** §4.4.4 (SPI register list), pp. 526–540. Focus on `SSPCR0`, `SSPCR1`, `SSPCPSR`, `SSPDR`, `SSPSR`.
- **RP2040 datasheet** §2.19 (IO_BANK0), p. 244, Table 269 — confirm GP18/19/16 map to SPI0 on FUNCSEL=1.
- **RP2040 datasheet** §2.7 (RESETS), p. 105 — bit 16 of `RESETS_RESET` is SPI0.
- **SSD1306 datasheet** §8.1.5 (4-wire SPI interface), p. 19.
- **SSD1306 datasheet** §10 (Command Table), pp. 28–32.
- **Lecture 1** of this week.

## Steps

### 1. Add SPI register definitions to your project

Create `rp2040_spi.h`:

```c
#ifndef RP2040_SPI_H
#define RP2040_SPI_H

#include <stdint.h>

#define SPI0_BASE  0x4003C000u
#define SPI1_BASE  0x40040000u

typedef struct {
    volatile uint32_t SSPCR0;
    volatile uint32_t SSPCR1;
    volatile uint32_t SSPDR;
    volatile uint32_t SSPSR;
    volatile uint32_t SSPCPSR;
    volatile uint32_t SSPIMSC;
    volatile uint32_t SSPRIS;
    volatile uint32_t SSPMIS;
    volatile uint32_t SSPICR;
    volatile uint32_t SSPDMACR;
} ssp_t;

#define SPI0  ((ssp_t *)SPI0_BASE)
#define SPI1  ((ssp_t *)SPI1_BASE)

#define SSPSR_TFE   (1u << 0)
#define SSPSR_TNF   (1u << 1)
#define SSPSR_RNE   (1u << 2)
#define SSPSR_RFF   (1u << 3)
#define SSPSR_BSY   (1u << 4)

#endif
```

**Checkpoint:** `arm-none-eabi-gcc -c rp2040_spi.h` does not error (a header should compile as a sanity check; or place `static_assert(sizeof(ssp_t) >= 0x28)` in a C file and verify).

### 2. Write `spi0_init`

```c
#include "rp2040_spi.h"
#include "rp2040_resets.h"   /* from Week 3 */
#include "rp2040_io_bank.h"  /* from Week 3 */

void spi0_init_1mhz(void) {
    /* RESETS_RESET bit 16 = SPI0. Bring out of reset; wait for done. */
    resets_clear(1u << 16);
    while ((resets_done() & (1u << 16)) == 0) { }

    /* IO_BANK0: GP18/19 to SPI0 (FUNCSEL=1). GP16 (MISO) optional. */
    io_bank_funcsel(18, 1);
    io_bank_funcsel(19, 1);
    /* GP17 (CSn) is software-driven: FUNCSEL = 5 (SIO). */
    io_bank_funcsel(17, 5);
    sio_oe_set(1u << 17);
    sio_out_set(1u << 17);   /* idle high */

    /* GP20 (D/C) and GP21 (RST): software-driven outputs. */
    io_bank_funcsel(20, 5);
    io_bank_funcsel(21, 5);
    sio_oe_set((1u << 20) | (1u << 21));
    sio_out_set((1u << 20) | (1u << 21));

    /* Configure SPI0: mode 0, 8-bit, master, ~1 MHz. */
    SPI0->SSPCPSR = 124;                       /* prescaler */
    SPI0->SSPCR0  = (0u << 8)                  /* SCR = 0 */
                  | (0u << 7)                  /* SPH = 0 */
                  | (0u << 6)                  /* SPO = 0 */
                  | (0u << 4)                  /* FRF = Motorola */
                  | (0x07);                    /* DSS = 8-bit */
    SPI0->SSPCR1  = (1u << 1);                 /* SSE = 1, master */
}
```

**Checkpoint:** Flash this and probe SCK with a scope. The line should idle at 0 V; you have not pushed any data yet, so no clock activity, but SCK should not be floating.

### 3. Write `spi0_xfer` (polled byte exchange)

```c
uint8_t spi0_xfer(uint8_t tx) {
    while (!(SPI0->SSPSR & SSPSR_TNF)) { }
    SPI0->SSPDR = tx;
    while (!(SPI0->SSPSR & SSPSR_RNE)) { }
    return (uint8_t)SPI0->SSPDR;
}
```

**Checkpoint:** In a test loop, push `0xAA` 100 times. Capture with the logic analyzer. You should see 100 bytes of `0xAA` clocked out at ~1 MHz on MOSI, with SCK pulsing 8 times per byte.

### 4. Hardware reset of the SSD1306

The SSD1306 requires a pulse on its `RST` pin at power-on (datasheet §8.5, p. 25). Drive RST low for ≥ 3 µs, then high.

```c
void ssd1306_hw_reset(void) {
    sio_out_clr(1u << 21);    /* RST low */
    delay_us(10);
    sio_out_set(1u << 21);    /* RST high */
    delay_us(10);
}
```

### 5. Send a command byte

The D/C̄ pin is held *low* during commands. CSn brackets the transaction.

```c
void ssd1306_send_cmd(uint8_t cmd) {
    sio_out_clr(1u << 20);    /* D/C = 0 (command) */
    sio_out_clr(1u << 17);    /* CSn low */
    (void)spi0_xfer(cmd);
    sio_out_set(1u << 17);    /* CSn high */
}
```

### 6. Send the 26-byte init sequence

```c
void ssd1306_init(void) {
    static const uint8_t init_seq[] = {
        0xAE,           /* display off */
        0xD5, 0x80,     /* clock divide */
        0xA8, 0x3F,     /* multiplex (64 rows) */
        0xD3, 0x00,     /* offset 0 */
        0x40,           /* start line 0 */
        0x8D, 0x14,     /* charge pump enable */
        0x20, 0x00,     /* horizontal addressing */
        0xA1,           /* segment remap */
        0xC8,           /* COM scan direction remap */
        0xDA, 0x12,     /* COM pins config */
        0x81, 0xCF,     /* contrast */
        0xD9, 0xF1,     /* pre-charge */
        0xDB, 0x40,     /* VCOMH */
        0xA4,           /* RAM contents */
        0xA6,           /* normal display */
        0xAF,           /* display on */
    };
    for (size_t i = 0; i < sizeof(init_seq); i++) {
        ssd1306_send_cmd(init_seq[i]);
    }
}
```

**Checkpoint:** Flash and reset. After ~200 µs the OLED should show "noise" — the GDDRAM is uninitialized so you see a random pixel pattern, but the panel is *on*. If the panel is dark, see "Common faults" below.

### 7. Clear the frame buffer

```c
void ssd1306_clear(void) {
    sio_out_set(1u << 20);    /* D/C = 1 (data) */
    sio_out_clr(1u << 17);    /* CSn low */
    for (size_t i = 0; i < 1024; i++) {
        (void)spi0_xfer(0x00);
    }
    sio_out_set(1u << 17);    /* CSn high */
}
```

**Checkpoint:** Panel goes black.

### 8. Write a string with a font

For a fixed-width font (5 pixels wide, 8 pixels tall = 1 byte per column), `"crunch-wire w04"` is 15 characters × 6 column-bytes (5 glyph + 1 spacing) = 90 bytes. Send them with D/C = 1.

A minimal 5×8 font table covering `[a-z0-9 -]` is ~40 entries × 5 bytes = 200 bytes. Borrow one from `pico-examples/i2c/oled_i2c/ssd1306_font.h` or write your own from a glyph design.

```c
void ssd1306_set_cursor(uint8_t col, uint8_t page) {
    ssd1306_send_cmd(0x21); ssd1306_send_cmd(col); ssd1306_send_cmd(127);
    ssd1306_send_cmd(0x22); ssd1306_send_cmd(page); ssd1306_send_cmd(7);
}

void ssd1306_putc(char c) {
    sio_out_set(1u << 20);    /* D/C = 1 */
    sio_out_clr(1u << 17);
    for (int i = 0; i < 5; i++) spi0_xfer(font5x8[c - ' '][i]);
    spi0_xfer(0x00);           /* 1-column spacing */
    sio_out_set(1u << 17);
}

void ssd1306_puts(const char *s) {
    while (*s) ssd1306_putc(*s++);
}
```

**Checkpoint:** Call `ssd1306_set_cursor(0, 0); ssd1306_puts("crunch-wire w04");`. The OLED renders the string in the top-left.

### 9. Capture a Saleae trace of the full transaction

In Logic 2, capture the boot-up sequence: hardware reset, init bytes, clear, string write. Use the SPI protocol analyzer with mode 0 to decode. Save as `traces/ex01-bring-up.sal`. This is your committed artifact.

## Artifact

Commit to your Week-4 repo (under `exercises/ex01/`):

1. `rp2040_spi.h` — SPI register definitions.
2. `spi.c` — `spi0_init_1mhz` and `spi0_xfer`.
3. `ssd1306.c` — `ssd1306_init`, `ssd1306_clear`, `ssd1306_putc`, `ssd1306_puts`.
4. `font5x8.h` — 5×8 font table (or a small subset covering `[a-z0-9 -]`).
5. `main.c` — wires it all together; renders `"crunch-wire w04"`.
6. `Makefile` — extends your Week 3 Makefile.
7. `traces/ex01-bring-up.sal` — Saleae capture of the full boot-up SPI transaction.
8. `README.md` — one paragraph describing what the exercise builds, what the trace shows, and any deviations from the spec.

## Common faults

| Symptom | Cause | First diagnostic |
|---|---|---|
| Panel dark, no pixels | Charge-pump not enabled (`0x8D, 0x14` skipped) | Scope `VBAT` pin on the OLED — should be ~9 V after init |
| Panel briefly lit, then dark | Charge pump enabled but display-on `0xAF` skipped, or VBAT cap not present | Scope `VBAT`; check init seq for `0xAF` |
| Wrong characters render (e.g. `c` looks like `b`) | Font table column order swapped, or D/C̄ flipped on string write | Check `ssd1306_putc` D/C handling |
| Random pixels everywhere | Init succeeded but `ssd1306_clear` was not called | Add the clear step |
| Display upside-down | `0xA1` (segment remap) or `0xC8` (COM remap) missing | Check init seq |
| Display works for ~100 ms then freezes | TX FIFO overflow on long writes — you pushed without checking TNF | Verify the `while (!TNF)` guard in `spi0_xfer` |
| Every byte is shifted by one bit | Wrong CPOL/CPHA (mode 3 instead of mode 0) | Scope SCK + MOSI; SCK should idle low, data stable on rising edge |
| CSn pulses per-byte instead of per-transaction | CSn driven by SPI peripheral instead of software | Confirm GP17 FUNCSEL = 5 (SIO), not 1 (SPI) |

## Stretch goal

Replace the polled `spi0_xfer` loop in `ssd1306_clear` with a DMA channel paced by `DREQ_SPI0_TX`. Measure the CPU time on a scope-toggled GPIO before and after the clear. Expected: polled = ~10 ms, DMA = ~50 µs of CPU + 10 ms of DMA running in the background. DMA setup is RP2040 datasheet §2.5, p. 87.

## Hand-in

When the OLED renders `"crunch-wire w04"` on the bench and the Saleae capture is committed, push to GitHub. Tag the commit `w04-ex01`. Your reviewer signs off on the trace before you proceed to Exercise 2.
