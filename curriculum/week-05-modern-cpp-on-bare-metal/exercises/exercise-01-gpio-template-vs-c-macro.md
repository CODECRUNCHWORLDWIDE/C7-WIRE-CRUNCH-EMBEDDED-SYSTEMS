# Exercise 1 — `Gpio<Pin>` template vs C macro

> *Build the `Gpio<Pin>` C++20 template from Lecture 2. Compile both the C macro version (from Week 3) and the C++ template version with `-Os`. Disassemble both. Diff. The bodies must be byte-for-byte identical.*

## Goal

Prove, by disassembly, that the `Gpio<Pin>` C++20 template produces the same machine code as the C macro `GPIO_OUT_SET = (1u << pin)`. Demonstrate that `static_assert(Pin < 30)` rejects `Gpio<30>` at compile time. Hand a reviewer a `objdump -d` excerpt showing six instructions on the C side and six instructions on the C++ side, identical except for the symbol name.

By the end you can:
- Author a `Gpio<Pin>` template with `constexpr` mask, `[[gnu::always_inline]]` accessors, and a `static_assert` bound on `Pin`.
- Build with the freestanding C++ flag set (`-fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit`).
- Read an `objdump -d` excerpt and identify each instruction.
- Commit a `disasm-diff.txt` artifact showing zero functional difference.

## Setup

### Toolchain

- `arm-none-eabi-gcc 13.2` and `arm-none-eabi-g++ 13.2` (or newer). Verify:
  ```sh
  arm-none-eabi-gcc --version
  arm-none-eabi-g++ --version
  ```
- Optional: Compiler Explorer (Godbolt) at <https://godbolt.org/>. Selects `ARM gcc 13.2.0 (none)` from the dropdown. Useful for quick disassembly without a local toolchain. Add `-std=c++20 -mcpu=cortex-m0plus -mthumb -Os -ffreestanding -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit` to the flags pane.

### Parts

None. This exercise is desk-only.

### Prerequisites

- Week 3 mini-project — you have a working C macro GPIO driver in `gpio.h`.
- Lectures 1 and 2 of this week read.

## Reading

- **Lecture 1** of this week — the freestanding flag set.
- **Lecture 2** of this week — the `Gpio<Pin>` template example and the disassembly walk-through.
- **RP2040 datasheet** §2.3.1.7, p. 50 — SIO atomic register layout (`GPIO_OUT_SET` at `0x14`, `GPIO_OUT_CLR` at `0x18`).
- **ARMv6-M ARM** §A6.7.137 — `STR (immediate)`.

## Steps

### 1. Write the C version

Create `gpio_c.c`:

```c
#include <stdint.h>

#define SIO_BASE        0xd0000000u
#define GPIO_OUT_SET    (*(volatile uint32_t *)(SIO_BASE + 0x14))
#define GPIO_OUT_CLR    (*(volatile uint32_t *)(SIO_BASE + 0x18))

void blink_18(void) {
    GPIO_OUT_SET = (1u << 18);
    GPIO_OUT_CLR = (1u << 18);
}
```

Compile:

```sh
arm-none-eabi-gcc -std=c11 -mcpu=cortex-m0plus -mthumb -Os \
    -ffreestanding -nostdlib -c gpio_c.c -o gpio_c.o
arm-none-eabi-objdump -d gpio_c.o > gpio_c.dis
```

**Checkpoint:** `gpio_c.dis` shows a `blink_18` symbol with about six instructions in the body. Confirm.

### 2. Write the C++ version

Create `gpio.hpp`:

```cpp
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

Create `gpio_cpp.cpp`:

```cpp
#include "gpio.hpp"

extern "C" void blink_18_cpp() {
    pico::Gpio<18>::set();
    pico::Gpio<18>::clr();
}
```

Compile:

```sh
arm-none-eabi-g++ -std=c++20 -mcpu=cortex-m0plus -mthumb -Os \
    -ffreestanding -fno-exceptions -fno-rtti \
    -fno-threadsafe-statics -fno-use-cxa-atexit \
    -nostdlib -c gpio_cpp.cpp -o gpio_cpp.o
