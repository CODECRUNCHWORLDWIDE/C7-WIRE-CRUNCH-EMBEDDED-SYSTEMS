# Lecture 1 — SPI: the 3-wire bus (and the fourth)

> *SPI is two shift registers connected by a wire and a clock. Everything else is bookkeeping.*

## Why we open with SPI

SPI is the easier bus to think about, harder to over-engineer. You drive a clock; you shift a bit out of master while the slave shifts a bit back; you toggle the chip-select around the transaction. There is no addressing in-band, no clock stretching, no ACK, no multi-master arbitration. A scope shot of a working SPI bus is *legible* in a way a working I²C bus is not — every byte fits in a screen, every edge has a single meaning.

We open with SPI so that when you reach I²C tomorrow you already have a mental model of a synchronous bus, and the *extra* complexity of I²C (open-drain, ACK, RESTART, clock stretching, arbitration) lands as additions to a known thing.

## The mental model: two shift registers and a clock

Picture two 8-bit shift registers, one in the master, one in the slave. Their serial-out pins are tied together at the wire level: master's serial-out goes to slave's serial-in (this wire is called `MOSI` — Master Out, Slave In), and slave's serial-out goes back to master's serial-in (`MISO`). A clock line (`SCK`) is driven by the master and clocks both shift registers in lockstep. On every clock edge, one bit shifts out of master into slave, and simultaneously one bit shifts out of slave into master.

After 8 clock edges, the master has transmitted one byte and *received one byte*. SPI is fundamentally full-duplex: there is no such thing as a one-way SPI transaction on the wire — the master always receives 8 bits for every 8 bits it sends. If the slave has nothing to say, those 8 bits are typically `0x00` or `0xFF`; the master discards them. If the master has nothing to send but wants to receive, the master sends a "don't care" byte (`0x00` or `0xFF`) to clock the slave's data out. This is why "SPI read" is really "SPI write-then-receive-while-writing-junk."

```
Master shift register:  [b7 b6 b5 b4 b3 b2 b1 b0] ---> MOSI
                                                       \
                                                        \
                                       MISO  <--- [s7 s6 s5 s4 s3 s2 s1 s0]  Slave shift register
                                                                              ^
                                                                              |  SCK ticks both at once
                                                                              |
                              SCK ----------> SCK
```

The fourth wire is `CSn` — chip-select, active-low. The master pulls `CSn` low to address a specific slave; the slave only listens to `SCK` when its `CSn` is low. Multiple slaves can share `SCK`/`MOSI`/`MISO` and be distinguished by separate `CSn` lines. With N slaves you need 3 + N wires.

## The four SPI modes

Two binary axes:

- **CPOL (clock polarity):** the idle level of `SCK`. CPOL=0 → idles low. CPOL=1 → idles high.
- **CPHA (clock phase):** which edge of `SCK` samples the data. CPHA=0 → leading edge samples (and trailing edge changes). CPHA=1 → leading edge changes (and trailing edge samples).

The four combinations:

| Mode | CPOL | CPHA | SCK idle | Sample edge | Setup edge | Common slaves                    |
|------|------|------|----------|-------------|------------|----------------------------------|
| 0    | 0    | 0    | Low      | Rising      | Falling    | SSD1306 OLED, W25Q flash, BMP280 |
| 1    | 0    | 1    | Low      | Falling     | Rising     | Rare — some legacy ADCs          |
| 2    | 1    | 0    | High     | Falling     | Rising     | Rare                             |
| 3    | 1    | 1    | High     | Rising      | Falling    | ADXL345, some Microchip parts    |

Mode 0 dominates. Mode 3 is the runner-up. Modes 1 and 2 exist because in 1985 someone at National Semiconductor wanted them; if a modern datasheet lists mode 1 or 2 it is usually a typo and you should double-check by capturing the working configuration on a scope.

On the RP2040, mode is set in `SSPCR0`:
- `SPO` (bit 6) = CPOL
- `SPH` (bit 7) = CPHA

For mode 0: `SSPCR0[7:6] = 00`. For mode 3: `SSPCR0[7:6] = 11`. (Datasheet §4.4.4, p. 528.)

The SSD1306 datasheet (rev 1.1, §8.1.5, p. 19) shows a timing diagram with SCK idling low, data sampled on the rising edge. That is mode 0.

