# Week 4 — SPI, I²C, and Bus Driver Patterns

> *Three or four wires between two chips. If you cannot draw the timing diagram from the datasheet without looking, you cannot debug the bus when it lies.*

Welcome to Week 4 of C7. Last week you produced a Pi Pico W blink with no SDK code in the tree — your own linker script, your own startup, your own vector table, your own `.data` copy loop. The board toggled GP15 at 1 Hz and printed `"crunch-wire w03 boot ok"` over UART0. That firmware spoke to *itself* — no sensor, no display, no peer. This week the board starts talking to *other silicon*. Two wires for I²C, four for SPI, sometimes a fifth for chip-select on a shared bus. Three or four signals between two chips, each chip with its own datasheet, its own register map, its own idea of what time `0xA0` should be valid on the line.

This is the week most embedded engineers learn that "the bus works" and "the device works" are two different sentences. Your I²C clock is at 100 kHz on the scope, the START condition is clean, the address byte is `0x76` shifted left to `0xEC`, the device ACKs — and the register you read back is `0x00` because the BMP280 has a power-on `ctrl_meas` of `0x00` (forced mode disabled, no sampling). You did not misread the datasheet; you read it correctly and stopped one page too early. By Sunday you will not stop early. You will write three drivers — one for an SSD1306 OLED over SPI, one for a BMP280 over I²C, one abstract transport layer that both drivers call into — and you will produce Saleae captures for every transaction in your repo.

---

## Learning objectives

By the end of this week, you will be able to:

- **Draw**, from memory, an SPI transaction: `CSn` falls, then `SCK` either idles low (CPOL=0) or high (CPOL=1), data on `MOSI`/`MISO` is sampled on the leading (CPHA=0) or trailing (CPHA=1) edge of `SCK`, and `CSn` rises after the last bit. Annotate the four modes (0,0), (0,1), (1,0), (1,1) and name which mode the SSD1306 OLED uses. Cite RP2040 datasheet §4.4 (SPI), p. 503–540 (Sep-2024 rev), and the SSD1306 datasheet rev 1.1 §8.1.5, p. 19.
- **Draw**, from memory, an I²C transaction: SDA pulled low while SCL is high (START), 7-bit address shifted out MSB-first, R/W̄ bit, ACK from the slave (SDA pulled low during the 9th clock), then payload bytes each ACK'd, then STOP (SDA released to high while SCL is high). Annotate clock stretching: a slave that holds SCL low to defer the next bit. Cite UM10204 (NXP I²C-Bus Specification rev 7.0, Oct-2021) §3.1.1–3.1.6, pp. 9–14.
- **Read** the RP2040 SPI controller register table from datasheet §4.4.4, pp. 526–540: `SSPCR0` (clock format, data size, frame format — Motorola SPI vs TI synchronous serial vs National Microwire), `SSPCR1` (enable, master/slave, loopback), `SSPCPSR` (clock prescaler), `SSPDR` (data FIFO), `SSPSR` (status: TFE, TNF, RNE, RFF, BSY), and the DMA pacing registers `SSPDMACR`. Compute the SCK frequency from `clk_peri`, `CPSDVSR`, and the `SCR` field for a target of 1 MHz. Cite p. 526.
- **Read** the RP2040 I²C controller register table from datasheet §4.3.16, pp. 463–502: `IC_CON` (master mode, 7/10-bit addressing, speed: standard / fast / fast-mode-plus / high-speed), `IC_TAR` (target address), `IC_DATA_CMD` (the FIFO with embedded START/STOP/CMD/RESTART bits), `IC_SS_SCL_HCNT`/`IC_SS_SCL_LCNT` (timing for 100 kHz), `IC_FS_SCL_HCNT`/`IC_FS_SCL_LCNT` (timing for 400 kHz), `IC_RAW_INTR_STAT` (the 14 interrupt sources), `IC_STATUS` (activity, FIFO levels). Compute `HCNT`/`LCNT` values for a 100 kHz bus at `clk_peri = 125 MHz`. Cite p. 463.
- **Author** an SPI driver in C that brings up the RP2040 SPI0 peripheral from registers up — release `RESETS_RESET.spi0`, configure `SSPCR0`/`SSPCR1`/`SSPCPSR` for SPI mode 0 at 1 MHz with 8-bit frames, set the IO_BANK0 function-select for GP18 (SCK), GP19 (MOSI), GP16 (MISO), and a soft-driven GP17 for `CSn` — and drive an SSD1306 128×64 OLED to display the string `"crunch-wire w04"`. Cite RP2040 datasheet §4.4 and §2.19 (IO_BANK0), and SSD1306 datasheet §10 (Command Table), pp. 28–32.
- **Author** an I²C driver in C that brings up the RP2040 I²C0 peripheral from registers up — release `RESETS_RESET.i2c0`, configure `IC_CON` for 7-bit master, set `IC_TAR` to `0x76` (BMP280 default), program `IC_SS_SCL_HCNT`/`LCNT` for 100 kHz, enable the peripheral, push a register-read sequence into `IC_DATA_CMD` with the `CMD` bit for read, drain `IC_DATA_CMD` on the receive side — and read the BMP280 `id` register at `0xD0` (must return `0x58`). Cite BMP280 datasheet rev 1.20 §4.3.3, p. 24, and §5 (Memory Map), p. 26.
- **Distinguish** the *transport layer* (bytes on a bus: `i2c_write_read(addr, tx_buf, tx_len, rx_buf, rx_len)`) from the *protocol layer* (semantic operations on a device: `bmp280_read_pressure_raw(&dev)`). Show that the protocol layer should never know which peripheral instance is in use, which pins are routed, or which DMA channel is paced — those are transport concerns. Defend the split in code review.
- **Recognize**, from a Saleae capture, three common bus faults: (1) the missing ACK on byte 2 of a write that reveals the device's internal write-protect bit; (2) the held-low SDA after an aborted transaction (the bus is *stuck*, requires nine-clock manual recovery per UM10204 §3.1.16, p. 20); (3) the inverted CPHA where every byte reads as the previous byte shifted by one bit position.
- **Multiplex** three I²C devices on a single bus with three different 7-bit addresses (BMP280 at `0x76`, MPU6050 at `0x68`, SSD1306 — used over I²C in the mini-project — at `0x3C`) and drive a fourth device (a different SSD1306 panel) over SPI from the same MCU. Document the bus topology, the pull-up values, and the worst-case bus capacitance in a `FAULT-MODEL.md` artifact.

