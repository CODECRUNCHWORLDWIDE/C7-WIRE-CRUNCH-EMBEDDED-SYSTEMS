# Week 5 — Modern C++ on Bare Metal

> *C++ on an MCU is a contract: you use the parts that compile to register writes, and you do not touch the parts that allocate, throw, or carry runtime type information. Get the subset right and a `Gpio<18>` set call becomes a single `str` instruction.*

Welcome to Week 5 of C7. Last week you wrote three drivers in C — an SSD1306 over SPI, a BMP280 over I²C, an `mpu6050` against your hand-rolled `bus_t` abstraction — and you committed three Saleae captures plus a one-page `FAULT-MODEL.md`. Your protocol-layer code did not import a single peripheral header; you could swap I²C for SPI in `main.c` and the BMP280 driver did not notice. That is the discipline. This week we keep the discipline and change the language.

C++ on an MCU is misunderstood from two directions. From one side: "C++ is too heavy for embedded — RTTI, exceptions, `new`, dynamic dispatch, the STL pulls in 80 KB of bloat." From the other side: "C++ is just C with classes, sprinkle some `struct` keywords and you are done." Both are wrong. The truth is in the middle and is precise: there is a *subset* of C++20 that compiles to the same machine code as hand-written register writes — and there is a *complement* of that subset that you do not link in, on penalty of a 60 KB `.elf` with no exception handler reachable. The job this week is to learn the line between the two and walk it with your eyes open.

By Sunday you will have re-implemented your Week 4 SPI/I²C drivers in C++20 — using `constexpr` peripheral addresses, `concepts` to constrain template parameters, and zero-cost CRTP for the device-driver hierarchy — and you will have proven, by reading `arm-none-eabi-objdump -d` output side by side with the C version, that your C++ code generates the same instructions. The `.elf` size will be within a few hundred bytes of the C build. The Saleae captures will be byte-for-byte identical. And you will know, from disassembly, exactly which C++ features you used and exactly which you did not.

---

## Learning objectives

By the end of this week, you will be able to:

- **Name** the C++20 features that are safe on a freestanding Cortex-M target: `constexpr`, `consteval`, `if constexpr`, `concepts`, `requires`, `std::span`, `std::array`, `std::bit_cast`, `std::endian`, `[[nodiscard]]`, `[[gnu::always_inline]]`, `enum class`, scoped enumerations, `using` type aliases, `inline` variables, CRTP (Curiously Recurring Template Pattern), policy-based templates, and trivial destructors. Cite which features are *strictly* zero runtime cost and which carry a hidden cost (e.g. `std::function` carries a heap allocation in libstdc++ unless `-fno-rtti -fno-exceptions` and a custom allocator are in play).
- **Name** the C++ features you must disable on a freestanding Cortex-M target and the compiler flag that disables each: `-fno-exceptions` (kills `throw`, `try`, `catch`, and the `__cxa_throw` machinery — about 40 KB of unwinder), `-fno-rtti` (kills `typeid`, `dynamic_cast`, and the per-class type-info tables — about 1–2 KB per polymorphic class), `-fno-threadsafe-statics` (kills the `__cxa_guard_acquire`/`__cxa_guard_release` pair around `static` locals — about 200 bytes plus a libc dependency), `-fno-use-cxa-atexit` (kills the registration of destructors for globals — pulls in `__cxa_atexit` and a 64-slot fixed table by default), `-ffreestanding` (declares "no hosted libc, no `main` magic"), and `-nostdlib`/`-nostartfiles` (do not link the host startup). Cite the resulting binary-size delta on the RP2040 toolchain (`arm-none-eabi-gcc 13.2`, `-Os`): about 50–80 KB saved on a non-trivial program.
- **Author** a `constexpr` peripheral-address table that produces a single immediate-load instruction at every register access, with no runtime dispatch. Show that `Gpio<18>::set()` and `*(volatile uint32_t *)0xd0000014 = (1u << 18)` produce identical disassembly, instruction for instruction, on `-O2` and on `-Os`. Cite ARMv6-M ARM (DDI 0419D) §A6.7.137 (`STR (immediate)`) for the instruction encoding.
- **Use** C++20 concepts to constrain a template parameter to "a type that satisfies the SPI device protocol" — `requires(T t, std::span<const std::uint8_t> tx) { { t.write(tx) } -> std::same_as<int>; }` — and have the compiler reject a misuse at the call site with a one-line error instead of a 200-line template-instantiation stack. Cite cppreference's `requires` clause and ISO/IEC 14882:2020 §13.5.
- **Use** CRTP (the Curiously Recurring Template Pattern) to give a peripheral base class a non-virtual `set()` method that dispatches to the derived class's `do_set()` via static, zero-cost polymorphism. Prove with `objdump` that the dispatch is inlined to a single direct write — no v-table, no indirect branch. Cite "Modern C++ Design" (Alexandrescu, 2001) chapter 1 and the LLVM blog post "Polymorphism Without Virtual Functions" (2022).
- **Author** a `Pin<Port, N>` template that compiles to a single `str` of `(1u << N)` to the `GPIO_SET` register. Verify under `-O2` and `-Os` that the template produces the same code as the C macro-driven version from Week 3. Diff the `.elf` sizes — they should be within 100 bytes.
- **Recognize** when C is still the right answer: a non-templated UART driver that ships as a flat `.c` to a Yocto build, a vendor BSP whose header includes `extern "C"` and a thousand C identifiers that the C++ name mangler would garble, a kernel module that links against the Linux kernel build (which is C-only by policy), and a MISRA-C-certified codebase where switching to C++ requires re-certification against AUTOSAR C++14 or MISRA C++ 2023 (both are stricter than MISRA-C and require new tooling). Cite AUTOSAR C++14 Coding Guidelines, MISRA C++ 2023, and the Linux kernel `Documentation/process/programming-language.rst`.
- **Author** a `SpiDevice<Bus, ChipSelect, Mode>` C++ template that brings up the Pi Pico W's SPI0 peripheral, drives an SSD1306 OLED, and produces a `.elf` whose `.text` section is within 256 bytes of the equivalent C build from Week 4. Cite RP2040 datasheet §4.4.4 (SPI registers, pp. 526–540, Sep-2024 rev).
- **Author** the same driver for an I²C device, using `concepts` to constrain the bus type and `constexpr` to compute the `HCNT`/`LCNT` timing constants at compile time. Show that the `HCNT` arithmetic — which involves an integer division and a constraint check against UM10204's minimum low time — produces *zero* runtime code: the compiler computes the result during compilation and embeds it as an immediate. Cite cppreference `consteval` and UM10204 Table 10 (p. 38).
- **Benchmark** the C version against the C++ version for the SPI driver: identical `.text` size (within 1%), identical instruction count on the hot path, identical Saleae capture. If the C++ is even 1% larger, find the feature you accidentally pulled in (most often: a `static` local with thread-safe init, or a non-trivial destructor). Re-flatten and re-measure.

---

## Prerequisites

You have shipped the Week 4 mini-project. Your Pi Pico W reads a BMP280, an MPU-6050, and a second SSD1306 over I²C0, drives a primary SSD1306 over SPI0, all built on top of your `bus_t` abstraction, with three Saleae captures and a `FAULT-MODEL.md` on GitHub. If not, finish Week 4 — this week's mini-project is a direct C++20 reimplementation of that artifact, and you will be diffing the two side by side.

You have `arm-none-eabi-g++` 13.2 or newer installed (the same toolchain that shipped `arm-none-eabi-gcc`). C++20 mode requires GCC 11+; we use C++20 throughout. Verify with `arm-none-eabi-g++ --version` and `arm-none-eabi-g++ -std=c++20 -x c++ -E - < /dev/null | grep __cplusplus` — must print `202002L` or higher.

You have the C++20 standard library headers available for the freestanding subset: `<cstdint>`, `<cstddef>`, `<array>`, `<span>`, `<concepts>`, `<type_traits>`, `<bit>`, `<utility>`. The full STL (`<vector>`, `<string>`, `<iostream>`, `<map>`) is *not* needed and *should not* be included — they pull in allocators, exceptions, and locale machinery that we have disabled. If your build accidentally pulls them in, the linker will tell you with an `undefined reference to '__cxa_throw'`.

You have the Embedded Template Library (`etl`, Aster Consulting Ltd, MIT licensed) cloned: <https://github.com/ETLCPP/etl>. We use `etl::array`, `etl::span`, `etl::circular_buffer`, and `etl::optional` as drop-in replacements for `std::` types where the freestanding `std::` versions are missing or incomplete. ETL is header-only and trivially integrated by adding its include directory to the compile flags.

