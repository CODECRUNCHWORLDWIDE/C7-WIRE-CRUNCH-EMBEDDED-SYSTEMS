# C7 · Crunch Wire — Track Charter

> The design rationale behind the 24-week Embedded Systems & IoT Engineering redesign.
> Crunch Labs tier · sub-brand **Wire** (`#C2410C`) · GPL-3.0

This document explains **why** C7 looks the way it does. The course outcomes live in [`README.md`](./README.md); the week-by-week plan lives in [`SYLLABUS.md`](./SYLLABUS.md). This file is for anyone — student, contributor, future maintainer, peer institution — who wants to understand the choices.

---

## Why 24 weeks, not 12

The prior C7 stub was a 10-week Arduino-centric introduction. That format is fine for hobbyist exposure; it is not enough to produce engineers who can be trusted with a connected product. The honest reason was always scope: embedded systems engineering is broad on purpose. A graduating C7 student must hold four distinct competencies in their head at once:

1. **Low-level systems competence** — linker scripts, startup, registers, ISRs, DMA.
2. **Real-time competence** — RTOS scheduling, latency budgets, priority inversion.
3. **Distributed-systems competence** — fleets, brokers, TLS, OTA, certificates.
4. **Hardware literacy** — schematics, layout, signal integrity, EMC briefing.

A 12-week course can teach competency 1 plus a taste of competency 2. A 24-week semester (~864 hours full-time, or ~430 hours self-paced) lets us reach all four with room for a single substantial capstone. Embedded engineering does not reward speed-running; the bench artifacts (Saleae traces, scope shots, SWO captures) take real time to learn to read.

Compressing further would force one of three sacrifices we are not willing to make: dropping bare-metal bring-up, dropping fleet-scale operations, or dropping the capstone. None are negotiable.

---

## Why this redesign was necessary

The Spring 2025 stub (preserved under `SPRING-2025/` for historical reference) was a workshop sequence aimed at first-year undergraduates with no programming background. It used Arduino, breadboard kits, and a "build a blinking LED" capstone. That sequence is still useful — for a club intro, an outreach week, or a high-school feeder program — but it is **not the Crunch Labs C7 track** we are now committing to.

Specifically, the prior version:

- Did not cover any RTOS (FreeRTOS or Zephyr).
- Did not cover wireless protocols beyond a token Wi-Fi demo.
- Did not cover bootloaders, OTA, or any production firmware lifecycle.
- Did not cover any Rust, modern C++, or MicroPython — Arduino-flavored C++ only.
- Did not produce any hireable artifact for a senior firmware loop.
- Did not differentiate from a robotics track (which we now own separately, in C24).

The redesigned C7 is consistent with the C18+ Crunch Labs tier governance: a production-engineering depth, an open-source-first stance, a single substantial capstone, a published charter, and an on-call drill. The retroactive promotion of C7 to Crunch Labs tier is intentional.

---

## Topic ordering: why this order and not another

The four phases are not arbitrary. The dependency graph is:

```text
Foundations  →  Bare-Metal  →  RTOS  →  IoT Fleet  →  Edge ML  →  Production
   (1-6)        (3, 7-8)     (9-12)    (13-15)       (16-17)     (18-24)
```

A few choices deserve their own explanation:

### Why bare-metal before RTOS

If you learn FreeRTOS first, every bug looks like a scheduler bug. If you learn registers first, you can diagnose an RTOS problem because you understand the layer underneath it. Real production firmware almost always requires you to drop into the bare-metal layer at least once — to debug a peripheral, write a custom driver, or audit a vendor HAL. C7 makes you comfortable there before adding any abstraction.

We push the linker script + startup file lab into Week 3 deliberately. It is the single best inoculation against magical thinking about firmware.

### Why RTOS before IoT

A connected device that does not yet have a sane task model will not survive its first burst of network traffic. The classic failure is "Wi-Fi works on the bench, falls over in the field" — almost always a priority / queue / blocking-call problem, not a radio problem. Weeks 9–12 build the muscle to design a task graph that can absorb network latency without dropping sensor data.

### Why edge ML last (and small)

TinyML is the topic most likely to be over-marketed. We place it at Week 17, after the student has internalized RAM and flash budgets, after they have profiled CPU load on a logic analyzer, and after they understand power. A keyword-spotter is a perfect first model because the constraints are honest and the failure modes are visible. We use TensorFlow Lite Micro + CMSIS-NN as the open path; Edge Impulse is shown as a useful aid but never as the only path.

### Why hardware (KiCad) in Phase IV

Most engineers will read more schematics than they design. We teach schematic reading throughout the course (every datasheet read-along includes a reference-design page) and concentrate KiCad layout in Week 21, after the student knows what the firmware needs from the board. Designing a board before you know what its software demands is how bad PCBs get made.

### Why Linux/Yocto near the end

The MCU-to-MPU transition is a graduation moment. We delay it deliberately so that a student does not retreat into "I will just put Linux on it" before they have built the bare-metal intuition that lets them know **when** Linux is the right answer (and when it is the lazy answer).

---

## Open-source-first, vendor-aware

C7 commits to an open-source-first stance, applied unevenly because the embedded ecosystem is unevenly open.

