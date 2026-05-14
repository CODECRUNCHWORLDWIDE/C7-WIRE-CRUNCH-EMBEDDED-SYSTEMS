# Week 5 — Homework

Six practice problems. Do them by Sunday. Each one is self-graded against the references; bring questions to Friday studio.

---

## H1 — Read GCC's `-fno-exceptions` implementation

Open the GCC source tree (any release in the 13.x series): clone <https://gcc.gnu.org/git/gcc.git> if you do not have it locally. Navigate to `gcc/cp/except.cc` and read the function `build_throw` (~150 lines).

Write a one-paragraph annotation of what the function does when `flag_exceptions` is false (the `-fno-exceptions` case). What does a `throw` statement become — a `__builtin_trap()`, a `nullptr` dereference, or something else? Cite the line numbers.

Bring the annotation to studio.

---

## H2 — Compute a `consteval` baud divider

For a target UART baud of 921600 with `clk_peri = 125 MHz`, manually compute the RP2040 baud divider per datasheet §4.2.7.1, p. 423:

```
baud_rate_div = clk_peri / (16 * baud)
ibrd = floor(baud_rate_div)
fbrd = round((baud_rate_div - ibrd) * 64)
```

What are the exact values of `ibrd` and `fbrd`? What is the resulting *actual* baud rate? What is the error in PPM relative to the requested 921600?

(Hint: the answer is `ibrd = 8`, `fbrd = 30`. Verify the actual baud and error.)

Write a `consteval` function that computes `(ibrd, fbrd)` and produces a `static_assert` if the error exceeds 1% (the UART receiver's typical tolerance).

---

## H3 — Concepts vs SFINAE

Convert the following C++17 SFINAE template to a C++20 concept-constrained template:

```cpp
template<typename T,
          typename = std::enable_if_t<
              std::is_invocable_r_v<int, decltype(&T::write), T *, const std::uint8_t *, std::size_t>
          >>
void send_init(T *bus) {
    bus->write(/* ... */);
}
```

The concept-constrained version should be 4–6 lines and produce a one-line error if the type does not satisfy the constraint.

Cite C++20 standard §13.5 (constraints and concepts).

---

## H4 — CRTP disassembly walk

Take Exercise 1's `Gpio<Pin>` template. Modify it to use CRTP instead of a plain template:

```cpp
template<typename Derived>
class GpioBase {
public:
    void set() { static_cast<Derived *>(this)->do_set(); }
    void clr() { static_cast<Derived *>(this)->do_clr(); }
};

class Gpio18 : public GpioBase<Gpio18> {
public:
    void do_set();
    void do_clr();
};
```

Disassemble `Gpio18::set()`. How many instructions are in the body? Compare to the non-CRTP template version. Is the CRTP version larger, smaller, or the same?

Then make `GpioBase`'s `set()` `[[gnu::noinline]]` and disassemble again. Now is it different? Why?

---

## H5 — The AUTOSAR C++14 rule walk

Open AUTOSAR C++14 Coding Guidelines (or use the publicly-circulated 14-03 version from a search). Pick five rules from §6 (Language) that are *strictly* zero-runtime-cost and explain why.

Examples:
- Rule A0-1-1 (no unused entities) — pure compile-time check.
- Rule A2-7-3 (declarations commented) — compile-time check via documentation tooling.
- Rule A3-3-2 (static / thread-local objects shall be constant-initialized) — compile-time check; runtime cost is unchanged.
- Rule A15-0-1 (`noexcept` function does not throw) — annotation; compile-time check.
- (Add five.)

Write a one-page summary that AUTOSAR-aware code is approximately the same size as un-checked code; the discipline is in the analyzer, not the runtime.

---

## H6 — Read a Zephyr C++ driver

Open the Zephyr project's `samples/cpp_synchronization/src/main.cpp`: <https://github.com/zephyrproject-rtos/zephyr/tree/main/samples/cpp_synchronization>

Identify the C++ features used. Which ones from this week's lecture? Which ones are *not* in our subset? Why does Zephyr allow them (or not)?

Then look at Zephyr's `CMakeLists.txt` for the sample — what flags does it pass to `arm-none-eabi-g++`? Compare to our freestanding flag triad.

Write a one-paragraph comparison.

---

## Submission

Push your homework answers as `homework/h1.md` … `homework/h6.md` in the Week-5 repo. Bring questions to Friday studio.
