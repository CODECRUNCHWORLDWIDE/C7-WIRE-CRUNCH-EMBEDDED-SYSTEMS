# Lecture 1 — The C++20 embedded subset

> *C++ on an MCU is not a smaller C++. It is a subset of C++ chosen by removing every feature whose runtime cost is unbounded or unmeasurable. The remainder is sharper than C and as cheap.*

## The two halves of the language

C++20 is approximately 1800 pages of standard. We will use approximately 200 of those pages and ignore the rest. Drawing the line precisely is most of this lecture.

The half you use:

```
constexpr / consteval        templates                  CRTP
concepts / requires          std::array<T, N>           std::span<T>
std::optional<T>             std::bit_cast<T>(src)      enum class
[[nodiscard]]                 [[gnu::always_inline]]   non-virtual member fns
inline variables             constinit                  noexcept
non-capturing lambdas        scoped using declarations  trivial destructors
```

The half you do not use:

```
throw / try / catch          typeid / dynamic_cast      operator new (global)
std::function                std::string                std::vector
std::iostream                std::thread                std::shared_ptr
virtual functions            multiple inheritance       static locals with non-trivial dtors
exceptions (any form)         RTTI                      threadsafe-static guards
```

The half you do not use is not "bad C++." It is C++ that costs runtime — heap, indirect dispatch, unwinder tables, locale machinery — and the cost is not justifiable on a 256 KB-flash microcontroller. On a hosted system (Linux server, macOS desktop) these features are correct defaults. On a freestanding Cortex-M0+ they are a 50 KB tax for which you receive no value.

The discipline this week is to learn which feature belongs to which half, and to verify by disassembly that the half you use compiles to the same machine code as hand-written C.

## What "freestanding" means in C++20

ISO/IEC 14882:2020 §16.4.2.5 defines a *freestanding implementation* — a C++ implementation that runs without a host operating system. The standard explicitly enumerates which library headers a freestanding implementation must provide:

| Header                | Provided in freestanding? | Purpose                                                  |
|-----------------------|---------------------------|----------------------------------------------------------|
| `<cstddef>`           | YES                       | `size_t`, `ptrdiff_t`, `nullptr_t`                       |
| `<cstdint>`           | YES                       | `uint8_t`, `int32_t`, `uintptr_t`, etc.                  |
| `<climits>`           | YES                       | `INT_MAX`, etc.                                          |
| `<limits>`            | YES                       | `std::numeric_limits<T>`                                  |
| `<type_traits>`       | YES                       | `std::is_same_v`, `std::enable_if_t`, etc.               |
| `<concepts>`          | YES                       | `std::same_as`, `std::convertible_to`, etc.              |
| `<bit>`               | YES                       | `std::bit_cast`, `std::endian`, `std::countl_zero`       |
| `<atomic>`            | YES (subset)              | `std::atomic<T>` for lock-free types                     |
| `<utility>`           | YES                       | `std::move`, `std::forward`, `std::pair`, `std::swap`    |
| `<initializer_list>`  | YES                       | `std::initializer_list<T>`                                |
| `<exception>`         | YES (the type only)        | The `std::exception` type — no `try`/`catch` machinery   |
| `<typeinfo>`          | YES (the type only)        | The `std::type_info` type — `typeid` may be unimplemented |
| `<new>`               | YES (placement-new only)   | `void *operator new(std::size_t, void *) noexcept`       |
| **NOT freestanding:** | NO                        | `<iostream>`, `<vector>`, `<string>`, `<map>`, `<thread>`, `<chrono>`, `<filesystem>` |

The set "freestanding library" is small but non-empty. C++20 added `<concepts>`, `<bit>`, and several others to it relative to C++17. C++23 expanded it further (most of `<chrono>` is freestanding in C++23, and parts of `<format>` are as well).

The practical consequence: you may freely `#include <cstdint>` and `#include <span>` and `#include <concepts>` in your firmware. If you `#include <iostream>` you will get a linker error mentioning `__cxa_throw` or `__cxa_pure_virtual`, and the fix is to remove the include.