| Domain | Open-source primary | Vendor secondary | Rationale |
| --- | --- | --- | --- |
| RTOS | **Zephyr**, FreeRTOS | ThreadX, ChibiOS | Apache-2.0 / MIT. Vendor-neutral. Strong BSP coverage. |
| Toolchain | **GCC + Clang + GDB + OpenOCD** | Vendor IDEs (Cube, MCUXpresso) | We will not lock students into an IDE they cannot reproduce on CI. |
| Build | **CMake + west + PlatformIO** | Vendor make systems | Reproducible, scriptable, ports across vendors. |
| Wireless | **MQTT (Mosquitto), CoAP, Matter** | AWS IoT Core, GCP IoT Core, Azure IoT Hub | We teach the protocols; cloud is a shown not lock-in. AWS IoT Core appears as an example, never as the only path. |
| BLE / Thread | **Zephyr BLE, OpenThread, connectedhomeip** | Nordic SoftDevice, Silabs stacks | The open stacks are now production-grade. |
| Edge ML | **TFLite Micro + CMSIS-NN** | Edge Impulse (aid only), vendor NN SDKs | Open path always available; Edge Impulse shown as a productivity tool but never required. |
| Debugging | **OpenOCD, probe-rs, GDB, SystemView** | J-Link tools (vendor) | J-Link EDU is permitted; we teach the open path first. |
| EDA | **KiCad 8** | Altium, Eagle | KiCad is now the right call for a 2-layer board, and our students should own their files. |
| Compliance | (briefing only) | Accredited test labs | We brief students on the shape of the work; certification is a paid lab engagement. |

The point is not purity. The point is **portability of the student's own work**. A C7 graduate who took a job at a shop that uses, say, ThreadX, can ramp up to it in a week. A graduate who only knows ThreadX would not be able to do the reverse without retraining.

---

## Why these specific tools

A short defense of choices a reviewer might question:

- **STM32F446 as the bring-up target.** Mature, well-documented, ARM Cortex-M4F, available everywhere, supported by Zephyr and FreeRTOS, debuggable with both ST-Link and J-Link. The reference manual is the right level of intimidating.
- **ESP32-S3 for Wi-Fi / BLE work.** Free vendor SDK (ESP-IDF) under Apache-2.0; first-class FreeRTOS and Zephyr support; current, widely deployed in industry, cheap.
- **nRF52840 for BLE Mesh / Thread / Matter.** Industry-standard radio; OpenThread and `connectedhomeip` upstream support; first-class with Zephyr.
- **RP2040 (Pico) for MicroPython and Rust.** Friendly to two of our four supported languages; cheap; great for the "let me prototype this in two hours" use case.
- **AVR (ATmega328P).** Included not because we recommend it for new products, but because reading 8-bit AVR assembly is the cleanest way to build intuition about machine code on a tiny target.
- **J-Link EDU Mini** as the debug probe. Honest pricing (~$20), works with OpenOCD via J-Link's GDB server, and lets us teach SWO/ITM properly. Black Magic Probe is an accepted alternative.
- **Saleae Logic 8.** Yes, it is commercial hardware. Its API is open, the captures are portable, and the protocol decoders are best-in-class. A `sigrok`-based alternative is accepted but loses some quality of life. Cohorts share loaners.
- **KiCad 8.** Open, capable, increasingly the industry default for small and medium shops. Altium is overkill for what we ask the student to design.
- **Mosquitto** as the dev broker. Lightweight, scriptable, free, runs in a container.

---

## How C7 relates to C24 (Robotics) and C18/C19 (Cloud Edge)

Crunch Labs has three adjacent tracks that share some surface area with C7. Clear boundaries:

| Track | Owns | Does not own |
| --- | --- | --- |
| **C7 — Crunch Wire (this)** | Single-device firmware; IoT fleets; edge ML on MCU; OTA; secure boot; fleet ops | Multi-actuator autonomy; ROS2; SLAM; large-scale cloud K8s |
| **C24 — Robotics** | ROS2; motion planning; SLAM; multi-sensor autonomous robots | MCU firmware lifecycle; IoT fleets; OTA at fleet scale |
| **C18 / C19 — Cloud / Edge Platform** | K8s on edge gateways; observability; telemetry pipelines; cloud-side fleet provisioning | MCU firmware; bare-metal bring-up; RTOS internals; radio compliance |

There is intentional overlap at the seams:

- **C7 ↔ C24** overlap on motor control basics (brushed DC, servos), sensor fusion (IMU + Madgwick), and BLE / Thread. C7 stays on the device; C24 stays on the robot.
- **C7 ↔ C18/C19** overlap on MQTT and fleet provisioning. C7 stays on the firmware side of the wire; C18/C19 owns the backend.

A capstone team **can** cross tracks, but each student's individual deliverables must be inside their own track's scope.

---

## Signature line

This charter is committed to by the Code Crunch Club — Crunch Labs working group as the design contract for C7 · Crunch Wire. Changes that affect more than wording (week count, capstone shape, language coverage, open-source posture) require a charter revision and a PR review, not a silent edit.

C7 is licensed under **GPL-3.0**. See [`LICENSE`](./LICENSE). Fork, teach, remix; PR improvements back to <https://github.com/CODE-CRUNCH-CLUB>.

— Code Crunch Club, Crunch Labs working group, 2026.