You have read AUTOSAR C++14 Guidelines (the automotive coding standard for C++ on safety-critical embedded; ~500 pages, the rules that matter for us are in §6 and §A) at least skimmed. You do not need to follow every rule — many are conservative for ASIL-D and overstrict for our purposes — but you need to know the *shape* of the rules: "no `goto`", "no `union`", "no `dynamic_cast`", "every `class` has a virtual destructor or is `final`". The week's C++ style is AUTOSAR-aware but not AUTOSAR-strict.

You have read MISRA C++ 2023 (the most recent revision; ~250 pages) at least the executive summary. MISRA C++ 2023 supersedes MISRA C++ 2008 and aligns with C++17/20. It is the standard your firmware will be measured against in any automotive, medical, or aerospace job interview from 2024 onwards.

You have a logic analyzer (Saleae, Kingst LA1010, DSLogic Plus) and the Saleae captures from Week 4 — you will reuse them as the *reference* against which your C++ version must produce identical traces.

---

## Topics covered

- **The C++20 freestanding subset.** ISO/IEC 14882:2020 §16.4.2.5 enumerates the "freestanding implementation" subset of the standard library — the headers and types that an implementation may provide even without an OS underneath. The set is small: `<cstddef>`, `<cstdint>`, `<climits>`, `<limits>`, `<type_traits>`, `<concepts>`, `<bit>`, `<atomic>`, `<utility>`, `<initializer_list>`, plus the language-only headers like `<typeinfo>` (RTTI-disabled). Notably *missing*: `<iostream>`, `<vector>`, `<string>`, `<thread>`, `<memory>` (most of it), `<chrono>` (most of it). When you include something not in the freestanding subset, the linker error is your indicator.
- **`constexpr` and `consteval` for register addresses.** A C macro like `#define GPIO_OUT_SET (*(volatile uint32_t *)0xd0000014)` is a textual substitution: it has no type, no namespace, and can be redefined silently by another header. A C++ `constexpr` equivalent — `constexpr std::uintptr_t GPIO_OUT_SET = 0xd0000014;` — has a type, a name, a scope, and can be used in `static_assert` expressions. With `consteval` we go further: a function marked `consteval` *must* be evaluated at compile time, or the build fails. We use this for the I²C `HCNT`/`LCNT` arithmetic — if the requested SCL frequency cannot be expressed with valid divider values, the build fails with a one-line error at the call site.
- **Concepts as documented preconditions.** A C function pointer `int (*write)(bus_t *self, const uint8_t *tx, size_t tx_len)` carries no information about what the callback contract is — the caller must read the comment, the callee must remember it. A C++20 `concept` makes the contract a compiler-checked predicate: `template<typename T> concept BusWrite = requires(T &t, std::span<const std::uint8_t> tx) { { t.write(tx) } -> std::convertible_to<int>; };`. A function that takes `BusWrite auto &b` rejects, at compile time and with a one-line diagnostic, a type that does not implement `write` with the right signature. Concepts replace the SFINAE incantations of pre-C++20 and the runtime virtual dispatch of C-with-classes.
- **CRTP — Curiously Recurring Template Pattern.** The shape: a base class `Peripheral<Derived>` is templated on its derived class, and a method of the base calls `static_cast<Derived *>(this)->do_thing()` to dispatch. Because the type is known at compile time, the call is inlined; there is no v-table, no virtual function pointer in the object, no indirect branch. This is "static polymorphism" — the API surface looks like inheritance, but the binary is the same as a flat function call. We use it for the `Gpio<Pin>` hierarchy and for the `SpiDevice<Bus, CS, Mode>` driver.
- **Why no exceptions.** Exception handling in GCC/clang on ARM Cortex-M pulls in `libsupc++` and the `__cxa_personality_v0` / `__cxa_throw` / `__cxa_allocate_exception` / `__cxa_begin_catch` chain — about 30–50 KB of unwinder tables in the `.eh_frame` section, plus runtime machinery that walks the stack on a throw. On a 256 KB flash MCU this is a non-starter. The replacement is *return-value error reporting* (as in Week 4 with our `BUS_ERR_*` enum), wrapped in `std::optional<T>`, `etl::expected<T, E>`, or `tl::expected<T, E>` for a richer "success-or-error" type that the optimizer can flatten to a struct-by-value return. Cite "Why exceptions are not used in embedded" (Lawrence Crowl, ISO/IEC JTC1/SC22/WG21 N4234, 2014).
- **Why no RTTI.** RTTI emits per-class `type_info` tables (mangled type name, parent-class chain) into the binary — 100–500 bytes per polymorphic class, multiplied by every class with a virtual function. `dynamic_cast<T*>(ptr)` is a runtime tree walk through the inheritance hierarchy. On freestanding, we disable both with `-fno-rtti` and replace any need for downcasts with `std::variant` (zero-runtime-cost, but requires `std::visit`) or with explicit type tags carried in a `struct`. Cite "Optimizing for size on Cortex-M0+", Memfault (2021).
- **Why no `new` (the global one).** `operator new` in the standard library calls `malloc`, which on a freestanding target either does not exist (linker error) or calls into a 4 KB `sbrk` shim with a heap segment in your linker script. Neither is acceptable on a real-time MCU: `malloc` is not bounded in worst-case time, can fragment, and offers no failure recovery beyond returning `nullptr` (or throwing `std::bad_alloc`, which we have disabled). The replacement: stack allocation, `etl::pool<T, N>`, or placement-new into a `std::array<std::byte, N>` arena. Cite AUTOSAR C++14 Rule A18-5-1 (no dynamic memory) and CERT C++ MEM52-CPP (detect and handle memory allocation errors).
- **CRTP vs virtual functions, in a bench measurement.** A virtual function call on Cortex-M0+ is approximately: `ldr r0, [r0]` (load v-table), `ldr r0, [r0, #N]` (load slot), `blx r0` (indirect call) — three instructions, plus the branch-target prediction penalty of an indirect branch on M0+ (which has none, so always 2–3 cycles wasted). A CRTP call after inlining is *zero* instructions — the function body is spliced in line. The `objdump` diff in Lecture 2 makes this concrete. For a function called 1000 times per second on a 4 Hz refresh loop, the difference is in the noise. For a function called once per ADC sample at 48 kHz, the difference is measurable.
- **The "when does C still win" check.** Three classes of problem: (1) **Tooling.** The customer's static analyzer is MISRA-C-licensed and does not parse C++ at all. (2) **Boundary.** You ship a library that other people will link from Rust, Go, Python, Zig — every one of those FFI surfaces wants C calling conventions and C ABI, and you do not want to maintain a `extern "C"` shim across every method. (3) **Certification.** You are inheriting a DO-178C-Level-A codebase written in C; switching to C++ requires re-certification against MISRA C++ 2023 or AUTOSAR C++14 and is a 6–18 month effort with seven-figure tooling licenses. C wins by default in all three. We cover this honestly in Lecture 3.
- **The C++ subset, table form.**