The standard library headers ship with your toolchain. `arm-none-eabi-g++` 13.2 ships libstdc++ headers configured for freestanding mode — the headers compile, but the implementations that pull in the host runtime are guarded out. If you wandered into a non-freestanding code path (say, by accidentally using `std::sort` which uses an `std::function` somewhere), the link fails. Read the error.

## `constexpr` and `consteval` — moving work to compile time

The simplest C++ embedded discipline is "if a value is known at compile time, make it a `constexpr`." In C:

```c
#define IO_BANK0_BASE  0xd0000000u
#define GPIO_OUT_SET   (*(volatile uint32_t *)(IO_BANK0_BASE + 0x14))
```

In C++:

```cpp
inline constexpr std::uintptr_t IO_BANK0_BASE = 0xd0000000u;
inline volatile std::uint32_t *gpio_out_set =
    reinterpret_cast<volatile std::uint32_t *>(IO_BANK0_BASE + 0x14u);
```

Both compile to a load of the immediate `0xd0000014` followed by a store. The C++ version is type-safe (`std::uintptr_t` is the correct integer type for an address; `unsigned int` is not, on a 64-bit system), namespaced, and visible to the debugger as a named symbol. The C version is a textual macro that does not exist in the symbol table.

`constexpr` extends from variables to functions:

```cpp
constexpr std::uint32_t gpio_set_mask(std::uint8_t pin) noexcept {
    return 1u << pin;
}

// Used:
*gpio_out_set = gpio_set_mask(18);
```

The compiler evaluates `gpio_set_mask(18)` at compile time and embeds `1u << 18` (= 0x40000) as an immediate in the `str` instruction's operand. The function does not exist in the binary unless something calls it with a non-constant argument.

`consteval` is stricter: a function marked `consteval` *must* be evaluated at compile time. If you call it with a non-constant argument, the build fails. We use this for safety-critical compile-time arithmetic — the I²C `HCNT`/`LCNT` computation, the SCK divider check, the SPI mode validation:

```cpp
consteval std::uint16_t i2c_hcnt(std::uint32_t clk_peri,
                                  std::uint32_t scl_target,
                                  std::uint32_t t_high_min_ns) {
    const std::uint32_t period_ns = 1'000'000'000u / scl_target;
    const std::uint32_t high_ns   = period_ns / 2u;
    // Verify the bus electrical spec is satisfiable:
    if (high_ns < t_high_min_ns) {
        // Failing a consteval at compile time:
        throw "i2c_hcnt: t_high < t_high_min";  // never executes; just halts compilation
    }
    const std::uint32_t hcnt = (high_ns * (clk_peri / 1'000'000u)) / 1000u;
    if (hcnt > 0xFFFF) {
        throw "i2c_hcnt: hcnt > 16 bits";
    }
    return static_cast<std::uint16_t>(hcnt);
}

constexpr auto kBmp280Hcnt = i2c_hcnt(125'000'000u, 100'000u, 4000u);
// kBmp280Hcnt = 625; embedded as an immediate at every use site.
```

If you call `i2c_hcnt(clk_peri, scl_target, 4000)` with a non-`constexpr` `clk_peri`, the build fails with `error: 'i2c_hcnt' called in a constant expression, but the argument is not constant`. If you call it with a `scl_target` so high that `period_ns / 2 < t_high_min`, the `throw` triggers (during constant evaluation, where it is *not* a runtime exception but an evaluation failure) and the build fails with the message embedded in the `throw`. The check costs nothing at runtime because the only outputs of `consteval` functions are constant values.

This is the load-bearing discipline of modern embedded C++: every check the compiler can make at compile time, the compiler makes — and any check the compiler cannot make at compile time becomes a build failure, not a runtime trap. The MCU never sees a misconfigured peripheral.

## `concepts` and `requires` — constraints with one-line diagnostics

Pre-C++20, you constrained templates with SFINAE — `std::enable_if_t<std::is_integral_v<T>>` in a return type. The diagnostic on misuse was 200 lines of template-instantiation trace. With C++20 concepts, the diagnostic is one line.

A concept is a named predicate over types:

```cpp
template<typename T>
concept SpiDevice = requires(T &t,
                              std::span<const std::uint8_t> tx,
                              std::span<std::uint8_t> rx) {
    { t.write(tx) }                       -> std::same_as<int>;
    { t.write_read(tx, rx) }              -> std::same_as<int>;
    { T::kClockMode }                     -> std::convertible_to<std::uint8_t>;
};
```

A `requires` expression checks: "does `t.write(tx)` compile, and does it return `int`?" If yes, the concept is satisfied; if no, the concept is not satisfied and the compiler rejects the template instantiation.

Usage:

```cpp
template<SpiDevice Dev>
void send_init_sequence(Dev &dev) {
    constexpr std::array<std::uint8_t, 26> kInit = { /* SSD1306 init */ };
    dev.write(std::span(kInit));
}
```

If you pass `send_init_sequence(my_uart)` where `my_uart` has no `write_read` method, the compiler emits:

```
error: no matching function for call to 'send_init_sequence<Uart>'
note: candidate template requires 'SpiDevice' concept; 'Uart' does not satisfy 'SpiDevice':
        'my_uart.write_read(tx, rx)' is invalid (no matching member function)
```

Three lines. Not two hundred.

Concepts are zero-cost at runtime — they are purely a compile-time constraint check. A function with a concept-constrained template parameter compiles to identical machine code as the same function with no constraint; the only difference is whether the call site is accepted at compile time.

The concepts we will use this week:

```cpp
// A type that can be used as the address of an I²C peripheral.
template<typename T>
concept I2cPeripheral = std::is_same_v<T, i2c_hw_t *> ||
                       std::is_same_v<T, std::uintptr_t>;

// A type that exposes the SPI bus contract.
template<typename T>
concept SpiBus = requires(T &t, std::span<const std::uint8_t> tx) {
    { t.write(tx) }      -> std::same_as<int>;
    { t.write_read(tx, std::span<std::uint8_t>{}) } -> std::same_as<int>;
};

// A type that exposes the I²C bus contract.
template<typename T>
concept I2cBus = requires(T &t,
                          std::uint8_t addr,
                          std::span<const std::uint8_t> tx,
                          std::span<std::uint8_t> rx) {
    { t.write(addr, tx) }                 -> std::same_as<int>;
    { t.write_read(addr, tx, rx) }        -> std::same_as<int>;
};

// A valid GP pin number on the RP2040 (0–29).
template<std::uint8_t Pin>
concept PinNumber = (Pin < 30u);
```

The `PinNumber<Pin>` concept restricts template *values* (not just types). `Gpio<30>` is a compile-time error because `30 < 30` is false; the diagnostic is one line.

## No exceptions — what `-fno-exceptions` removes

Exceptions in C++ rely on three machinery pieces:

1. **The unwinder.** When you `throw`, the runtime walks the stack frame by frame, calling destructors for every local in scope, until it finds a `catch` clause. On ARM Cortex-M with the Itanium C++ ABI, this requires per-function unwind tables in the `.eh_frame` and `.eh_frame_hdr` sections. For a non-trivial firmware these tables can be 20–40 KB.
2. **The exception object allocator.** `throw X(args)` allocates the exception object on a special exception-heap (`__cxa_allocate_exception`). On freestanding, this heap does not exist unless you provide it.
3. **The personality routine.** Every function compiled with exceptions enabled carries a reference to `__cxa_personality_v0`, which is called by the unwinder to decide whether the function has a `catch` clause for the exception type. This pulls in libsupc++ (~30 KB on ARM).

`-fno-exceptions` removes all three. The flag has three concrete effects:

- A `throw` statement becomes `__builtin_trap()` (an undefined instruction that triggers the hardfault handler). You should not write `throw` in `-fno-exceptions` code; the compiler will warn.
- A `try`/`catch` block compiles to just the `try` body — the `catch` clauses are dead code stripped at link.
- No `.eh_frame` section is emitted (also requires `-fno-unwind-tables -fno-asynchronous-unwind-tables` for full effect).

