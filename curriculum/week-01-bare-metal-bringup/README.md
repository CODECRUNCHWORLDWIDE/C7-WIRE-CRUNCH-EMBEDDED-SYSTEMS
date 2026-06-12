# Week 1 — Bare-Metal MCU Bring-Up

> *What is a microcontroller, really? What sits between the C you write and the LED that turns on? And why are we using two languages to find out?*

Welcome to C7. Week 1 is the field tour of the part itself: the silicon, the datasheet, the toolchain, and the first 10 mA of LED current that proves the whole stack works. By Sunday you will have flashed a Raspberry Pi Pico W twice — once in C through the official SDK, once in MicroPython — and you will be able to argue, from numbers and not feelings, which one belongs in a shipped product.

This week is light on abstraction and heavy on bench time. That is deliberate. Every later week of C7 — linker scripts, RTOS, OTA, secure boot — assumes you can answer the question "is the board powered, programmed, and blinking?" without thinking about it. If that loop is not reflex by Sunday, the rest of the course gets harder, not easier.

---

## Learning objectives

By the end of this week, you will be able to:

- **Define** an MCU vs a CPU vs an SoC in three sentences, with concrete part numbers (RP2040, Cortex-M0+, Intel i7-1280P, BCM2711) attached to each category.
- **Open** the RP2040 datasheet, locate the GPIO function-select table on page 244 of the September 2024 revision, and explain what a "function-select" register actually does.
- **Install** the `arm-none-eabi-gcc` toolchain (≥ 13.2.Rel1), `openocd` (≥ 0.12.0), `probe-rs` (≥ 0.24), and `gdb-multiarch`, and prove each works against a connected target.
- **Build and flash** a Raspberry Pi Pico W blink program written in C against the official `pico-sdk` (≥ 1.5.1) using CMake, and verify the LED toggle on an oscilloscope or logic analyzer at the GP0 (or `WL_GPIO0`) pin.
- **Build and flash** the same blink in MicroPython (≥ 1.22) by dragging the `.uf2` and running over the REPL.
- **Compare** the two artifacts on five axes: binary size (bytes of flash), boot time (ms), peak RAM, time-to-first-blink (developer minutes), and modifiability under cohort review.
- **Capture** a UART hello-world over a USB-serial bridge at 115200 8N1 and decode it on a Saleae or `sigrok` logic capture.
- **Write** the Week 1 bring-up note: a one-page artifact stating exactly what works on your bench and what does not, in the C7 voice. Week 2 of the syllabus expects this file.

---

## Prerequisites

You have done C1 (Python fluency), or you can read a C function without panic. You have read the [Course README](../../README.md), [Syllabus](../../SYLLABUS.md), and [Brand guide](../../assets/branding/BRAND.md). You have the Pi Pico W on your desk; if not, see the [hardware section in resources.md](./resources.md).

If you cannot read a C `for` loop or a basic `struct`, pause C7 and take C1 first. You will save yourself two months of frustration.

## Topics covered

- Microcontroller vs microprocessor vs system-on-chip — the three taxonomy axes (memory map, peripheral integration, OS expectation).
- RP2040 architecture: dual Cortex-M0+ at 133 MHz, 264 KB SRAM, 2 MB external QSPI flash, 30 GPIO, two PIO state-machine blocks.
- Pi Pico W vs Pi Pico: the CYW43439 wireless module, why the on-board LED moved from GP25 to `WL_GPIO0`, and what that means for your blink code.
- Reading a datasheet: cover page → block diagram → memory map → pin function table → register reference. The four phases of reading hardware docs.
- The GCC ARM Embedded toolchain: `arm-none-eabi-gcc`, `-mcpu=cortex-m0plus`, `--specs=nosys.specs`, what each flag costs and buys.
- The SWD wire protocol vs the BOOTSEL+UF2 flow. When you need a debug probe, when you do not.
- CMake on `pico-sdk`: `pico_sdk_init()`, `target_link_libraries`, `pico_add_extra_outputs`, the `.uf2` artifact.
- MicroPython on RP2040: the REPL, `mip`, the `machine` module, why `time.sleep_ms()` is fine for blink and not fine for a 1 µs pulse.
- UART 0 on the Pico: GP0 (TX) / GP1 (RX), 115200 8N1, ground reference, the FTDI / CP2102 / CH340 bridge problem.
- The C-vs-MicroPython decision rule we will keep using for 24 weeks: **C for footprint and certifiability, MicroPython for rapid prototyping and bench tools, never confuse the two.**

---

## Weekly schedule

