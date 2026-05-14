# Week 5 — Mini-project: the SPI/I²C driver, rewritten in C++20

> *Re-implement Week 4's SPI and I²C drivers in C++20. Templates for the bus and the device. Concepts for the contracts. CRTP where polymorphism is needed. Compile-time-validated peripheral configuration. `.text` size within 1% of last week's C version. Saleae captures byte-for-byte identical.*

## The artifact

By Sunday 23:59 local time you produce a public GitHub repo containing a Pi Pico W firmware that:

1. Re-implements the **BMP280 driver** in C++20 against an `I2cBus` concept.
2. Re-implements the **SSD1306 driver** (the SPI variant from Week 4 Exercise 1) in C++20 against the `SpiBus` concept from Exercise 2 of this week.
3. Re-implements the **transport layer** (`SpiTransport<SpiInstance>`, `I2cTransport<I2cInstance>`) as C++ templates with `constexpr` peripheral addresses and `consteval` divider arithmetic.
4. Reproduces the Week 4 mini-project behavior: BMP280 readings refreshed at 4 Hz, rendered on an SSD1306 over SPI, with a UART log line per refresh.
5. Builds two configurations in the same `Makefile`: a **C** version (Week 4's code, unchanged) and a **C++** version (this week's rewrite). The build outputs are `build/w05-c.elf` and `build/w05-cpp.elf`.
6. Includes a `BENCHMARK.md` with the side-by-side comparison: `.text`, `.data`, `.bss`, top-10 symbols, function-by-function disassembly diff for the SSD1306 init and the BMP280 init.
7. Ships with a `FAULT-MODEL.md` updated to flag three C++-specific failure modes (a non-`constexpr` global with a constructor pulling in `__cxa_atexit`; an accidentally-virtual destructor; an `std::optional<int>` returned where an `int` suffices).
8. Includes three Saleae captures: I²C transaction with the BMP280 (C version), I²C transaction with the BMP280 (C++ version), SPI transaction with the SSD1306 (C++ version). The first two must be **byte-identical** within decoder tolerance.

Pass criteria:
- The C++ `.text` is within **1%** of the C version. If larger, you have leaked a runtime feature; find and remove.
- Both binaries flash and run on the same Pi Pico W hardware.
- The Saleae captures for the I²C transactions are byte-identical between the C and C++ builds.

## System diagram

```
                          +--------------------------+
                          |        Pi Pico W         |
                          |                          |
   I²C bus (100 kHz)      |  GP4 -> SDA, GP5 -> SCL  |    SPI bus (1 MHz)
   pull-ups 4.7 kΩ        |                          |
        ---+              |  GP16 -> MISO (unused)   |
           |              |  GP17 -> CSn (software)  |              +----
   +-------+--+           |  GP18 -> SCK             |              |
   |  BMP280  |           |  GP19 -> MOSI            +-------+      +-+--+
   |  @ 0x76  |           |  GP20 -> D/C̄ (software) |     [SSD1306]
   +----------+           |  GP21 -> RST (software)  |     [  SPI ]
        |                 +--------------------------+      +----+
        (same wiring as Week 4 mini-project)
```

One I²C device (BMP280), one SPI device (SSD1306). Smaller scope than Week 4 — the goal here is the C++ rewrite and the benchmark, not the multi-device sensor mesh.

## Spec — required behavior

