# C7 · Crunch Wire — Embedded Systems & IoT Engineering

> Code Crunch Club · Crunch Labs tier · sub-brand **Wire** (`#C2410C`)
> 24 weeks · ~864 hours · GPL-3.0
> Track home: `C7-WIRE-CRUNCH-EMBEDDED-SYSTEMS/`

Twenty-four weeks of disciplined, production-grade embedded engineering. By the end you will have driven an ARM Cortex-M from a blank linker script to a signed, OTA-updatable firmware image talking MQTT-over-TLS to a fleet broker, with a TinyML classifier running at the edge and a logic analyzer on the bench proving every byte. You will write firmware in C, modern C++20, Rust (`embassy`, `embedded-hal`), and MicroPython; you will boot Zephyr and FreeRTOS; you will design and order a small KiCad board; you will pass a senior firmware interview.

This is not a hobbyist Arduino course. It is the curriculum we wish existed when we first shipped a connected product.

---

## Who this is for

Four personas, all welcome, all stretched:

1. **The industry engineer leveling up.** You write firmware today, mostly against vendor SDKs (STM32Cube, ESP-IDF, Nordic nRF Connect). You want to understand the layers below — linker scripts, startup files, ISR latency, RTOS internals — and the layers above — fleet OTA, secure boot, device shadows.
2. **The FAANG-bound new grad with Python.** You finished C1 (Code Crunch Convos). You can write Python. You want to break into firmware, IoT, or systems roles at companies like Apple Silicon, Tesla, SpaceX, Anduril, Nest, or a hardware-heavy startup. You need C fluency, hardware intuition, and a portfolio that proves both.
3. **The hardware founder.** You have a product idea — a sensor, a wearable, a connected appliance — and you need to ship a fleet, not a prototype. You need to understand firmware lifecycle, OTA, attestation, compliance, and what your contract manufacturer is going to ask you for.
4. **The security researcher pivoting to firmware.** You reverse engineer firmware. Now you want to build it: understand bootloader chains, signing infrastructure, secure elements (ATECC608, OPTIGA), TLS on a 256 KB MCU, and how real attacks against fleet OTA actually work.

If you can read a C function and a datasheet without panic, you are ready. If you cannot, take C1 and C14 (Linux) first.

---

## What you will be able to do at the end

Concrete capabilities you should have on day 168:

1. Bring up a new ARM Cortex-M target from scratch — write the linker script, the startup file, the vector table, and blink an LED with zero vendor HAL.
2. Read a 1,200-page reference manual and a 90-page datasheet without flinching, and translate register tables into working peripheral drivers.
3. Write production embedded C that passes MISRA-C-aligned review, and modern C++20 (`constexpr`, `concepts`, no-RTTI, no-exceptions) where it earns its keep.
4. Write embedded Rust with `embassy` async executors and the `embedded-hal` trait ecosystem, and explain when Rust beats C and when it does not.
5. Architect a FreeRTOS or Zephyr application — tasks, queues, semaphores, mutexes — and diagnose priority inversion with a scope and an SWO trace.
6. Bring up GPIO, UART, SPI, I2C, CAN, USB-CDC, ADC, DAC, PWM, DMA, and timers, and explain the trade-offs of polled vs interrupt vs DMA-driven I/O.
7. Drive a six-axis IMU, a ToF sensor, an environmental sensor, a brushed DC motor, and a hobby servo, with calibration, filtering, and fault handling.
8. Build wireless connectivity over BLE 5, Wi-Fi (ESP32), LoRaWAN, Zigbee/Thread, and Matter; speak MQTT, MQTT-SN, and CoAP fluently.
9. Train and deploy a TinyML model with TensorFlow Lite Micro (open path; Edge Impulse used as an aid, not a lock-in), profile its memory and latency, and ship it inside a 200 KB firmware image.
10. Design a dual-bank bootloader with secure boot, code signing, and a delta-encoded OTA flow that survives a power loss mid-update.
11. Bring up Linux on an embedded SoC with Yocto or Buildroot — write a recipe, modify a device tree, build a BSP.
12. Debug like a professional: GDB over J-Link, OpenOCD, SWO/ITM, Saleae logic captures, scope shots — and ship a runbook that on-call engineers can actually use at 3 AM.

---

## Prerequisites

| Required | Helpful | Not required |
| --- | --- | --- |
| **C1 — Code Crunch Convos** (Python fluency) | **C14 — Linux Engineering** | A four-year CS degree |
| Comfortable reading a C function | Some prior Arduino or Raspberry Pi tinkering | Prior firmware job experience |
| A laptop (Linux preferred; macOS fine; Windows via WSL2) | An oscilloscope (we provide loaners by cohort) | An EE background |

Hardware kit (~$240, ships week 0, GPL-clean alternatives listed in the syllabus): STM32 Nucleo-F446, ESP32-S3-DevKitC, RP2040 Pico, ATmega328P breadboard kit, J-Link EDU Mini, Saleae Logic 8 (cohort loaner) or equivalent, jumper kit, BME280, MPU-6050, VL53L1X, an SD card, an SSD1306 OLED, a hobby servo, a small Li-Po pack.

---

## Program at a glance — four phases

| Phase | Weeks | Title | Focus | Capstone milestone |
| --- | --- | --- | --- | --- |
| I | 1–6 | Foundations | C/C++ discipline, hardware literacy, bare-metal bring-up | Bare-metal driver library on STM32 |
| II | 7–12 | Bare-Metal & RTOS | FreeRTOS, Zephyr, peripherals, debugging | Multi-task sensor hub with SWO traces |
| III | 13–18 | IoT Fleet & Edge | Wireless, MQTT, BLE/Thread/Matter, TinyML | Fleet of three nodes streaming to broker |
| IV | 19–24 | Production & Capstone | Bootloaders, OTA, security, Linux/Yocto, capstone build | Signed OTA, chaos drill, video, postmortem |