| Feature | Verdict | Cost |
|---------|---------|------|
| `constexpr` / `consteval` variables and functions | YES | 0 bytes runtime |
| `concepts` and `requires` clauses | YES | 0 bytes runtime |
| `std::array<T, N>` (stack-allocated fixed-size array) | YES | 0 bytes runtime |
| `std::span<T>` (non-owning view over an array) | YES | 0 bytes runtime |
| `std::optional<T>` (T + bool, returned by value) | YES | sizeof(T) + 1 byte |
| `std::bit_cast<T>(src)` (type-punning, defined behavior) | YES | 0 bytes runtime |
| Templates, including CRTP | YES | 0 bytes runtime |
| `enum class` and scoped enumerations | YES | 0 bytes runtime |
| Non-virtual member functions | YES | 0 bytes runtime |
| `inline` variables | YES | depends on use |
| `constinit` (compile-time-only init) | YES | 0 bytes runtime |
| Lambda expressions (non-capturing, or stack-captured) | YES | 0 bytes runtime |
| `noexcept` | YES | 0 bytes runtime (annotation) |
| Exceptions (`throw`/`try`/`catch`) | NO | 30–50 KB |
| RTTI (`typeid`, `dynamic_cast`) | NO | 1–2 KB per class |
| Global `operator new` / `delete` | NO | pulls heap |
| `std::function` (type-erased callable) | NO | heap unless customized |
| `std::string` | NO | heap |
| `std::vector` | NO | heap |
| `std::iostream` | NO | locale + heap + buffering |
| `std::thread` | NO | pthread |
| `std::shared_ptr` | NO | atomic refcount + heap |
| `std::unique_ptr` with default deleter | OK (but no `new`, so use placement-new + custom deleter) | depends |
| Virtual member functions | DISCOURAGED | v-table per class, indirect call |
| Multiple inheritance | DISCOURAGED | thunks, this-adjustments |
| `friend` declarations | OK | 0 bytes runtime (annotation) |

