# Week 4 — Resources

Every reference here is **free** and **publicly accessible**. Page numbers cite the document revision noted; later revisions move tables but not concepts. If a register address or section number moves between revisions, re-check.

## Primary datasheets and manuals

- **RP2040 Datasheet** (Raspberry Pi Ltd, ~640 pages, Sep-2024 rev) — the silicon. This week:
  - §2.5 (DMA), pp. 87–98 — the 12 DMA channels, DREQ pacing for SPI and I²C, used in the SPI stretch goal.
  - §2.7 (Subsystem resets), pp. 105–115 — `RESETS_RESET`. You bring `spi0`, `spi1`, `i2c0`, `i2c1` out of reset by hand this week.
  - §2.19 (IO_BANK0), pp. 240–290 — the function-select mux. SPI0 routes onto GP16/17/18/19 (function 1), I²C0 routes onto GP4/5 or GP20/21 (function 3). The mapping is in Table 269, p. 244.
  - §4.3 (I²C controllers), pp. 432–502 — the load-bearing chapter for Lecture 2.
  - §4.3.1 (Overview), pp. 432–433 — what the Synopsys DW_apb_i2c block does, what it does not.
  - §4.3.7 (Operation Modes), pp. 444–448 — master/slave, 7-bit/10-bit, the four speeds.
  - §4.3.10 (DMA Controller Interface), pp. 449–452 — `DREQ_I2C0_TX`, `DREQ_I2C0_RX`. Used in the DMA stretch goal.
  - §4.3.16 (List of Registers), pp. 463–502 — every register, every field, every reset value. **Read this twice.**
  - §4.4 (SPI controllers), pp. 503–540 — the load-bearing chapter for Lecture 1.
  - §4.4.1 (Overview), pp. 503–505 — what the ARM PrimeCell SSP (PL022) block does, what it does not.
  - §4.4.2 (Frame Formats), pp. 505–512 — Motorola SPI, Texas Instruments synchronous serial, National Microwire. We use Motorola SPI exclusively.
  - §4.4.3 (Operation), pp. 512–525 — clock generation, FIFOs, interrupts, DMA pacing.
  - §4.4.4 (List of Registers), pp. 526–540 — `SSPCR0`, `SSPCR1`, `SSPCPSR`, `SSPDR`, `SSPSR`, etc.
  <https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf>