## The RP2040 SPI controller — register walk

The RP2040 wraps an ARM PrimeCell SSP (PL022). The register set is documented in datasheet §4.4.4, pp. 526–540, and at deeper level in the PL022 TRM (ARM DDI 0194H). Two instances, `SPI0` at `0x4003_C000` and `SPI1` at `0x4004_0000`. The register *offsets* are identical across instances.

### `SSPCR0` — Control Register 0 (offset `0x000`, reset `0x0000_0000`)

| Bits  | Field     | Meaning                                                            |
|-------|-----------|--------------------------------------------------------------------|
| 3:0   | `DSS`     | Data Size Select. `0x07` = 8-bit frame. `0x0F` = 16-bit frame.     |
| 5:4   | `FRF`     | Frame Format. `00` = Motorola SPI. `01` = TI SSI. `10` = Microwire. |
| 6     | `SPO`     | Clock polarity (CPOL).                                             |
| 7     | `SPH`     | Clock phase (CPHA).                                                |
| 15:8  | `SCR`     | Serial Clock Rate divisor. Final SCK = `clk_peri / (CPSDVSR × (1 + SCR))`. |

For an 8-bit, mode-0, Motorola-SPI configuration: `SSPCR0 = 0x0007`. Then set `SCR` and `CPSDVSR` for the target frequency.

### `SSPCR1` — Control Register 1 (offset `0x004`, reset `0x0000_0000`)

| Bits | Field | Meaning                                                                 |
|------|-------|-------------------------------------------------------------------------|
| 0    | `LBM` | Loopback mode. 1 = MOSI wraps internally to MISO. Useful for self-test. |
| 1    | `SSE` | Synchronous Serial port Enable. **Must be 1 to operate.**               |
| 2    | `MS`  | Master/Slave. 0 = master. 1 = slave.                                    |
| 3    | `SOD` | Slave-Mode Output Disable. 0 = normal. 1 = MISO tri-stated.             |

For master mode, enabled: `SSPCR1 = 0x0002`.

### `SSPCPSR` — Clock Prescaler (offset `0x010`, reset `0x0000_0000`)

| Bits | Field      | Meaning                                                |
|------|------------|--------------------------------------------------------|
| 7:0  | `CPSDVSR`  | Clock prescale divider. Even number from 2 to 254.     |

### `SSPSR` — Status Register (offset `0x00C`, read-only)

| Bit | Field | Meaning                                                          |
|-----|-------|------------------------------------------------------------------|
| 0   | `TFE` | Transmit FIFO Empty. 1 = TX FIFO has no data.                    |
| 1   | `TNF` | Transmit FIFO Not Full. 1 = there is room to push another byte.  |
| 2   | `RNE` | Receive FIFO Not Empty. 1 = there is a byte to pop.              |
| 3   | `RFF` | Receive FIFO Full.                                               |
| 4   | `BSY` | Busy. 1 = transmitting or shifting.                              |

The polled SPI loop looks like:

```c
while (!(spi->SSPSR & (1u << 1))) {}   /* wait for TNF — TX FIFO has room */
spi->SSPDR = tx_byte;
while (!(spi->SSPSR & (1u << 2))) {}   /* wait for RNE — RX FIFO has data */
rx_byte = (uint8_t)spi->SSPDR;
```

That is the inner loop of every polled SPI driver you will ever write. It moves one byte each direction per iteration, and on the RP2040 at 1 MHz SCK it takes ~10 µs (8 SCK cycles + ~1 cycle setup/hold + the polling overhead, which is dwarfed by the bus time).

### Clock-rate arithmetic worked

The PL022 generates SCK as:

```
Fsck = Fperi / (CPSDVSR × (1 + SCR))
```

Where `Fperi` is `clk_peri`. On the boot defaults from the SDK, `clk_peri = 125 MHz` (sourced from the system PLL after the SDK runs `runtime_init_clocks`). On a no-SDK firmware that runs straight off XOSC, `clk_peri = 12 MHz` (the Pi Pico W uses a 12 MHz crystal). Choose your numbers accordingly.