---

## Prerequisites

You have shipped the Week 3 mini-project. Your Pi Pico W blinks GP15 at 1 Hz with zero `pico-sdk` source files in the tree, and your `MEMORY-MAP.md` is on GitHub. If not, finish Week 3 — this week assumes you can already configure `IO_BANK0`, `PADS_BANK0`, `RESETS`, and `UART0` from registers without help.

You have the RP2040 datasheet (Sep-2024 rev, 640 pages) open to §4.3 (I²C, pp. 432–502) and §4.4 (SPI, pp. 503–540). Page numbers in this week's notes assume that revision.

You have the BMP280 datasheet (rev 1.20, Bosch Sensortec, ~50 pages) downloaded. You will read §3 (Functional description, pp. 16–22), §4 (Global memory map, pp. 24–28), and §5 (Compensation formulas, pp. 45–48) this week.

You have the MPU-6050 datasheet (rev 4.2, InvenSense, ~52 pages) downloaded. §3.1 (register map, pp. 7–11) is the load-bearing section. The `WHO_AM_I` register at `0x75` returns `0x68`.

You have the SSD1306 datasheet (rev 1.1, Solomon Systech, ~63 pages) downloaded. §8.1.5 (SPI write-only mode, p. 19) and §10 (Command Table, pp. 28–32) are this week's reading.

You have UM10204 (NXP I²C-Bus Specification rev 7.0, Oct-2021, 65 pages) downloaded. §3 (I²C-bus characteristics, pp. 8–22) is required reading; §3.1.16 (bus clear, p. 20) is the one most engineers skip and then regret.

You have a Saleae Logic 8 or Logic Pro 8 (or equivalent, e.g. Kingst LA1010, DSLogic Plus) and Logic 2 software installed. Channel 0 = SCK / SCL, channel 1 = MOSI / SDA, channel 2 = MISO, channel 3 = CSn. The free DSView and Sigrok PulseView decoders work too; the cadence of "capture, decode, annotate, commit" is the same.

---

## Topics covered

