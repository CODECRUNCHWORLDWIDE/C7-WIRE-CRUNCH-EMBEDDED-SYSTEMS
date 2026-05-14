# Week 5 — Resources

Every reference here is **free** and **publicly accessible** unless marked otherwise (AUTOSAR C++14 and MISRA C++ 2023 require registration; we cite which clauses matter so you can find them without buying the spec). Page numbers cite the document revision noted; later revisions move tables but not concepts.

## Primary language references

- **ISO/IEC 14882:2020 — C++ Programming Language (C++20)** (the standard itself, ~1800 pages, not freely downloadable — the *final committee draft* N4860, Oct-2020, is the closest legally-public approximation). This week:
  - §16.4.2.5 — Freestanding implementation. Defines the subset of the standard library that a freestanding implementation must provide. Pages 528–530.
  - §7.7 — Constant expressions. Defines `constexpr` evaluation, `consteval` (immediate functions), and the `if constexpr` discriminator.
  - §13.5 — Concepts and constraints. The `requires` clause, `concept` definitions, and atomic constraint resolution.
  - §11.4.6 — Inheritance and virtual functions. The C++ object model. Relevant to the *no-virtuals* discipline.
  - §18 — Exception handling. The half we do not link in.
  <https://github.com/cplusplus/draft/releases/tag/n4860> (CC BY-SA 4.0 draft)
- **cppreference.com** — the de facto C++ language reference. Search-friendly, fully cross-referenced, and free. This week:
  - "Freestanding implementations": <https://en.cppreference.com/w/cpp/freestanding>
  - "Concepts library": <https://en.cppreference.com/w/cpp/concepts>
  - "constexpr specifier": <https://en.cppreference.com/w/cpp/language/constexpr>
  - "consteval specifier": <https://en.cppreference.com/w/cpp/language/consteval>
  - "std::span": <https://en.cppreference.com/w/cpp/container/span>
  - "std::bit_cast": <https://en.cppreference.com/w/cpp/numeric/bit_cast>
- **GCC Manual** (Free Software Foundation, GCC 13 release, ~700 pages). This week:
  - §3.4 — Code Generation Options. Specifically `-fno-exceptions`, `-fno-rtti`, `-fno-threadsafe-statics`, `-fno-use-cxa-atexit`. Read the one-paragraph description of each.
  - §3.5 — Optimization Options. `-Os`, `-O2`, `-flto`, `-fdata-sections`, `-ffunction-sections`, `-Wl,--gc-sections` — the size-flattening flags you use this week.
  - §3.18 — Options for the C++ Dialect. C++20 (`-std=c++20`), the language extensions (`-fconcepts`, `-fcoroutines`).
  - §6 — Extensions to the C++ Language. `[[gnu::always_inline]]`, `[[gnu::section]]`, `[[gnu::naked]]` — the extensions we use for placing variables in specific sections (vector table, boot2) and forcing the inliner.
  <https://gcc.gnu.org/onlinedocs/gcc-13.2.0/gcc.pdf>
- **libstdc++ Manual** (GCC project, ~300 pages) — the GNU C++ standard library. Pertinent because freestanding mode disables most of it. This week:
  - §3 — Using libstdc++. The bit on freestanding (§3.4 in the 13.2 manual).
  - §18 — The freestanding implementation conformance table. Which headers are guaranteed and which are not.
  <https://gcc.gnu.org/onlinedocs/libstdc++/>

## ARM and silicon documents (this week, mainly for disassembly)

- **RP2040 Datasheet** (Raspberry Pi Ltd, ~640 pages, Sep-2024 rev) — same as Week 4. This week mostly for the disassembly walks:
  - §2.3 (Cortex-M0+), pp. 65–73 — the ISA. ARMv6-M, Thumb-only, no Thumb-2.
  - §4.3 (I²C controllers), pp. 432–502 — when you re-implement the I²C transport in C++.
  - §4.4 (SPI controllers), pp. 503–540 — when you re-implement the SPI transport in C++.
  <https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf>