- **The week's decision rule:** **if it compiles to a runtime-allocating or runtime-dispatching artifact, do not include it. If it compiles to immediate-load and store instructions, use it.** The `objdump` is the arbiter. When in doubt, disassemble and look.

---

## Weekly schedule

| Day       | Focus                                                | Lectures | Exercises | Challenges | Quiz/Read | Homework | Mini-Project | Bench/Self-Study | Daily Total |
|-----------|------------------------------------------------------|---------:|----------:|-----------:|----------:|---------:|-------------:|-----------------:|------------:|
| Monday    | The C++20 freestanding subset; constexpr/concepts    |   2h     |   1h      |    0h      |   0.5h    |   1h     |     0h       |       0.5h       |     5h      |
| Tuesday   | Zero-cost abstractions; CRTP; objdump-driven design  |   2h     |   2h      |    0h      |   0.5h    |   1h     |     0h       |       0.5h       |     6h      |
| Wednesday | When C still wins: tooling, boundary, certification  |   2h     |   2h      |    0.5h    |   0.5h    |   1h     |     1h       |       0.5h       |     7.5h    |
| Thursday  | Mixed C/C++ in one build; `extern "C"`; ABI rules    |   1h     |   2h      |    0.5h    |   0.5h    |   1h     |     1h       |       0.5h       |     6.5h    |
| Friday    | Disassembly studio; AUTOSAR/MISRA C++ rule walk      |   0h     |   0h      |    2h      |   0.5h    |   1h     |     1h       |       0.5h       |     5h      |
| Saturday  | Mini-project deep work; benchmark vs the C version   |   0h     |   0h      |    0h      |   0h      |   1h     |     3h       |       0.5h       |     4.5h    |
| Sunday    | Quiz; review; polish the size diff and the artifact  |   0h     |   0h      |    0h      |   0.5h    |   0h     |     0h       |       0h         |     0.5h    |
| **Total** |                                                      | **7h**   | **7h**    |  **3h**    |  **3h**   |  **6h**  |   **6h**     |     **3h**       |   **35h**   |

Self-paced cohorts compress to ~12 h/week. The load-bearing items are Lecture 1 (the subset), Lecture 2 (zero-cost abstractions and the CRTP disassembly), Exercise 3 (the mixed C/C++ build with both translation units), and the mini-project (the SPI/I²C driver rewrite with the benchmark). Skip Challenge 1 (UART driver rewrite) only if your Week 4 UART artifact is already on GitHub; otherwise the challenge is the only way you produce a working UART artifact under the C++ discipline this week.

---

## How to navigate this week

| File                                                                                                       | What's inside                                                                  |
|------------------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------|
| [README.md](./README.md)                                                                                   | This overview                                                                  |
| [resources.md](./resources.md)                                                                             | C++20 standard sections, ETL, AUTOSAR C++14, MISRA C++ 2023, GCC freestanding docs |
| [lecture-notes/01-cpp20-embedded-subset.md](./lecture-notes/01-cpp20-embedded-subset.md)                   | The freestanding subset; constexpr; concepts; no exceptions; no RTTI; build flags |
| [lecture-notes/02-zero-cost-abstractions.md](./lecture-notes/02-zero-cost-abstractions.md)                 | CRTP; templates; `Gpio<Pin>`; objdump-driven design; the "prove it's zero-cost" discipline |
| [lecture-notes/03-when-c-still-wins.md](./lecture-notes/03-when-c-still-wins.md)                           | Tooling, boundary, certification; when to pick C; AUTOSAR vs MISRA-C; the Linux kernel rule |
| [exercises/README.md](./exercises/README.md)                                                               | Index of exercises                                                             |
| [exercises/exercise-01-gpio-template-vs-c-macro.md](./exercises/exercise-01-gpio-template-vs-c-macro.md)   | Build a `Gpio<Pin>` template and diff the disassembly against the C macro version |
| [exercises/exercise-02-cpp-spi-driver-with-concepts.md](./exercises/exercise-02-cpp-spi-driver-with-concepts.md) | Re-implement Week 4's SPI driver in C++20 with `concepts` constraining the device type |
| [exercises/exercise-03-mixed-c-and-cpp-translation-units.md](./exercises/exercise-03-mixed-c-and-cpp-translation-units.md) | Build a single `.elf` where the SPI transport is C++, the BMP280 driver is C, and they call each other across `extern "C"` |
| [challenges/README.md](./challenges/README.md)                                                             | Index of challenges                                                            |
| [challenges/challenge-01-cpp-rewrite-of-uart-driver.md](./challenges/challenge-01-cpp-rewrite-of-uart-driver.md) | Re-implement Week 3's UART driver in C++20; produce the same Saleae trace; diff the `.elf` |
| [quiz.md](./quiz.md)                                                                                       | 10 questions; subset-rule + disassembly + AUTOSAR/MISRA grade                  |
| [homework.md](./homework.md)                                                                               | Six practice problems                                                          |
| [mini-project/README.md](./mini-project/README.md)                                                         | Re-implement the SPI/I²C driver from Week 4 in C++20; benchmark `.text` size and Saleae traces vs the C version |