| Day       | Focus                                            | Lectures | Exercises | Challenges | Quiz/Read | Homework | Mini-Project | Bench/Self-Study | Daily Total |
|-----------|--------------------------------------------------|---------:|----------:|-----------:|----------:|---------:|-------------:|-----------------:|------------:|
| Monday    | What an MCU is; RP2040 tour; datasheet read-along|   2h     |   1h      |    0h      |   0.5h    |   1h     |     0h       |       0.5h       |     5h      |
| Tuesday   | Toolchain install; first C build from `pico-sdk` |   1h     |   2h      |    0.5h    |   0.5h    |   1h     |     0h       |       0.5h       |     5.5h    |
| Wednesday | First blink in C; verify on scope/Saleae         |   1h     |   2h      |    0.5h    |   0.5h    |   1h     |     1h       |       0.5h       |     6.5h    |
| Thursday  | MicroPython port; size and boot comparison       |   1h     |   1.5h    |    0.5h    |   0.5h    |   1h     |     2h       |       0.5h       |     7h      |
| Friday    | UART hello-world challenge; reading datasheets   |   1h     |   0h      |    1h      |   0.5h    |   1h     |     2h       |       0.5h       |     6h      |
| Saturday  | Mini-project deep work — bring-up note artifact  |   0h     |   0h      |    0h      |   0h      |   1h     |     3h       |       0.5h       |     4.5h    |
| Sunday    | Quiz; review; polish the bring-up note           |   0h     |   0h      |    0h      |   0.5h    |   0h     |     0h       |       0h         |     0.5h    |
| **Total** |                                                  | **6h**   | **6.5h**  |  **2.5h**  |  **3h**   |  **6h**  |   **8h**     |     **3h**       |   **35h**   |

Self-paced cohorts compress to ~12 h/week; pick the toolchain install (Tuesday) and the mini-project (Saturday) as load-bearing. Skip the comparison polish, never the bring-up artifact.

---

## How to navigate this week

| File                                                                                         | What's inside                                                          |
|----------------------------------------------------------------------------------------------|------------------------------------------------------------------------|
| [README.md](./README.md)                                                                     | This overview                                                          |
| [resources.md](./resources.md)                                                               | Datasheets, SDK docs, ARM references, hardware procurement             |
| [lecture-notes/01-what-is-a-microcontroller.md](./lecture-notes/01-what-is-a-microcontroller.md) | MCU vs CPU vs SoC; the RP2040 tour; why "embedded" is its own discipline |
| [lecture-notes/02-the-toolchain-tour.md](./lecture-notes/02-the-toolchain-tour.md)           | GCC ARM, OpenOCD, probe-rs, GDB — what each does and why                |
| [lecture-notes/03-reading-a-datasheet.md](./lecture-notes/03-reading-a-datasheet.md)         | The four-phase reading protocol; worked example on the RP2040 GPIO block |
| [exercises/README.md](./exercises/README.md)                                                 | Index of exercises                                                     |
| [exercises/exercise-01-toolchain-install.md](./exercises/exercise-01-toolchain-install.md)   | Install and prove GCC ARM, OpenOCD, probe-rs                            |
| [exercises/exercise-02-first-blink-c.md](./exercises/exercise-02-first-blink-c.md)           | Pi Pico W blink via the official `pico-sdk` and CMake                  |
| [exercises/exercise-03-first-blink-micropython.md](./exercises/exercise-03-first-blink-micropython.md) | Same blink in MicroPython; size and boot comparison           |
| [challenges/README.md](./challenges/README.md)                                               | Index of challenges                                                    |
| [challenges/challenge-01-uart-hello-world.md](./challenges/challenge-01-uart-hello-world.md) | UART TX at 115200 8N1, decode on a logic analyzer                      |
| [quiz.md](./quiz.md)                                                                         | 10 questions, datasheet-grade                                          |
| [homework.md](./homework.md)                                                                 | Six practice problems                                                  |
| [mini-project/README.md](./mini-project/README.md)                                           | The Week 1 bring-up note — blink + UART + button on one board          |

---

## The Week 1 deliverable, in one line

By Sunday 23:59 local time you produce a single artifact: a public GitHub repo containing a Pi Pico W firmware (in C, via `pico-sdk`) that blinks the on-board LED at 1 Hz, prints `"crunch-wire week-01 boot ok"` over UART 0 at 115200 8N1 once per second, and toggles the blink rate to 5 Hz when GP15 is pulled low through a push-button. A bring-up note (`BRING-UP-NOTE.md`) at the repo root states which steps worked, which did not, and includes one scope or Saleae screenshot per signal.

Week 2 of [`SYLLABUS.md`](../../SYLLABUS.md) references this artifact by name. Do not skip it.

---

## Stretch goals

- Read the RP2040 datasheet, §2 (System Description), end to end. It is 26 pages. Annotate one register you do not understand and bring the question to Friday studio.
- Wire a J-Link EDU Mini (or a Picoprobe — a second Pico flashed as a debug probe) to the SWD pins and step through your blink under `arm-none-eabi-gdb`. Watch the program counter cross the reset handler.
- Flash the Rust `embassy` blink example on the same board (see [`rp-rs/rp-hal`](https://github.com/rp-rs/rp-hal)). Compare the `.elf` size to the C build under `size`.
- Read the first 30 pages of the ARMv6-M Architecture Reference Manual (free with registration). You do not need to memorize it. You need to know it exists and what it covers.

---

## Up next

[Week 2 — Embedded C, Properly](../week-02/) — once your bring-up note is on GitHub and your reviewer has signed off on the scope shot.
