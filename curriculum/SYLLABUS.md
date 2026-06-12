# C7 · Crunch Wire — Syllabus

> Embedded Systems & IoT Engineering
> 24 weeks · ~864 hours · 1 capstone · GPL-3.0
> Crunch Labs tier · sub-brand **Wire** (`#C2410C`)

This document covers the week-by-week plan, assessment matrix, capstone specification, and career engineering pack. For design rationale, see [`CHARTER.md`](./CHARTER.md). For audience and outcomes, see [`README.md`](./README.md).

---

## Phases

| Phase | Weeks | Title | Theme |
| --- | --- | --- | --- |
| **I — Foundations** | 1–6 | C/C++ Discipline & Bare-Metal Bring-Up | Datasheets, registers, linker scripts, blink without HAL |
| **II — Bare-Metal & RTOS** | 7–12 | Peripherals, FreeRTOS, Zephyr, Debugging | Tasks, queues, ISRs, DMA, scope-and-logic-analyzer hygiene |
| **III — IoT Fleet & Edge** | 13–18 | Wireless, MQTT, BLE/Thread/Matter, TinyML | Many devices, one broker, models that fit in flash |
| **IV — Production & Capstone** | 19–24 | Bootloaders, OTA, Security, Linux, Capstone | What it takes to ship and own a fleet |

---

## Phase I — Foundations (Weeks 1–6)

### Week 1 — Embedded Engineering, From First Principles

- **Topics:** What an MCU is vs an MPU; toolchains (`arm-none-eabi-gcc`, Clang, LLVM); hosted vs freestanding C; the `volatile` keyword; reading reference manuals.
- **Lecture:** A field tour of ARM Cortex-M0/M4/M7/M33, Xtensa LX7 (ESP32-S3), RISC-V (Pico 2), and AVR — when each is the right answer. Read the first 30 pages of the STM32F446 reference manual together.
- **Hands-on:** **Lab 01 — Toolchain bring-up.** Install `arm-none-eabi-gcc`, `clang`, `cmake`, `openocd`, `gdb-multiarch`; cross-compile and run a sized "hello" that prints over semihosting.
- **Skills earned:**
  - Build an ARM cross-compile toolchain from scratch.
  - Read a register table in a Cortex-M reference manual.
  - Explain the difference between hosted and freestanding C.

### Week 2 — Embedded C, Properly

- **Topics:** Storage classes; `volatile` + `static const`; bitfields and packed structs; fixed-width integers; undefined behavior on small targets; `errno`-free APIs.
- **Lecture:** What MISRA-C actually says and what it does not. Code review of a real production firmware function. The cost of `printf` (~30 KB) and what to use instead.
- **Hands-on:** **Lab 02 — `ring_buffer.c`.** Implement a lock-free single-producer / single-consumer ring buffer, unit-test on host with Unity, prove no UB with `-fsanitize=undefined`.
- **Skills earned:**
  - Write a freestanding C library with host-side tests.
  - Reason about UB on 32-bit and 8-bit targets.
  - Apply MISRA-C-aligned style to your own code.

### Week 3 — Bare-Metal Bring-Up: Linker Scripts & Startup

- **Topics:** Memory map; flash vs SRAM; `.text` / `.data` / `.bss` / `.stack`; vector table; reset handler; `_start`; `__libc_init_array`.
- **Lecture:** What `arm-none-eabi-ld` actually does. Walk a real linker script for the STM32F446 line by line. Why your first `main()` segfaults before it runs.
- **Hands-on:** **Lab 03 — Blink without HAL.** Write your own linker script, your own `startup.s`, your own vector table; toggle `GPIOA->ODR` directly; flash with OpenOCD; verify on a scope.
- **Skills earned:**
  - Author a linker script for a new Cortex-M target.
  - Write a startup file and vector table from scratch.
  - Toggle a GPIO with zero vendor HAL code.

### Week 4 — Modern C++ on Bare Metal

- **Topics:** Why C++20 (`constexpr`, `consteval`, `concepts`, `std::span`); why not (RTTI, exceptions, default `new`); the embedded C++ subset; CRTP for zero-cost peripheral drivers.
- **Lecture:** A tour of the `etl` (Embedded Template Library) and what it replaces from `std`. Why static `constexpr` peripheral templates compile to the same machine code as raw register writes, but are safer.
- **Hands-on:** **Lab 04 — `Gpio<Port, Pin>` template.** Build a CRTP-based GPIO driver in C++20 that produces identical assembly to hand-written register writes; verify with `objdump`.
- **Skills earned:**
  - Use C++20 on a freestanding target without bloat.
  - Read disassembly to prove zero-cost abstractions.
  - Justify when to reach for C++ and when to stay in C.

