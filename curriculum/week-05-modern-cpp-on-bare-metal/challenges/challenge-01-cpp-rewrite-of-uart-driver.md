# Challenge 1 — C++ rewrite of the UART driver

> *Re-implement your Week 3 UART driver in C++20. Use a `Uart<Instance, Baud>` template. Produce a Saleae trace identical to the C version, and a `.text` size within 32 bytes. Document any deltas in `BENCHMARK.md`.*

## Goal

Take the polled UART driver you wrote in Week 3 (the one that prints `"crunch-wire w03 boot ok"` over UART0 at 115200 baud) and re-implement it as a C++20 templated class. The template parameters are the UART instance (`Uart0`, `Uart1`) and the baud rate. Compile, flash, scope UART0 on a Saleae or USB-TTL converter, confirm the bytes are identical to Week 3, and report the `.text` size delta in a `BENCHMARK.md`.

Production-grade bar: the C++ template must also pass a host-side unit test that verifies the baud-rate divider arithmetic at compile time. If the requested baud is unreachable with the available `clk_peri`, the build must fail with a `static_assert` message naming the cause.

By the end you can:
- Author a `Uart<Instance, Baud>` template with `constexpr` divider arithmetic.
- Use `consteval` to compute the integer and fractional baud-rate divider parts (per RP2040 §4.2.7.1, p. 423) at compile time.
- Fail the build (not the runtime) when an impossible baud is requested.
- Diff the `.text` size of the C version against the C++ version and report within 32 bytes.
- Capture a Saleae trace at 115200 baud and confirm it is byte-identical to the C version.

## Setup

### Parts

- 1× Pi Pico W
- 1× USB-TTL converter (e.g. FT232, CP2102) OR a logic analyzer with serial decoder
- 1× breadboard, 3× jumper wires (GP0 → RX of converter, GP1 ← TX, GND)

### Prerequisites

- Week 3 mini-project — your C version of the UART driver works.
- Exercises 1 and 2 of this week.
- Lectures 1 and 2.

## Reading

- **RP2040 datasheet** §4.2 (UART), pp. 412–431. Specifically §4.2.7.1 (Baud Rate Divisor), p. 423 — the integer/fractional divider equation.
- **ARM PL011 UART Technical Reference Manual** (ARM DDI 0183G) — the IP block underneath the RP2040 UART. Same register names as the RP2040 datasheet.
- **Lecture 1** of this week — `consteval` for compile-time arithmetic.
- **Lecture 2** of this week — `[[gnu::always_inline]]` and the disassembly arbiter.

## Steps

### 1. Recall the C baseline

Your Week 3 `uart0_init_115200` and `uart0_putc` from `uart.c` are the starting point. Get the `.text` size:

```sh
arm-none-eabi-size build/uart.o
```

Note the number. This is the C target you must match.

### 2. Design the template signature

```cpp
template<std::uint8_t Instance,    // 0 = UART0, 1 = UART1
          std::uint32_t Baud,
          std::uint32_t ClkPeri = 125'000'000u>
class Uart;
```