arm-none-eabi-objdump -d gpio_cpp.o > gpio_cpp.dis
```

**Checkpoint:** `gpio_cpp.dis` shows a `blink_18_cpp` symbol with the *same* number of instructions in the body as `gpio_c.dis`. Verify.

### 3. Diff the disassemblies

Strip the symbol name and address columns before diffing (the symbols differ, the bodies should not). Use this awk pipeline:

```sh
awk '/^[ \t]*[0-9a-f]+:/ { sub(/^[^:]*:[ \t]*[0-9a-f ]*/, ""); print }' gpio_c.dis   > gpio_c.body
awk '/^[ \t]*[0-9a-f]+:/ { sub(/^[^:]*:[ \t]*[0-9a-f ]*/, ""); print }' gpio_cpp.dis > gpio_cpp.body
diff gpio_c.body gpio_cpp.body
```

**Checkpoint:** The diff is **empty**. If it is not, one of three things has happened:
- You forgot a `-fno-*` flag. Re-check the build command.
- You forgot `[[gnu::always_inline]]` on the template methods; the optimizer inlined anyway at `-Os`, but you should annotate explicitly.
- The version of `arm-none-eabi-g++` differs from `arm-none-eabi-gcc`. Both must be 13.2 for the disassembly to match exactly. Use `which arm-none-eabi-g++` and `which arm-none-eabi-gcc` to confirm they are from the same package.

If the diff is non-empty after addressing the above, inspect the differing lines. The most common difference is the `pop {r4, pc}` epilogue vs `bx lr` epilogue; both are correct, both are equivalent. The instruction count and the load/store pattern should still match.

### 4. Verify the compile-time bound check

Add to `gpio_cpp.cpp`:

```cpp
extern "C" void blink_42_cpp() {
    pico::Gpio<42>::set();   // INTENT: trigger the static_assert
}
```

Compile. Expected error:

```
gpio.hpp:11:5: error: static assertion failed: RP2040 has GP0–GP29
   11 |     static_assert(Pin < 30u, "RP2040 has GP0–GP29");
      |     ^~~~~~~~~~~~~
```

**Checkpoint:** The build fails with this one-line error. The C macro version has no equivalent — `gpio_set(42)` in C compiles silently and writes to bit 42 of `GPIO_OUT_SET` at runtime (a silent no-op because bits 30+ are reserved; the bug is undetected).

Remove the `blink_42_cpp` definition after verifying.

### 5. Look at the `.elf` section table

```sh
arm-none-eabi-readelf -S gpio_cpp.o
```

**Checkpoint:** The section list contains `.text`, `.text.blink_18_cpp` (because `-ffunction-sections` is on), `.bss`, `.rodata` — but *not* `.eh_frame`, `.eh_frame_hdr`, or `.gcc_except_table`. If any of those are present, you forgot `-fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables`.

Re-check:

```sh
arm-none-eabi-readelf -S gpio_cpp.o | grep -iE 'eh_frame|except'
```

Output should be empty.

### 6. Inspect the symbol table

```sh
arm-none-eabi-nm gpio_cpp.o
```

**Checkpoint:** The symbols are `blink_18_cpp` (text) and possibly some constants in `.rodata`. There must be *no* `__cxa_*` symbols, no `_ZTV*` (v-tables), no `_ZTI*` (type-info), no `__gxx_personality_v0`, no `_Unwind_Resume`. If any appear, you have leaked a runtime feature.

Re-check:

```sh
arm-none-eabi-nm gpio_cpp.o | grep -E '__cxa_|_ZTV|_ZTI|__gxx_personality_v0|_Unwind_Resume'
```

Output should be empty.

### 7. (Optional) Flash and verify on the bench

If you want to confirm the firmware *actually toggles* GP18, build a complete firmware with your Week 3 startup files:

```sh
arm-none-eabi-g++ -std=c++20 -mcpu=cortex-m0plus -mthumb -Os \
    -ffreestanding -fno-exceptions -fno-rtti \
    -fno-threadsafe-statics -fno-use-cxa-atexit \
    -nostdlib -nostartfiles \
    boot2_w25q080.S startup.c gpio_cpp.cpp main.cpp \
    -T pico.ld -o build/w05-ex01.elf
arm-none-eabi-objcopy -O binary build/w05-ex01.elf build/w05-ex01.bin
picotool load -f build/w05-ex01.uf2
```

Where `main.cpp` is:

```cpp
extern "C" void blink_18_cpp();

extern "C" int main() {
    while (true) {
        blink_18_cpp();
        for (volatile int i = 0; i < 100000; i++) {}
    }
}
```

Scope GP18 — you should see a square wave at roughly 30–100 Hz depending on the optimizer's loop generation. The visible bench artifact is "the LED on GP18 blinks." This step is optional; the disassembly diff is the load-bearing one.

## Artifact

Commit to `exercises/ex01/`:

1. `gpio_c.c` — the C baseline.
2. `gpio.hpp` — the C++20 template header.
3. `gpio_cpp.cpp` — the C++ test driver.
4. `Makefile` — produces `gpio_c.o` and `gpio_cpp.o` and disassembles each.
5. `gpio_c.dis` and `gpio_cpp.dis` — the disassemblies.
6. `disasm-diff.txt` — the (empty) output of `diff gpio_c.body gpio_cpp.body`.
7. `static-assert-check.txt` — the captured build error from the `Gpio<42>` attempt.
8. `README.md` — describe what you built, paste the two disassembly excerpts side by side, and confirm the diff is empty.

A `Makefile` template:

```makefile
TOOL_PREFIX  ?= arm-none-eabi-
CC            = $(TOOL_PREFIX)gcc
CXX           = $(TOOL_PREFIX)g++
OBJDUMP       = $(TOOL_PREFIX)objdump