| Item | Spec |
|------|------|
| Boot-up output | The SSD1306 (SPI) shows `crunch-wire w05 cpp` for 500 ms, then transitions to live data |
| Refresh rate | 4 Hz (250 ms per cycle). BMP280 read + OLED refresh + UART line all within 250 ms |
| Display layout | Line 0: `T=25.08C` (centi-degrees). Line 2: `P=1006.5 hPa` (decimal centi-hPa). Line 4: `[cpp build]`. Line 6: `flash=NNNN B` (size of `.text` from `arm-none-eabi-size`) |
| UART log | One line per refresh: `T=25.08 P=100653 cpp` (UART0, 115200 baud) |
| Build configurations | `make c` builds Week 4's code; `make cpp` builds this week's; `make both` builds both side by side; `make benchmark` runs the size comparison |
| Saleae traces | `traces/i2c-bmp280-c.sal`, `traces/i2c-bmp280-cpp.sal`, `traces/spi-ssd1306-cpp.sal`; the first two must be byte-identical at the decoder layer |
| Abstraction | The two device-driver source files (`bmp280.hpp`, `ssd1306.hpp`) use `concepts` to constrain the bus type; both compile against a *host-side mock bus* used in unit tests |
| Host tests | `make test` builds and runs host-side unit tests for `bmp280::compensate` and `ssd1306::init_sequence`; all tests pass |
| FAULT-MODEL.md | Updated to flag three C++-specific failure modes; references the C++ subset rules |
| Build | `make all` produces `build/w05-c.elf`, `build/w05-cpp.elf`, both `.uf2`s, and `BENCHMARK.md` |
| Runtime resilience | Bus-stuck recovery from Week 4 Challenge 1 ported to the C++ side; the recovery logic is in a template `BusRecovery<Sda, Scl>` |

## Repo layout

```
w05-mini/
├── Makefile
├── README.md
├── BENCHMARK.md
├── FAULT-MODEL.md
├── pico.ld
├── startup.c
├── boot2_w25q080.S
├── src-c/                         # Week 4 code, copied verbatim
│   ├── main.c
│   ├── bmp280.c / .h
│   ├── ssd1306_spi.c / .h
│   ├── i2c_transport.c / .h
│   ├── spi_transport.c / .h
│   ├── rp2040_*.c / .h
│   ├── bus.h
│   └── uart.c / .h
├── src-cpp/                       # New this week
│   ├── main.cpp
│   ├── bmp280.hpp
│   ├── ssd1306.hpp
│   ├── spi0.hpp
│   ├── i2c0.hpp
│   ├── spi_bus.hpp                # concept
│   ├── i2c_bus.hpp                # concept
│   ├── gpio.hpp
│   ├── uart.hpp                   # C++ UART (Challenge 1)
│   └── bus_recovery.hpp           # template
├── tests/
│   ├── test_bmp280_cpp.cpp        # host-side, uses mock bus
│   ├── test_ssd1306_cpp.cpp
│   └── Makefile
└── traces/
    ├── i2c-bmp280-c.sal
    ├── i2c-bmp280-cpp.sal
    └── spi-ssd1306-cpp.sal
```

## The BENCHMARK.md template

Your `BENCHMARK.md` must follow this structure:

```markdown
# w05-mini — Benchmark: C vs C++

Last updated: YYYY-MM-DD. Reviewer: <name>.

## Toolchain

- arm-none-eabi-gcc 13.2.1
- arm-none-eabi-g++ 13.2.1
- Build flags (C): -std=c11 -mcpu=cortex-m0plus -mthumb -Os -ffreestanding
- Build flags (C++): -std=c++20 -mcpu=cortex-m0plus -mthumb -Os -ffreestanding
                     -fno-exceptions -fno-rtti -fno-threadsafe-statics
                     -fno-use-cxa-atexit -fno-unwind-tables
                     -fno-asynchronous-unwind-tables

## Top-level sizes (-Os)

| Build         | .text bytes | .data bytes | .bss bytes | Total flash |
|---------------|-------------|-------------|------------|-------------|
| w05-c.elf     | 7,124       | 256         | 2,048      | 9,428       |
| w05-cpp.elf   | 7,168       | 256         | 2,048      | 9,472       |
| Delta         | +44 (0.6%)  | 0           | 0          | +44         |

Pass: delta is < 1%. ✓

## Top 10 symbols by size

| Symbol (C)                | Bytes | Symbol (C++)                       | Bytes |
|---------------------------|-------|------------------------------------|-------|
| bmp280_compensate          | 412   | _ZN4pico6Bmp28010compensate...     | 408   |
| ssd1306_init_seq           | 384   | _ZN4pico7Ssd1306IS...4initEv       | 392   |
| ...                       | ...   | ...                                | ...   |

## Function-by-function disassembly diff

### bmp280_compensate (C) vs Bmp280::compensate (C++)

```
$ diff <(arm-none-eabi-objdump -d w05-c.elf | sed -n '/bmp280_compensate/,/^$/p' | strip-addrs) \
       <(arm-none-eabi-objdump -d w05-cpp.elf | sed -n '/Bmp280.*compensate/,/^$/p' | strip-addrs)