- **SPI as a four-wire synchronous protocol.** Master drives `SCK`, master and slave shift in lockstep on `MOSI` (Master Out Slave In) and `MISO` (Master In Slave Out). `CSn` (chip-select, active-low) gates which slave is being talked to. Full-duplex: every clock edge moves one bit *each direction*. There is no addressing on the wire — addressing is *which `CSn` line you pull low*. Maximum clock: device-limited; SSD1306 is rated to 10 MHz (datasheet p. 19), W25Q080 flash to 133 MHz, ADXL345 accelerometer to 5 MHz.
- **The four SPI modes and why they exist.** Two binary axes: clock polarity (CPOL: idle level of `SCK`) and clock phase (CPHA: which edge samples data). Mode 0 (CPOL=0, CPHA=0) is the most common — SCK idles low, data is sampled on the rising edge. Mode 3 is the second most common (CPOL=1, CPHA=1) — SCK idles high, data sampled on the rising edge (but the *trailing* one). Modes 1 and 2 exist for legacy National Semiconductor parts. On the RP2040, the mode is set via `SSPCR0.SPO` (CPOL) and `SSPCR0.SPH` (CPHA) at bits 6 and 7. Datasheet §4.4.4, p. 528.
- **The RP2040 SPI controllers.** Two instances — `SPI0` at `0x4003_C000`, `SPI1` at `0x4004_0000`. Each is a PrimeCell SSP (PL022) IP block from ARM, supporting Motorola SPI, Texas Instruments synchronous serial, and National Semiconductor Microwire frame formats. We use Motorola SPI (FRF=`00`) exclusively. The TX FIFO is 8 entries deep, the RX FIFO is 8 entries deep; you can drive them by polling `SSPSR.TNF`/`RNE`, by interrupt (`SSPIMSC`/`SSPMIS`/`SSPICR`), or by DMA (`SSPDMACR.TXDMAE`/`RXDMAE`). Datasheet §4.4.1, p. 503.
- **I²C as a two-wire half-duplex protocol with addressing on the wire.** SDA (data) and SCL (clock) are open-drain — every device on the bus can pull either line low, neither can drive high; pull-ups (typically 2.2 kΩ–10 kΩ) bring the lines high when no one is talking. The 7-bit address is shifted out by the master at the start of every transaction, followed by an R/W̄ bit. The addressed slave ACKs (pulls SDA low during the 9th clock); every other slave stays quiet. Throughput tops out at 100 kbit/s (standard mode), 400 kbit/s (fast mode), 1 Mbit/s (fast-mode plus), 3.4 Mbit/s (high-speed mode). UM10204 §3.1.1, p. 9.
- **The RP2040 I²C controllers.** Two instances — `I2C0` at `0x4004_4000`, `I2C1` at `0x4004_8000`. Each is a Synopsys DesignWare DW_apb_i2c IP block. The register names are *unusual* by ARM standards because they are Synopsys's, not ARM's: `IC_CON`, `IC_TAR`, `IC_DATA_CMD`, etc. The TX/RX FIFOs are 16 entries deep. Per-byte START/STOP/RESTART/CMD control is encoded in the upper bits of `IC_DATA_CMD` (bits 8–10), which means you can build an entire repeated-START sequence by pushing bytes into one FIFO with the right top-bits. Datasheet §4.3.16, p. 463.
- **Clock stretching.** A slow slave can hold SCL low after an ACK to buy time for its internal processing (e.g. an EEPROM doing a page write). The master *must* wait. The RP2040 I²C controller does this automatically — you do not see it in software. But you *do* see it on a scope as a wider-than-expected low pulse on SCL. The BMP280 stretches the clock on a `forced-mode` measurement; budget for it.
- **The "transport vs protocol" split.** A `bmp280` driver should not contain `spi_inst_t *` or `i2c_inst_t *` references — it should call into a generic `bus_t` (a struct of function pointers: `write`, `read`, `write_read`). The transport layer is what binds `bus_t` to a peripheral. This split is *the* discipline that distinguishes a one-board hack from code that survives a chip swap. We will write the abstraction in Lecture 3 and use it in Exercise 3 and the mini-project.
- **Pull-up sizing on I²C.** A pull-up too weak and the rising edge takes forever (the bus capacitance dominates; UM10204 §7.2 gives the rise-time budget: 1 µs for standard mode, 300 ns for fast mode). A pull-up too strong and the open-drain transistors in the slave cannot pull SDA low against the resistor (UM10204 §7.1 limits sink current to 3 mA). The sweet spot on a small board with 2–3 devices is 4.7 kΩ. The Pi Pico W has no on-board I²C pull-ups; you supply them on the breadboard.
- **The FAULT MODEL discipline.** Every bus you bring up gets a one-page `FAULT-MODEL.md`: every known failure mode for this bus, with the symptom you would see on a scope, and the first three diagnostic steps. This is not optional — the week's mini-project requires it. You will reuse the format for every peripheral in Phase II.
- **The week's decision rule:** **transport on the bottom, protocol on top, datasheet pages cited at every register write.** If you cannot point to the page that says "register X bit Y means Z," you have not earned the right to write that register.

