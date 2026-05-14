# Exercise 3 — Mixed C and C++ translation units in one `.elf`

> *Build one firmware where the SPI transport is C++20 (from Exercise 2), the BMP280 protocol driver is plain C (from Week 4 Exercise 2), and they call each other across `extern "C"`. Prove the boundary works on the wire by capturing a Saleae trace.*

## Goal

Demonstrate that mixed C and C++ translation units link into a single ELF, with the `extern "C"` boundary preserving the ABI in both directions: a C function that calls a C++ function (via `extern "C"` wrappers) and a C++ function that calls a C function (via `#include` and `extern "C"`). The artifact is a working Pi Pico W firmware that reads a BMP280 sensor through this mixed stack.

By the end you can:
- Author an `extern "C"` shim in C++ that exposes a C++ class's methods as C functions.
- Include a C header from a C++ source with the `#ifdef __cplusplus / extern "C" { ... }` guard pattern.
- Build a `Makefile` that compiles `.c` files with `arm-none-eabi-gcc` and `.cpp` files with `arm-none-eabi-g++` and links both with `arm-none-eabi-g++` (so the C++ runtime is pulled in if needed).
- Diagnose linker errors at the C/C++ boundary (most common: forgetting `extern "C"`, which mangles a name the C side cannot find).
- Confirm by Saleae trace that the mixed stack produces the same I²C/SPI traffic as a pure-C or pure-C++ version.

## Setup

### Parts

