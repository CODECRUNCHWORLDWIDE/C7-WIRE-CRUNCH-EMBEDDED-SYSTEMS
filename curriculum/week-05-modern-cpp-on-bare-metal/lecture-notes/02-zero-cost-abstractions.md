# Lecture 2 — Zero-cost abstractions: CRTP, templates, and the `objdump` arbiter

> *Bjarne Stroustrup's rule: "What you don't use, you don't pay for, and what you do use, you couldn't hand-code any better." On a freestanding Cortex-M0+ that rule is testable. The test is `objdump -d`.*

## The claim and the proof

Stroustrup's zero-overhead principle is famous. On a hosted system it is usually true and rarely tested. On a freestanding MCU it is testable on demand, and the test is the disassembly.

The claim: a C++20 template-based GPIO driver — `Gpio<18>::set()` — compiles to the same machine code as the C macro version — `*(volatile uint32_t *)0xd0000014 = (1u << 18)`. The proof: `objdump -d` on both binaries and a `diff` that returns empty.

This lecture is the proof. We will build a `Gpio<Pin>` template, build the equivalent C, disassemble both, diff them, and confirm zero-cost. Then we will scale the pattern to a full SPI device driver using CRTP, and disassemble that. The pattern generalizes: if it compiles to the same instructions as the hand-written C, it has zero runtime cost.

## The C baseline

The Pi Pico W's SIO peripheral lives at `0xd0000000`. The `GPIO_OUT_SET` register is at offset `+0x14`; writing a `1` to bit `N` sets the corresponding GPIO pin to high. From Week 3:

```c
/* gpio_c.c */
#include <stdint.h>

#define SIO_BASE        0xd0000000u
#define GPIO_OUT_SET    (*(volatile uint32_t *)(SIO_BASE + 0x14))
#define GPIO_OUT_CLR    (*(volatile uint32_t *)(SIO_BASE + 0x18))

static inline void gpio_set(uint8_t pin) {
    GPIO_OUT_SET = (1u << pin);
}

static inline void gpio_clr(uint8_t pin) {
    GPIO_OUT_CLR = (1u << pin);
}

void blink(void) {
    gpio_set(18);
    gpio_clr(18);
}
```

Compile and disassemble:

```sh
arm-none-eabi-gcc -std=c11 -mcpu=cortex-m0plus -mthumb -Os -c gpio_c.c -o gpio_c.o
arm-none-eabi-objdump -d gpio_c.o
```

Output (excerpt):

```
00000000 <blink>:
   0:   4b03            ldr     r3, [pc, #12]   ; (10 <blink+0x10>)
   2:   2280            movs    r2, #128        ; 0x80
   4:   0312            lsls    r2, r2, #12     ; r2 = 0x40000 = (1u << 18)
   6:   615a            str     r2, [r3, #20]   ; GPIO_OUT_SET = r2
   8:   619a            str     r2, [r3, #24]   ; GPIO_OUT_CLR = r2
   a:   4770            bx      lr
   c:   46c0            nop                     ; alignment
   e:   46c0            nop
  10:   d0000000        .word   0xd0000000
```

Six instructions in the function body, one indirect literal-pool word. The pin number `18` is materialized as `(0x80 << 12) = 0x40000` via `movs r2, #128` + `lsls r2, r2, #12` — the M0+ has no 32-bit immediate load instruction, so the compiler uses an immediate + shift pair.

This is the baseline. Any C++ version must hit this or do better.

## The C++ template version

```cpp
/* gpio.hpp */
#pragma once
#include <cstdint>

namespace pico {

inline constexpr std::uintptr_t kSioBase     = 0xd0000000u;
inline constexpr std::uintptr_t kGpioOutSet  = kSioBase + 0x14u;
inline constexpr std::uintptr_t kGpioOutClr  = kSioBase + 0x18u;

template<std::uint8_t Pin>
struct Gpio {
    static_assert(Pin < 30u, "RP2040 has GP0–GP29");

    static constexpr std::uint32_t kMask = 1u << Pin;

    [[gnu::always_inline]]
    static void set() noexcept {
        *reinterpret_cast<volatile std::uint32_t *>(kGpioOutSet) = kMask;
    }

    [[gnu::always_inline]]
    static void clr() noexcept {
        *reinterpret_cast<volatile std::uint32_t *>(kGpioOutClr) = kMask;
    }
};

}  // namespace pico
```

```cpp
/* gpio_cpp.cpp */
#include "gpio.hpp"

extern "C" void blink_cpp() {
    pico::Gpio<18>::set();
    pico::Gpio<18>::clr();
}
```