(empty — instructions identical)
```

### ssd1306_init (C) vs Ssd1306::init (C++)

```
(2 instructions differ — the C++ version uses lsls instead of mov for one mask;
 the C version uses an immediate that fits in 8 bits.)
```

## Saleae trace diff

Both `i2c-bmp280-c.sal` and `i2c-bmp280-cpp.sal` decode to:

| Byte # | C trace                | C++ trace             | Match? |
|--------|------------------------|------------------------|--------|
| 1      | 0xEC (write to 0x76)   | 0xEC                   | ✓      |
| 2      | 0xD0 (read id)         | 0xD0                   | ✓      |
| 3      | RESTART, 0xED          | RESTART, 0xED          | ✓      |
| 4      | 0x58 (read)            | 0x58                   | ✓      |
| ...    | ...                    | ...                    | ...    |

All bytes match. ✓

## Forbidden symbols check

```
$ arm-none-eabi-nm build/w05-cpp.elf | grep -E '__cxa_|_Unwind_|__gxx_personality|_ZTV|_ZTI'
(empty)
```

✓ Pass.

## What the C++ build gained

- Compile-time pin validation: `Gpio<42>` is a build error, not a silent runtime no-op.
- Compile-time baud/divider validation: `Uart<0, 50'000'000>` is a build error.
- Concept-checked bus interfaces: passing a `Uart` to an `Ssd1306` constructor fails at the call site with a one-line message.
- Type-safe `std::span` arguments: no buffer-overrun by passing the wrong length.

## What the C++ build cost

- +44 bytes of `.text` (0.6%) — within budget.
- Build time +12% (the templates expand and the optimizer works harder).
- One additional include path for `etl/`.
- One additional CMake/Make rule (`make cpp`).

## Decision

For a greenfield 32-bit MCU project without strict certification, the C++ version is the new baseline. The C version is kept for reference and for parity verification.
```

## The FAULT-MODEL.md addendum

Add to your Week 4 `FAULT-MODEL.md` a new section:

```markdown
## C++ build failure modes (new in Week 5)

| #   | Symptom                                  | Likely cause                            | First 3 diagnostic steps                                                |
|-----|------------------------------------------|------------------------------------------|-------------------------------------------------------------------------|
| C.1 | `.text` is +30 KB larger than C version  | Forgot `-fno-exceptions -fno-unwind-tables` | (1) `arm-none-eabi-nm --size-sort *.elf \| head -10`; top symbol is `__gxx_personality_v0`. (2) Re-add the flag. (3) Re-build, re-measure. |
| C.2 | Linker error: `undefined reference to '__cxa_atexit'` | C++ global with non-trivial destructor | (1) `grep -rn 'class.*~' src-cpp/` — find the destructor. (2) Make the global `constinit` with a `constexpr` constructor and trivial destructor. (3) Add `-fno-use-cxa-atexit`. |
| C.3 | Linker error: `undefined reference to '__cxa_pure_virtual'` | Accidental virtual function (often a virtual destructor) | (1) `grep -rn 'virtual' src-cpp/`. (2) Mark the destructor `= default; final` or remove the virtual. (3) If polymorphism is genuinely needed, use CRTP instead. |
| C.4 | `static_assert(SpiBus<MyType>, ...)` fails | The concept rejected `MyType`; signature mismatch | (1) Read the concept in `spi_bus.hpp`. (2) Match the `MyType::xfer` signature exactly. (3) Re-instantiate. |
| C.5 | `consteval` build error: "called in non-constant context" | Argument to a `consteval` function is not a compile-time constant | (1) Make the argument `constexpr` or a template parameter. (2) If the argument is genuinely runtime, drop to `constexpr` (which permits runtime calls) instead of `consteval`. |
```