**Target 1 MHz, `clk_peri = 125 MHz`:**
- `CPSDVSR × (1 + SCR) = 125`. Pick `CPSDVSR = 125`, `SCR = 0`. Done.
- Or `CPSDVSR = 250`, `SCR = -0.5`. Cannot — SCR is an integer ≥ 0.
- Or `CPSDVSR = 50`, `SCR = 1.5`. Cannot.
- Or `CPSDVSR = 2`, `SCR = 61.5`. Cannot.
- The clean fit is `CPSDVSR = 250, SCR = 0` if you want a guaranteed even prescaler ≤ 254 — but `CPSDVSR = 250` is also valid as an even number, and `SCR = 0` produces SCK at exactly 500 kHz, not 1 MHz. The arithmetic above gives 1 MHz only with `CPSDVSR = 125`, which is *odd*; the PL022 requires even, so the nearest even is `CPSDVSR = 124` (yields `125e6 / 124 ≈ 1.008 MHz`) or `CPSDVSR = 126` (yields `~992 kHz`). Either is within tolerance for an SSD1306.

**Target 5 MHz, `clk_peri = 125 MHz`:**
- `CPSDVSR × (1 + SCR) = 25`. Pick `CPSDVSR = 2, SCR = 11.5` — no, integer. `CPSDVSR = 2, SCR = 11` → `125 / 24 ≈ 5.21 MHz`. Close.

The takeaway: pick `CPSDVSR` first (small, even, ≥ 2), then derive `SCR` and round to integer. Always *verify on a scope* — the formula is right, but `clk_peri` may not be what you think it is until you have measured.

## The IO_BANK0 routing for SPI0

SPI0 is mapped on three different pin groups on the RP2040. The most common on the Pi Pico W:

| Function     | GP pin | Function-select value |
|--------------|--------|------------------------|
| SPI0_RX (MISO) | GP16   | 1                      |
| SPI0_CSn       | GP17   | 1                      |
| SPI0_SCK       | GP18   | 1                      |
| SPI0_TX (MOSI) | GP19   | 1                      |

Set the IO_BANK0 `GPIO_CTRL.FUNCSEL` field to 1 (SPI) for each of these pins. The CSn line at GP17 *can* be driven by the SPI peripheral hardware, but for most use cases you drive it as a software GPIO — the PL022 only asserts CSn for one frame at a time, which is wrong for chips like the SSD1306 that expect CSn low across many frames. Configure GP17 as a normal output (FUNCSEL=5 = SIO) and toggle it from software.

This is the first place a working SPI driver looks unlike a textbook SPI driver: the chip-select is not on the SPI peripheral.

## A minimal SPI init for SPI0 on the Pi Pico W

```c
#include "rp2040.h"  /* your register definitions from Week 2 */

#define SPI0_BASE         0x4003C000u
#define RESETS_BASE       0x4000C000u
#define IO_BANK0_BASE     0x40014000u
#define PADS_BANK0_BASE   0x4001C000u

typedef struct {
    volatile uint32_t SSPCR0;
    volatile uint32_t SSPCR1;
    volatile uint32_t SSPDR;
    volatile uint32_t SSPSR;
    volatile uint32_t SSPCPSR;
    /* ... omitted ... */
} ssp_t;

#define SPI0  ((ssp_t *)SPI0_BASE)

void spi0_init_1mhz(void) {
    /* 1. Bring SPI0 out of reset. RP2040 datasheet §2.7, RESETS_RESET bit 16. */
    *(volatile uint32_t *)(RESETS_BASE + 0x3000) &= ~(1u << 16);  /* CLR alias */
    while ((*(volatile uint32_t *)(RESETS_BASE + 0x8) & (1u << 16)) == 0) {}

    /* 2. Configure SCK/MOSI/MISO function-select to SPI (FUNCSEL=1). */
    /*    IO_BANK0 GPIO18_CTRL at 0x40014000 + 0x098, GPIO19_CTRL at 0x0A0, GPIO16_CTRL at 0x080. */
    *(volatile uint32_t *)(IO_BANK0_BASE + 0x098) = 1;  /* GP18 -> SPI0_SCK */
    *(volatile uint32_t *)(IO_BANK0_BASE + 0x0A0) = 1;  /* GP19 -> SPI0_TX  */
    *(volatile uint32_t *)(IO_BANK0_BASE + 0x080) = 1;  /* GP16 -> SPI0_RX  */

    /* 3. Configure GP17 as software-driven CSn (function = SIO, default high). */
    *(volatile uint32_t *)(IO_BANK0_BASE + 0x088) = 5;  /* GP17 -> SIO */
    /* OE = 1 (output enable) via SIO; set high. */
    /* ... see Week 2 GPIO code ... */

    /* 4. Configure SPI0: mode 0, 8-bit, ~1 MHz, master, enabled. */
    SPI0->SSPCPSR = 124;                /* even prescaler, ~1.008 MHz */
    SPI0->SSPCR0 = (0u << 8)            /* SCR = 0   */
                 | (0u << 7)            /* SPH = 0   */
                 | (0u << 6)            /* SPO = 0   */
                 | (0u << 4)            /* FRF = Motorola */
                 | (0x07);              /* DSS = 8-bit */
    SPI0->SSPCR1 = (1u << 1);           /* SSE = 1 (enable), MS = 0 (master) */
}
```