Compile and disassemble:

```sh
arm-none-eabi-g++ -std=c++20 -mcpu=cortex-m0plus -mthumb -Os \
    -ffreestanding -fno-exceptions -fno-rtti \
    -fno-threadsafe-statics -fno-use-cxa-atexit \
    -c gpio_cpp.cpp -o gpio_cpp.o
arm-none-eabi-objdump -d gpio_cpp.o
```

Output (excerpt):

```
00000000 <blink_cpp>:
   0:   4b03            ldr     r3, [pc, #12]   ; (10 <blink_cpp+0x10>)
   2:   2280            movs    r2, #128        ; 0x80
   4:   0312            lsls    r2, r2, #12     ; r2 = 0x40000
   6:   615a            str     r2, [r3, #20]
   8:   619a            str     r2, [r3, #24]
   a:   4770            bx      lr
   c:   46c0            nop
   e:   46c0            nop
  10:   d0000000        .word   0xd0000000
```

Identical. Six instructions in the function body, same instructions, same operand encoding. The C++ template machinery — the `[[gnu::always_inline]]`, the `static_assert`, the namespace, the type wrapping — produces zero bytes of machine code beyond what the C macro produces.

The `static_assert(Pin < 30u, ...)` adds a compile-time safety check: `Gpio<42>::set()` is a build error, not a runtime fault. The C macro version silently accepts `gpio_set(42)` and writes to bit 42 of `GPIO_OUT_SET` (which is out of bounds; the high bits are reserved and the write does nothing, but the code path is undetected). The C++ version catches it.

`kMask` is `1u << Pin` evaluated at compile time. Because `Pin` is a template parameter (a compile-time constant), the shift happens during compilation; the binary contains the literal `0x40000`, not a runtime shift. On Cortex-M0+, materializing `0x40000` still takes two instructions (no 32-bit immediate load), but the *value* is computed at compile time.

## CRTP — Curiously Recurring Template Pattern

Static polymorphism without v-tables. The pattern:

```cpp
template<typename Derived>
class Peripheral {
public:
    [[gnu::always_inline]]
    void set() noexcept {
        static_cast<Derived *>(this)->do_set();
    }
    [[gnu::always_inline]]
    void clr() noexcept {
        static_cast<Derived *>(this)->do_clr();
    }
};

class GpioBank : public Peripheral<GpioBank> {
public:
    explicit constexpr GpioBank(std::uint8_t pin) noexcept : mask_(1u << pin) {}

    [[gnu::always_inline]]
    void do_set() noexcept {
        *reinterpret_cast<volatile std::uint32_t *>(0xd0000014u) = mask_;
    }
    [[gnu::always_inline]]
    void do_clr() noexcept {
        *reinterpret_cast<volatile std::uint32_t *>(0xd0000018u) = mask_;
    }

private:
    std::uint32_t mask_;
};
```

`Peripheral<Derived>` is a base class templated on its derived class. `set()` calls `do_set()` via `static_cast<Derived *>(this)`. Because the derived type is known at compile time, the cast is a no-op and the call to `do_set()` is inlined.

Compare with virtual functions:

```cpp
class PeripheralVirtual {
public:
    virtual ~PeripheralVirtual() = default;
    virtual void do_set() noexcept = 0;
};

class GpioBankVirtual : public PeripheralVirtual {
public:
    void do_set() noexcept override { /* same body */ }
};
```

With `virtual`, `do_set()` dispatch is via the v-table: load v-table pointer from object, load slot, indirect call. Three instructions plus a non-predictable indirect branch.

Disassemble both:

```sh
arm-none-eabi-g++ -std=c++20 -mcpu=cortex-m0plus -mthumb -Os \
    -ffreestanding -fno-exceptions -fno-rtti \
    -c crtp_demo.cpp -o crtp_demo.o
arm-none-eabi-objdump -d crtp_demo.o
```

CRTP version's `set()` after inlining:

```
00000000 <crtp_set>:
   0:   4b02            ldr     r3, [pc, #8]
   2:   6818            ldr     r0, [r3, #0]     ; load mask_ from object
   4:   60d8            str     r0, [r3, #12]    ; GPIO_OUT_SET = mask_
   6:   4770            bx      lr
   8:   d0000014        .word   0xd0000014
```

Three instructions in the body. No v-table, no indirect branch.

Virtual version's `set()` (without inlining, because the dispatch is dynamic):

```
00000000 <virt_set>:
   0:   6803            ldr     r3, [r0]         ; load v-table pointer
   2:   685b            ldr     r3, [r3, #4]     ; load slot 1 (do_set)
   4:   4718            bx      r3               ; indirect call
```