## Grading

| Axis                                              | Weight |
|---------------------------------------------------|--------|
| C++ firmware compiles, flashes, runs              | 20%    |
| `.text` delta vs C ≤ 1%                            | 25%    |
| Saleae traces byte-identical (I²C)                 | 15%    |
| BENCHMARK.md present and conforming                | 15%    |
| FAULT-MODEL.md updated with C++-specific section   | 10%    |
| Host-side unit tests pass                          | 10%    |
| Code review (concepts used; CRTP where appropriate; no leaked runtime symbols) | 5% |

Passing: 70%. The `.text` budget is a hard prerequisite — without ≤ 1% delta, the project does not pass regardless of other axes (because the whole point of the week is "C++ at zero runtime cost," and you have failed the proof).

## Suggested build sequence

1. **Day 1 (Saturday morning, 2 h):** copy Week 4's C code into `src-c/`. Verify it still builds, flashes, and runs. Capture `i2c-bmp280-c.sal` from the running firmware. This is your reference.
2. **Day 1 (Saturday morning, 2 h):** port `gpio.h` to `gpio.hpp` (you have this from Exercise 1). Port `uart.h/c` to `uart.hpp` (from Challenge 1).
3. **Day 1 (Saturday afternoon, 2 h):** port `spi_transport.c` to `spi0.hpp`. Port `ssd1306_spi.c` to `ssd1306.hpp` (you have this from Exercise 2). Verify the OLED renders.
4. **Day 1 (Saturday evening, 2 h):** port `i2c_transport.c` to `i2c0.hpp`. Port `bmp280.c` to `bmp280.hpp`. Verify the BMP280 reads via UART log.
5. **Day 2 (Sunday morning, 2 h):** capture `i2c-bmp280-cpp.sal` and `spi-ssd1306-cpp.sal`. Diff against the C-version capture. They must match.
6. **Day 2 (Sunday afternoon, 1 h):** write `BENCHMARK.md` from the template. Run `arm-none-eabi-size` and `arm-none-eabi-nm` on both builds. Fill in the tables.
7. **Day 2 (Sunday evening, 1 h):** update `FAULT-MODEL.md`. Polish `README.md`. Push, tag `w05-mini`, request review.

## Stretch (not graded, for portfolio)

- **`tl::expected<T, BusError>` return types.** Replace `int` returns with `tl::expected` for richer error reporting. Measure the binary delta (should be ≤ 200 bytes).
- **DMA-paced SPI for the SSD1306.** Reuse Week 4's stretch goal. Confirm the binary delta is the same as in C (the DMA setup is mostly register pokes, which compile identically).
- **Compile-time-checked SSD1306 framebuffer layout.** The 128×64 panel has 1024 bytes laid out as 8 pages × 128 columns. Make the framebuffer a `std::array<std::array<std::uint8_t, 128>, 8>` and write a `consteval` function that fills it from a compile-time string ("crunch-wire w05"). The font glyphs are part of the rodata. The display update is one DMA push.
- **Host-side property-based testing.** Use [rapidcheck](https://github.com/emil-e/rapidcheck) or [Catch2](https://github.com/catchorg/Catch2) on the host to property-test `bmp280::compensate` — for any valid `(raw_t, raw_p, calib)` triple, the output is in plausible ranges. Run on every push via GitHub Actions.

## Final note

The mini-project this week is the calibration: did you actually pay zero runtime cost for the C++ abstractions? The Saleae trace says yes. The `.text` size says yes. The `arm-none-eabi-nm` grep for forbidden symbols says yes. If any of those three say no, find what leaked and remove it.

When the `.text` delta is in single digits of bytes, the Saleae traces overlay perfectly, and the `BENCHMARK.md` is on the repo, this week is done.