This is ~30 lines and works on the bench. The SDK's `spi_init()` is ~80 lines because it handles all four modes, both frame sizes, the DREQ wiring, the loopback test, and reset sequencing for a hot init. You will see all of that in Exercise 1; here we ship a minimal version.

## Polled byte exchange

```c
uint8_t spi0_xfer(uint8_t tx) {
    while (!(SPI0->SSPSR & (1u << 1))) {}   /* TNF */
    SPI0->SSPDR = tx;
    while (!(SPI0->SSPSR & (1u << 2))) {}   /* RNE */
    return (uint8_t)SPI0->SSPDR;
}
```

Four lines. Push one byte, pop one byte, return it. For an SSD1306 you discard the return value because the display is write-only on SPI (its `D0` line is unidirectional; the MISO line you tie to the controller is never sampled).

For a flash chip (W25Q080) or an ADC (MCP3008) you keep the return value — that is how reads work.

## Asserting CSn around a transaction

```c
static inline void spi0_cs_low(void)  { sio_gpio_clr(17); }
static inline void spi0_cs_high(void) { sio_gpio_set(17); }

void ssd1306_send_cmd(uint8_t cmd) {
    gpio_clr(SSD1306_DC_PIN);     /* command mode: D/Cbar = 0 */
    spi0_cs_low();
    spi0_xfer(cmd);
    spi0_cs_high();
}

void ssd1306_send_data(const uint8_t *buf, size_t len) {
    gpio_set(SSD1306_DC_PIN);     /* data mode: D/Cbar = 1 */
    spi0_cs_low();
    for (size_t i = 0; i < len; i++) {
        (void)spi0_xfer(buf[i]);
    }
    spi0_cs_high();
}
```

Two functions. The SSD1306's D/C̄ pin tells the controller whether the byte is a command (D/C̄ = 0) or a data byte for GDDRAM (D/C̄ = 1). The CSn pulse brackets each transaction.