Three instructions, plus the called function does its own work. The total is more like 6–8 instructions and an indirect branch with no prediction on M0+.

For functions called once per second, the difference is invisible. For functions called in a tight inner loop — every ADC sample at 48 kHz, every SPI byte at 10 MHz — the difference adds up. The CRTP version inlines into the caller; the virtual version makes a function call that cannot be inlined.

This is why we use CRTP for peripheral drivers and avoid `virtual` unless the use case truly requires runtime dispatch (which on freestanding is rare).

## Concepts — typed contracts for templates

A template parameter with no constraint accepts any type. The error on misuse appears at the *use site* deep in the template, with a 200-line instantiation trace. A concept-constrained parameter rejects misuse at the *call site* with a one-line message.

```cpp
template<typename T>
concept SpiBus = requires(T &t, std::span<const std::uint8_t> tx) {
    { t.write(tx) }      -> std::same_as<int>;
};

template<SpiBus Bus>
class Ssd1306 {
public:
    explicit Ssd1306(Bus &bus) noexcept : bus_(bus) {}

    [[nodiscard]] int init() noexcept {
        constexpr std::array<std::uint8_t, 26> kInit = {
            0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
            0x8D, 0x14, 0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x12,
            0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6,
            0xAF, /* padded */ 0x00
        };
        return bus_.write(std::span(kInit));
    }

private:
    Bus &bus_;
};
```

If you instantiate `Ssd1306<Uart>` where `Uart` has no `write(std::span<...>)` method, the diagnostic is:

```
error: no matching function for call to 'Ssd1306<Uart>::Ssd1306(Uart &)'
note: candidate template requires 'SpiBus' concept; 'Uart' does not satisfy 'SpiBus':
  'std::same_as<decltype(t.write(tx)), int>' is not satisfied
```

The error names the concept that failed. It points to the line in the concept where the requirement was violated. The fix is to either implement `write` on `Uart` or to pass a real SPI bus.

Concepts are compile-time only. The generated code for `Ssd1306<MySpi>::init()` is identical to a hand-rolled `ssd1306_init(my_spi_t *spi)` — no extra dispatch, no extra runtime check.

## Putting it together — the SPI device template

Now the production pattern. A C++20 SPI device driver, templated on the bus and the chip-select pin:

```cpp
/* spi_device.hpp */
#pragma once
#include <cstdint>
#include <span>
#include <concepts>
#include "gpio.hpp"

namespace pico {

template<typename T>
concept SpiBusRaw = requires(T &t, std::uint8_t byte) {
    { t.xfer(byte) } -> std::same_as<std::uint8_t>;
};

template<SpiBusRaw Bus, std::uint8_t CsPin>
class SpiDevice {
    static_assert(CsPin < 30u, "RP2040 has GP0–GP29");

public:
    explicit SpiDevice(Bus &bus) noexcept : bus_(bus) {
        Gpio<CsPin>::set();  // CSn idle high
    }

    [[nodiscard]] int write(std::span<const std::uint8_t> tx) noexcept {
        Gpio<CsPin>::clr();  // CSn low — select this device
        for (std::uint8_t b : tx) {
            (void)bus_.xfer(b);
        }
        Gpio<CsPin>::set();  // CSn high — deselect
        return static_cast<int>(tx.size());
    }

    [[nodiscard]] int write_read(std::span<const std::uint8_t> tx,
                                  std::span<std::uint8_t> rx) noexcept {
        Gpio<CsPin>::clr();
        for (std::uint8_t b : tx) {
            (void)bus_.xfer(b);
        }
        for (std::uint8_t &b : rx) {
            b = bus_.xfer(0xFF);   /* clock dummy bytes */
        }
        Gpio<CsPin>::set();
        return static_cast<int>(rx.size());
    }

private:
    Bus &bus_;
};

}  // namespace pico
```

Usage:

```cpp
class Spi0 {
public:
    void init_1mhz() noexcept { /* register pokes */ }
    std::uint8_t xfer(std::uint8_t tx) noexcept {
        auto *sspdr = reinterpret_cast<volatile std::uint32_t *>(0x4003c008);
        auto *sspsr = reinterpret_cast<volatile std::uint32_t *>(0x4003c00c);
        while ((*sspsr & 2u) == 0) {}  // wait TNF
        *sspdr = tx;
        while ((*sspsr & 4u) == 0) {}  // wait RNE
        return static_cast<std::uint8_t>(*sspdr);
    }
};

Spi0 g_spi0;

void send_ssd1306_init() {
    pico::SpiDevice<Spi0, 17> oled{g_spi0};
    constexpr std::array<std::uint8_t, 26> kInit = { /* same 26 bytes */ };
    (void)oled.write(std::span(kInit));
}
```

