# Lecture 3 — When C still wins

> *Three honest reasons to write embedded code in C in 2026: your tooling does not understand C++; your library crosses an ABI boundary that C++ name-mangling would corrupt; or your certification regime adds 6–18 months of re-work if you switch languages. Outside these cases, modern C++ is a better default.*

## The honest framing

The last two lectures argued that modern C++20 on a freestanding Cortex-M0+ is approximately the same binary size as C, with stricter type checking, better abstraction, and compile-time-validated peripheral configuration. Every example we ran disassembled to the same machine code as the equivalent C. The size delta on a non-trivial firmware was 4–44 bytes — noise.

That argument is complete. It is also incomplete. There are three classes of project where C still wins, decisively, and the calculus is not about elegance or features. It is about toolchains, ABIs, and certification cost. We cover all three in this lecture, with examples.

The point of this lecture is not to talk you out of using C++ — most of you should use C++ for most embedded work going forward. The point is to give you the framework to *decide*, per project, and to defend the decision in front of a senior reviewer.

## Reason 1 — Tooling

Static analysis, code generation, and IDE support for C are deeper, older, and broader than for C++. The toolchain ecosystem you inherit from a customer or vendor matters more than the language you prefer.

### MISRA-C is everywhere; MISRA C++ 2023 is not

MISRA-C 2012 is the dominant C coding standard in automotive, medical, and aerospace embedded. Every vendor's static analyzer (Coverity, LDRA, Klocwork, Polyspace, Helix QAC, cppcheck, PVS-Studio) ships first-class MISRA-C rule support. The compliance reports are well-understood by reviewers and auditors. Customers know how to read them.

MISRA C++ 2023 exists and supersedes MISRA C++ 2008 — but the tooling for it is *younger*. Many static analyzers added MISRA C++ 2023 support in late 2024 or 2025; some still flag it as "beta." Audit trails for MISRA C++ 2023 are not as well understood by certification bodies as those for MISRA-C. When you submit a MISRA-C compliance report to a Tier-1 automotive customer, the reviewer recognizes every rule. When you submit a MISRA C++ 2023 report, the reviewer may need to look up half the rules.

The practical consequence: if your customer's compliance pipeline is built around MISRA-C, switching to C++ requires either re-tooling the pipeline (months) or running both analyses in parallel for the duration of the project (cost). C is the path of least resistance.

### AUTOSAR Classic Platform is C; AUTOSAR Adaptive is C++

AUTOSAR (AUTomotive Open System ARchitecture) is the dominant in-vehicle software architecture. It comes in two flavors:

- **AUTOSAR Classic Platform** (2003–present) — the architecture for traditional ECUs (engine control, body control, brake control). Written in C, with the AUTOSAR C coding guidelines. Every classic-platform ECU on the road today is C.
- **AUTOSAR Adaptive Platform** (2017–present) — the architecture for high-performance ECUs (ADAS, infotainment, the next-generation domain controllers). Written in C++, with the AUTOSAR C++14 Coding Guidelines.

If you are writing code for a classic-platform ECU, the answer is C. The AUTOSAR Classic generator (Vector DaVinci, EB tresos, ETAS RTA-CAR) emits C. The OS (OSEK, AUTOSAR OS) is C. The vendor BSPs are C. C++ does not live in this world.

If you are writing code for an adaptive-platform ECU, the answer is C++. The Adaptive Platform Foundation (`ara::com`, `ara::log`, `ara::diag`) is C++.

The choice is determined by which platform your customer is on, not by your preference.

### The vendor BSP is C

The Pi Pico SDK, ESP-IDF, STM32 HAL/LL, Nordic nRF Connect SDK, NXP MCUXpresso SDK, Renesas e² studio SDK — every major MCU vendor ships their BSP in C. When you call vendor code from C++, you wrap the headers in `extern "C"` and the call works. When you mix C++ types (a `std::span<uint8_t>`) with vendor C APIs (an `uint8_t *` + `size_t`), you adapt at the boundary.