- 1× Pi Pico W
- 1× BMP280 module (same as Week 4 Exercise 2)
- 1× SSD1306 OLED, 4-wire SPI variant (same as Week 4 Exercise 1 and this week's Exercise 2)
- 1× breadboard, jumpers, 2× 4.7 kΩ pull-ups (for I²C)
- 1× logic analyzer

### Wiring

Combined: SPI for the OLED (GP16/17/18/19/20/21) and I²C for the BMP280 (GP4/5). Same as Week 4 mini-project minus the MPU-6050.

| Device  | Bus  | Pico GP                                     |
|---------|------|---------------------------------------------|
| BMP280  | I²C  | GP4 (SDA), GP5 (SCL)                         |
| SSD1306 | SPI  | GP17 (CS), GP18 (SCK), GP19 (MOSI), GP20 (DC), GP21 (RST) |

### Prerequisites

- Exercise 1 and 2 of this week.
- Week 4 Exercise 2 (BMP280 in C).
- Lectures 1 and 2 of this week.

## Reading

- **Lecture 1** of this week — the freestanding subset.
- **Lecture 3** of this week — when C still wins; the boundary case.
- **Itanium C++ ABI**, §5.1 (Name mangling): <https://itanium-cxx-abi.github.io/cxx-abi/abi.html>
- **ARM AAPCS**, §6 (Parameter passing): <https://github.com/ARM-software/abi-aa/releases>
- **GCC manual**, §6 (C++ extensions, specifically `extern "C"` linkage):
  <https://gcc.gnu.org/onlinedocs/gcc-13.2.0/gcc/Vague-Linkage.html>

## Steps

### 1. Take stock of what you have

From Week 4:
- `bmp280.c` / `bmp280.h` — protocol-layer driver in C, takes a `bus_t *`.
- `bus.h` — the C struct of function pointers.
- `i2c0_transport.c` / `i2c0_transport.h` — I²C transport in C.

From this week, Exercise 2:
- `spi0.hpp`, `spi_device.hpp`, `ssd1306.hpp`, `gpio.hpp` — C++ SPI + SSD1306.

Goal: combine them into one firmware where:
- The BMP280 is driven through the **C** transport and the **C** protocol driver (unchanged from Week 4).
- The SSD1306 is driven through the **C++** transport and the **C++** protocol driver (from Exercise 2).
- `main.c` is **C++** (so it can construct the `Ssd1306` class) but calls into the C BMP280 driver via `extern "C"`.

### 2. Write the `extern "C"` guard in `bus.h`

Edit `bus.h` (the Week 4 header) to support being included from C++:

```c
/* bus.h */
#ifndef BUS_H
#define BUS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

typedef struct bus_s bus_t;

struct bus_s {
    int (*write)(bus_t *self, const uint8_t *tx, size_t tx_len);
    int (*write_read)(bus_t *self,
                      const uint8_t *tx, size_t tx_len,
                      uint8_t *rx, size_t rx_len);
    void *ctx;
};

#ifdef __cplusplus
}
#endif

#endif
```

The `#ifdef __cplusplus / extern "C"` pattern is the canonical way to make a header usable from both C and C++. From C, the `extern "C"` block is invisible (the `__cplusplus` macro is not defined); from C++, the block forces every declaration inside it to have C linkage (no name mangling).

Do the same for `bmp280.h` and `i2c0_transport.h`:

```c
/* bmp280.h */
#ifndef BMP280_H
#define BMP280_H

#ifdef __cplusplus
extern "C" {
#endif

#include "bus.h"

typedef struct { /* ... */ } bmp280_t;

int  bmp280_init(bmp280_t *dev, bus_t *bus);
int  bmp280_read_raw(bmp280_t *dev, int32_t *raw_t, int32_t *raw_p);
int  bmp280_compensate(bmp280_t *dev, int32_t raw_t, int32_t raw_p,
                       int32_t *t_centi_c, uint32_t *p_pa);

#ifdef __cplusplus
}
#endif

#endif
```

**Checkpoint:** the Week 4 C build still works against these headers — the C compiler ignores the `extern "C"` block. Confirm by rebuilding the Week 4 mini-project.

### 3. Write `main.cpp` that calls both stacks

```cpp
/* main.cpp */
#include <cstdint>

extern "C" {
#include "bmp280.h"
#include "i2c0_transport.h"
#include "rp2040_resets.h"
#include "rp2040_io_bank.h"
}

#include "spi0.hpp"
#include "ssd1306.hpp"

extern "C" void enable_gpio_outputs();
extern "C" void uart0_init_115200();
extern "C" int  uart0_putc(int c);

namespace {
pico::Spi0 g_spi0;
}

extern "C" int main() {
    enable_gpio_outputs();
    uart0_init_115200();
    g_spi0.init_1mhz();

    // BMP280 over C transport + C driver.
    i2c0_init_100khz();
    static i2c_transport_ctx_t bmp_ctx = { .peri = I2C0, .address = 0x76 };
    static bus_t bmp_bus;
    i2c_transport_make(&bmp_bus, &bmp_ctx);

    bmp280_t bmp;
    if (bmp280_init(&bmp, &bmp_bus) < 0) {
        while (true) {}  // hang on init failure
    }

    // SSD1306 over C++ transport + C++ driver.
    pico::Ssd1306<pico::Spi0, 17, 20, 21> oled{g_spi0};
    if (oled.init() < 0) {
        while (true) {}
    }

    for (;;) {
        std::int32_t raw_t, raw_p;
        if (bmp280_read_raw(&bmp, &raw_t, &raw_p) == 0) {
            std::int32_t t_centi;
            std::uint32_t p_pa;
            bmp280_compensate(&bmp, raw_t, raw_p, &t_centi, &p_pa);
            // Render to OLED — framebuffer pre-computation omitted.
            // For this exercise, log raw values over UART instead:
            char line[64];
            int n = snprintf(line, sizeof(line),
                             "T=%ld.%02ld C  P=%lu Pa\r\n",
                             (long)t_centi / 100, (long)t_centi % 100,
                             (unsigned long)p_pa);
            for (int i = 0; i < n; i++) uart0_putc(line[i]);
        }
        for (volatile int i = 0; i < 1'000'000; i++) {}  // ~250 ms delay
    }
}
```

The C++ source includes the C BMP280 header inside an `extern "C"` block. From the C++ side, `bmp280_init` is treated as a C function with C linkage and no mangling. The linker matches the call to the `bmp280_init` symbol emitted by the C compilation of `bmp280.c`.

**Checkpoint:** the `main.cpp` compiles. If you get errors like `undefined reference to 'bmp280_init'` at link time, the `extern "C"` block is missing or in the wrong place; re-check.

### 4. Write the `Makefile`

```makefile
TOOL_PREFIX  ?= arm-none-eabi-
CC            = $(TOOL_PREFIX)gcc
CXX           = $(TOOL_PREFIX)g++
LD            = $(TOOL_PREFIX)g++
OBJCOPY       = $(TOOL_PREFIX)objcopy
SIZE          = $(TOOL_PREFIX)size

ARCH_FLAGS    = -mcpu=cortex-m0plus -mthumb
COMMON_FLAGS  = $(ARCH_FLAGS) -Os -ffreestanding -nostdlib \
                -ffunction-sections -fdata-sections \
                -Wall -Wextra -Wpedantic

CFLAGS        = -std=c11 $(COMMON_FLAGS)
CXXFLAGS      = -std=c++20 $(COMMON_FLAGS) \
                -fno-exceptions -fno-rtti \
                -fno-threadsafe-statics -fno-use-cxa-atexit \
                -fno-unwind-tables -fno-asynchronous-unwind-tables

LDFLAGS       = $(ARCH_FLAGS) -nostdlib -nostartfiles \
                -Wl,--gc-sections -Wl,-T,pico.ld \
                -Wl,-Map=build/w05-ex03.map

C_SRCS        = startup.c bmp280.c i2c0_transport.c rp2040_i2c.c \
                rp2040_resets.c rp2040_io_bank.c uart0.c
CXX_SRCS      = main.cpp
ASM_SRCS      = boot2_w25q080.S

C_OBJS        = $(C_SRCS:.c=.o)
CXX_OBJS      = $(CXX_SRCS:.cpp=.o)
ASM_OBJS      = $(ASM_SRCS:.S=.o)
ALL_OBJS      = $(addprefix build/, $(C_OBJS) $(CXX_OBJS) $(ASM_OBJS))

all: build/w05-ex03.uf2

build/%.o: %.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/%.o: %.cpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/%.o: %.S
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/w05-ex03.elf: $(ALL_OBJS)
	$(LD) $(LDFLAGS) $(ALL_OBJS) -o $@
	$(SIZE) $@

build/w05-ex03.bin: build/w05-ex03.elf
	$(OBJCOPY) -O binary $< $@

build/w05-ex03.uf2: build/w05-ex03.bin
	# Convert .bin to .uf2 using picotool or elf2uf2 (vendor tool).
	picotool uf2 convert -f $< $@ --offset 0x10000000

clean:
	rm -rf build
```

**Key point:** the *linker* is `$(CXX)`, not `$(CC)`. C++ object files reference C++-runtime symbols (e.g. `_Z*` mangled names; static initializers register via `__cxa_atexit` even when stripped to a no-op). Linking with `g++` ensures the right libstdc++ stubs are pulled — and on freestanding with `-nostdlib`, the linker errors if anything is genuinely missing. Linking with `gcc` would silently miss C++ symbols.

### 5. Build

```sh
make
```

**Checkpoint:** the build succeeds. The `arm-none-eabi-size` output reports:

```
   text	   data	    bss	    dec	    hex	filename
   N1KB	     0	     X	    Y	    Z	build/w05-ex03.elf
```

Note the `.text` size. We will compare against pure-C and pure-C++ baselines.

### 6. Flash and verify

```sh
picotool load -f build/w05-ex03.uf2
```

Open a UART terminal:

```sh
screen /dev/tty.usbmodem* 115200
```

Expected output, refreshing every 250 ms:

```
T=24.83 C  P=101325 Pa
T=24.84 C  P=101326 Pa
T=24.83 C  P=101326 Pa
...
```

The OLED is initialized but blank (the framebuffer rendering is omitted in this exercise; the load-bearing artifact is the BMP280 readings via the mixed stack).

### 7. Capture a Saleae trace

Capture the I²C bus during a BMP280 register read. The trace should be identical to your Week 4 Exercise 2 capture — the I²C transport is unchanged, only the *main loop* is now C++. Commit as `traces/i2c-bmp280-mixed.sal`.

### 8. Verify the name-mangling at the link layer

```sh
arm-none-eabi-nm build/w05-ex03.elf | grep bmp280
```

Expected output:

```
20000NNN T bmp280_init
20000NNN T bmp280_read_raw
20000NNN T bmp280_compensate
```

Note: **no `_Z` prefix**. The `bmp280_*` symbols have C linkage (unmangled). If you see `_Z9bmp280_initP*`, the header was not in an `extern "C"` block when `main.cpp` included it; fix the header.

```sh
arm-none-eabi-nm build/w05-ex03.elf | grep -E '_ZN4pico'
```

Expected output:

```
20000NNN T _ZN4pico4Spi04xferEh
20000NNN T _ZN4pico8Ssd1306ILNS_3Spi0EE...4initEv
...
```

These are the C++ symbols (mangled). The mangled names start with `_ZN4pico` (namespace `pico::`). This is correct — C++ symbols stay mangled, C symbols stay unmangled, the linker matches each.

### 9. Diff the `.text` sizes

```sh
arm-none-eabi-size build/w05-ex03.elf > size-mixed.txt
arm-none-eabi-size ../../week-04-spi-i2c-bus-drivers/mini-project/build/w04-mini.elf > size-c-only.txt
diff size-c-only.txt size-mixed.txt
```

**Checkpoint:** the mixed-language version is within ~100 bytes of the pure-C Week 4 version. Most of the delta is the SSD1306 C++ wrapper (which is templated) — comparable to the SSD1306 C code from Week 4. No C++ runtime overhead should be present.

## Artifact

Commit to `exercises/ex03/`:

1. `bus.h`, `bmp280.h`, `i2c0_transport.h` — with `#ifdef __cplusplus / extern "C"` guards.
2. `bmp280.c`, `i2c0_transport.c`, `rp2040_*.c`, `startup.c`, `uart0.c` — copied from Week 4 (unchanged).
3. `spi0.hpp`, `spi_device.hpp`, `ssd1306.hpp`, `gpio.hpp` — from Exercise 2.
4. `main.cpp` — the mixed-language main.
5. `Makefile` — builds C with gcc, C++ with g++, links with g++.
6. `pico.ld`, `boot2_w25q080.S` — from Week 3.
7. `traces/i2c-bmp280-mixed.sal` — Saleae capture.
8. `README.md` — describe the boundary:
   - Where C is, where C++ is, and why each was chosen for each side.
   - The two `extern "C"` patterns (header guard vs source-side wrapper).
   - The `.text` size for: pure C (Week 4), pure C++ (Exercise 2), mixed (this exercise).
   - The `arm-none-eabi-nm` output showing both unmangled and mangled symbols.

## Common faults

| Symptom | Cause | First diagnostic |
|---|---|---|
| Linker error: `undefined reference to 'bmp280_init'` from `main.cpp` | `bmp280.h` not wrapped in `extern "C"` (or `main.cpp` did not include it inside an `extern "C"` block) | `arm-none-eabi-nm bmp280.o` — symbol should be `bmp280_init`, not `_Z11bmp280_initP*` |
| Linker error: `undefined reference to '__cxa_atexit'` | A C++ global has a non-trivial destructor; the build wants the host runtime to register it | Add `-fno-use-cxa-atexit` to `CXXFLAGS`; or provide a no-op `__cxa_atexit` in `startup.c` |
| Linker error: `undefined reference to '_ZSt7nothrow'` or `operator new` | The C++ code is using `new` somewhere | `grep -n new main.cpp ssd1306.hpp spi_device.hpp` — find the use, replace with stack allocation |
| `.text` is 30 KB larger than Week 4's | Forgot the freestanding flag triad | `arm-none-eabi-nm --size-sort build/w05-ex03.elf | head -20` — top symbol will be `__gxx_personality_v0` |
| BMP280 reads zero | I²C transport not properly initialized | Same diagnostic as Week 4: scope SDA/SCL, confirm address byte |
| OLED is dark | SSD1306 init returns -1; the SPI bus is not driving correctly | Scope MOSI/SCK; confirm CSn toggles around the init transaction |
| Build succeeds, firmware crashes immediately | Linker order: C++ objects pulled in libstdc++ but the linker is `gcc` not `g++` | Confirm the link command uses `arm-none-eabi-g++` |

## Stretch goals

- Wrap the C++ `Ssd1306` class in an `extern "C"` C-style API: `void ssd1306_create(spi_t *bus, int cs, int dc, int rst, void *opaque)`, `int ssd1306_init(void *opaque)`, etc. Then write a *pure-C* `main.c` that uses both the BMP280 driver and the SSD1306 driver — only the C++ implementation is hidden behind the C ABI. This is the "library boundary" pattern from Lecture 3.
- Move the SSD1306 framebuffer rendering into the firmware (5x8 font, 16 chars × 4 lines = 1024-byte framebuffer). Update the OLED at 4 Hz with the BMP280 readings. Confirm the system still has at least 50 KB of free flash and 200 KB of free SRAM.
- Add a `unit_test_main.c` that runs only on the host (compiled with `gcc -DHOST_TEST`, not `arm-none-eabi-gcc`). The test stubs out `i2c_transport_make` with a mock and verifies `bmp280_init` calls it with the right arguments. Same pattern as Week 4 Exercise 3 but with the mixed build.
- Replace `snprintf` (which pulls in ~3 KB of libc) with a tiny hand-rolled integer-to-string formatter in C++. Measure the binary size delta.

## Hand-in

Push to GitHub, tag `w05-ex03`, when:
- The firmware reads the BMP280 and prints over UART at 4 Hz.
- The Saleae I²C capture matches Week 4's byte-for-byte.
- `arm-none-eabi-nm build/w05-ex03.elf | grep bmp280` shows unmangled C symbols.
- `arm-none-eabi-nm build/w05-ex03.elf | grep _ZN4pico` shows mangled C++ symbols.
- The `.text` size delta vs Week 4 is < 200 bytes.
- The reviewer signs off on the `README.md`.