Three parameters: instance, baud, clock. The clock has a default of 125 MHz (the Pi Pico's `clk_peri` after the SDK boot, though we set it ourselves in our SDK-free build).

### 3. Compute the divider with `consteval`

Per RP2040 §4.2.7.1, p. 423:

```
baud_rate_div = clk_peri / (16 × baud_rate)
ibrd = floor(baud_rate_div)           (16-bit integer part)
fbrd = round((baud_rate_div - ibrd) × 64)   (6-bit fractional part)
```

Constraints:
- `baud_rate_div` must be ≥ 1.0 (otherwise the divider underflows).
- `ibrd` must fit in 16 bits.
- If `fbrd == 64`, increment `ibrd` and set `fbrd = 0`.

`consteval` version:

```cpp
struct UartDivider {
    std::uint16_t ibrd;
    std::uint8_t  fbrd;
};

consteval UartDivider compute_uart_divider(std::uint32_t clk_peri,
                                            std::uint32_t baud) {
    if (baud == 0) {
        throw "baud must be > 0";
    }
    const std::uint32_t baud_x16 = baud * 16u;
    if (clk_peri < baud_x16) {
        throw "baud > clk_peri / 16: impossible";
    }
    const std::uint32_t baud_rate_div_q6 =
        (clk_peri * 4u + (baud_x16 >> 1)) / (baud_x16 >> 4);
    // baud_rate_div_q6 is the divider in Q26.6 format (after some algebra).
    // Extract integer and fractional parts:
    const std::uint32_t ibrd = baud_rate_div_q6 >> 6;
    const std::uint32_t fbrd = baud_rate_div_q6 & 0x3Fu;
    if (ibrd > 0xFFFF) {
        throw "ibrd > 16 bits";
    }
    return { static_cast<std::uint16_t>(ibrd),
             static_cast<std::uint8_t>(fbrd) };
}
```

Note: the exact divider arithmetic in the RP2040 datasheet involves a careful rounding step; we approximate here. For production use, lift the arithmetic from the Pi Pico SDK's `hardware_uart/uart.c` (the `uart_set_baudrate` function, ~30 lines) and convert it to `consteval`.

### 4. Build the `Uart` template

```cpp
template<std::uint8_t Instance,
          std::uint32_t Baud,
          std::uint32_t ClkPeri = 125'000'000u>
class Uart {
    static_assert(Instance < 2u, "RP2040 has UART0 and UART1");
    static_assert(Baud >= 110u && Baud <= 5'000'000u,
                   "baud out of supported range");

    static constexpr UartDivider kDiv = compute_uart_divider(ClkPeri, Baud);

    static constexpr std::uintptr_t kBase =
        (Instance == 0) ? 0x40034000u : 0x40038000u;
    static constexpr std::uintptr_t kDr    = kBase + 0x00u;
    static constexpr std::uintptr_t kFr    = kBase + 0x18u;
    static constexpr std::uintptr_t kIbrd  = kBase + 0x24u;
    static constexpr std::uintptr_t kFbrd  = kBase + 0x28u;
    static constexpr std::uintptr_t kLcrH  = kBase + 0x2Cu;
    static constexpr std::uintptr_t kCr    = kBase + 0x30u;

public:
    [[gnu::always_inline]]
    static void init() noexcept {
        // RESETS and IO_BANK0 omitted — assumed done by caller.
        reg(kIbrd) = kDiv.ibrd;
        reg(kFbrd) = kDiv.fbrd;
        reg(kLcrH) = (3u << 5) | (1u << 4);   // 8-bit, FIFO enable
        reg(kCr)   = (1u << 0) | (1u << 8) | (1u << 9);  // UARTEN | TXE | RXE
    }

    [[gnu::always_inline]]
    static int putc(char c) noexcept {
        while ((reg(kFr) & (1u << 5)) != 0u) {}   // wait FIFO not full
        reg(kDr) = static_cast<std::uint8_t>(c);
        return c;
    }

    [[gnu::always_inline]]
    static int puts(std::string_view s) noexcept {
        for (char c : s) {
            (void)putc(c);
        }
        return static_cast<int>(s.size());
    }

private:
    [[gnu::always_inline]]
    static volatile std::uint32_t &reg(std::uintptr_t addr) noexcept {
        return *reinterpret_cast<volatile std::uint32_t *>(addr);
    }
};
```

The `kDiv` is computed once at compile time. The `kBase` is chosen by the `Instance` parameter at compile time. Both compile to immediates in the generated code.

### 5. Verify compile-time failure on impossible baud

Add to your test source:

```cpp
extern "C" void test_impossible() {
    Uart<0, 50'000'000>::init();  // 50 MBaud > 125 MHz / 16 = 7.8 MBaud — should fail
}
```

Compile. Expected error:

```
error: 'compute_uart_divider' called in constant expression
note: in evaluation of consteval call
note: the constraint 'baud > clk_peri / 16: impossible' is violated
```

**Checkpoint:** the build fails. The C version would silently configure the UART with an invalid divider; the runtime symptom would be garbage on the wire.

Remove `test_impossible` after verifying.

### 6. Build and flash

```sh
arm-none-eabi-g++ -std=c++20 -mcpu=cortex-m0plus -mthumb -Os \
    -ffreestanding -fno-exceptions -fno-rtti \
    -fno-threadsafe-statics -fno-use-cxa-atexit \
    -fno-unwind-tables -fno-asynchronous-unwind-tables \
    -nostdlib -nostartfiles \
    boot2_w25q080.S startup.c uart_cpp.cpp main.cpp \
    -T pico.ld -o build/w05-ch01.elf

arm-none-eabi-objcopy -O binary build/w05-ch01.elf build/w05-ch01.bin
picotool uf2 convert -f build/w05-ch01.bin build/w05-ch01.uf2 --offset 0x10000000
picotool load -f build/w05-ch01.uf2
```

`main.cpp`:

```cpp
#include "uart.hpp"

extern "C" int main() {
    enable_gpio_outputs();
    Uart<0, 115200>::init();
    Uart<0, 115200>::puts("crunch-wire w05 ch01 boot ok\r\n");
    while (true) {}
}
```

**Checkpoint:** open a serial terminal:

```sh
screen /dev/tty.usbmodem* 115200
```

You should see `crunch-wire w05 ch01 boot ok` exactly once. Verify with a Saleae or USB-TTL capture that the byte sequence is identical to Week 3.

### 7. Diff `.text` sizes and write `BENCHMARK.md`

```sh
arm-none-eabi-size build/w05-ch01.elf > size-cpp.txt
arm-none-eabi-size ../../week-03-linker-scripts-and-startup/mini-project/build/w03-mini.elf > size-c.txt
diff size-c.txt size-cpp.txt
```

Expected delta: < 32 bytes. Write `BENCHMARK.md`:

```markdown
# Week 5 Challenge 1 — UART C vs C++

## Sizes (Cortex-M0+, -Os)

| Build                                     | .text bytes | .data bytes | .bss bytes |
|-------------------------------------------|-------------|-------------|------------|
| Week 3 C version (`uart.o`)               | 412         | 0           | 0          |
| Week 5 Challenge 1 C++ template version   | 416         | 0           | 0          |
| Delta                                     | +4 bytes    | 0           | 0          |

## Disassembly diff

Both `uart0_init_115200` (C) and `Uart<0, 115200>::init` (C++) compile to the same 8-instruction sequence. Disassembly attached at `disasm-c.dis` and `disasm-cpp.dis`. The diff (after symbol-stripping) is empty.

## Compile-time validation

`Uart<0, 50'000'000>::init()` fails to compile with the message `"baud > clk_peri / 16: impossible"`. The equivalent C call `uart0_init_baud(50000000)` compiles silently and produces garbage on the wire.

## Saleae trace

Captured at 115200 baud. Byte sequence: `c`, `r`, `u`, `n`, `c`, `h`, `-`, `w`, `i`, `r`, `e`, ` `, `w`, `0`, `5`, ` `, `c`, `h`, `0`, `1`, ` `, `b`, `o`, `o`, `t`, ` `, `o`, `k`, `\r`, `\n`. Matches Week 3 exactly. Trace committed at `traces/uart-c-vs-cpp.sal`.
```

## Artifact

Commit to `challenges/ch01/`:

1. `uart.hpp` — the C++ template.
2. `main.cpp` — uses `Uart<0, 115200>`.
3. `Makefile` — builds the firmware.
4. `disasm-c.dis` and `disasm-cpp.dis` — disassemblies of the C version (Week 3) and C++ version.
5. `traces/uart-c-vs-cpp.sal` — Saleae or USB-TTL capture.
6. `BENCHMARK.md` — the comparison report.
7. `static-assert-check.txt` — captured compile failure for `Uart<0, 50'000'000>`.
8. `README.md` — describe what you built.

## Common faults

| Symptom | Cause | First diagnostic |
|---|---|---|
| `consteval` call refuses to compile even with a literal `Baud` | `throw` in a `consteval` is treated as a runtime statement before C++23 — use `if (...) { /* invalid expr */ }` or wrap in `static_assert` instead | Replace `throw "msg"` with `static_assert(false, "msg")` inside an `if constexpr (...)` |
| `.text` is 200 bytes larger than C | Forgot `-fno-threadsafe-statics`; the local `static constexpr UartDivider kDiv` pulled `__cxa_guard_*` | `arm-none-eabi-nm build/w05-ch01.elf \| grep __cxa_guard` |
| Saleae shows bytes at the wrong baud | `compute_uart_divider` arithmetic is wrong; cross-check against the Pi Pico SDK's `uart_set_baudrate` | Run the divider math in Python; compare to the SDK output |
| Build error: `static_assert(false, ...)` triggers unconditionally | C++20 does not allow `static_assert(false)` in a template *unless* the template is instantiated; you need `static_assert(always_false<Baud>::value, "...")` to guard | Use a dependent-false pattern |

## Stretch goals

- Add a `Uart<>::printf` method using `{fmt}` or a hand-rolled compile-time format-string parser. Measure the binary delta.
- Add interrupt-driven TX: a ring buffer plus a UART TX interrupt that drains it. Compare CPU load on a GP toggle: should drop from "busy during a 30-byte send" to "idle after enqueue."
- Implement a `consteval`-checked custom format string for a `Uart::log` method: `Uart<>::log<"baud={:d}, t={:d}ms">(baud, time_ms)`. Build fails if the format string doesn't match the argument types.
- Convert the entire Week 3 startup sequence (boot2 → vector table → `_start` → `__libc_init_array` shim → `main`) to use C++ constructors for global initialization. Confirm `arm-none-eabi-readelf -s` shows no `__cxa_atexit` references.

## Hand-in

Push to GitHub, tag `w05-ch01`, when:
- The firmware boots and emits the test string on UART0 at 115200 baud.
- The Saleae capture matches Week 3 byte-for-byte.
- The `.text` delta is < 32 bytes.
- `static-assert-check.txt` shows the impossible-baud build failure.
- The reviewer signs off on `BENCHMARK.md`.