This is mostly a non-issue — the `extern "C"` mechanism is well-defined and the boundary is tractable. But it does mean that "C is the lingua franca of the BSP" is structurally true; if your project lives mostly inside vendor BSP code, you spend most of your time in C anyway, and the C++ on top is a thin veneer.

### IDE auto-complete is sharper on C

Modern IDEs (VSCode + clangd, CLion, Eclipse CDT, Vim + ccls) understand both languages, but C++ template-heavy code can confuse the index. Auto-complete on a `Gpio<18>::set()` call may or may not resolve cleanly depending on the indexer; auto-complete on `gpio_set(18)` always resolves. For a small team or a new hire, the friction of "why does my IDE not know what this template expands to" is real.

This is a *small* point — modern clangd handles C++20 concepts and templates well — but if your team is mixed-experience or you are onboarding interns, the simpler tooling story of C has real value.

## Reason 2 — ABI boundaries

C has *the* portable ABI. Every language that can talk to a C library can talk to your code. C++ has name-mangling, exception ABI, RTTI ABI, and a v-table layout — all of which differ across compilers and standards revisions.

### The "shared library to many languages" case

You are writing an embedded library that will be linked from C, Rust, Go, Python (via `ctypes` or `cffi`), Zig, Lua, and OCaml. Every one of those FFI surfaces wants C calling conventions and a flat C header.

Option A: Write the library in C. Every consumer links directly. The header file is the same one everyone uses.

Option B: Write the library in C++. Wrap every public function in `extern "C"` and a C-style API. Distribute *that* as your interface. Internally, write whatever C++ you like, but the *public surface* is C.

Option B is fine — it is what most of the C++ embedded libraries (FreeRTOS-Plus-TCP, mbedTLS, lvgl) do. But it is *more* work than Option A, not less. Every public function gets a `extern "C"` wrapper, every public type gets a C-compatible mirror, every callback uses C function pointers. If the library is small or the public surface is wide, Option A may be a better fit.

### The Linux kernel boundary

The Linux kernel is C. Specifically, GNU C99 with a few C11 extensions, plus the kernel-specific extensions (kernel-doc, `__init`, `__exit`, `EXPORT_SYMBOL`). Kernel modules are C. Until 2022, kernel modules could *only* be C. Since 2022, the kernel has experimental Rust support — but never C++.

The kernel community has rejected C++ multiple times over the years, citing: exceptions (kernel cannot allocate exception objects), RTTI (kernel has no `__cxa_*` runtime), templates (debugging template-heavy code in kdb is impractical), and ABI stability (C++ ABI is unstable across compilers, kernel ABI must be stable).

The kernel programming-language policy (`Documentation/process/programming-language.rst`) is explicit: "The kernel is written in the C programming language [c-language]." The C language version is GNU C, with `-std=gnu11` plus kernel extensions.

If your job is to write a Linux kernel driver, you write it in C. There is no debate.

### Buildroot and Yocto layer recipes

Most build systems for embedded Linux (Buildroot, Yocto/OpenEmbedded) accept C++ packages, but the canonical *toolkit* — autoconf, automake, the standard `make install` flow — is built around C conventions. If you ship a Buildroot package that is a C library, it slots in. If you ship a C++ library with templates in headers and a `-std=c++20` requirement, you may need to upstream a recipe and shepherd it through the maintainer's review.

This is *operational friction*, not a hard block. But on a tight-deadline embedded Linux project, the friction matters.

## Reason 3 — Certification

Safety-critical embedded systems undergo certification: DO-178C (avionics), ISO 26262 (automotive), IEC 62304 (medical), IEC 61508 (industrial). The certification process measures the codebase against a coding standard, requires traceability from requirements to code, and demands evidence that every line of code was reviewed and tested.

### Re-certifying a codebase is expensive

A DO-178C-Level-A codebase (the highest assurance level, "catastrophic failure category") is certified against a specific set of tools and a specific coding standard. The C version is typically certified against MISRA-C 2012 with a specific static-analysis tool (e.g. LDRA). The certification artifacts — the requirements traceability matrix, the structural coverage report, the tool qualification report — are all built around C.

Switching the codebase to C++ would require:

1. Re-certifying the *tools* — every static analyzer, every compiler, every test framework — against the new language. This is the most expensive part; tool qualification under DO-178C is a six-figure cost per tool, sometimes seven.
2. Re-writing the requirements traceability matrix — every requirement that pointed to a C function now points to a C++ method or template.
3. Re-running structural coverage analysis — MC/DC coverage on templates is a research area, not a solved problem.
4. Re-training the team — DO-178C requires that engineers be trained on the language and the standard. A C-trained team is not a C++-trained team.

A typical estimate from major aerospace primes (Boeing, Airbus, Lockheed, Northrop) is 6–18 months of effort and US$2–5M of tooling and labor to migrate a Level-A codebase from C to C++. Unless you have a compelling reason — new features that genuinely require C++, customer mandate, end-of-life of the C tooling — the answer is "keep it in C."

### ISO 26262 (automotive) is more permissive but still expensive

ISO 26262 (the automotive functional-safety standard) accepts both C and C++ at all ASIL levels (A through D). The coding standard for C is MISRA-C 2012; for C++ it is MISRA C++ 2008, MISRA C++ 2023, or AUTOSAR C++14, depending on the platform. Tool qualification cost is lower than DO-178C but still substantial (US$500K–$1.5M for a full toolchain qualification).

The same calculus applies: a new project picks the language up front based on the platform (Classic = C, Adaptive = C++). An existing project does not migrate without a compelling reason.

### IEC 62304 (medical) accepts both, but the recommendation is C

The IEC 62304 standard for medical-device software lifecycle is less prescriptive than DO-178C or ISO 26262 about language choice. The standard requires a documented software-development plan, a documented coding standard, and traceability from risk analysis to code. Both C and C++ are acceptable.

In practice, medical-device embedded code skews toward C — the tooling is older, the certification staff is more familiar with C, and the conservatism of the field favors known patterns. Greenfield medical projects in 2025 are increasingly written in C++, but legacy is C.

## When C++ wins despite the above

The framing of this lecture is "when C wins." For balance, here are the cases where C++ wins despite the C-favorable factors above:

1. **Type-rich peripheral drivers.** A `Gpio<18>` template with a `static_assert(Pin < 30)` is genuinely safer than `gpio_set(18)`. For a complex MCU with hundreds of pins and dozens of peripheral instances, the compile-time validation saves an entire class of bug. (Lecture 2.)
2. **Generic algorithms with compile-time constants.** The I²C `HCNT`/`LCNT` arithmetic done at compile time, with a `static_assert` for the bus-electrical constraint, is impossible in C without preprocessor hacks. The C++ version is one `consteval` function. (Lecture 1.)
3. **Heterogeneous device fleets.** When you have 10+ device drivers that share a transport, the CRTP pattern lets you template the driver on the transport type and the device address, and the compiler instantiates the right code per device. C does the same thing with a `bus_t` struct of function pointers, but C++ adds compile-time type-checking. (Week 4 vs this week.)
4. **You are starting from scratch and the certification regime is loose or absent.** A hobby project, an internal tool, an early prototype, a non-safety-critical product — C++ is the better default in 2025. The size cost is negligible with the right flags; the type-safety win is real.

## The decision table

For each new firmware project, ask:

| Question                                                | If yes, lean C | If yes, lean C++ |
|---------------------------------------------------------|----------------|------------------|
| Will the code link into the Linux kernel?               | x              |                  |
| Is the target platform AUTOSAR Classic?                  | x              |                  |
| Is the target platform AUTOSAR Adaptive?                 |                | x                |
| Does the codebase need DO-178C Level A/B certification, inheriting a C-certified pipeline? | x | |
| Does the codebase need DO-178C, starting fresh?         |                | x (if tooling allows) |
| Will the library be linked from many other languages via FFI? | x        |                  |
| Will the library be used only inside one C++ application? |              | x                |
| Is your static-analysis pipeline MISRA-C-licensed only?  | x              |                  |
| Are you targeting modern C++ (20/23) with `concepts`/`constexpr` benefits? |  | x       |
| Is the team mixed-skill, with many new C engineers?      | x              |                  |
| Is the team experienced and aware of the C++ subset?     |                | x                |
| Is the target an 8-bit MCU (AVR, PIC8) with < 4 KB RAM?   | x              | (C++ works but adds friction) |
| Is the target a 32-bit MCU (Cortex-M, RISC-V, Xtensa)?    |                | x                |
| Is the target an MPU running Linux (Cortex-A, RPi, BBB)?  | (kernel)       | x (userspace)    |