The replacement for exceptions in our code is *return-value error reporting*, as in Week 4's `bus_status_t` enum. C++ wraps this in `std::optional<T>` (the result-or-nothing case) or `etl::expected<T, E>` (the result-or-rich-error case):

```cpp
// Old C-style:
int bmp280_read_id(bmp280_t *dev, uint8_t *out);

// New C++-style with std::optional:
[[nodiscard]] std::optional<std::uint8_t> read_id() const noexcept;

// Even better with etl::expected:
[[nodiscard]] etl::expected<std::uint8_t, BusError> read_id() const noexcept;
```

`[[nodiscard]]` makes the compiler warn at every call site that ignores the return — so a missed error-check is caught at build time, not at deployment time.

`std::optional<int>` returned by value: on Cortex-M0+ AAPCS, returns up to 8 bytes in `r0`/`r1`, so a `std::optional<int>` (5 bytes: 4-byte int + 1-byte engaged flag) packs into `r0`/`r1` and costs zero memory traffic. The optimizer flattens the check; the assembly is the same as a two-output C function with a returned status code.

`noexcept` on every function: tells the compiler "this function never throws" so it does not need to emit any unwind tables for the function. Mark *every* function `noexcept` in freestanding code; it is a 0-cost annotation that helps the linker strip more.

## No RTTI — what `-fno-rtti` removes

RTTI (Run-Time Type Information) supports `typeid(x)` and `dynamic_cast<T*>(x)`:

```cpp
class Animal { public: virtual ~Animal() = default; };
class Dog : public Animal {};
Animal *a = new Dog();
if (Dog *d = dynamic_cast<Dog *>(a)) { /* It's a Dog */ }
```

For `dynamic_cast` and `typeid` to work, the compiler emits a `type_info` table per polymorphic class. The table contains the mangled name of the type and a chain to the parent-class's `type_info`. On a 32-bit ARM, each `type_info` is 12–32 bytes per class.

`-fno-rtti` disables this: `typeid` and `dynamic_cast` become compile errors. No `type_info` tables are emitted.

What you lose: the ability to ask "is this `Animal *` actually a `Dog *`?" at runtime. The replacement: either know statically (use a templated function and let the compiler resolve), or carry a discriminator field in the base class (a `std::variant`-style tag), or just do not have a class hierarchy.

In freestanding C++ we rarely have class hierarchies anyway — CRTP gives us static polymorphism, which does not need RTTI. We use `-fno-rtti` unconditionally.

## No `__cxa_atexit` — what `-fno-use-cxa-atexit` removes

A C++ global with a constructor needs a corresponding destructor call at program exit. On a hosted system, `__cxa_atexit` registers the destructor with the runtime, and the runtime calls it on `exit()`. On freestanding, programs do not exit, so this registration is wasted.

```cpp
class Logger {
public:
    Logger();   // constructor
    ~Logger();  // destructor — registered via __cxa_atexit by default
};

Logger g_logger;  // global; the constructor runs at startup, destructor never
```

Without `-fno-use-cxa-atexit`, the compiler emits a call to `__cxa_atexit(&g_logger::~Logger, &g_logger, &__dso_handle)` after the constructor runs. `__cxa_atexit` is from libsupc++ and pulls in a 64-slot atexit table (about 800 bytes), `__dso_handle` is a libsupc++ symbol, and the slot table itself is dead data on a system that never exits.

With `-fno-use-cxa-atexit`, the compiler emits the destructor registration via `atexit` instead (the C-style version), and on freestanding we *also* provide an empty `atexit` shim that does nothing. The net is no destructor is ever called for a global — which is fine, because the program never exits.

Better: do not have globals with non-trivial constructors. Use `constexpr` constructors and `constinit` to force compile-time initialization:

```cpp
class GpioBank {
public:
    constexpr GpioBank() = default;
    void set(std::uint8_t pin) noexcept;
    void clr(std::uint8_t pin) noexcept;
};

constinit GpioBank g_gpio_bank;
```

`constinit` requires that the global is initialized at compile time — if it cannot be, the build fails. This guarantees no `__cxa_atexit` registration is needed and no runtime constructor call is required. The variable is a `.bss` zero-init from the linker.

