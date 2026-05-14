# Week 5 — Quiz

Ten questions, ~30 minutes. References open — this is a subset-discipline and disassembly-grade quiz, not a memorization test. Cite a section number or page on every answer that warrants one.

---

## Q1 — The freestanding flag triad

Name the four GCC flags that you add to *every* `arm-none-eabi-g++` invocation on a freestanding Cortex-M target to suppress C++ runtime features that are unavailable. For each flag, name *one* `__cxa_*` symbol (or other libsupc++ symbol) it removes from the link.

---

## Q2 — `constexpr` vs `consteval`

Define each keyword in one sentence. Give one example of when you would use `constexpr` but *not* `consteval`, and one example of when you would use `consteval` but *not* `constexpr`. Cite cppreference for both.

---

## Q3 — Disassembly arithmetic

The C code

```c
*(volatile uint32_t *)0xd0000014 = (1u << 18);
```

compiles on Cortex-M0+ at `-Os` to:

```
ldr   r3, [pc, #N]      ; load 0xd0000000 from literal pool
movs  r2, #128          ; r2 = 0x80
lsls  r2, r2, #12       ; r2 = 0x40000
str   r2, [r3, #20]
```

Why are *two* instructions (`movs` + `lsls`) used to materialize `0x40000` instead of one? Cite ARMv6-M ARM §A6.7 (Thumb instruction encodings).

---

## Q4 — Concepts as constraints

Given the concept

```cpp
template<typename T>
concept SpiBus = requires(T &t, std::uint8_t b) {
    { t.xfer(b) } -> std::same_as<std::uint8_t>;
};
```

Which of the following types satisfy `SpiBus`? Justify each.

1. `struct A { std::uint8_t xfer(std::uint8_t b) noexcept; };`
2. `struct B { int xfer(std::uint8_t b); };`
3. `struct C { std::uint8_t xfer(int b); };`
4. `struct D { std::uint8_t xfer(std::uint8_t b) const; };`

---

## Q5 — CRTP vs virtual: instruction count

Given

```cpp
class IPeripheral { public: virtual void set() = 0; virtual ~IPeripheral() = default; };
class GpioA : public IPeripheral { public: void set() override; };
```

vs

```cpp
template<typename Derived> class Peripheral { public: void set() { static_cast<Derived*>(this)->do_set(); } };
class GpioA : public Peripheral<GpioA> { public: void do_set(); };
```

How many Cortex-M0+ instructions are executed for `obj.set()` in each version, assuming `set()`/`do_set()` is inlined where the language permits? Show the disassembly difference.

---

## Q6 — Why `-fno-exceptions` matters

A C++ source file compiled with default flags emits a `.eh_frame` section. On a typical 5-KB embedded program, how large is `.eh_frame`? What is its purpose at runtime? What flag removes it? Cite the GCC manual section that describes the flag.

---

## Q7 — `std::optional<int>` ABI

A function `[[nodiscard]] std::optional<int> read_id() noexcept;` returns a `std::optional<int>`. On Cortex-M0+ AAPCS, how is the return value passed (which registers, in which order, totaling how many bytes)? Cite ARM IHI 0042F §6.4.

---

## Q8 — `extern "C"` and name mangling

You have

```cpp
namespace foo {
class Bar {
public:
    int baz(int x);
};
}
```

What is the *mangled* symbol name for `foo::Bar::baz(int)` under the Itanium C++ ABI (the one GCC and Clang use on ARM)? If you wanted the symbol to be linkable from C as `bar_baz`, what would the C++ declaration need to look like?

---

## Q9 — The C-or-C++ decision

You are starting a new firmware project for a 32-bit Cortex-M4 ECU. The customer requires MISRA-C 2012 compliance and uses LDRA for static analysis. The customer's other ECUs are AUTOSAR Classic Platform.

Per Lecture 3's decision table, do you write the firmware in C or C++? Cite the three rows of the table that decide the call.

---

## Q10 — The disassembly arbiter

You wrote a C++20 SPI driver. On `arm-none-eabi-nm --size-sort build/spi.elf | head -10`, you see:

```
00001500 T __gxx_personality_v0
00000800 T _ZN4pico10SpiDeviceI...EE5writeE...
00000200 T main
...
```

Identify the problem and the fix (with the specific flag to add).

---

## Scoring

10 points per question; 70 to pass. Reviewer signs off; you can retake once per week if needed.

The questions that most often trip students:
- Q3 (the Cortex-M0+ has no 32-bit immediate load; many students don't know that ARMv6-M only has 8-bit immediates and must materialize larger values with shifts).
- Q5 (the inlined CRTP version is 1 instruction, not 0; the str is the work — many students count 0 because they conflate "inlined" with "no work").
- Q7 (the `std::optional<int>` packs into r0+r1 because it is 5 bytes — students who answer "5 bytes in r0" are wrong because AAPCS pads to 4-byte boundaries).
- Q10 (the `__gxx_personality_v0` is the unwinder; the fix is `-fno-exceptions` *and* `-fno-unwind-tables -fno-asynchronous-unwind-tables`).
