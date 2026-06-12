# Resources — Week 12

Every link below is free unless explicitly marked otherwise. The five load-bearing references are the RP2040 datasheet's debug and processor sections, the ARMv6-M Architecture Reference Manual, the OpenOCD + Raspberry Pi "Getting started" docs, the SEGGER RTT documentation, and the sigrok protocol-decoder docs. If you read only these five, you will pass the quiz and the mini-project will work.

---

## Primary references — read this week

### 1. RP2040 datasheet — debug, processor, and I²C sections

- **Source:** Raspberry Pi Ltd., "RP2040 Datasheet" rev. 2.1, March 2022.
- **URL:** <https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf>
- **Sections we use:**
  - §2.3.4, pp. 14–15 — "SWD". The two-wire debug port, the multidrop selection of core 0 vs core 1, the pad assignment.
  - §2.4, pp. 16–18 — "Cortex-M0+ Processor". The crucial fact: no MemManage/BusFault/UsageFault, all faults escalate to HardFault. The DWT and the 4 hardware breakpoint / 2 watchpoint comparators are listed here.
  - §2.8, pp. 130–141 — "Bootrom". Cross-reference for the watchdog scratch registers and `reset_to_usb_boot`, which the crash-store uses.
  - §4.3, pp. 446–490 — "I²C". The DesignWare I²C IP, used in the hung-bus challenge.
  - §4.7, pp. 559–570 — "Watchdog". The scratch registers (`WATCHDOG_SCRATCH0..7`) that persist a minimal crash record across reset.
- **Cite as:** "RP2040 datasheet §X.Y, p. Z".

### 2. ARMv6-M Architecture Reference Manual (DDI0419)

- **Source:** ARM Ltd., "ARMv6-M Architecture Reference Manual", DDI0419.
- **URL:** <https://developer.arm.com/documentation/ddi0419/latest/>
- **License:** Free to read with an ARM developer account; the PDF is downloadable.
- **Sections we use:**
  - §B1.5 — "Exceptions". The exception model, exception numbers (§B1.5.2), the stacked frame layout (§B1.5.6), and the `EXC_RETURN` encoding (§B1.5.8). This is the spine of Lecture 2.
  - §B3.2 — "System Control Space". The SCB registers (`ICSR`, `VTOR`, `AIRCR`, `SHCSR`) at `0xE000ED00`.
  - §A6.7 — the `BKPT`, `WFI`, `WFE`, `DSB`, `ISB` instruction definitions, for the fault handler and the barriers around register relocation.
- **Cite as:** "ARMv6-M ARM §B1.5.X".

### 3. OpenOCD + Raspberry Pi "Getting started with Raspberry Pi Pico"

- **Source:** Open On-Chip Debugger project; Raspberry Pi Ltd. documentation.
- **URLs:**
  - OpenOCD user guide: <https://openocd.org/doc/html/index.html>
  - Raspberry Pi RP2040 OpenOCD fork: <https://github.com/raspberrypi/openocd>
  - "Getting started with Raspberry Pi Pico" (the C/C++ SDK book), Appendix A "Using Picoprobe" and Appendix B "Using OpenOCD": <https://datasheets.raspberrypi.com/pico/getting-started-with-pico.pdf>
  - Raspberry Pi Debug Probe docs: <https://www.raspberrypi.com/documentation/microcontrollers/debug-probe.html>
- **License:** OpenOCD is GPL-2.0; the Raspberry Pi docs are CC-BY-SA.
- **Sections we use:** Appendix A (wiring a second Pico as the probe, flashing `debugprobe.uf2`), Appendix B (the OpenOCD command line, `target/rp2040.cfg`, `monitor` commands). The OpenOCD user guide's "GDB and OpenOCD" and "RTT" chapters.
- **Cite as:** "OpenOCD user guide, §title" or "RPi Getting Started, Appendix A/B".

### 4. SEGGER RTT (Real-Time Transfer)

- **Source:** SEGGER Microcontroller GmbH.
- **URL:** <https://www.segger.com/products/debug-probes/j-link/technology/about-real-time-transfer/>
- **Also:** the RTT implementation reference (`SEGGER_RTT.c`/`SEGGER_RTT.h`) is BSD-licensed and shipped in the free J-Link software pack and mirrored at <https://github.com/SEGGERMicro/RTT>.
- **License:** The reference RTT sources are BSD; the article is free to read.
- **What we use it for:** the control-block layout (the `"SEGGER RTT"` magic the host scans SRAM for), the ring-buffer protocol, and the "skip vs block when full" modes. We reimplement a single up-channel from scratch in Exercise 2; the SEGGER sources are the ground truth for the on-wire layout that `openocd`'s `rtt` command and `JLinkRTTViewer` expect.
- **Cite as:** "SEGGER RTT docs" / "SEGGER_RTT.h".

### 5. sigrok / PulseView protocol decoders

