# Exercise 2 — C++20 SPI driver with `concepts`

> *Re-implement your Week 4 SSD1306-over-SPI driver in C++20. Use a `SpiBus` concept to constrain the bus type. Use the `Gpio<Pin>` template from Exercise 1 for the chip-select line. Confirm the disassembly is within 50 bytes of the C version.*

## Goal

Build a `SpiDevice<Bus, CsPin>` template that brings up the Pi Pico W's SPI0 peripheral, drives an SSD1306 OLED through a `concept`-constrained bus, and renders `"crunch-wire w05"` on the panel. Diff the `.text` section size against your Week 4 C version of the same driver. The delta must be ≤ 50 bytes.

By the end you can:
- Define a C++20 `SpiBus` concept that constrains a template parameter to "a type with a `write(span)` method returning `int`."
- Author a `SpiDevice<Bus, CsPin>` template using CRTP for the device-driver base class.
- Use `constexpr` to embed the SSD1306 init sequence as a 26-byte `std::array<uint8_t, 26>` in `.rodata`.
- Use `std::span<const std::uint8_t>` to pass the init sequence to the bus without copying.
- Compile, flash, scope, and verify the SSD1306 renders the same output as Week 4's C version.
- Report the `.text` size delta in a `BENCHMARK.md`.

## Setup

### Parts

- 1× Pi Pico W (same as Week 4)
- 1× SSD1306 128×64 OLED, 4-wire SPI variant (same as Week 4 Exercise 1)
- 1× breadboard
- 6× jumper wires
- 1× Saleae or equivalent logic analyzer

### Wiring

Identical to Week 4 Exercise 1:

| OLED pin | Pico GP | Purpose                |
|----------|---------|------------------------|
| GND      | GND     | Ground                 |
| VCC      | 3V3     | 3.3 V power            |
| D0/SCK   | GP18    | SCK                    |
| D1/MOSI  | GP19    | MOSI                   |
| RST      | GP21    | Reset (software-driven) |
| DC       | GP20    | D/C̄ (software-driven)  |
| CS       | GP17    | CSn (software-driven)   |

### Prerequisites

- Exercise 1 of this week — you have the `Gpio<Pin>` template working.
- Week 4 Exercise 1 — your C version of the SPI/SSD1306 driver is on GitHub.
- Lectures 1 and 2 of this week read.

## Reading

- **Lecture 1** of this week — the freestanding C++ subset and the build flags.
- **Lecture 2** of this week — the `SpiDevice<Bus, CsPin>` example.
- **RP2040 datasheet** §4.4 (SPI), pp. 503–540 — same chapter as Week 4 Lecture 1.
- **SSD1306 datasheet** §10 (Command Table), pp. 28–32 — the init sequence.
- **cppreference, "concepts library"**: <https://en.cppreference.com/w/cpp/concepts>

## Steps

### 1. Define the `SpiBus` concept

Create `spi_bus.hpp`:

```cpp
#pragma once
#include <concepts>
#include <cstdint>
#include <span>

namespace pico {

// A type that exposes the SPI byte-transfer contract.
// Required methods: xfer(byte) -> uint8_t.
template<typename T>
concept SpiBus = requires(T &t, std::uint8_t byte) {
    { t.xfer(byte) } -> std::same_as<std::uint8_t>;
};

}  // namespace pico
```

**Checkpoint:** Compile the header alone:

```sh
arm-none-eabi-g++ -std=c++20 -mcpu=cortex-m0plus -mthumb -Os \
    -ffreestanding -fno-exceptions -fno-rtti -fsyntax-only \
    -x c++ spi_bus.hpp
```

No errors expected. The concept is a compile-time declaration; it has no runtime cost.

### 2. Implement `Spi0` — a concrete bus

Create `spi0.hpp`:

```cpp
#pragma once
#include <cstdint>
#include "spi_bus.hpp"

namespace pico {

class Spi0 {
public:
    static constexpr std::uintptr_t kBase     = 0x4003c000u;
    static constexpr std::uintptr_t kSspcr0   = kBase + 0x00u;
    static constexpr std::uintptr_t kSspcr1   = kBase + 0x04u;
    static constexpr std::uintptr_t kSspdr    = kBase + 0x08u;
    static constexpr std::uintptr_t kSspsr    = kBase + 0x0cu;
    static constexpr std::uintptr_t kSspcpsr  = kBase + 0x10u;

    [[gnu::always_inline]]
    void init_1mhz() noexcept {
        // Release SPI0 from reset (omitted; see Week 4 Exercise 1).
        // Configure SSPCR0: SPI mode 0, 8-bit data, FRF=Motorola.
        reg(kSspcr0)  = 0x0007u;             // 8-bit, Motorola SPI, mode 0
        reg(kSspcpsr) = 125u;                // prescaler = 125 → 1 MHz at clk_peri=125 MHz
        reg(kSspcr1)  = (1u << 1);            // SSE: enable
        // IO_BANK0 + PADS_BANK0 setup omitted; see Week 4.
    }

    [[gnu::always_inline]]
    std::uint8_t xfer(std::uint8_t tx) noexcept {
        // Wait until TX FIFO has space.
        while ((reg(kSspsr) & 0x02u) == 0u) {}
        reg(kSspdr) = tx;
        // Wait until RX FIFO has data.
        while ((reg(kSspsr) & 0x04u) == 0u) {}
        return static_cast<std::uint8_t>(reg(kSspdr));
    }

private:
    [[gnu::always_inline]]
    static volatile std::uint32_t &reg(std::uintptr_t addr) noexcept {
        return *reinterpret_cast<volatile std::uint32_t *>(addr);
    }
};

static_assert(SpiBus<Spi0>, "Spi0 must satisfy the SpiBus concept");

}  // namespace pico
```

The `static_assert(SpiBus<Spi0>, ...)` is the load-bearing line: it forces the compiler to verify *at the declaration site* that `Spi0` satisfies the concept. If we accidentally change `xfer` to return `int` instead of `std::uint8_t`, the build fails here, not at the use site.

**Checkpoint:** Compile:

```sh
arm-none-eabi-g++ -std=c++20 -mcpu=cortex-m0plus -mthumb -Os \
    -ffreestanding -fno-exceptions -fno-rtti -fsyntax-only \
    -x c++ spi0.hpp
```

If the `static_assert` fails, your `xfer` signature does not match the concept; fix it.

### 3. Implement `SpiDevice<Bus, CsPin>`

Create `spi_device.hpp`:

```cpp
#pragma once
#include <cstdint>
#include <span>
#include "gpio.hpp"
#include "spi_bus.hpp"

namespace pico {

template<SpiBus Bus, std::uint8_t CsPin>
class SpiDevice {
    static_assert(CsPin < 30u, "RP2040 has GP0–GP29");

public:
    explicit SpiDevice(Bus &bus) noexcept : bus_(bus) {
        // CSn idle high. Caller must have configured the pad as output.
        Gpio<CsPin>::set();
    }

    [[nodiscard, gnu::always_inline]]
    int write(std::span<const std::uint8_t> tx) noexcept {
        Gpio<CsPin>::clr();
        for (std::uint8_t b : tx) {
            (void)bus_.xfer(b);
        }
        Gpio<CsPin>::set();
        return static_cast<int>(tx.size());
    }

    [[nodiscard, gnu::always_inline]]
    int write_read(std::span<const std::uint8_t> tx,
                    std::span<std::uint8_t> rx) noexcept {
        Gpio<CsPin>::clr();
        for (std::uint8_t b : tx) {
            (void)bus_.xfer(b);
        }
        for (std::uint8_t &b : rx) {
            b = bus_.xfer(0xFFu);  // dummy clock
        }
        Gpio<CsPin>::set();
        return static_cast<int>(rx.size());
    }

private:
    Bus &bus_;
};

}  // namespace pico
```

`SpiDevice<Spi0, 17>` is a complete type — one type per `(bus, chip-select)` pair. The `[[nodiscard]]` attribute warns at any call site that ignores the byte-count return.

### 4. Implement `Ssd1306<Bus, CsPin, DcPin, RstPin>`

Create `ssd1306.hpp`:

```cpp
#pragma once
#include <array>
#include <cstdint>
#include <span>
#include "gpio.hpp"
#include "spi_bus.hpp"
#include "spi_device.hpp"

namespace pico {

template<SpiBus Bus,
          std::uint8_t CsPin,
          std::uint8_t DcPin,
          std::uint8_t RstPin>
class Ssd1306 {
    static_assert(CsPin  < 30u, "RP2040 has GP0–GP29");
    static_assert(DcPin  < 30u, "RP2040 has GP0–GP29");
    static_assert(RstPin < 30u, "RP2040 has GP0–GP29");

public:
    explicit Ssd1306(Bus &bus) noexcept : dev_(bus) {}

    [[nodiscard]] int init() noexcept {
        // Hardware reset pulse: low for 1 ms, high for 1 ms.
        Gpio<RstPin>::clr();
        delay_ms(1);
        Gpio<RstPin>::set();
        delay_ms(1);

        // Command mode.
        Gpio<DcPin>::clr();

        constexpr std::array<std::uint8_t, 26> kInit = {
            0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
            0x8D, 0x14, 0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x12,
            0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6,
            0xAF, 0x00  // 26th byte for alignment; SSD1306 ignores it
        };
        return dev_.write(std::span<const std::uint8_t>(kInit).first(25));
    }

    [[nodiscard]] int draw_string(std::span<const std::uint8_t> framebuf) noexcept {
        Gpio<DcPin>::set();   // data mode
        return dev_.write(framebuf);
    }

private:
    SpiDevice<Bus, CsPin> dev_;

    static void delay_ms(std::uint32_t ms) noexcept {
        for (std::uint32_t i = 0; i < ms * 1000u; i++) {
            asm volatile("nop");
        }
    }
};

}  // namespace pico
```

The init sequence is a `constexpr std::array<std::uint8_t, 26>` — embedded in `.rodata`, no runtime cost. The `.first(25)` slice keeps the actual 25 init bytes; the 26th is padding for alignment.

### 5. Wire it up in `main.cpp`

```cpp
#include "spi0.hpp"
#include "ssd1306.hpp"

extern "C" void enable_gpio_outputs();   // your Week 3 IO_BANK0/PADS_BANK0 code
extern "C" int main();

pico::Spi0 g_spi0;

int main() {
    enable_gpio_outputs();
    g_spi0.init_1mhz();

    pico::Ssd1306<pico::Spi0, /*CS=*/17, /*DC=*/20, /*RST=*/21> oled{g_spi0};
    if (oled.init() < 0) {
        // hang on error
        while (true) {}
    }

    // Draw "crunch-wire w05" using a precomputed framebuffer (~1024 bytes).
    constexpr std::array<std::uint8_t, 1024> kFramebuf = { /* ... 5x8 font glyphs ... */ };
    (void)oled.draw_string(std::span<const std::uint8_t>(kFramebuf));

    while (true) { asm volatile("nop"); }
}
```

**Checkpoint:** Build, flash, observe.

```sh
make w05-ex02.uf2
picotool load -f build/w05-ex02.uf2
```

The OLED should display `crunch-wire w05`. Capture a Saleae trace of the SPI bus during the init phase.

### 6. Diff the C++ disassembly against Week 4's C version

```sh
arm-none-eabi-size build/w05-ex02.elf > size-cpp.txt
arm-none-eabi-size ../week-04-spi-i2c-bus-drivers/exercises/ex01/build/w04-ex01.elf > size-c.txt
```

Compute the `.text` delta:

```sh
diff size-c.txt size-cpp.txt
```

**Checkpoint:** The `.text` section size in the C++ version is within 50 bytes of the C version. If it is more, find the culprit:

```sh
arm-none-eabi-nm --size-sort build/w05-ex02.elf | head -20
```

Top symbols should be `main`, `Ssd1306::init`, the SSD1306 framebuffer, and the SPI init code. If you see `__cxa_*`, `_ZTV*`, or `_Unwind_Resume`, you forgot a flag.

### 7. Capture the Saleae trace

Capture the SPI bus during the SSD1306 init phase. Confirm:
- 25 bytes are transmitted with CSn held low.
- The byte sequence matches the `kInit` array (0xAE, 0xD5, 0x80, ...).
- The clock is 1 MHz ± 5%.
- D/C̄ (GP20) is low throughout the init (command mode).