### Week 5 — Rust on Embedded

- **Topics:** `no_std` Rust; `embedded-hal` traits; `cortex-m-rt`; `probe-rs` and `defmt`; `embassy` async executor; the `Pin`/`Send`/`Sync` story on MCUs.
- **Lecture:** Why Rust prevents whole classes of firmware bugs (concurrent access to peripherals, dangling DMA pointers). What it does not prevent (logic bugs, datasheet misreading, supply chain).
- **Hands-on:** **Lab 05 — Embassy blink + UART echo.** Port Lab 03 to Rust + `embassy`; add an async UART echo task; flash with `probe-rs run`.
- **Skills earned:**
  - Build a `no_std` Rust firmware with `embassy` and `probe-rs`.
  - Map `embedded-hal` traits to vendor HALs.
  - Compare Rust and C binary sizes for the same firmware.

### Week 6 — MicroPython, and Then Choosing Your Language

- **Topics:** When MicroPython is the right call (rapid prototyping, devops scripts, internal tools); REPL on MCU; FFI to C; memory and GC trade-offs.
- **Lecture:** Decision framework: C for footprint and certifiability, Rust for safety, C++ for type-rich drivers, MicroPython for time-to-prototype. We will keep all four alive for the rest of the course.
- **Hands-on:** **Lab 06 — MicroPython sensor logger.** On an RP2040 Pico, sample a BME280 over I2C in MicroPython, log to SD card, and produce a CSV. Then port the inner loop to C for size comparison.
- **Skills earned:**
  - Run MicroPython on RP2040 and ESP32-S3.
  - Decide language per subsystem with explicit criteria.
  - Quantify binary size and RAM cost across C / C++ / Rust / MicroPython.

---

## Phase II — Bare-Metal & RTOS (Weeks 7–12)

### Week 7 — Peripherals I: GPIO, UART, Timers, ADC

- **Topics:** Alternate functions; baud-rate generation; timer modes (PWM, input capture, output compare); SAR vs sigma-delta ADCs; sampling vs aliasing.
- **Lecture:** From the STM32F446 timer block diagram to a clean software API. Why your UART drops bytes at 921600 baud and how to fix it with DMA.
- **Hands-on:** **Lab 07 — Logic-analyzer driver-shootout.** Implement UART TX three ways: polled, IRQ, DMA. Measure CPU load on a Saleae capture. Push baud until each one breaks.
- **Skills earned:**
  - Configure clocks, AF mux, and timers from registers up.
  - Compare polled / IRQ / DMA on a logic analyzer.
  - Read a Saleae trace for protocol-level bugs.

### Week 8 — Peripherals II: SPI, I2C, CAN, USB

- **Topics:** SPI clock polarity & phase; I2C clock stretching; CAN frame format and arbitration; USB CDC enumeration.
- **Lecture:** Why I2C looks simple and is not. CAN bus arbitration as a worked example with two nodes on a real bench. USB-CDC enumeration trace under Wireshark/usbmon.
- **Hands-on:** **Lab 08 — Multi-bus sensor hub.** Read an MPU-6050 (I2C), an external flash (SPI), and accept commands over USB-CDC. Decode all three on Saleae simultaneously.
- **Skills earned:**
  - Bring up SPI, I2C, CAN, and USB-CDC end-to-end.
  - Decode and triage bus errors on a logic capture.
  - Reason about peripheral clock budgets and DMA conflicts.

### Week 9 — FreeRTOS

- **Topics:** Tasks; priorities; queues; semaphores (binary, counting); mutexes; priority inheritance; software timers; tickless idle.
- **Lecture:** A real priority-inversion bug on a real board, reproduced and fixed live with `vTaskGetRunTimeStats` and an SWO trace.
- **Hands-on:** **Lab 09 — Sensor hub with FreeRTOS.** Refactor Lab 08 into three tasks (sample, log, command) connected by queues; measure jitter with SWO/ITM under load.
- **Skills earned:**
  - Architect a FreeRTOS app with explicit task / queue / mutex roles.
  - Diagnose priority inversion with a runtime-stats dump.
  - Capture and read SWO/ITM traces.

### Week 10 — Zephyr RTOS