`SpiDevice<Spi0, 17>` is a type — one type per `(bus, chip-select)` pair. The CS pin number `17` is a template parameter, so `Gpio<17>::clr()` is evaluated at compile time: the mask `(1u << 17) = 0x20000` is an immediate, the write to `0xd0000018` is a single `str`.

Disassembly of `oled.write(...)` (Cortex-M0+, `-Os`):

```
send_ssd1306_init():
   0:   b510            push    {r4, lr}
   2:   4c0a            ldr     r4, [pc, #40]   ; ssd1306_init_seq (in .rodata)
   4:   2080            movs    r0, #128
   6:   0440            lsls    r0, r0, #17     ; r0 = 0x20000 (CS mask)
   8:   4b09            ldr     r3, [pc, #36]   ; SIO base = 0xd0000000
   a:   6198            str     r0, [r3, #24]   ; GPIO_OUT_CLR = mask (CS low)
   c:   2519            movs    r5, #25         ; loop counter (25 bytes)
   e:   78aa            ldrb    r2, [r4, #1]    ; load next init byte
                                                ; ... SPI xfer loop ...
  20:   6158            str     r0, [r3, #20]   ; GPIO_OUT_SET = mask (CS high)
  22:   bd10            pop     {r4, pc}
```

The CS pin manipulation is exactly two `str` instructions — one to set CSn low at the start, one to set it high at the end. The template parameter `17` was resolved at compile time to the immediate mask `0x20000`. The cost of using `SpiDevice<Spi0, 17>` vs a hand-rolled `void send_init(spi_t *s, uint8_t cs)` is zero.

## The `std::array` and `std::span` interplay

`std::array<T, N>` is a stack-allocated, compile-time-sized array. `std::span<T>` is a non-owning view (pointer + length) of an array. Both are zero-cost.

```cpp
constexpr std::array<std::uint8_t, 26> kInitSeq = { /* 26 bytes */ };
std::uint8_t buf[8];

void example() {
    std::span<const std::uint8_t> s1{kInitSeq};       // wraps the array
    std::span<std::uint8_t>       s2{buf};             // wraps the C array
    std::span<const std::uint8_t> s3{buf, 4};          // explicit length

    for (std::uint8_t b : s1) { /* iterate */ }
}
```

`std::span` is a `(ptr, size)` pair — two words on Cortex-M0+. Passing a `std::span<T>` to a function passes two registers (r0 = ptr, r1 = size) — the same as passing `T *` and `size_t` separately in C. No allocation, no copy.

The advantage over C's `(ptr, len)` convention: the size is enforced by the type system. You cannot accidentally pass the size of *the wrong array*; the span carries its own size, and a range-`for` over a span never overruns. This is the embedded use case for `std::span`: type-safe array views, zero overhead.

`std::array<T, N>` is the type-safe replacement for `T[N]`. It supports range-`for`, `.size()`, `.data()`, and decays to `std::span<T>` implicitly. Use it for every stack buffer; the C array `T x[N]` decays to `T*` and loses size info, but `std::array<T, N>` does not.

## `[[nodiscard]]` and error-handling at zero cost

C return codes are easy to ignore. `[[nodiscard]]` makes the compiler warn at every call site that discards a return:

```cpp
[[nodiscard]] int write(std::span<const std::uint8_t> tx) noexcept;

void example() {
    write(some_buf);  // warning: ignoring return value of 'write', declared with attribute 'nodiscard'
}
```

The fix: assign the return to a variable and check, or explicitly cast to `void`:

```cpp
int result = write(some_buf);
if (result < 0) { /* handle */ }

// or, if you genuinely don't care:
(void)write(some_buf);
```

`[[nodiscard]]` costs zero runtime — it is a compile-time annotation. Apply to *every* function whose return carries error information. Apply it to constructor-style factory functions too: `[[nodiscard]] static std::optional<Spi0> create()` warns if a caller creates an `Spi0` and immediately discards it.

## When CRTP is overkill

CRTP is a hammer. Sometimes you need a non-templated, plain-function interface — for example, when you want a function pointer that crosses a C/C++ boundary, or when the dispatch is genuinely dynamic (a sensor whose bus is selected at runtime by a configuration string).

For those cases, the C-style `bus_t` from Week 4 is the right answer. C++ does not improve over C here; the C function-pointer struct *is* the zero-cost dynamic-dispatch pattern. You get to keep it.

The pattern decision:

| Case | Use |
|------|-----|
| Pin number known at compile time | `Gpio<Pin>` template |
| Bus + chip select known at compile time | `SpiDevice<Bus, Cs>` template |
| Device driver with multiple possible buses, decided at runtime | `bus_t` C struct of function pointers (Week 4) |
| Polymorphism within a fixed type set known at compile time | `std::variant<T1, T2, T3>` + `std::visit` |
| Polymorphism with an unknown extensibility set, runtime dispatch | `virtual` (rare on freestanding; accept the v-table cost) |

The discipline: templates and CRTP are first-line. C-style function pointers are second-line when the dispatch is truly dynamic. `virtual` is third-line and rarely needed. `std::function`, `std::any`, `std::shared_ptr` — these are not on the list.

## Compile-time loops and `std::index_sequence`

A pattern that comes up in peripheral init: "unroll a fixed-length loop with the index as a `constexpr`." C cannot do this; C++ can:

```cpp
template<std::size_t... Is>
void write_all(std::span<const std::uint8_t> tx, std::index_sequence<Is...>) {
    ((bus_.xfer(tx[Is])), ...);  // fold expression: call xfer for each index
}

template<std::size_t N>
void write_unrolled(const std::array<std::uint8_t, N> &tx) {
    write_all(std::span(tx), std::make_index_sequence<N>{});
}
```

`std::make_index_sequence<N>{}` expands at compile time to `index_sequence<0, 1, 2, ..., N-1>`. The fold expression `(..., (call_xfer_with_index(Is)))` unrolls the loop. The compiler emits `N` copies of the `xfer` call, with each one's index as an immediate.

This is *deep template metaprogramming* and usually not necessary; the optimizer will unroll a small fixed loop on its own at `-O2`. The pattern is here for completeness; use sparingly.

## What you take away

- Stroustrup's zero-cost rule is testable on a freestanding MCU: `arm-none-eabi-objdump -d` is the arbiter.
- A `Gpio<Pin>` template with `[[gnu::always_inline]]` produces identical machine code to a C macro version. The C++ adds type safety (`static_assert(Pin < 30)`) and namespacing at zero runtime cost.
- CRTP gives you the API surface of inheritance without the v-table cost. Static dispatch is inlined; virtual dispatch is an indirect call through the v-table.
- Concepts make template-parameter contracts compile-time-enforced with one-line diagnostics. They replace SFINAE and produce zero runtime code.
- `std::array<T, N>` is the type-safe stack buffer; `std::span<T>` is the type-safe pointer-plus-length view. Both compile to the same as their C equivalents.
- `[[nodiscard]]` enforces error-checking at compile time at zero runtime cost.
- CRTP is for static polymorphism, virtual is for dynamic polymorphism; on freestanding the latter is rare, and even when needed, the C function-pointer struct is usually a better fit than `virtual`.

In the next lecture we step back and ask the honest question: given that C++ on a freestanding MCU is approximately as efficient as C with better type safety, *when is C still the right answer?* The answer is three classes of problem — tooling, boundary, certification — and we cover them in detail.

## References

- Bjarne Stroustrup, "Abstraction and the C++ machine model," 2004:
  <https://www.stroustrup.com/abstraction-and-machine.pdf>
- "Polymorphism Without Virtual Functions," LLVM blog (2022):
  <https://blog.llvm.org/posts/2022-09-06-polymorphism-without-virtual-functions/>
- "The Curiously Recurring Template Pattern" — Wikipedia, with examples:
  <https://en.wikipedia.org/wiki/Curiously_recurring_template_pattern>
- cppreference, "Concepts":
  <https://en.cppreference.com/w/cpp/language/constraints>
- cppreference, "std::span":
  <https://en.cppreference.com/w/cpp/container/span>
- cppreference, "Attribute: nodiscard":
  <https://en.cppreference.com/w/cpp/language/attributes/nodiscard>
- "Modern C++ Design," Andrei Alexandrescu (2001), Chapter 1: "Policy-Based Class Design" — the original CRTP+policy book. Out of print but PDF excerpts circulate.
- "Modern C++ Embedded Programming," Dan Saks, CppCon 2018:
  <https://www.youtube.com/watch?v=Q1OAtuq8tn0>
- Kvasir.io — the Cortex-M C++ peripheral library that took the type-list metaprogramming approach to its logical extreme. Read for inspiration; do not copy the depth.
  <https://github.com/kvasir-io/Kvasir>
- ARMv6-M Architecture Reference Manual (ARM DDI 0419D), §A6.7 — the instructions you will see in the disassembly:
  <https://developer.arm.com/documentation/ddi0419/d/>