- **ARM PrimeCell Synchronous Serial Port (PL022) Technical Reference Manual** (ARM DDI 0194H, ~150 pages) — the IP block underneath the RP2040 SPI peripheral. Same register names, same bit layouts, same FIFOs. When the RP2040 datasheet is terse, this is the deeper reference.
  - §3 (Programmer's Model), pp. 3-1 to 3-30 — every register, with timing diagrams.
  <https://developer.arm.com/documentation/ddi0194/h/>
- **Synopsys DesignWare APB I²C (DW_apb_i2c) Databook** (Synopsys proprietary; not freely downloadable). The RP2040 datasheet §4.3 reproduces the relevant register tables. We do not need the Synopsys databook directly.
- **UM10204 — I²C-Bus Specification and User Manual** (NXP, rev 7.0, Oct-2021, ~65 pages) — the canonical I²C spec. This week:
  - §3.1 (Standard-mode, Fast-mode, and Fast-mode Plus I²C-Bus protocols), pp. 8–22 — START, STOP, address byte, ACK, RESTART, clock stretching, multi-master arbitration.
  - §3.1.4 (Data validity), p. 10 — when SDA may change relative to SCL.
  - §3.1.6 (Acknowledge (ACK) and Not Acknowledge (NACK)), p. 12.
  - §3.1.10 (Clock synchronization), p. 16 — how multi-master clock arbitration works.
  - §3.1.11 (Arbitration), p. 17 — what happens when two masters START at the same time.
  - §3.1.16 (Bus clear), p. 20 — the nine-clock recovery procedure for a stuck SDA.
  - §3.1.17 (Reserved addresses), p. 21 — `0b0000_xxxx` (general call, start byte, CBUS, Hs-mode master code) and `0b1111_0xxx` (10-bit addressing). Do not use these as device addresses.
  - §7 (Electrical specifications), pp. 36–48 — rise/fall times, pull-up sizing, bus capacitance budget.
  <https://www.nxp.com/docs/en/user-guide/UM10204.pdf>
- **BMP280 Digital Pressure Sensor Datasheet** (Bosch Sensortec, rev 1.20, ~50 pages) — the I²C/SPI pressure + temperature sensor for Exercise 2 and the mini-project. This week:
  - §3 (Functional description), pp. 16–22 — power modes (sleep, forced, normal), oversampling, IIR filter.
  - §3.3 (Compensation formulas), pp. 21–23 — the integer arithmetic you implement on the MCU to convert raw 20-bit pressure to a hPa value.
  - §4 (Global memory map), pp. 24–28 — every register, every bit. The `id` register at `0xD0` must return `0x58`.
  - §5 (Digital interfaces), pp. 30–36 — I²C address selection (`0x76` if SDO=GND, `0x77` if SDO=VDDIO), SPI mode (mode 0 or 3), three-wire SPI option.
  <https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bmp280-ds001.pdf>
- **MPU-6050 Product Specification (PS-MPU-6000A-00)** (InvenSense, rev 3.4, ~52 pages) — the I²C accelerometer + gyroscope for the mini-project. This week:
  - §6.6 (I²C Interface), pp. 24–27 — addressing (`0x68` if AD0=GND, `0x69` if AD0=VDD), START/STOP timing, register read/write framing.
  - §8 (Register Map), pp. 31–55 — the register list. `WHO_AM_I` at `0x75` returns `0x68`. `PWR_MGMT_1` at `0x6B` must be cleared to wake the chip.
  - §8.34 (`ACCEL_XOUT_H` at `0x3B` through `ACCEL_ZOUT_L` at `0x40`), p. 47 — the six bytes you read back for an accelerometer sample.
  <https://invensense.tdk.com/wp-content/uploads/2015/02/MPU-6000-Datasheet1.pdf>
- **MPU-6000/6050 Register Map and Descriptions (RM-MPU-6000A-00)** (InvenSense, rev 4.2, ~46 pages) — the deeper register reference. §4 has every register with reset value and bit description.
  <https://invensense.tdk.com/wp-content/uploads/2015/02/MPU-6000-Register-Map1.pdf>
- **SSD1306 Datasheet** (Solomon Systech, rev 1.1, ~63 pages) — the 128×64 monochrome OLED controller used in Exercise 1 and the mini-project. This week:
  - §8.1 (MCU Interface Selection), pp. 17–20 — parallel 8080/6800, I²C, 3-wire SPI, 4-wire SPI. We use 4-wire SPI (Exercise 1) and I²C (mini-project, second panel).
  - §8.1.5 (MCU Serial Interface (4-wire SPI)), p. 19 — the SPI mode (mode 0), the D/C̄ (data/command) pin, max clock 10 MHz.
  - §8.4 (Graphic Display Data RAM (GDDRAM)), pp. 24–26 — 128 × 64 / 8 = 1024 bytes, organized as 8 "pages" of 128 bytes each.
  - §10 (Command Table), pp. 28–32 — every command. The init sequence in Exercise 1 references this table by command byte.
  <https://cdn-shop.adafruit.com/datasheets/SSD1306.pdf>

## Pi Pico SDK reference code (read, do not blindly copy)

- **`pico-sdk/src/rp2_common/hardware_spi/spi.c`** — the SDK's polled SPI driver. ~250 lines. The init sequence is the canonical one. Read it before writing your own; cite the lines you borrow.
  <https://github.com/raspberrypi/pico-sdk/blob/master/src/rp2_common/hardware_spi/spi.c>
- **`pico-sdk/src/rp2_common/hardware_i2c/i2c.c`** — the SDK's polled I²C driver. ~300 lines. Pay attention to how it builds the `CMD` bits into `IC_DATA_CMD`; that is the load-bearing trick.
  <https://github.com/raspberrypi/pico-sdk/blob/master/src/rp2_common/hardware_i2c/i2c.c>
- **`pico-sdk/src/rp2_common/hardware_spi/include/hardware/spi.h`** — the public API. Useful for naming conventions, not for copying.
- **`pico-examples/spi/spi_master_slave/`** — a working SPI master + slave between two Picos. Saleae traces in the PR history are public and worth reading.
  <https://github.com/raspberrypi/pico-examples/tree/master/spi>
- **`pico-examples/i2c/bmp280_i2c/`** — a working BMP280 reader. ~150 lines. The compensation arithmetic is correct but not annotated; we annotate it in Exercise 2.
  <https://github.com/raspberrypi/pico-examples/tree/master/i2c/bmp280_i2c>
- **`pico-examples/i2c/mpu6050_i2c/`** — a working MPU-6050 reader. ~120 lines. Reads accelerometer, gyroscope, and on-die temperature.
  <https://github.com/raspberrypi/pico-examples/tree/master/i2c/mpu6050_i2c>
- **`pico-examples/i2c/oled_i2c/`** — an SSD1306 driven over I²C. The font and framebuffer code is reusable for the mini-project.
  <https://github.com/raspberrypi/pico-examples/tree/master/i2c/oled_i2c>

## Reference drivers from the open-source ecosystem

- **Bosch's official BMP280 driver** (`BMP280_driver`, C, ~1500 lines) — the reference implementation with the full compensation formulas, both 32-bit and 64-bit variants. Read `bmp280.c`'s `bmp280_compensate_temperature` and `bmp280_compensate_pressure` end-to-end; you will reimplement a subset in Exercise 2.
  <https://github.com/boschsensortec/BMP280_driver>
- **Adafruit's BMP280 Arduino library** — same chip, a different idiom. Useful for comparing how a higher-level driver layers atop a transport. Reads as a counterexample of the transport/protocol split (Adafruit's library *does* depend on the Arduino `Wire` library directly; a clean abstraction would not).
  <https://github.com/adafruit/Adafruit_BMP280_Library>