- **Topics:** `west` workflow; device tree on Zephyr; Kconfig; threads vs work queues; the Zephyr driver model; `logging` and `shell` subsystems.
- **Lecture:** Why a permissive-licensed, vendor-neutral RTOS matters at fleet scale. The trade-off: Zephyr is heavier than FreeRTOS but ships you 80% of the BSP for free.
- **Hands-on:** **Lab 10 — Zephyr port of the sensor hub.** Take Lab 09 and re-implement on Zephyr with `west`, an overlay device tree, and the Zephyr shell. Diff the binary sizes.
- **Skills earned:**
  - Run a Zephyr project end-to-end with `west`.
  - Write and override a device-tree overlay.
  - Compare FreeRTOS vs Zephyr for a given product.

### Week 11 — DMA, Interrupts, and Real-Time Discipline

- **Topics:** NVIC; interrupt priorities; ISR safety rules; DMA channels and arbitration; double-buffering; latency budgets.
- **Lecture:** What an ISR may not do. How to measure worst-case interrupt latency with a GPIO toggle and a scope. The shape of a real real-time audio loop.
- **Hands-on:** **Lab 11 — DMA double-buffer audio.** Sample 48 kHz audio via ADC+DMA in a ping-pong buffer, run a 64-tap FIR, output to DAC+DMA. Measure end-to-end latency on a scope.
- **Skills earned:**
  - Design a DMA-driven real-time data path.
  - Measure and bound interrupt latency.
  - Apply ISR-safety rules under review.

### Week 12 — Debugging Like a Senior

- **Topics:** GDB scripting; OpenOCD vs J-Link; SWO/ITM for `printf`-free tracing; SystemView; Saleae high-level analyzers; scope triggering on protocol edges.
- **Lecture:** A taxonomy of firmware bugs (timing, memory, peripheral state, supply, EMI) and the right tool for each. Live debugging of a hung-bus I2C case study.
- **Hands-on:** **Lab 12 — "Find the bug" pentathlon.** Five pre-broken firmware images; you have 90 minutes per bug. Tools: GDB, OpenOCD, SWO, Saleae, scope. Document the runbook.
- **Skills earned:**
  - Drive a GDB+OpenOCD session over SWD/JTAG.
  - Capture and decode SystemView/ITM traces.
  - Author a runbook for a real production bug.

**Midterm (end of Week 12):** 90-minute oral + bench exam. You bring up an unknown peripheral on an unfamiliar Cortex-M board, from datasheet to working driver, in front of a reviewer.

---

## Phase III — IoT Fleet & Edge (Weeks 13–18)

### Week 13 — Wi-Fi, TLS, MQTT

- **Topics:** ESP-IDF; lwIP; `mbedTLS` on MCU; MQTT v3.1.1 and v5; QoS levels; LWT; retained messages; Mosquitto broker.
- **Lecture:** What a TLS handshake costs on a 240 MHz Xtensa (RAM, flash, latency). MQTT topic-design patterns that survive a 10,000-device fleet.
- **Hands-on:** **Lab 13 — Telemetry node to Mosquitto.** Bring up Wi-Fi on ESP32-S3 with ESP-IDF; publish sensor JSON to a local Mosquitto broker over TLS with mutual auth.
- **Skills earned:**
  - Configure mTLS with `mbedTLS` on an MCU.
  - Design MQTT topic hierarchies for fleet scale.
  - Operate a local Mosquitto broker for development.

### Week 14 — BLE 5 and BLE Mesh

- **Topics:** GAP, GATT, advertisements vs connections; pairing & bonding; BLE Mesh provisioning; the Nordic SoftDevice and the Zephyr BLE stack.
- **Lecture:** When BLE wins (low power, phone integration) and loses (throughput, hop count). BLE Mesh as a real alternative to Zigbee for lighting/sensing fleets.
- **Hands-on:** **Lab 14 — BLE-Mesh occupancy fleet.** Three nRF52840 nodes form a BLE-Mesh network; one is the occupancy sensor publisher, others are subscribers; bridge to MQTT via a gateway.
- **Skills earned:**
  - Implement a GATT server and a BLE-Mesh model.
  - Pair and bond securely with LE Secure Connections.
  - Bridge BLE Mesh to MQTT.

### Week 15 — LoRaWAN, Zigbee, Thread, Matter