CFLAGS    = -std=c11 -mcpu=cortex-m0plus -mthumb -Os -ffreestanding -nostdlib
CXXFLAGS  = -std=c++20 -mcpu=cortex-m0plus -mthumb -Os \
            -ffreestanding -fno-exceptions -fno-rtti \
            -fno-threadsafe-statics -fno-use-cxa-atexit \
            -fno-unwind-tables -fno-asynchronous-unwind-tables \
            -ffunction-sections -fdata-sections \
            -Wall -Wextra -Wpedantic -nostdlib

all: gpio_c.dis gpio_cpp.dis disasm-diff.txt

gpio_c.o: gpio_c.c
	$(CC) $(CFLAGS) -c $< -o $@

gpio_cpp.o: gpio_cpp.cpp gpio.hpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

%.dis: %.o
	$(OBJDUMP) -d $< > $@

disasm-diff.txt: gpio_c.dis gpio_cpp.dis
	awk '/^[ \t]*[0-9a-f]+:/ { sub(/^[^:]*:[ \t]*[0-9a-f ]*/, ""); print }' gpio_c.dis   > gpio_c.body
	awk '/^[ \t]*[0-9a-f]+:/ { sub(/^[^:]*:[ \t]*[0-9a-f ]*/, ""); print }' gpio_cpp.dis > gpio_cpp.body
	diff gpio_c.body gpio_cpp.body > $@ || true

clean:
	rm -f *.o *.dis *.body disasm-diff.txt
```

## Common faults

| Symptom | Cause | First diagnostic |
|---|---|---|
| `gpio_cpp.o` is 2 KB larger than `gpio_c.o` | Forgot `-fno-exceptions`; `.eh_frame` is being emitted | `arm-none-eabi-readelf -S gpio_cpp.o \| grep eh_frame` — confirm the section exists, add `-fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables` |
| Disassembly diff is non-empty, instruction sequences differ | The optimizer made different choices because the C and C++ versions used slightly different access patterns | Compare the instruction *count* — if it matches, accept it; if it does not, the C++ has a pattern the optimizer cannot collapse |
| `Gpio<42>` compiles silently | You forgot the `static_assert` | Re-check `gpio.hpp` line 11 |
| Linker error: `undefined reference to '__cxa_pure_virtual'` | You added a virtual function (probably accidentally — e.g. by deriving from a base with a virtual destructor) | Mark the destructor `= default` and inline; or remove the virtual entirely; or add `= delete` for the dynamic dispatch case |
| Linker error: `undefined reference to '__cxa_atexit'` | A global has a non-trivial destructor; the build wants to register it | Make the global `constinit` with a `constexpr` constructor and a trivial destructor; or add `-fno-use-cxa-atexit` |
| Compiler error: `'static_assert' not declared` | Using `-std=c++17` instead of `-std=c++20` (works in C++17 but the message form differs) | Check `arm-none-eabi-g++ -v` output for the language standard |

## Stretch goals

- Add a `Gpio<Pin>::toggle()` method that uses the SIO's `GPIO_OUT_XOR` register at offset `0x1C` (RP2040 datasheet §2.3.1.7, p. 50). Verify the disassembly is one `str` of the mask to `0xd000001c`.
- Convert the C version to use a `static inline void gpio_set(uint8_t pin)` function instead of a macro. Compare its disassembly to the macro version — at `-Os` they should be identical, because the optimizer inlines the function. At `-O0` they differ (function call vs inlined macro).
- Author a `PinNumber` concept (C++20) that constrains a non-type template parameter:
  ```cpp
  template<std::uint8_t N>
  concept PinNumber = (N < 30u);

  template<PinNumber auto Pin>
  struct GpioC { /* ... */ };
  ```
  Verify the error message for `GpioC<42>` is shorter and more informative than the `static_assert` version. Cite the C++20 standard §13.5 (concepts as constraint).
- Write a `Gpio<Pin>` template for the STM32F4 (different SIO layout, different register addresses). Show that the *pattern* transfers across silicon — same template body, different `constexpr` addresses.

## Hand-in

Push to GitHub, tag `w05-ex01`, when:
- The `Makefile` builds both `.o` files and produces an empty `disasm-diff.txt`.
- `static-assert-check.txt` shows the `Gpio<42>` build failure.
- `arm-none-eabi-nm gpio_cpp.o | grep __cxa_` returns nothing.
- `arm-none-eabi-readelf -S gpio_cpp.o | grep eh_frame` returns nothing.
- The reviewer signs off on the `README.md`.