---

## Weekly schedule

| Day       | Focus                                                | Lectures | Exercises | Challenges | Quiz/Read | Homework | Mini-Project | Bench/Self-Study | Daily Total |
|-----------|------------------------------------------------------|---------:|----------:|-----------:|----------:|---------:|-------------:|-----------------:|------------:|
| Monday    | SPI — the 3-wire bus (+ CSn); the four modes         |   2h     |   1h      |    0h      |   0.5h    |   1h     |     0h       |       0.5h       |     5h      |
| Tuesday   | I²C — addressing, ACK, clock stretching, pull-ups    |   2h     |   2h      |    0h      |   0.5h    |   1h     |     0h       |       0.5h       |     6h      |
| Wednesday | Driver design patterns: transport vs protocol        |   2h     |   2h      |    0.5h    |   0.5h    |   1h     |     1h       |       0.5h       |     7.5h    |
| Thursday  | Multi-device on a shared bus; pull-up sizing         |   1h     |   2h      |    0.5h    |   0.5h    |   1h     |     1h       |       0.5h       |     6.5h    |
| Friday    | Saleae studio; bus fault triage; fault-model card    |   0h     |   0h      |    2h      |   0.5h    |   1h     |     1h       |       0.5h       |     5h      |
| Saturday  | Mini-project deep work; the FAULT-MODEL artifact     |   0h     |   0h      |    0h      |   0h      |   1h     |     3h       |       0.5h       |     4.5h    |
| Sunday    | Quiz; review; polish the artifact                    |   0h     |   0h      |    0h      |   0.5h    |   0h     |     0h       |       0h         |     0.5h    |
| **Total** |                                                      | **7h**   | **7h**    |  **3h**    |  **3h**   |  **6h**  |   **6h**     |     **3h**       |   **35h**   |

Self-paced cohorts compress to ~12 h/week. The load-bearing items are Lecture 2 (I²C), Lecture 3 (driver patterns), Exercise 3 (bus driver abstraction), and the mini-project. Skip Challenge 1 (shared-bus multi-device sequencing) only if you are tight on time; it returns as an interview question in Week 24.

---

## How to navigate this week

| File                                                                                                       | What's inside                                                                  |
|------------------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------|
| [README.md](./README.md)                                                                                   | This overview                                                                  |
| [resources.md](./resources.md)                                                                             | RP2040 §4.3 / §4.4, BMP280 / MPU6050 / SSD1306 datasheets, UM10204, Saleae docs |
| [lecture-notes/01-spi-the-3-wire-bus.md](./lecture-notes/01-spi-the-3-wire-bus.md)                         | SPI as a synchronous shift register; CPOL/CPHA; PL022 register walk; PIO as a fallback SPI |
| [lecture-notes/02-i2c-multi-master-and-addressing.md](./lecture-notes/02-i2c-multi-master-and-addressing.md) | I²C electrical, addressing, ACK, RESTART, clock stretching, multi-master arbitration |
| [lecture-notes/03-driver-design-patterns.md](./lecture-notes/03-driver-design-patterns.md)                 | The transport/protocol split; the `bus_t` abstraction; `errno`-free returns; testability |
| [exercises/README.md](./exercises/README.md)                                                               | Index of exercises                                                             |
| [exercises/exercise-01-spi-an-oled.md](./exercises/exercise-01-spi-an-oled.md)                             | Bring up an SSD1306 128×64 OLED over SPI0 on a Pi Pico W; render `"crunch-wire w04"` |
| [exercises/exercise-02-i2c-a-bmp280.md](./exercises/exercise-02-i2c-a-bmp280.md)                           | Read the BMP280 `id` register at `0xD0`, then a raw pressure sample, then a compensated reading in hPa |
| [exercises/exercise-03-bus-driver-abstraction.md](./exercises/exercise-03-bus-driver-abstraction.md)       | Refactor Exercises 1 and 2 to use a shared `bus_t` interface; prove drivers don't know the transport |
| [challenges/README.md](./challenges/README.md)                                                             | Index of challenges                                                            |
| [challenges/challenge-01-shared-bus-multiple-devices.md](./challenges/challenge-01-shared-bus-multiple-devices.md) | Three I²C devices on one bus; bus-stuck recovery; multi-master arbitration |
| [quiz.md](./quiz.md)                                                                                       | 10 questions; datasheet + bus-timing grade                                     |
| [homework.md](./homework.md)                                                                               | Six practice problems                                                          |
| [mini-project/README.md](./mini-project/README.md)                                                         | Week 4 deliverable — Pi Pico W reading 3 I²C sensors + driving an SPI OLED, with the FAULT MODEL card |