- **Topics:** LoRa modulation and LoRaWAN class A/B/C; Zigbee Pro vs Thread (both 802.15.4); Matter over Thread vs Matter over Wi-Fi; CSA certification shape.
- **Lecture:** Long-range vs low-latency vs mesh — choosing the right radio. Matter as an industry consolidation moment and what it does not solve.
- **Hands-on:** **Lab 15 — Matter-over-Thread light.** Build a Matter commissionable light on an nRF52840 + OpenThread Border Router; pair to Apple Home / Google Home; sniff with `pyspinel`.
- **Skills earned:**
  - Bring up an OpenThread Border Router.
  - Implement a Matter device with the open-source `connectedhomeip` SDK.
  - Decide between BLE Mesh / Zigbee / Thread / LoRaWAN per product.

### Week 16 — Sensors, Actuators, and Sensor Fusion

- **Topics:** IMU calibration (gyro bias, accel offset, magnetometer hard/soft iron); complementary and Madgwick filters; ToF & lidar basics; motor control (brushed DC PWM, BLDC FOC intro, hobby servo); current sensing.
- **Lecture:** Why naive IMU integration drifts and how to fuse it cheaply. The difference between control-grade and consumer-grade sensors.
- **Hands-on:** **Lab 16 — Tilt-compensated heading.** Fuse MPU-6050 + magnetometer with a Madgwick filter on Cortex-M4F; plot drift over 10 minutes vs a reference.
- **Skills earned:**
  - Calibrate an IMU end-to-end.
  - Implement and tune a Madgwick / complementary fusion filter.
  - Drive a hobby servo and a brushed DC motor under closed-loop control.

### Week 17 — TinyML on the Edge

- **Topics:** TensorFlow Lite Micro; model quantization (INT8); operator selection; memory arenas; CMSIS-NN; Edge Impulse as an aid (not lock-in); when to NOT use ML.
- **Lecture:** Power, latency, memory: the three constraints. Why a 10 KB anomaly detector beats a 10 MB CNN on an MCU. Honest accuracy reporting.
- **Hands-on:** **Lab 17 — Keyword-spotting on Cortex-M4F.** Train a small CNN (TF or Keras), quantize to INT8, deploy with TFLite Micro + CMSIS-NN, measure RAM and latency on the bench.
- **Skills earned:**
  - Train, quantize, and deploy a TFLite Micro model.
  - Profile model RAM, flash, and inference latency.
  - Decide when not to use ML on an MCU.

### Week 18 — Power & Battery Design

- **Topics:** Low-power modes (Sleep, Stop, Standby); RTC wakeups; sub-µA budgets; Li-Po chemistry; fuel-gauging (MAX17048); power profiling with Nordic PPK2 / Joulescope.
- **Lecture:** What "10 years on a coin cell" requires (it is mostly not the MCU). Bench measurement vs simulation.
- **Hands-on:** **Lab 18 — Sub-10-µA telemetry node.** Take Lab 13's telemetry node; redesign duty cycle, sleep modes, and radio bursts to hit a < 10 µA average; prove it on a PPK2 trace.
- **Skills earned:**
  - Design a duty-cycled sensor for multi-year battery life.
  - Use a Nordic PPK2 / Joulescope for production power profiling.
  - Specify a Li-Po + charger + fuel-gauge stack.

---

## Phase IV — Production & Capstone (Weeks 19–24)

### Week 19 — Bootloaders, Dual-Bank, OTA

- **Topics:** Bootloader chains; MCUboot; dual-bank vs single-bank; A/B updates; rollback; delta encoding (`detools`); power-loss safety.
- **Lecture:** Anatomy of an OTA that bricks a fleet, and how to avoid each step. MCUboot's swap-and-revert dance.
- **Hands-on:** **Lab 19 — MCUboot OTA on Zephyr.** Set up MCUboot with image signing; deploy a signed update over MQTT; force a power loss mid-update; verify rollback works.
- **Skills earned:**
  - Configure MCUboot for dual-bank updates.
  - Sign and verify firmware images.
  - Survive a power-loss-during-update test.

### Week 20 — Firmware Security

- **Topics:** Secure boot (root of trust, immutable bootrom); code signing (ECDSA P-256); secure elements (ATECC608, OPTIGA Trust M); TLS on MCU; device identity & attestation; threat modeling (STRIDE applied to firmware).
- **Lecture:** A walk through real published firmware attacks (the ones we are allowed to discuss). Why "obscurity is not security" hits especially hard in embedded.
- **Hands-on:** **Lab 20 — Provisioning + attestation.** Provision per-device ECDSA keys to a fleet of three boards using a secure element; attest device identity to the broker before accepting commands.
- **Skills earned:**
  - Build a chain of trust from bootrom to application.
  - Use a secure element for key storage and signing.
  - Threat-model an embedded product against STRIDE.