- **TinyUSB** (for context only) — TinyUSB's `tusb_hal.h` is a clean example of a transport abstraction in an active project; ~80 functions, no peripheral-specific types in the public header. Read for inspiration on the API surface.
  <https://github.com/hathach/tinyusb>
- **`embedded-hal`** (Rust, for context) — the Rust trait set that does for `no_std` Rust what `bus_t` does in our C code. Lecture 3 references the `embedded_hal::i2c::I2c` trait directly.
  <https://github.com/rust-embedded/embedded-hal>

## Free books and write-ups

- **"How to Read Schematics" — SparkFun tutorial** — covers SPI/I²C wiring conventions, pull-up notation, and where to find the chip-select net on a Eagle/KiCad schematic. ~3000 words, free.
  <https://learn.sparkfun.com/tutorials/how-to-read-a-schematic>
- **"I²C in a Nutshell" — Interrupt blog, Memfault** — the cleanest one-page explainer of I²C arbitration and clock stretching. ~2500 words.
  <https://interrupt.memfault.com/blog/i2c-in-a-nutshell>
- **"SPI vs I²C — When to Use Which" — Predictable Designs** — a one-page decision matrix. Useful in a job interview.
  <https://predictabledesigns.com/spi-vs-i2c/>
- **"The Right Way to Read Data from a Sensor" — Embedded Artistry** — a write-up of the transport/protocol split in production code. Phillip Johnston covers exactly the discipline of Lecture 3.
  <https://embeddedartistry.com/blog/2019/01/14/sensor-driver-architecture-best-practices/>
- **"BMP280 — Read raw temperature with the I²C protocol"** — a community write-up that walks through the BMP280 init sequence byte by byte with a logic-analyzer screenshot. Good complement to the datasheet.
  <https://lastminuteengineers.com/bmp280-arduino-tutorial/>
- **"Solomon Systech SSD1306 — A Deep Dive"** — a community write-up of the SSD1306 init sequence with commentary on each command. Reference for Exercise 1.

## Videos (free)

- **"SPI Tutorial — Serial Peripheral Interface"** (Ben Eater, YouTube) — Ben builds a 74HC595-style SPI shift register on a breadboard, scopes every line. ~30 minutes, painfully clear.
  <https://www.youtube.com/watch?v=MCi7dCBhVpQ>