Tally the boxes. If C wins by 3+, the answer is C. If C++ wins by 3+, the answer is C++. If it is close, the deciding factor is the team's familiarity and the customer's pipeline.

## The honest summary

Modern C++20 on a freestanding 32-bit MCU, with the freestanding flag triad and the subset discipline, is approximately the same size and speed as C with better type safety. For greenfield projects on 32-bit MCUs without strict certification regimes, C++ is the better default in 2025.

For projects with: Linux kernel boundaries, AUTOSAR Classic, DO-178C-certified C inheritance, MISRA-C-licensed-only tooling, 8-bit MCUs, or a pipeline that links the code from many other languages via FFI — C is still the right answer.

The decision is per-project, not per-engineer. A good embedded engineer can be productive in both. The discipline is to pick the right one for the project and defend the choice in code review with reference to *this* table or one like it, not to an aesthetic preference.

In Week 6 you will be asked to make this choice — for the BMP280 driver, for the UART driver, for the wireless stack — as part of the language-choice exercise. By Sunday of Week 5 you have written the BMP280 driver twice (in C, in C++) and you have data on the binary-size delta. By Sunday of Week 6 you will have decided which one to keep and why.

## What you take away

- C wins decisively when your tooling pipeline is MISRA-C/AUTOSAR Classic, when your code links into the Linux kernel, when your library has many non-C FFI consumers, or when your DO-178C/ISO 26262 certification regime would re-cost millions to migrate.
- C++ wins as the modern default on 32-bit MCUs without strict certification, when you have type-rich peripheral driver work, when you want compile-time-validated peripheral configuration, and when you can adopt the freestanding subset discipline (no exceptions, no RTTI, no heap).
- The decision is per-project, not per-engineer. Read the table; tally the boxes; defend the call.
- Most embedded code shipped in 2025 is still C. Most *new* greenfield embedded code on 32-bit MCUs in 2025 is increasingly C++. The migration is gradual; do not be the engineer who forces a switch without business justification.
- In Week 6 you will use this framework to pick a language *per subsystem* of your capstone. The framework is not academic; it is the working tool you will use for the rest of the course and your career.

## References

- Linux kernel programming-language policy:
  <https://docs.kernel.org/process/programming-language.html>
- AUTOSAR Classic Platform (introduction):
  <https://www.autosar.org/standards/classic-platform/>
- AUTOSAR Adaptive Platform (introduction):
  <https://www.autosar.org/standards/adaptive-platform/>
- AUTOSAR C++14 Coding Guidelines (registration walled):
  <https://www.autosar.org/standards/adaptive-platform/>
- MISRA-C 2012 (paid; executive summary free):
  <https://misra.org.uk/product/misra-c2012-third-edition-first-revision/>
- MISRA C++ 2023:
  <https://misra.org.uk/product/misra-cpp2023/>
- DO-178C, "Software Considerations in Airborne Systems and Equipment Certification," RTCA (paid):
  <https://www.rtca.org/>
- ISO 26262 — Road vehicles — Functional safety (paid):
  <https://www.iso.org/standard/68383.html>
- IEC 62304 — Medical device software — Software life cycle processes (paid):
  <https://www.iec.ch/>
- "The Embedded Muse," Jack Ganssle, issue 411 — "C vs C++ for embedded":
  <http://www.ganssle.com/tem.htm>
- Bjarne Stroustrup, "Why C++ is not 'just C with classes'" (1994):
  <https://www.stroustrup.com/whyC.pdf>
- "Why isn't the Linux kernel written in C++?" — kernel mailing list archive, the canonical answer:
  <https://lkml.org/lkml/2004/1/20/20>