### Week 21 — Hardware: Schematics, KiCad, PCB Basics

- **Topics:** Reading schematics; reference designs from vendor datasheets; KiCad 8 workflow; common layout mistakes (return paths, decoupling, antenna keep-outs); DRC.
- **Lecture:** What a senior PCB reviewer looks for in 20 seconds. Why your first board will work but not pass EMC.
- **Hands-on:** **Lab 21 — Two-layer KiCad sensor board.** Design a small two-layer board: MCU + sensor + USB-C + Li-Po charger; pass DRC; export Gerbers; (optional) order from a fab.
- **Skills earned:**
  - Draft a schematic from a vendor reference design.
  - Lay out a two-layer board with sensible decoupling and grounding.
  - Export production-ready Gerbers and a BOM.

### Week 22 — Linux on Embedded: Yocto & Buildroot

- **Topics:** When you graduate from MCU to MPU (BeagleBone, i.MX 8M, Raspberry Pi CM4); Yocto layers and recipes; Buildroot for simplicity; device tree; kernel modules (intro); BSPs.
- **Lecture:** The spectrum from "tiny Linux" (Buildroot, 20 MB) to "platform Linux" (Yocto, multi-GB) and where your product belongs.
- **Hands-on:** **Lab 22 — Custom Yocto image for a CM4.** Add a custom layer; build a minimal image with your application baked in; modify the device tree to enable a SPI peripheral; boot it.
- **Skills earned:**
  - Author a Yocto layer and recipe.
  - Modify a device tree and verify with `/proc/device-tree`.
  - Decide between Buildroot and Yocto for a project.

### Week 23 — Fleet Operations, Observability, Compliance Briefing

- **Topics:** Device shadows / twins; fleet provisioning; certificate rotation; telemetry pipelines (MQTT → Kafka / Kinesis → time-series DB); Grafana for fleet dashboards; FCC Part 15, CE RED, EN 301 489 (briefing only).
- **Lecture:** What an on-call rotation for a million-device fleet actually looks like. Where the wins are (good provisioning, good observability) and where the pain is (cert rotation, radio compliance).
- **Hands-on:** **Lab 23 — Fleet dashboard + cert rotation drill.** Stand up an open MQTT broker, push three nodes' telemetry into InfluxDB, dashboard in Grafana, then execute a cert rotation across the fleet with zero downtime.
- **Skills earned:**
  - Build a fleet telemetry pipeline with open-source components.
  - Execute a zero-downtime certificate rotation.
  - Read a compliance test plan and translate it for engineering.

### Week 24 — Capstone Showcase

- **Topics:** Demo discipline; postmortem writing; portfolio packaging; the senior-firmware interview loop.
- **Lecture:** What hiring managers at Apple Silicon, Tesla Powertrain, Anduril, SpaceX Avionics, and Nest actually ask in a senior firmware loop, and how your capstone covers each axis.
- **Hands-on:** **Capstone defense.** Live demo of the fleet (three nodes, gateway, broker, dashboard, edge ML, signed OTA). 20-minute Q&A from a reviewer panel. Public postmortem of one chaos drill.
- **Skills earned:**
  - Demo a fleet-scale system without it falling over.
  - Defend architectural decisions in front of senior reviewers.
  - Ship a capstone artifact you would happily send to a hiring manager.

---

## Assessment matrix

| Component | Weight | Cadence | Format |
| --- | --- | --- | --- |
| Weekly quiz | 10% | Weeks 1–23 | Auto-graded, ~30 min, reference-manual-heavy |
| Weekly mini-project | 30% | Weeks 1–23 | Reviewed by peers + a TA, with a logic-analyzer or scope artifact |
| Midterm (oral + bench) | 15% | End of Week 12 | 90 min, unknown peripheral bring-up |
| Capstone | 30% | Weeks 19–24 | Live deploy, 5-min video, postmortem |
| On-call drill | 5% | Week 23 | 60-min timed incident on a pre-broken fleet |
| Mock interview | 10% | Week 24 | 60-min senior firmware loop with an external reviewer |

Passing bar: 70% overall, AND a passing capstone, AND a passing on-call drill. A weak quiz week is forgivable; a non-functional capstone is not.