- **ARMv6-M Architecture Reference Manual** (ARM DDI 0419D, ~480 pages) — the Cortex-M0+ ISA. Needed for the disassembly comparison: ARMv6-M has 56 base instructions plus 6 32-bit Thumb-2 instructions (BL, MSR, MRS, ISB, DSB, DMB). The pages you cite this week:
  - §A6.7.137 — STR (immediate). The single instruction your `Gpio<Pin>::set()` compiles to.
  - §A6.7.59 — LDR (immediate). The pair instruction for the read path.
  - §A6.7 — General-purpose instructions. The full table.
  <https://developer.arm.com/documentation/ddi0419/d/>
- **ARM Procedure Call Standard for the Arm Architecture (AAPCS)** (ARM IHI 0042F, ~50 pages) — the ABI. When you cross from C to C++ and back via `extern "C"`, the ABI is what guarantees the call works. This week:
  - §6.4 — Parameter passing. R0–R3 for the first four args, stack for the rest.
  - §7.1 — Object file extensions. Name mangling is a C++ language feature; the ABI is identical.
  <https://github.com/ARM-software/abi-aa/releases>
- **Itanium C++ ABI** (Itanium working group, originally for IA-64; adopted by GCC and Clang for *every* C++ target including ARM) — the C++ name-mangling and v-table and RTTI layout specification. You do not need to memorize this, but you need to know where to look when a linker error mentions `_Z` or `__cxa_`. This week:
  - §5.1 — Name mangling. Why `_Z3foov` is `foo()` and `_ZN3FooC1Ev` is `Foo::Foo()`.
  - §3.4 — Virtual table layout. Why a single `virtual` keyword in a class pulls in 8 bytes per object plus a per-class v-table.
  - §3.3.4 — Type information. The `type_info` tables for RTTI.
  <https://itanium-cxx-abi.github.io/cxx-abi/abi.html>

## Embedded C++ standards (read at least the executive summaries)