- **Source:** the sigrok project.
- **URL:** <https://sigrok.org/wiki/Protocol_decoders>
- **License:** GPL-3.0.
- **What we use it for:** the `i2c`, `spi`, and `uart` decoders, and stacked decoders, that turn a logic-analyzer capture into a decoded transcript. The `sigrok-cli` man page (<https://sigrok.org/doc/sigrok-cli/unstable/sigrok-cli.html>) for headless capture; PulseView for the GUI. The cheap FX2-based analyzers are supported via the `fx2lafw` firmware.
- **Cite as:** "sigrok i2c decoder docs".

---

## Secondary references — consult as needed

### ARM Debug Interface Architecture Specification (ADIv5.2, IHI0031)

- **Source:** ARM Ltd., "ARM Debug Interface Architecture Specification ADIv5.0 to ADIv5.2", IHI0031.
- **URL:** <https://developer.arm.com/documentation/ihi0031/latest/>
- **What it is:** the spec that defines SWD and JTAG as debug-link protocols, the DP/AP register model, and the MEM-AP that gives a probe its memory window. Read §B (SWD protocol) and the MEM-AP chapter if you want to understand what OpenOCD is actually doing on the two wires. Not required for the exercises; required if you ever write a probe driver.

### Cortex-M0+ Technical Reference Manual (DDI0484)

- **Source:** ARM Ltd., "Cortex-M0+ Technical Reference Manual", DDI0484.
- **URL:** <https://developer.arm.com/documentation/ddi0484/latest/>
- **What we use it for:** the DWT chapter (the cycle counter, the reduced comparator set) and the breakpoint/watchpoint unit (BPU/DWT) comparator counts: 4 breakpoint comparators, 2 watchpoint comparators on the RP2040's configuration. This is where "you only get 4 hardware breakpoints" comes from.

### GDB user manual

- **Source:** Free Software Foundation.
- **URL:** <https://sourceware.org/gdb/current/onlinedocs/gdb/>
- **License:** GFDL.
- **Sections we use:** "Remote Debugging" (the `target extended-remote` workflow), "Breakpoints" (`hbreak`, conditions, `commands`), "Watchpoints" (`watch`/`rwatch`/`awatch`), "Extending GDB" (the Python API), and "Convenience Variables" (`$pc`, `$sp`, `$_`). The Python API chapter is what the `cc-debug` GDB extension is built on.

### NXP I²C-bus specification (UM10204)

- **Source:** NXP Semiconductors, "I²C-bus specification and user manual", UM10204 rev. 7.0, October 2021.
- **URL:** <https://www.nxp.com/docs/en/user-guide/UM10204.pdf>
- **Sections we use:** §3.1.16 "Bus clear" — the canonical nine-clock-pulse recovery for a stuck bus, which the hung-I²C challenge implements. §3.1.10 (START/STOP) and §3.1.6 (ACK/NACK) for reading the analyzer transcript.
- **Cite as:** "NXP UM10204 §3.1.16".

### Raspberry Pi debugprobe firmware

- **Source:** Raspberry Pi Ltd., `raspberrypi/debugprobe`.
- **URL:** <https://github.com/raspberrypi/debugprobe>
- **License:** BSD/Apache.
- **What we use it for:** the `debugprobe.uf2` you flash onto the second Pico to make it a CMSIS-DAP SWD probe (plus a UART bridge). The README documents the GP2/GP3 SWD pins and the GP4/GP5 UART bridge pins.

---

## Tertiary references — context and depth

### "Debugging" — David J. Agans

- **Source:** David J. Agans, *Debugging: The 9 Indispensable Rules for Finding Even the Most Elusive Software and Hardware Problems*, AMACOM, 2002. ISBN 978-0814474570.
- **What we use it for:** the methodology spine. "Understand the system", "Make it fail", "Quit thinking and look", "Divide and conquer", "Change one thing at a time", "Keep an audit trail" — the heisenbug challenge is Rule 2 ("Make it fail") and Rule 5 ("Change one thing at a time") in firmware form. Not free; library copy. The best 150 pages on debugging ever written.

### Cyril Fougeray / Memfault — "How to debug a HardFault on an ARM Cortex-M MCU"

- **Source:** Interrupt (Memfault's engineering blog), multiple posts.
- **URLs:**
  - "How to debug a HardFault on an ARM Cortex-M MCU": <https://interrupt.memfault.com/blog/cortex-m-hardfault-debug>
  - "A deep dive into ARM Cortex-M debug interfaces": <https://interrupt.memfault.com/blog/a-deep-dive-into-arm-cortex-m-debug-interface>
  - "Coredumps on ARM Cortex-M": <https://interrupt.memfault.com/blog/a-deep-dive-into-coredumps-with-gdb> and the coredump series.
- **License:** free to read.
- **What we use it for:** the canonical practitioner write-ups of exactly the fault-handler and core-dump techniques in Lecture 2. The Interrupt blog is the best free firmware-debugging resource on the internet; the HardFault and coredump posts are required reading for the mini-project.

### SystemView and SEGGER Ozone

- **Source:** SEGGER Microcontroller GmbH.
- **URLs:** <https://www.segger.com/products/development-tools/systemview/>, <https://www.segger.com/products/development-tools/ozone-j-link-debugger/>
- **License:** free for non-commercial / evaluation use; requires a J-Link.
- **What we use it for:** the upgrade path. SystemView visualizes RTOS task switches and ISR timing over RTT — the prettier sibling of the DWT-timing work in this week. Ozone is a graphical debugger over J-Link. Both run against the RP2040 with a J-Link. Mentioned, not required.

### The Pico SDK's hardware_dma / pico_stdlib panic path

- **Source:** Raspberry Pi Ltd., `raspberrypi/pico-sdk`.
- **URL:** <https://github.com/raspberrypi/pico-sdk>
- **Files worth opening:** `src/rp2_common/pico_runtime/runtime.c` (the default `runtime_init` and where the vector table comes from), `src/rp2_common/hardware_exception/exception.c` (the SDK's `exception_set_exclusive_handler` — how to install a HardFault handler the supported way), and `src/rp2_common/pico_platform/include/pico/platform.h` (`__not_in_flash_func`, `panic`). The SDK's panic prints over stdio; our fault handler is the souped-up version that survives without stdio.

---

## Free Apple/Adafruit/Sparkfun teaching materials

### Adafruit Learn — "Debugging the SAMD21 / Cortex-M with GDB and a probe"

- **URL:** <https://learn.adafruit.com/debugging-the-samd21-with-gdb>
- **What it covers:** the same OpenOCD-plus-GDB-plus-probe workflow on Adafruit's SAMD21 boards. The probe and target differ; the GDB session is identical. A friendly first read before the dense OpenOCD manual.

### Sparkfun — "Logic analyzer tutorial"

- **URL:** <https://learn.sparkfun.com/tutorials/using-the-usb-logic-analyzer-with-sigrok-pulseview>
- **What it covers:** wiring a cheap USB logic analyzer, installing PulseView, and applying protocol decoders — the exact bench setup for Thursday's bus-decoding exercise. 20 minutes.

### Hackaday — "Heisenbugs" and the observer effect in embedded

- **URL:** <https://hackaday.com/tag/debugging/>
- **What it covers:** pop-engineering write-ups of memorable embedded bugs, including several heisenbug stories (the bug that vanished when the scope probe's capacitance changed an edge; the race that only appeared in the release build). Useful context-setter for Friday.

---

## Tooling — what to install

You should have these on your path by Monday evening:

- **Pico SDK** at v1.5.1 or later, with `PICO_SDK_PATH` set.
- **A second Pico flashed as `debugprobe`** (`debugprobe.uf2` from <https://github.com/raspberrypi/debugprobe/releases>), or a Raspberry Pi Debug Probe / J-Link / any CMSIS-DAP adapter.
- **OpenOCD** new enough to know the RP2040 (`brew install openocd` on macOS; build the `raspberrypi/openocd` fork on Linux if your distro's package is too old).
- **`gdb-multiarch`** (Debian/Ubuntu) or **`arm-none-eabi-gdb`** (Homebrew, or the ARM toolchain bundle).
- **`picotool`** built from source or `brew install picotool`.
- **`sigrok-cli` + `pulseview`** (<https://sigrok.org/>) with `fx2lafw` firmware for cheap FX2 analyzers; or the **Saleae Logic** software if you have a genuine Saleae.
- **Python 3.10+** with `pyserial>=3.5`, `pylink-square` *optional* (only if you use a J-Link instead of OpenOCD).
- A **logic analyzer**, three **jumper wires** for SWD, and a **breadboard** for the I²C target (any I²C sensor you have from Week 4 — a BME280, an SSD1306 OLED, a DS3231 RTC).

A `requirements.txt` in `mini-project/` pins the Python versions.

---

## Reading time budget

| Reference                                            | Time     | When               |
|------------------------------------------------------|----------|--------------------|
| RP2040 datasheet §2.3.4 + §2.4                       | 30 min   | Monday morning     |
| RPi Getting Started, Appendix A + B                  | 45 min   | Monday morning     |
| OpenOCD user guide (GDB + RTT chapters)              | 30 min   | Monday afternoon   |
| ARMv6-M ARM §B1.5 (exceptions)                       | 45 min   | Tuesday/Wednesday  |
| Memfault Interrupt HardFault + coredump posts        | 45 min   | Wednesday          |
| SEGGER RTT docs                                      | 20 min   | Wednesday          |
| Cortex-M0+ TRM (DWT chapter)                         | 20 min   | Thursday           |
| sigrok protocol-decoder docs + NXP UM10204 §3.1.16   | 30 min   | Thursday/Friday    |
| Total                                                | ~4.5 h   | spread across week |

If you do the readings on the day they support, the lectures and exercises track cleanly. Defer them and you will be wiring SWD in the dark on Saturday with the midterm on Monday — measurably slower and much more stressful.