---

## The Week 4 deliverable, in one line

By Sunday 23:59 local time you produce a single artifact: a public GitHub repo containing a Pi Pico W firmware that (a) reads a BMP280 (temperature + pressure), an MPU-6050 (accelerometer + gyroscope), and a second SSD1306 (used as an I²C peripheral at `0x3C`) on `I2C0`, (b) drives an SSD1306 128×64 OLED over `SPI0`, displaying the three sensor readings refreshed at 4 Hz, (c) is built on top of a hand-written `bus_t` abstraction so that none of the three device drivers know whether they are talking over SPI or I²C, and (d) ships with a one-page `FAULT-MODEL.md` enumerating every failure mode for each bus and the first three diagnostic steps. The repo includes at least three Saleae captures committed as `.sal` files: one I²C transaction with the BMP280, one I²C transaction with the MPU-6050, and one SPI write to the SSD1306. The `bus_t` abstraction must compile and link with the same protocol-layer driver against either transport — proven by a unit test on the host that swaps the transport to a mock.

Week 5 of [`SYLLABUS.md`](../../SYLLABUS.md) (Rust on Embedded) builds on this artifact: you will port the BMP280 driver to `embedded-hal` traits in Rust, and the trait abstraction will look very familiar because you wrote `bus_t` in C this week. The mental model transfers cleanly.

---

## Stretch goals

- Replace the polled SPI driver from Exercise 1 with a DMA-paced one. Configure DMA channel 0 to pull from a buffer and write into `SSPDR`, paced by the `DREQ_SPI0_TX` signal. Datasheet §2.5 (DMA), pp. 87–98, plus §4.4.3.5 (DMA Interface), p. 525. Measure CPU load on a GPIO toggle: the polled driver pegs the CPU during a frame buffer flush; the DMA driver returns to idle in under 50 µs.
- Take Exercise 2 and add a CRC over every I²C transaction. The BMP280 has no CRC field — you compute one on the host side over the (address, register, value) tuple and log it. When a value is suspicious, you cross-check the CRC against a logged historical value. This is the bench discipline that catches a flaky pull-up before it ships.
- Read the Saleae `.sal` file format spec and write a Python script that asserts every I²C transaction in a capture matches an expected (address, register, length) tuple. Commit the script as a CI check that runs against every capture in the repo. The point: bus correctness is testable, not just observable.
- Bring up the SSD1306 over I²C *instead of* SPI and time-multiplex with the BMP280 and MPU-6050 on the same bus. The challenge: the SSD1306 framebuffer write is 1024 bytes — at 400 kHz fast mode that is ~25 ms per refresh, during which the BMP280 and MPU-6050 cannot be polled. Calculate the budget and decide whether you accept a lower refresh rate or split the panels onto separate buses.
- Pick a third I²C device you have on the bench (an SHT31, an SCD41, an INA219, an AHT20) and write a fourth protocol-layer driver against the same `bus_t` transport without modifying the transport layer. If you have to touch the transport, the transport leaked an abstraction; refactor before continuing.

---

## Up next

[Week 5 — Rust on Embedded](../week-05/) — once your three-I²C-plus-one-SPI Pi Pico W is on GitHub, your reviewer has signed off on the `FAULT-MODEL.md`, and you can answer "what does a `RESTART` do that a `STOP+START` does not?" in one sentence.