Detailed week-by-week breakdown lives in [`SYLLABUS.md`](./SYLLABUS.md). Design rationale (why 24 weeks, why this ordering, why open-source-first) lives in [`CHARTER.md`](./CHARTER.md).

---

## Weekly cadence

| Day | Block | Typical content |
| --- | --- | --- |
| Mon | Lecture (2h) | Topic intro, datasheet read-along, reference architecture |
| Mon | Lab (3h) | Guided exercise on the kit |
| Wed | Lecture (2h) | Deeper dive, code review of prior week's lab |
| Wed | Lab (3h) | Open-ended mini-project sprint |
| Fri | Studio (4h) | Office hours, debugging clinic, scope/logic-analyzer time |
| Sun | Quiz (~30m) + reading | Auto-graded; covers the week's reference manual sections |

Self-paced cohorts compress to a 12h/week schedule; full-time cohorts run ~36h/week. Each week ships one mini-project, one quiz, and one logged-bench-time entry.

---

## Recommended pre/post tracks

```text
C1 (Code Crunch Convos · Python)
        |
        v
C14 (Linux Engineering)   <-- strongly recommended before C7
        |
        v
*** C7 (Crunch Wire — Embedded Systems & IoT Engineering) ***
        |
        +--> C18 / C19  (Cloud Edge & Platform Engineering)
        |       for fleet-scale backend, K8s edge, observability
        |
        +--> C24 (Robotics — ROS2, Autonomy, Multi-Actuator)
                for moving things in the physical world
```

- **C7 vs C24.** C7 owns the single device, the firmware, and the IoT fleet. C24 owns multi-actuator autonomous systems with ROS2, motion planning, and SLAM. They overlap on motor control and sensor fusion; they diverge on everything else. Take C7 first if you want to ship a connected product; take C24 first if you want to build a robot.
- **C7 to C18/C19.** If your IoT capstone makes you fall in love with the cloud side — fleet provisioning, telemetry pipelines, edge K8s — C18/C19 is the natural next step.

---

## What this course will NOT do

Honest expectations, set up front:

- **It will not make you a chip designer.** We teach you to read ARM Cortex-M datasheets and to use peripherals; we do not teach RTL, Verilog, or silicon tape-out. If that is what you want, look at FPGA-focused tracks.
- **It will not certify your product for FCC / CE.** We give a one-week compliance briefing covering the shape of the work, what an EMC chamber visit looks like, and which standards apply (FCC Part 15, CE RED, EN 301 489). Actual certification is a paid lab engagement.
- **It will not turn you into a power-electronics expert.** We cover battery design, low-power MCU modes, and power profiling. We do not cover SMPS design, magnetics, or kilowatt-scale systems.
- **It will not make you a PCB-layout professional.** We use KiCad, you will design and order one small two-layer board, and you will learn what a good layout reviewer looks for. Senior PCB layout is its own decade-long career.
- **It will not pretend "AI on the edge" is magic.** TinyML is real, useful, and severely constrained. We teach you the constraints (RAM, flash, MAC budget) before the models.
- **It will not lock you into any vendor.** ST, Espressif, Nordic, Raspberry Pi, and Microchip parts all appear. Zephyr and FreeRTOS, both. KiCad over Altium. Open MQTT brokers before AWS IoT Core.

---

## License & maintainers

This curriculum is licensed under **GPL-3.0**. See [`LICENSE`](./LICENSE).

You may fork, adapt, teach, and remix. If you improve it, please PR back to <https://github.com/CODECRUNCHWORLDWIDE>.

Maintainers: Code Crunch Club — Crunch Labs working group. Track lead: Wire (`#C2410C`). Issues, errata, and PRs at the GitHub org.


---

<!-- CCWW:AUTO-INDEX:START — generated by scripts/restructure_course_repos.py; edit ABOVE this marker -->

## Course at a glance

| Section | Count |
| --- | --- |
| Curriculum entries | 12 |
| Projects | 0 |
| Past sessions | 1 |

## Curriculum

- [SYLLABUS](curriculum/SYLLABUS.md)
- [week 01 bare metal bringup](curriculum/week-01-bare-metal-bringup/README.md)
- [week 02 gpio uart and registers](curriculum/week-02-gpio-uart-and-registers/README.md)
- [week 03 linker scripts and startup](curriculum/week-03-linker-scripts-and-startup/README.md)
- [week 04 spi i2c bus drivers](curriculum/week-04-spi-i2c-bus-drivers/README.md)
- [week 05 modern cpp on bare metal](curriculum/week-05-modern-cpp-on-bare-metal/README.md)
- [week 06 rtos basics freertos](curriculum/week-06-rtos-basics-freertos/README.md)
- [week 07 rtos patterns and exti sensors](curriculum/week-07-rtos-patterns-and-exti-sensors/README.md)
- [week 08 dma adc and ring buffers](curriculum/week-08-dma-adc-and-ring-buffers/README.md)
- [week 09 usb device classes tinyusb](curriculum/week-09-usb-device-classes-tinyusb/README.md)
- [week 10 bootloaders and firmware updates](curriculum/week-10-bootloaders-and-firmware-updates/README.md)
- [week 11 wireless wifi mqtt tls](curriculum/week-11-wireless-wifi-mqtt-tls/README.md)

## In this course

- **Community** — [community/](community/)
- **Curriculum** — [curriculum/](curriculum/)
- **Projects** — [projects/](projects/)
- **Resources** — [resources/](resources/)
- **Past sessions** — [past-sessions/](past-sessions/)

<!-- CCWW:AUTO-INDEX:END -->