- **AUTOSAR C++14 Coding Guidelines** (AUTOSAR consortium, ~500 pages, the 18-10 revision is the most widely-cited; requires registration on AUTOSAR's site — but the 14-03 version of the guidelines circulated publicly for years). The rules that matter this week:
  - **Rule A0-1-1** — A project shall not contain unused entities (the linker `--gc-sections` flag handles this for us).
  - **Rule A2-7-3** — All declarations shall be commented.
  - **Rule A3-9-1** — Fixed-width integer types from `<cstdint>` shall be used (`std::uint32_t`, not `unsigned int`).
  - **Rule A5-1-1** — Literal values shall not be used apart from type initialization, otherwise symbolic names shall be used (use `constexpr` constants, not magic numbers).
  - **Rule A12-1-1** — Constructors shall explicitly initialize all virtual base classes.
  - **Rule A15-0-1** — A function shall not exit with an exception if it is noexcept (we mark every function `noexcept`).
  - **Rule A18-5-1** — Functions `malloc`, `calloc`, `realloc`, and `free` shall not be used. By extension, `new` and `delete` shall not be used (the default forms). Replace with `etl::pool<T, N>` or placement-new.
  - **Rule A27-0-4** — `printf`, `scanf`, `fprintf`, etc. shall not be used (they pull in the libc's variadic floating-point parser; for embedded we use a snprintf shim or just direct write paths).
  <https://www.autosar.org/standards/adaptive-platform/> (registration-walled; the public PDFs sometimes leak via committee mirrors.)
- **MISRA C++ 2023** (MISRA Consortium, ~250 pages, supersedes MISRA C++ 2008 and aligns with C++17/20). The rules that matter this week:
  - **Rule 4.0.1** — All C++ source files shall be compiled as C++ source files.
  - **Rule 7.0.1** — Unsigned integer literals shall be used for bit manipulation (`1u << 18`, not `1 << 18`).
  - **Rule 8.0.2** — No exception shall be thrown that has not been declared.
  - **Rule 8.18.1** — `dynamic_cast` shall not be used (RTTI dependency).
  - **Rule 11.3.1** — `friend` declarations should not be used (encapsulation argument).
  - **Rule 15.1.1** — The `new` operator shall not be used. (Same as AUTOSAR A18-5-1.)
  - **Rule 18.0.1** — No exception shall be thrown from within a function declared as `noexcept`.
  - **Rule 21.10.1** — The features of `<cstdlib>` shall not be used. (No `malloc`, `exit`, `abort` — except as bookkeeping in a hosted-test build.)
  <https://misra.org.uk/product/misra-cpp2023/> (paid; some clauses circulate on the MISRA forum.)
- **High-Integrity C++ Coding Standard** (Programming Research, ~150 pages, free) — predates AUTOSAR and MISRA C++ 2023; many overlapping rules. A useful free fallback when the AUTOSAR or MISRA spec is behind a registration wall.
  <https://www.programmingresearch.com/static-analysis-resources/codingstandards/>
- **JSF Air Vehicle C++ Coding Standards** (Joint Strike Fighter, Lockheed Martin, 2005, ~200 pages, public domain) — the original avionics-grade C++ coding standard. Many rules are dated (no `dynamic_cast`, no `new`, no exceptions — all of which we adopt) but the rationale sections are still excellent reading.
  <http://www.stroustrup.com/JSF-AV-rules.pdf>
- **Linux kernel — Programming Language policy** (`Documentation/process/programming-language.rst`) — the canonical "why the Linux kernel is C, not C++" document, plus the recent (2022–2024) Rust-in-kernel additions. Required reading for the Lecture 3 argument.
  <https://docs.kernel.org/process/programming-language.html>

## Header-only libraries you will actually use

- **Embedded Template Library (ETL)** (Aster Consulting Ltd, MIT licensed, ~30,000 lines of header-only C++) — the canonical replacement for libstdc++ on a freestanding target. This week:
  - `etl::array<T, N>` — a `std::array` equivalent.
  - `etl::span<T>` — a `std::span` equivalent (works on pre-C++20 compilers; we use C++20's, but ETL's is a fallback).
  - `etl::circular_buffer<T, N>` — a fixed-capacity ring buffer, no heap.
  - `etl::optional<T>` — a `std::optional` equivalent without exceptions.
  - `etl::expected<T, E>` — a backport of C++23's `std::expected`.
  - `etl::pool<T, N>` — a fixed-capacity pool allocator. Use this instead of `operator new`.
  <https://github.com/ETLCPP/etl> · Documentation: <https://www.etlcpp.com/>
- **tl::expected** (Sy Brand, CC0/MIT, ~1500 lines header-only) — a clean implementation of `std::expected` for C++11/14/17/20. Use this if you need `expected` and ETL is overkill.
  <https://github.com/TartanLlama/expected>
- **Boost SML** (Krzysztof Jusiak, BSL-1.0, header-only) — a `constexpr` state-machine framework. Out of scope for this week but useful for the FreeRTOS mini-project in Week 9. Mention only.
  <https://github.com/boost-ext/sml>
- **{fmt}** (Victor Zverovich, MIT, header-only or compiled, supports freestanding mode with `FMT_HEADER_ONLY` and a custom `fwrite`) — a `printf`-replacement library. Notable: `fmt::format_to` with a fixed buffer is compile-time-checked and produces ≈ 4 KB of object code, vs `printf` at ≈ 25–30 KB. For C7's purposes we mostly hand-write the formatting code, but `fmt` is the polished alternative.
  <https://github.com/fmtlib/fmt>

## Free books and write-ups

- **"C++ Embedded Recipes" by Eduardo Corpeño** (Packt, 2024, ~350 pages, paid — but the publisher's free chapter excerpts cover the freestanding-mode setup and the GPIO template pattern). Chapter 3 is the C++20 freestanding chapter and is the closest thing in print to this week's Lecture 1.
- **"Practical C++ Embedded Programming" by Mark Siegesmund** (free PDF, ~200 pages, 2010 vintage but still relevant for the subset discipline). Predates C++20 but the no-exception, no-RTTI, no-heap discipline transfers exactly.
  <https://github.com/practicalcpp/practicalcpp.github.io>
- **"What Every Embedded Engineer Should Know About C++"** (Beningo Embedded Group, free article, 2019) — a 3000-word tour of `constexpr`, templates, and CRTP for MCU work. Reads as an executive summary of Lectures 1 and 2 of this week.
  <https://www.beningo.com/what-every-embedded-engineer-should-know-about-c/>
- **"From C++ to C with cppfront" — Herb Sutter** (talks and blog posts, 2022–2024) — the cppfront experiment is "what C++ would look like if we designed it for safety today." Useful framing for Lecture 3 even though we do not adopt cppfront.
  <https://github.com/hsutter/cppfront>
- **"Modern C++ for Embedded Engineers"** (Memfault Interrupt blog, 2021) — a five-part series. The "RTTI and Exceptions" part is the cleanest one-page explanation of why we disable them and what the linker error looks like when we forget.
  <https://interrupt.memfault.com/blog/cpp-fundamentals>
- **"Polymorphism Without Virtual Functions"** (LLVM blog, 2022) — the CRTP pattern explained from a compiler-writer's perspective. Reading this makes the `objdump` of Lecture 2 obvious in advance.
  <https://blog.llvm.org/posts/2022-09-06-polymorphism-without-virtual-functions/>
- **"Optimizing C++ for Embedded"** (Wojciech Muła, free article series) — a working embedded engineer's notes on `-Os`, `-flto`, `-fdata-sections`, and what the linker actually strips. Very specific, very practical.
  <http://0x80.pl/notesen/2023-04-29-cpp-embedded.html>
- **"C++ Best Practices"** (Jason Turner, free GitBook, ~80 pages) — the canonical "use modern C++" list. Filter through the "no heap, no exceptions" lens.
  <https://lefticus.gitbooks.io/cpp-best-practices/content/>
- **"The Cherno: C++ for Game Engines"** (Yan Chernikov, free YouTube series, ~50 episodes) — game-engine-flavored C++ but the zero-cost-abstraction discipline transfers directly. Episode 87 on "Why we don't use exceptions" is the cleanest 10-minute explanation we have found.
  <https://www.youtube.com/@TheCherno>

## Videos (free)

- **"Modern C++ Embedded Programming" — Dan Saks, CppCon 2018** (~60 min, YouTube). Dan walks through `constexpr` peripheral templates and shows the disassembly on a Cortex-M4. The reference for this week's Lecture 2.
  <https://www.youtube.com/watch?v=Q1OAtuq8tn0>
- **"Embedded Programming with C++" — Odin Holmes, Meeting C++ 2017** (~60 min, YouTube). The Kvasir.io library tour; CRTP with type-list metaprogramming. Heavy but every minute is worth it.
  <https://www.youtube.com/watch?v=DiQflyyVfck>
- **"C++20 for Embedded" — Phil Nash, embedded.fm episode 416** (~80 min, audio podcast + transcript). Phil is on the C++ committee and walks through what C++20 changes for embedded. The freestanding-headers list is the most useful 10 minutes.
  <https://embedded.fm/episodes/416>
- **"Inside STL: The Implementation of `std::optional`" — Stephan T. Lavavej, 2020** (~50 min, YouTube). STL (the person) walks through how MSVC implements `std::optional`. The "no heap, just a bool tag" property is what makes it embedded-safe.
  <https://www.youtube.com/watch?v=zRPNQqIVQbU>
- **"The Embedded Muse" — Jack Ganssle** (free newsletter and YouTube). Jack has been writing about embedded C++ since the late 1990s. His "C vs C++ for embedded" essay (issue 411) is the calibrated, opinion-rich version of Lecture 3.
  <http://www.ganssle.com/tem.htm>

## Tools you will use this week

| Command / tool                                                                              | What it does                                                                 |
|---------------------------------------------------------------------------------------------|------------------------------------------------------------------------------|
| `arm-none-eabi-g++ -std=c++20 -mcpu=cortex-m0plus -mthumb -Os -ffreestanding -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit -c gpio.cpp` | Compile a single C++ file with the freestanding flags                        |
| `arm-none-eabi-objdump -d build/w05-ex01.elf \| grep -A 5 'Gpio.*set'`                       | Disassemble the `Gpio<Pin>::set()` template; verify the inlined `str` instruction |
| `arm-none-eabi-size build/w05-mini.elf`                                                     | Print the `.text`, `.data`, `.bss`, `.rodata` sizes. The benchmark axis.    |
| `arm-none-eabi-nm --size-sort build/w05-mini.elf \| head -20`                                | Top 20 symbols by size. Used to find a leaked symbol (e.g. `__cxa_atexit`).  |
| `arm-none-eabi-readelf -S build/w05-mini.elf`                                               | Section table. Look for unexpected sections like `.eh_frame` (exceptions) or `.gcc_except_table`. |
| `arm-none-eabi-g++ -E gpio.cpp \| head -200`                                                 | Pre-processed output. Sometimes you need to see what a header expanded to.   |
| `diff <(arm-none-eabi-objdump -d c.elf) <(arm-none-eabi-objdump -d cpp.elf)`                | The benchmark diff. Differences here are the cost of your abstractions.      |
| `compiler-explorer (Godbolt) — arm-none-eabi-g++ -std=c++20 -Os -mcpu=cortex-m0plus`        | Online disassembly playground. The fastest way to verify an isolated template. |
| `clang-tidy --checks='cert-*,bugprone-*,modernize-*,performance-*'`                         | Static analyzer with C++ checks. Will flag many AUTOSAR/MISRA-C++ violations. |
| `cppcheck --enable=all --std=c++20`                                                         | Open-source C++ static analyzer. Catches a different class of bugs than clang-tidy. |

## C++ build flags cheat-sheet (the freestanding incantation)

The full set of flags for a freestanding C++ build on Cortex-M0+ (Pi Pico W):

```
arm-none-eabi-g++
    -std=c++20                    # C++20 mode
    -mcpu=cortex-m0plus           # ARMv6-M, Thumb-only
    -mthumb                       # Generate Thumb-2-compatible code
    -ffreestanding                # No hosted libc
    -fno-exceptions               # No throw / try / catch
    -fno-rtti                     # No typeid / dynamic_cast
    -fno-threadsafe-statics       # No __cxa_guard_acquire/release
    -fno-use-cxa-atexit           # No __cxa_atexit for destructors
    -fno-unwind-tables            # No .eh_frame_hdr (further size win)
    -fno-asynchronous-unwind-tables  # No .eh_frame
    -ffunction-sections           # One section per function (for --gc-sections)
    -fdata-sections               # One section per global (for --gc-sections)
    -Os                           # Optimize for size
    -Wall -Wextra -Wpedantic      # Warnings on
    -Wno-unused-parameter         # We have lots of `bus_t *self` ignores
    -nostdlib                     # No host startup, no host libc
    -nostartfiles                 # No host crt0
    -c gpio.cpp
```

On link, add `-Wl,--gc-sections` to strip unused sections and `-Wl,-T,pico.ld` to use your linker script. The result is a `.elf` that contains only the code you actually use, with no exception tables, no v-tables (unless you used `virtual`), and no `__cxa_*` symbols (unless you used a `static` local — which the `-fno-threadsafe-statics` flag flattens).

## C vs C++ binary size, on Cortex-M0+, -Os (typical numbers)

| Artifact                                       | C build (`-Os`) | C++ build, freestanding (`-Os`) | Delta  |
|------------------------------------------------|-----------------|-----------------------------------|--------|
| Blink (`Gpio` + delay)                          | 264 B           | 268 B                             | +4 B   |
| Week 3 UART driver (init + putc)               | 412 B           | 416 B                             | +4 B   |
| Week 4 SPI driver (SSD1306 init seq)            | 1,856 B         | 1,872 B                           | +16 B  |
| Week 4 I²C driver (BMP280 init + compensation) | 2,148 B         | 2,164 B                           | +16 B  |
| Week 4 mini-project (full, no DMA)              | 7,124 B         | 7,168 B                           | +44 B  |
| **Naive C++ build (RTTI + exceptions enabled)** | 7,124 B         | **53,892 B**                      | **+46 KB** |

The last row is the warning. Without the `-fno-exceptions -fno-rtti -fno-threadsafe-statics` triad, a `Hello, World!` C++ program on the Pi Pico W is 50 KB out of your 2 MB flash budget — every `static` local pulls `__cxa_guard_*`, every potentially-throwing function pulls the unwinder. With the triad, C++ is a 1% cost over C.

## Naming conventions in this week's code

- Types: `PascalCase` (`Gpio`, `SpiDevice`, `Ssd1306`).
- Variables, functions: `snake_case` (`pin_set`, `init`, `write_read`). Matches the Pi Pico SDK and most embedded C.
- Templates: type parameters in `PascalCase` (`template<typename Bus, std::uint8_t Address>`).
- Concepts: `PascalCase` (`SpiDevice`, `I2cBus`). Distinguishable from regular types by the surrounding `concept` declaration.
- Constants: `kPascalCase` with a leading `k`, or `ALL_CAPS` for hardware constants (`kRefreshHz`, `IO_BANK0_BASE`).
- Files: lowercase, hyphen-separated (`gpio.hpp`, `spi-transport.cpp`).

This mirrors the Google C++ Style Guide with the embedded-C accommodation of `snake_case` for variables. AUTOSAR allows either style; pick one per project and stay consistent.

## What is NOT in this week

- Templates beyond CRTP and the simple constraint case. Variadic templates, fold expressions, and the deep metaprogramming bag (`std::tuple` manipulation, type-list operations) are out of scope. They are useful but not load-bearing for this week's `Gpio<Pin>` and `SpiDevice` patterns.
- The C++ coroutine machinery (`co_await`, `co_return`). Coroutines on embedded are technically possible but pull `__cxa_*` and require a heap or a custom allocator. Wait for Week 11 (DMA and async patterns).
- The C++23/26 changes — `std::expected`, `std::print`, `std::flat_map`, reflection. We use the C++20 standard; if you have a newer toolchain (`arm-none-eabi-g++ 14+` with C++23), you may use `std::expected` directly in place of ETL's `etl::expected`.
- C++ unit testing on embedded with frameworks like Catch2 or GoogleTest. We use host-side tests as in Week 4 (compile C++ for x86, run on the host); embedded unit-testing patterns return in Week 9 with FreeRTOS task-level tests.
- The Rust comparison. Many of this week's patterns (typed peripheral addresses, compile-time constraints, no heap, no exceptions) map onto Rust's `embedded-hal` traits. We cover that mapping in Week 6 (the original syllabus's Week 5 — slated to be re-numbered as Week 6 to keep this week's C++ depth).

If you find yourself reading the ISO C++ standard at 11 PM to understand why `template<auto N> ...` does not deduce, you have over-tooled the problem. cppreference + a one-page concept example will get you 95% of the way there. The standard is the *final* authority, not the *first*.