---

## The Week 5 deliverable, in one line

By Sunday 23:59 local time you produce a single artifact: a public GitHub repo containing a Pi Pico W firmware that re-implements last week's SPI/I²C driver stack in C++20 (using `constexpr` peripheral addresses, `concepts` for bus contracts, CRTP for the device base class), produces a `.elf` whose `.text` section is within 1% of the C version's, generates Saleae captures that are byte-for-byte identical to last week's, and ships with a `BENCHMARK.md` documenting the comparison. The repo includes a `Makefile` that builds both the C and the C++ versions side by side, an `objdump-diff.sh` script that runs `objdump -d` on both and reports any divergence, and a `FAULT-MODEL.md` updated to flag the three C++-specific failure modes (a non-`constexpr` global with a constructor pulling in `__cxa_atexit`, an accidentally-virtual destructor pulling in a v-table, an `std::optional<int>` returned where an `int` would have sufficed, costing 4 bytes per call site).

Week 6 of the SYLLABUS (MicroPython, and Then Choosing Your Language) builds on this artifact: you will write the *same* BMP280 driver a third time in MicroPython, then in the Lecture 3 framework decide for each subsystem of your capstone whether it lives in C, C++, Rust, or MicroPython — with explicit per-axis criteria and the binary-size data to back each call.

---

## Stretch goals

- Replace `std::optional<int>` returns in the bus interface with `tl::expected<int, BusError>` (a popular header-only library implementing the C++23 `std::expected` proposal). Re-run the benchmark; verify the binary grows by ≤ 200 bytes and the error-path becomes explicit at every call site. <https://github.com/TartanLlama/expected>
- Implement the `Gpio<Pin>` template as a *constrained* template using a C++20 concept `PinNumber`. The concept restricts `Pin` to values in `0..29` (the RP2040's GP pin range). Hand the compiler `Gpio<42>` and verify the error message is one line: `'Pin' does not satisfy the 'PinNumber' concept`, not a 200-line template-instantiation trace.
- Write a `consteval` function `compute_i2c_dividers(uint32_t clk_peri, uint32_t scl_target)` that returns a `struct { uint16_t hcnt, lcnt; }` and *fails the build* (via `static_assert`) if the target SCL frequency is unreachable with `clk_peri`. Verify the build error message is informative — "i2c: scl_target > clk_peri / 4 is impossible at 100 kHz with clk_peri = 1 MHz".
- Read the GCC source for `-fno-exceptions` in `gcc/cp/except.cc`. Identify the function `build_throw` and what it emits when exceptions are disabled (a `__builtin_trap` or a no-op). Write a one-paragraph annotation. The point: the compiler is honest with you about what `throw` becomes when you disable it.
- Take your Exercise 2 C++ SPI driver and add a `[[nodiscard]]` attribute to the bus's `write` return type. Walk through the warning the compiler emits at every call site that ignores the return. Decide which call sites are correct to discard (e.g. a write you do not care about the byte-count of) and which are bugs. Fix the bugs.

---

## Up next

[Week 6 — MicroPython, and Then Choosing Your Language](../week-06/) — once your `Gpio<Pin>` template disassembles to a single `str` instruction, your C++ SPI driver's `.text` is within 1% of the C version, and you can answer "what does `-fno-threadsafe-statics` actually disable, and why does it matter on a single-core MCU?" in one sentence.