The Saleae capture is **byte-for-byte identical** to your Week 4 capture. Commit both files; the diff is the deliverable.

## Artifact

Commit to `exercises/ex02/`:

1. `spi_bus.hpp` — the `SpiBus` concept.
2. `spi0.hpp` — the `Spi0` concrete bus.
3. `spi_device.hpp` — the `SpiDevice<Bus, CsPin>` template.
4. `ssd1306.hpp` — the SSD1306 protocol driver.
5. `gpio.hpp` — from Exercise 1 (copy or symlink).
6. `main.cpp` — wires it together.
7. `Makefile` — builds the firmware.
8. `traces/spi-init-cpp.sal` — your Saleae capture.
9. `BENCHMARK.md` — a one-page report:
   - `.text` size for C version: NNNN bytes.
   - `.text` size for C++ version: NNNN bytes.
   - Delta: ±N bytes (must be ≤ 50).
   - Top 5 symbols by size in each build, side by side.
   - Confirmation that no `__cxa_*` symbols are present in the C++ build.
10. `README.md` — describe what you built, paste the disassembly excerpts for `Ssd1306::init` in C and C++ side by side.

## Common faults

| Symptom | Cause | First diagnostic |
|---|---|---|
| `static_assert(SpiBus<Spi0>, ...)` fails | `Spi0::xfer` signature does not match | Read the concept; ensure `xfer(uint8_t) -> uint8_t` |
| Build error: "no type named 'xfer' in 'Uart'" when using `SpiDevice<Uart, 17>` | The concept correctly rejected a non-SPI bus | Pass the right type; this is the concept *working* |
| OLED is dark | Charge pump not enabled | Confirm `0x8D, 0x14` is in `kInit` and is sent during init |
| Saleae shows only 24 bytes, not 25 | Off-by-one in `.first(25)` | Recount; the SSD1306 init in Lecture 1 is 25 commands (last byte 0xAF) |
| `.text` is 2 KB larger than the C version | Forgot a `-fno-*` flag | `arm-none-eabi-nm --size-sort build/w05-ex02.elf | head -20` — top symbol is `__gxx_personality_v0` or similar |
| Disassembly of `Ssd1306::init` includes `bl __cxa_*` | A `static` local with non-trivial initializer | Replace with `static constexpr` |
| CSn does not toggle around each `write` call | Forgot the GPIO setup; CS pin not configured as output | Run `enable_gpio_outputs` before the `Ssd1306` constructor |

## Stretch goals

- Replace `SpiDevice::write_read`'s polling with DMA pacing (RP2040 datasheet §2.5, p. 87 and §4.4.3.5, p. 525). Configure DMA channel 0 to push from `kFramebuf` into `SSPDR` paced by `DREQ_SPI0_TX`. Measure CPU load on a GP toggle: should drop from "pegged during framebuffer flush" to "idle within 50 µs".
- Add a `consteval` function `compute_spi_dividers(uint32_t clk_peri, uint32_t target_hz)` that returns a `struct { uint16_t cpsdvsr; uint8_t scr; }`. Use it to validate the 1 MHz target at compile time. Build will fail if the target is unreachable.
- Implement a `[[nodiscard]] etl::expected<int, SpiError>` return for `SpiDevice::write` instead of `int`. Add an `SpiError` enum class with `TX_TIMEOUT`, `RX_TIMEOUT`. Verify the binary delta is < 200 bytes.
- Read the LLVM blog "Polymorphism Without Virtual Functions" (linked in resources.md) and reimplement `SpiDevice` using the policy-based design pattern instead of CRTP. Compare the binary sizes.

## Hand-in

Push to GitHub, tag `w05-ex02`, when:
- The firmware flashes and renders `crunch-wire w05` on the OLED.
- The `BENCHMARK.md` shows `.text` delta ≤ 50 bytes vs Week 4 Exercise 1's C build.
- The Saleae capture is committed.
- `arm-none-eabi-nm build/w05-ex02.elf | grep -E '__cxa_|_Unwind_|_ZTV'` returns empty.
- The reviewer signs off on the `README.md`.