---

## Capstone — Fleet-Scale IoT Gateway + Edge ML Sensor Node

The capstone is **one** substantial system, not five toys. You will build:

```text
                                  +-------------------+
                                  |  Cloud / Local    |
                                  |  MQTT Broker      |
                                  |  (Mosquitto)      |
                                  +---------+---------+
                                            ^
                              MQTT/TLS, mTLS, QoS 1
                                            |
                                  +---------+---------+
                                  |  Gateway          |
                                  |  Linux (Yocto on  |
                                  |  CM4 or BBB)      |
                                  |  - BLE-Mesh proxy |
                                  |  - Thread BR      |
                                  |  - OTA orchestr.  |
                                  +----+----+----+----+
                                       |    |    |
                          BLE Mesh    Thread   USB-CDC
                                       |    |    |
                       +---------------+    |    +---------------+
                       |                    |                    |
              +--------v-------+   +--------v-------+   +--------v-------+
              | Node A (ESP32) |   | Node B (nRF52) |   | Node C (STM32) |
              | TinyML KWS     |   | Occupancy mesh |   | Power-profiled |
              | TFLite Micro   |   | Matter device  |   | sub-10µA loop  |
              +----------------+   +----------------+   +----------------+
                       \                    |                    /
                        \________ Signed OTA via MCUboot _______/
```

### Required deliverables

1. **Architecture document** (~10 pages) covering threat model, topic design, OTA strategy, and a hardware BOM.
2. **Three firmware images**, signed, OTA-deployable, one of which runs a TinyML model.
3. **A Linux gateway image** built with Yocto or Buildroot, with the broker bridge and OTA orchestrator baked in.
4. **A live deployment** (or a Dockerized runnable image of the broker + dashboard) the reviewer can hit during the demo.
5. **A 5-minute demo video** showing the fleet end-to-end. Voice-over required. No marketing edits.
6. **A postmortem (~3–5 pages)** of one of the following chaos drills you must run on yourself before the demo:
   - **Battery depletion** — a node's battery drops below threshold mid-update.
   - **Network partition** — the broker is offline for 30 minutes while three nodes are mid-publish.
   - **Cert rotation** — you must rotate the root CA across the fleet with no node going dark.
7. **A bench artifact pack** — at least one Saleae capture, one scope shot, and one SWO/SystemView trace from the system, with annotations.

### Capstone grading axes

| Axis | Weight |
| --- | --- |
| Firmware quality (review-ready C/Rust/C++) | 20% |
| System correctness (does it actually work end-to-end) | 25% |
| Production discipline (signed OTA, observability, runbook) | 20% |
| Hardware literacy (schematic, layout review, bench traces) | 10% |
| Communication (architecture doc + video + postmortem) | 15% |
| Chaos drill outcome | 10% |

---

## Career engineering pack

Delivered alongside the capstone:

### Interview prep topics (covered in Weeks 23–24)

- C language deep-dive: `volatile`, memory ordering, UB on small targets.
- RTOS internals: scheduler types, priority inheritance, IPC primitives.
- Peripheral bring-up under timed conditions (the "make this UART work" whiteboard).
- Linker scripts and startup files: explain every line.
- Power: estimate the average current for a duty-cycled node from a back-of-envelope sketch.
- TLS on MCU: handshake cost, certificate storage, mTLS vs PSK.
- OTA design: what goes wrong, how you recover.
- A real ISR-corruption bug story you can tell on the spot.

### Production runbook contents (template provided)

- Build & flash steps (every command, no hand-waving).
- JTAG/SWD pinout and how to attach.
- How to capture an SWO/ITM trace under load.
- How to extract logs from a deployed device without ssh.
- The five most common failure modes and the first three diagnostics for each.
- Cert rotation procedure.
- OTA rollback procedure.
- Who to call at 3 AM.

### Portfolio recommendations

- The capstone repo, public, GPL-3.0, with a real README and a real architecture doc.
- One PR merged into an open-source embedded project (Zephyr, MCUboot, `embassy`, `embedded-hal`, an ESP-IDF component, or a KiCad library).
- A short technical blog post explaining one bug from your bench-debugging pentathlon.
- A LinkedIn / website page that links to all of the above and does not contain the word "passionate."

---

## License

This curriculum is licensed under **GPL-3.0**. See [`LICENSE`](./LICENSE). Fork, teach, remix; PR improvements back to <https://github.com/CODE-CRUNCH-CLUB>.