## No `__cxa_guard_acquire` — what `-fno-threadsafe-statics` removes

A function-local `static` with a non-trivial initializer is required by C++11 to be initialized once, in a thread-safe manner, with subsequent calls blocking until the initialization completes. The implementation uses a "guard" variable around the static:

```cpp
const std::array<std::uint8_t, 26> &init_seq() {
    static const std::array<std::uint8_t, 26> seq = { 0xAE, 0xD5, /* ... */ };
    return seq;
}
```

Without `-fno-threadsafe-statics`, the compiler emits calls to `__cxa_guard_acquire(&guard_var)` and `__cxa_guard_release(&guard_var)` around the initialization. These are libsupc++ symbols that on a hosted system use a mutex; on freestanding they pull in libsupc++ machinery (~200 bytes).

On a single-core MCU with no preemption during initialization (you are pre-`main`, before interrupts are enabled), thread-safety of `static` initialization is meaningless. `-fno-threadsafe-statics` flattens the guard to a simple flag check.

The cleaner alternative: make the local `constexpr`, so it has no initializer at runtime at all:

```cpp
const std::span<const std::uint8_t> init_seq() {
    static constexpr std::array<std::uint8_t, 26> kSeq = { 0xAE, 0xD5, /* ... */ };
    return std::span<const std::uint8_t>(kSeq);
}
```

`static constexpr` is just a `const` in `.rodata`; no guard, no initializer, no `__cxa_*` call.

## Putting it together — the build invocation

Every C++ source file in this week's repo compiles with this flag set:

```
arm-none-eabi-g++
    -std=c++20                          # C++20 mode
    -mcpu=cortex-m0plus -mthumb          # ARMv6-M
    -ffreestanding                       # No hosted libc
    -fno-exceptions -fno-rtti            # No exceptions, no RTTI
    -fno-threadsafe-statics              # No __cxa_guard_*
    -fno-use-cxa-atexit                  # No __cxa_atexit
    -fno-unwind-tables                   # No .eh_frame_hdr
    -fno-asynchronous-unwind-tables      # No .eh_frame
    -ffunction-sections -fdata-sections  # For --gc-sections
    -Os                                  # Optimize for size
    -Wall -Wextra -Wpedantic -Werror     # Warnings as errors
    -c gpio.cpp
```

The link adds:

```
arm-none-eabi-g++
    -mcpu=cortex-m0plus -mthumb
    -nostdlib -nostartfiles
    -Wl,--gc-sections                    # Strip unused sections
    -Wl,-T,pico.ld                       # Your Week 3 linker script
    -Wl,-Map=build/w05-mini.map
    boot2.o startup.o main.o gpio.o spi.o ssd1306.o
    -o build/w05-mini.elf
```

Then `arm-none-eabi-objcopy -O binary build/w05-mini.elf build/w05-mini.bin` and the picotool UF2 step from Week 3.

If you forget any of `-fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit`, your binary will have a section called `.eh_frame` or a symbol called `__cxa_guard_acquire` or `__cxa_pure_virtual`, and the linker will complain that these symbols are undefined. The flags are not optional on freestanding.

## Verifying the subset — the disassembly check

The proof that you stayed in the subset is the disassembly. The discipline:

```sh
arm-none-eabi-objdump -d build/w05-mini.elf > w05-mini.dis
arm-none-eabi-objdump -d build/w04-mini.elf > w04-mini.dis
diff w04-mini.dis w05-mini.dis
```

The diff should be small — the C++ symbol names are mangled (`_ZN4GpioILh18EE3setEv` instead of `gpio18_set`) but the instructions per function should be identical or near-identical. If you see a sudden 200-line section of unfamiliar code, you have leaked a runtime feature. Common culprits:

- `__cxa_atexit` — a global with a destructor.
- `__cxa_guard_acquire` — a function-local `static` with a non-trivial initializer.
- `__cxa_pure_virtual` — a virtual function on an abstract base class with no `override` in scope.
- `_Unwind_Resume` — an exception was not stripped (forgot `-fno-exceptions`).
- `__gxx_personality_v0` — same as above.