When you scope this you will see CSn fall, then ~9 SCK pulses (8 data bits plus the controller's setup edge), then CSn rise. The SSD1306 latches on the rising edge of SCK (CPHA=0), so each bit on MOSI is valid by the rising edge.

## What CAN go wrong (read this before bringing up an SSD1306)

1. **Wrong mode.** If you configure mode 3 instead of mode 0 (`SSPCR0[7:6] = 11` instead of `00`), every byte the SSD1306 latches will be shifted by one bit position — the chip will see `0x55` instead of `0xAE` for "display off." Symptom: random pixels. Diagnostic: scope SCK + MOSI, check which SCK edge MOSI is stable across.
2. **CSn driven by hardware instead of software.** The PL022 deasserts CSn between every frame. The SSD1306 init has multi-byte commands (e.g. `0xD5, 0x80`). If CSn rises after byte 1, the chip resets its command state machine and byte 2 is treated as a new command. Symptom: init succeeds but the display is unconfigured. Diagnostic: scope CSn — it should stay low across multi-byte commands.
3. **D/C̄ flipped.** If your code sets D/C̄ = 1 for commands instead of 0, the controller writes the init bytes into GDDRAM instead of the command register. Symptom: blank panel, but the init bytes appear as a pattern of pixels if you advance the GDDRAM pointer. Diagnostic: read the SSD1306 datasheet §8.1.5 once more; the convention is "D/C̄ = 0 = command."
4. **Wrong SCK frequency.** The SSD1306 is rated to 10 MHz. Drive it at 20 MHz and bytes corrupt. Drive it at 100 kHz and the init still works (just slowly). Recommended: 1 MHz for first bring-up, then 4 MHz once it works.
5. **Wrong supply voltage.** The SSD1306 logic is 3.3 V. The Pi Pico W is 3.3 V. They match. But the OLED *driver* needs a 7–13 V boost — most breakout boards include a charge-pump that comes up when you send the `0x8D, 0x14` command. If you skip that command, the panel lights blue for a millisecond and goes dark. Symptom: panel briefly visible at power-on, then off. Diagnostic: scope the panel's `VBAT` pin; should be ~9 V after init.

This list is one of the most useful artifacts you produce this week. Copy it to your `FAULT-MODEL.md` for the mini-project.

## When polled isn't enough: FIFOs, IRQs, DMA

For an SSD1306 frame-buffer flush (1024 bytes), polled mode at 1 MHz takes ~10 ms — the CPU is pegged for the whole flush. For a 4 Hz refresh you spend 4% of CPU on display. That is usually fine.

If you need more, three levers in order of increasing complexity:

1. **Drain the TX FIFO without polling RX.** The SSP has an 8-entry TX FIFO. Push 8 bytes, then poll TFE (TX FIFO empty) before pushing the next 8. For SSD1306 you do not care about RX, so this works. ~3x speedup.
2. **Interrupt-driven.** Configure `SSPIMSC.TXIM` and `SSPIMSC.RXIM`. ISR pulls bytes from a software buffer into `SSPDR`. CPU returns to other work between interrupts. Latency cost: each IRQ entry is ~20 cycles on Cortex-M0+.
3. **DMA-paced.** Configure a DMA channel to write from a buffer into `SSPDR`, paced by `DREQ_SPI0_TX` (datasheet §2.5.3, p. 89). CPU does *zero* work after kicking off the DMA. ~50 µs to flush 1024 bytes. This is the stretch goal for Exercise 1.

We do all three in Week 11. For Week 4, polled is the standard.

## Edge case: PIO as a fallback SPI

The RP2040 has a PIO (Programmable IO) block that can implement arbitrary serial protocols at SCK rates up to half of `clk_peri`. If you need an SPI variant the PL022 cannot do — say, 9-bit frames (some LCD controllers use these) or LSB-first transmission — you write 4–8 PIO instructions and route them to GPIOs. We do not exercise PIO this week, but you should know it is the lever you pull when the hardware SPI cannot meet a spec.

The Pi Pico W's CYW43439 WiFi chip uses a non-standard 4-bit half-duplex SPI variant that is *not* PL022-compatible; the Pi Pico SDK drives it from PIO. Week 13 covers this.

## What you take away

- SPI is two shift registers and a clock. Master drives SCK and CSn. Every clock moves one bit each direction.
- The four modes are CPOL × CPHA. Most modern parts are mode 0. SSD1306 is mode 0.
- RP2040's SPI is an ARM PL022 PrimeCell. Three register words and a FIFO push/pop drive 80% of bring-up.
- Software-drive CSn for most chips. The PL022's hardware CSn deasserts between frames and breaks multi-byte commands.
- Scope every bring-up. The four most common faults (wrong mode, hardware CSn, wrong D/C̄, wrong frequency) are diagnosed in 30 seconds on a logic analyzer.

Tomorrow we add the second wire of complexity: I²C, where addressing happens on the data line and the slaves can hold the master hostage by stretching the clock.

## References

- RP2040 datasheet (Sep-2024), §4.4 SPI, pp. 503–540.
- ARM PrimeCell SSP (PL022) TRM, DDI 0194H, §3, pp. 3-1 to 3-30.
- SSD1306 datasheet (Solomon Systech rev 1.1), §8.1.5 (4-wire SPI), p. 19; §10 (Command Table), pp. 28–32.
- W25Q080 datasheet (Winbond rev L), §6 (Standard SPI Instructions), pp. 18–34 — context only; we do not bring up flash this week.
- ADXL345 datasheet (Analog Devices rev G), Table 12 (SPI Timing), p. 16 — mode 3, for the homework comparison.