- **"I²C — Inter-Integrated Circuit"** (EEVblog #1093, Dave Jones) — Dave reads the UM10204 spec on camera. 40 minutes, datasheet-grade.
  <https://www.youtube.com/watch?v=CAtj2k2g0os>
- **"SPI and I²C — When to Use Each"** (DigiKey TechForum) — 20-minute decision-framework video, ad-supported but useful.
- **"How I²C Works"** (Andreas Spiess) — a hardware-first take with a logic analyzer, BMP280 demo. ~15 minutes.

## Tools you will use this week

| Command / tool                                                                | What it does                                                                 |
|-------------------------------------------------------------------------------|------------------------------------------------------------------------------|
| `arm-none-eabi-gcc -c -mcpu=cortex-m0plus -mthumb -Os ssd1306.c bmp280.c`     | Compile the driver sources for the Pi Pico W target                          |
| `arm-none-eabi-objdump -d build/w04-mini.elf \| grep -A 20 ssd1306_write_cmd` | Disassemble a driver function; verify the inlined register writes            |
| `picotool load -f build/w04-mini.uf2`                                         | Flash over USB to a Pico in BOOTSEL mode                                     |
| `picotool info -a build/w04-mini.uf2`                                         | Sanity check: load address, boot2 CRC, binary info strings                   |
| Saleae Logic 2                                                                | Capture and decode SPI / I²C transactions; export `.sal` files for the repo  |
| Sigrok / PulseView                                                            | Open-source alternative to Saleae Logic 2; same protocol decoders            |
| `sigrok-cli -d fx2lafw -C "D0=SCL,D1=SDA" -P i2c:scl=D0:sda=D1 --time 1s`     | CLI logic capture + decode for headless CI                                   |
| `screen /dev/tty.usbmodemXXXX 115200`                                         | Open a UART console to read the Pi Pico W's debug output                     |
| `minicom -D /dev/tty.usbmodemXXXX -b 115200`                                  | Same, with a friendlier UI                                                   |

## Pull-up sizing cheat-sheet (UM10204 §7.2, p. 41)

| Bus speed         | Max rise time (Cb = 100 pF) | Recommended pull-up | Approx. bus capacitance budget |
|-------------------|-----------------------------|---------------------|--------------------------------|
| Standard 100 kHz  | 1000 ns                     | 4.7 kΩ – 10 kΩ      | 400 pF                         |
| Fast 400 kHz      | 300 ns                      | 2.2 kΩ – 4.7 kΩ     | 400 pF                         |
| Fast-mode plus 1 MHz | 120 ns                   | 1 kΩ – 2.2 kΩ       | 550 pF                         |
| High-speed 3.4 MHz | 80 ns                      | active termination required | 400 pF                  |

A small breadboard with 2–3 devices is typically 50–100 pF of bus capacitance; the recommended values above all work. A long ribbon cable or a 4-layer PCB with a stitched ground plane can push capacitance past 200 pF, at which point the pull-up must drop and you start caring about VOL.

## RP2040 SPI register map cheat-sheet (Sep-2024 rev, §4.4.4, p. 526)

| Offset  | Register     | Purpose                                                             |
|---------|--------------|---------------------------------------------------------------------|
| `0x000` | `SSPCR0`     | Frame format (FRF), data size (DSS), SCK polarity (SPO) + phase (SPH), serial clock rate (SCR) |
| `0x004` | `SSPCR1`     | Loopback (LBM), enable (SSE), master/slave (MS), slave-out disable (SOD) |
| `0x008` | `SSPDR`      | Read/write data FIFO. Reads pop RX FIFO, writes push TX FIFO        |
| `0x00C` | `SSPSR`      | Status: TFE (tx-fifo empty), TNF (tx-fifo not full), RNE (rx-fifo not empty), RFF (rx-fifo full), BSY (busy) |
| `0x010` | `SSPCPSR`    | Clock prescaler divider (CPSDVSR), even number ≥ 2                  |
| `0x014` | `SSPIMSC`    | Interrupt mask: RORIM, RTIM, RXIM, TXIM                             |
| `0x018` | `SSPRIS`     | Raw interrupt status (pre-mask)                                     |
| `0x01C` | `SSPMIS`     | Masked interrupt status (post-mask)                                 |
| `0x020` | `SSPICR`     | Interrupt clear (write 1 to clear)                                  |
| `0x024` | `SSPDMACR`   | DMA control: RXDMAE, TXDMAE                                         |

SPI0 base: `0x4003_C000`. SPI1 base: `0x4004_0000`.

SCK frequency formula: `Fsck = clk_peri / (CPSDVSR × (1 + SCR))`. With `clk_peri = 125 MHz`, `CPSDVSR = 125`, `SCR = 0` → 1 MHz. With `CPSDVSR = 2`, `SCR = 11` → 5.21 MHz (close to MPU-6050's 1 MHz limit for register access, but the SSD1306's 10 MHz limit comfortably).

## RP2040 I²C register map cheat-sheet (Sep-2024 rev, §4.3.16, p. 463)

| Offset  | Register             | Purpose                                                             |
|---------|----------------------|---------------------------------------------------------------------|
| `0x000` | `IC_CON`             | Master mode (MASTER_MODE), speed (SPEED: 1=Std, 2=Fast, 3=High), 7/10-bit, restart enable |
| `0x004` | `IC_TAR`             | Target address (the slave you are talking to)                       |
| `0x008` | `IC_SAR`             | Self address (when acting as a slave; unused this week)             |
| `0x010` | `IC_DATA_CMD`        | TX/RX FIFO. Bits 0–7 = data. Bit 8 = CMD (0=write, 1=read). Bit 9 = STOP. Bit 10 = RESTART. |
| `0x014` | `IC_SS_SCL_HCNT`     | High count for standard mode (100 kHz)                              |
| `0x018` | `IC_SS_SCL_LCNT`     | Low count for standard mode                                         |
| `0x01C` | `IC_FS_SCL_HCNT`     | High count for fast mode (400 kHz)                                  |
| `0x020` | `IC_FS_SCL_LCNT`     | Low count for fast mode                                             |
| `0x02C` | `IC_INTR_STAT`       | Interrupt status (post-mask)                                        |
| `0x030` | `IC_INTR_MASK`       | Interrupt mask                                                      |
| `0x034` | `IC_RAW_INTR_STAT`   | Raw interrupt status (pre-mask) — 14 sources                        |
| `0x06C` | `IC_ENABLE`          | 0=disable, 1=enable. Most config must be done while disabled.       |
| `0x070` | `IC_STATUS`          | Activity, FIFO levels                                               |
| `0x074` | `IC_TXFLR`           | TX FIFO level                                                       |
| `0x078` | `IC_RXFLR`           | RX FIFO level                                                       |

I²C0 base: `0x4004_4000`. I²C1 base: `0x4004_8000`.

`HCNT`/`LCNT` formula (datasheet §4.3.16 calls out the constraint): `Fscl = clk_peri / (HCNT + LCNT + spike_filter_cycles + sync_cycles)`. With `clk_peri = 125 MHz` and standard-mode 100 kHz target: `HCNT + LCNT ≈ 1250`. The SDK uses `HCNT = 650`, `LCNT = 730` after accounting for filter and sync (datasheet §4.3.5, p. 442 gives the exact equation; the SDK's choice satisfies both the 4.0 µs minimum low time and the 4.7 µs minimum high time of UM10204 Table 10).

## BMP280 register map cheat-sheet (Bosch BST-BMP280-DS001 rev 1.20, §4, p. 24)

| Reg     | Name              | R/W | Purpose                                                       |
|---------|-------------------|-----|---------------------------------------------------------------|
| `0x88`–`0x9F` | `calib00`–`calib25` | R | 26 bytes of factory-calibrated compensation coefficients   |
| `0xD0`  | `id`              | R   | Must return `0x58` (the chip-ID)                              |
| `0xE0`  | `reset`           | W   | Write `0xB6` to soft-reset                                    |
| `0xF3`  | `status`          | R   | `measuring` bit (3), `im_update` bit (0)                      |
| `0xF4`  | `ctrl_meas`       | R/W | `osrs_t[2:0]` (bits 7:5), `osrs_p[2:0]` (bits 4:2), `mode[1:0]` (bits 1:0) |
| `0xF5`  | `config`          | R/W | `t_sb[2:0]` (bits 7:5), `filter[2:0]` (bits 4:2), `spi3w_en` (bit 0) |
| `0xF7`–`0xF9` | `press_msb`/`press_lsb`/`press_xlsb` | R | 20-bit raw pressure |
| `0xFA`–`0xFC` | `temp_msb`/`temp_lsb`/`temp_xlsb`    | R | 20-bit raw temperature |

7-bit address: `0x76` (SDO=GND), `0x77` (SDO=VDDIO). On a Pi Pico breadboard with the BMP280 module's SDO tied to GND, the address is `0x76` and the 8-bit write byte is `0xEC`, the 8-bit read byte is `0xED`.

## MPU-6050 register map cheat-sheet (InvenSense RM-MPU-6000A-00 rev 4.2)

| Reg     | Name              | R/W | Purpose                                                       |
|---------|-------------------|-----|---------------------------------------------------------------|
| `0x1A`  | `CONFIG`          | R/W | DLPF setting (bits 2:0)                                        |
| `0x1B`  | `GYRO_CONFIG`     | R/W | `FS_SEL[1:0]` (bits 4:3): ±250 / ±500 / ±1000 / ±2000 °/s     |
| `0x1C`  | `ACCEL_CONFIG`    | R/W | `AFS_SEL[1:0]` (bits 4:3): ±2g / ±4g / ±8g / ±16g             |
| `0x3B`–`0x40` | `ACCEL_XOUT_H`–`ACCEL_ZOUT_L` | R | 6 bytes of accelerometer data (16-bit each axis)  |
| `0x41`–`0x42` | `TEMP_OUT_H`/`TEMP_OUT_L`     | R | On-die temperature (16-bit)                        |
| `0x43`–`0x48` | `GYRO_XOUT_H`–`GYRO_ZOUT_L`   | R | 6 bytes of gyroscope data (16-bit each axis)       |
| `0x6B`  | `PWR_MGMT_1`      | R/W | Bit 6 = `SLEEP`. **Power-on default is `0x40` (sleep mode on); you must clear this bit before any data is valid.** |
| `0x75`  | `WHO_AM_I`        | R   | Returns `0x68`                                                 |

7-bit address: `0x68` (AD0=GND), `0x69` (AD0=VDD). On most breakout modules AD0 is pulled to GND, so the address is `0x68`.

## SSD1306 init sequence (Solomon SSD1306 rev 1.1, §10, p. 28)

| Command byte(s)     | Meaning                                                  |
|---------------------|----------------------------------------------------------|
| `0xAE`              | Display off                                              |
| `0xD5, 0x80`        | Set display clock divide ratio / oscillator frequency    |
| `0xA8, 0x3F`        | Set multiplex ratio (64 rows for 128×64)                 |
| `0xD3, 0x00`        | Set display offset to 0                                  |
| `0x40`              | Set display start line to 0                              |
| `0x8D, 0x14`        | Enable charge-pump regulator                             |
| `0x20, 0x00`        | Memory addressing mode = horizontal                      |
| `0xA1`              | Segment remap (column 127 mapped to SEG0)                |
| `0xC8`              | COM output scan direction (remapped)                     |
| `0xDA, 0x12`        | Set COM pins hardware configuration                      |
| `0x81, 0xCF`        | Contrast = 0xCF                                          |
| `0xD9, 0xF1`        | Pre-charge period                                        |
| `0xDB, 0x40`        | VCOMH deselect level                                     |
| `0xA4`              | Display follows RAM contents                             |
| `0xA6`              | Normal display (not inverted)                            |
| `0xAF`              | Display on                                               |

Total: 26 bytes. With CSn toggled around each command and the D/C̄ pin held low (command mode), the whole init takes about 200 µs at 1 MHz SCK.

## What is NOT in this week

- DMA-paced bus drivers — stretch goal only. Full DMA + double-buffer is Week 11.
- Multi-master arbitration — covered in Lecture 2 but not exercised on the bench. We assume single-master throughout the exercises.
- Bus capacitance measurement with an LCR meter — touched in Lecture 2; the FAULT MODEL card asks for a budget calculation, not a measurement.
- The CYW43439 over SPI (the Pi Pico W's WiFi/BLE chip) — that uses a non-standard 4-bit half-duplex SPI which we cover in Week 13.
- 10-bit I²C addressing — UM10204 §3.1.13. We use 7-bit throughout this week; 10-bit returns in Week 16.

If you find yourself reading the Synopsys DW_apb_i2c databook to debug a hung bus on the BMP280, you have over-tooled the problem. The RP2040 datasheet §4.3.16 is sufficient.