The `arm-none-eabi-nm --size-sort build/w05-mini.elf | head -20` invocation gives the top 20 symbols by size; any unexpected `__cxa_*` near the top is your leak.

## Why the discipline matters

The Pi Pico W has 264 KB of SRAM and 2 MB of flash. The Week 4 mini-project takes ~7 KB of flash. A naive C++ build of the same code, with default flags (`g++ -std=c++20 main.cpp`), takes about 54 KB — about 8× the C version. Every `static` local pulled `__cxa_guard_*`, every potentially-throwing function pulled the unwinder, every `std::cout` would have pulled libstdc++'s locale machinery (we didn't use `std::cout`, but the linker doesn't know that until link-time).

The freestanding flags shrink the binary by approximately 47 KB. Most of that is the unwinder. The next 1–2 KB is RTTI. The remaining 200 bytes is the threadsafe-statics machinery. Without these flags, C++ on an MCU is a non-starter. With them, C++ is approximately the same size as C with stricter type-checking and better abstractions.

The discipline is: write your C++ as if you knew the disassembly. Use `constexpr` for every immediate; use `concepts` for every interface; use CRTP for every polymorphism need; use return-value error reporting for every fallible function; use stack allocation only.

The reward: a `Gpio<18>::set()` call that compiles to a single `str` instruction, and a `SpiDevice<SPI0, GP17, SpiMode::M0>` template that produces the same disassembly as Week 4's hand-written C — but you have type-safe pin numbers, compile-time-validated SPI modes, and zero-cost dispatch.

## What you take away

- The C++20 freestanding subset is defined by ISO/IEC 14882:2020 §16.4.2.5; it includes `<cstdint>`, `<span>`, `<concepts>`, `<bit>` and excludes `<iostream>`, `<vector>`, `<string>`.
- The flag triad `-fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit` removes the C++ runtime cost on a freestanding target.
- `constexpr` and `consteval` move computation from runtime to compile time; `consteval` makes the move *mandatory*.
- `concepts` and `requires` make template constraints checked at compile time with one-line diagnostics, replacing SFINAE.
- The replacement for exceptions is return-value error reporting wrapped in `std::optional<T>` or `etl::expected<T, E>`, with `[[nodiscard]]` enforcing call-site checking.
- `noexcept` on every function is a free annotation that helps the linker strip dead code.
- The disassembly is the arbiter — if `objdump` shows unexpected `__cxa_*` or `_Unwind_Resume` symbols, the subset has leaked and the binary has grown.

In the next lecture we put the subset to work: a `Gpio<Pin>` template that compiles to one instruction, a CRTP base class with no v-table, and a side-by-side `objdump` walk showing the C version and the C++ version are the same machine code.

## References

- ISO/IEC 14882:2020 §16.4.2.5 — Freestanding implementations. Final committee draft N4860:
  <https://github.com/cplusplus/draft/releases/tag/n4860>
- cppreference, "Freestanding implementations":
  <https://en.cppreference.com/w/cpp/freestanding>
- GCC manual, "Options Controlling C++ Dialect" (§3.5 in 13.2 manual):
  <https://gcc.gnu.org/onlinedocs/gcc-13.2.0/gcc/C_002b_002b-Dialect-Options.html>
- libstdc++ manual, "Using libstdc++":
  <https://gcc.gnu.org/onlinedocs/libstdc++/manual/using.html>
- "Why exceptions are not used in embedded," Lawrence Crowl, ISO/IEC JTC1/SC22/WG21 N4234 (2014):
  <https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2014/n4234.pdf>
- "Modern C++ for Embedded Engineers," Memfault Interrupt blog:
  <https://interrupt.memfault.com/blog/cpp-fundamentals>
- AUTOSAR C++14 Coding Guidelines, summary chapters:
  <https://www.autosar.org/standards/adaptive-platform/>
- MISRA C++ 2023 executive summary:
  <https://misra.org.uk/product/misra-cpp2023/>
- "Polymorphism Without Virtual Functions," LLVM blog (2022):
  <https://blog.llvm.org/posts/2022-09-06-polymorphism-without-virtual-functions/>
