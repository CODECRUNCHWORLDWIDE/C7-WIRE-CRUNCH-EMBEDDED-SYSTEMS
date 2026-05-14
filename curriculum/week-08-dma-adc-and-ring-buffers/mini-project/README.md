# Week 8 Mini-Project — 1 MS/s Aggregate ADC Capture, Zero Overruns

> *Take the architecture, the hardware features, the FreeRTOS patterns, the GPIO markers, and the bench instruments. Wire all of it together and let it run for an hour. The system that survives is the system that earns the line on your CV.*

This week's mini-project ties Week 7's deferred-interrupt + gatekeeper patterns to Week 8's DMA + ring-buffer architecture and runs a single piece of firmware that captures analog data at the maximum rate the RP2040 supports, processes it in real time, and reports its health over UART without ever blocking, busy-waiting, or dropping a sample.

The deliverable is a `.uf2` flashed to a Pico, a Saleae capture showing 60 seconds of activity, an `OVERRUN-COUNTERS.md` showing zero overruns, a `POWER.md` showing the measured current draw, and a `DMA-DESIGN-NOTE.md` defending your architectural choices in writing.

---

## The system at a glance

```
  GP26 (ADC0) ----+
                  |--> ADC peripheral, round-robin mask=0x03, full rate
  GP27 (ADC1) ----+    -> 500 kS/s aggregate writes into ADC FIFO
                        -> DREQ_ADC (index 36) gates the DMA

  DMA CH_A: ADC FIFO -> mp_ring[0..1023]      (chain to CH_B)
  DMA CH_B: ADC FIFO -> mp_ring[1024..2047]   (chain to CH_A)
            (4 KB total, aligned to 4 KB boundary)
            chain-complete IRQ -> consumer task notify

  Consumer task   (prio 4): drains the just-completed half, computes
                            sum-of-squares and peak for that chunk,
                            atomically merges into 1 s accumulator.

  Gatekeeper task (prio 3): owns the UART; receives log strings on a
                            queue and prints serially.

  Reporter task   (prio 2): every 1 s, snapshots the accumulator,
                            computes RMS via integer Newton sqrt,
                            posts one line to the gatekeeper.

  Idle hook       (prio 0): __WFI for power saving.
```

The Saleae markers (GP14 = IRQ, GP13 = consumer busy, GP12 = consumer end, GP11 = overrun) make every transition visible.

---

## Pass criteria

The mini-project is graded on five artifacts. All five must be in the repo.

### 1. Working firmware

A `.uf2` (or the project source tree with a CMakeLists that produces one) that flashes to a Pico and immediately begins emitting per-second log lines on USB-CDC serial:

```
t=1s chunks=488 rms=812 peak=1421 fifo_err=0 cons_ovr=0 irq=488
t=2s chunks=976 rms=815 peak=1438 fifo_err=0 cons_ovr=0 irq=976
t=3s chunks=1465 rms=809 peak=1407 fifo_err=0 cons_ovr=0 irq=1465
...
```

`chunks` increments by 488 per second (= 500000 / 1024, rounded to integer). `irq` does the same. `fifo_err` and `cons_ovr` must both stay at zero for the full run.

If `chunks` is much different from 488 per second, the ADC clock divider is wrong. If `fifo_err` is non-zero, the DMA is not draining the FIFO (check `INCR_READ`, `TREQ_SEL`). If `cons_ovr` is non-zero, the consumer is slower than 2 ms per chunk.

### 2. Saleae capture (60 s)

A `.sal` file committed in `bench-traces/` showing all four GPIO markers for at least 60 seconds. The capture must show:

- GP14 (IRQ): 488 +/- 1 rising edges per second, with no gap > 2.5 ms.
- GP13 (consumer busy): rises within 5 µs of every GP14 rise, falls within 200 µs of rising.
- GP12 (consumer end): pulses once for every GP13 fall.
- GP11 (overrun): no edges over the full 60 s.

Annotate at least three points in the capture (a screen-shot in the commit is fine): one near the start, one near the middle, one near the end.

### 3. `OVERRUN-COUNTERS.md`

A markdown file in the project root containing the final counter values at end of run:

```markdown
## Run summary

- Duration: 60 s
- Total chunks processed: 29297
- Total chain-complete IRQs: 29297
- FIFO overrun count: 0
- Consumer overrun count: 0
- Net 3V3 current draw (steady state): 53 mA

## Discrepancies

None.
```

If any of the counters is non-zero, you have not passed; the file is for record-keeping, not for hand-waving.

### 4. `POWER.md`

Measure the Pico's 3V3 current under three conditions and record the results. The intent is to demonstrate that DMA + idle `__WFI` actually saves power vs polled-ADC equivalents.

```markdown
## Power measurements

Measurement setup: INA219 in series between the Pico's 3V3 pin and a
5 V USB source. Multimeter readings averaged over 5 seconds.

| Condition                                  | 3V3 current  | Notes                              |
|--------------------------------------------|-------------:|------------------------------------|
| DMA streaming + WFI in idle hook           |       52 mA  | The pass-criterion configuration.  |
| Polled ADC (busy-wait on FIFO) at 500 kS/s |       85 mA  | Edit consumer_task to spin instead.|
| DMA streaming, WFI disabled in idle hook   |       71 mA  | Comment __asm wfi.                 |

The 33 mA delta between WFI-on and WFI-off, at 3.3 V, is 0.109 W of
power saving (33 mA * 3.3 V) - essentially the cost of having the CPU
core clocked through a tight idle loop vs halted.
```

If the measurements deviate by more than +/- 10 % from the values above, document the cause (e.g. "my Pico W has the WiFi module idle-draw of +20 mA").

### 5. `DMA-DESIGN-NOTE.md`

A one-page note (300-500 words) justifying your architectural choices. Address each of:

- **Chain-pair vs hardware ring.** Why chain-pair? When would you switch?
- **Chunk size.** Why 1024? What is the latency vs CPU-overhead trade-off?
- **Consumer priority.** Why 4 (highest)? What happens if you lower it to 1?
- **Round-robin vs single-channel ADC.** Why two channels in round-robin instead of one channel at the same total rate? What does it cost in per-channel Nyquist?

The note is graded for engineering reasoning, not literary style. Bullet points are fine.

---

## CMakeLists.txt (worked recipe)

The starter CMakeLists for the mini-project is below. Copy into a fresh project directory; drop `main.c`, `dma_adc_capture.h`, and the `../exercises/dma_ring.h` (or include the path) into the same folder; run `cmake -B build && cmake --build build`.

```cmake
cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)
include(FreeRTOS_Kernel_import.cmake)  # adjust path to your FreeRTOS

project(week08_minproject C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(week08_minproject
    main.c
)

target_include_directories(week08_minproject PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/..
)

target_link_libraries(week08_minproject
    pico_stdlib
    hardware_dma
    hardware_adc
    hardware_irq
    FreeRTOS-Kernel
    FreeRTOS-Kernel-Heap4
)

pico_enable_stdio_usb(week08_minproject 1)
pico_enable_stdio_uart(week08_minproject 0)

pico_add_extra_outputs(week08_minproject)
```

You will also need a `FreeRTOSConfig.h` in your include path. The Week 6 configuration is adequate; the only Week 8 additions are:

```c
#define configUSE_TASK_NOTIFICATIONS    1
#define configUSE_IDLE_HOOK             1
#define configCHECK_FOR_STACK_OVERFLOW  2
#define configUSE_PREEMPTION            1
#define configMAX_PRIORITIES            8
#define configTICK_RATE_HZ              1000
```

The `pico_sdk_import.cmake` and `FreeRTOS_Kernel_import.cmake` files are not shipped with this curriculum; they live in the pico-sdk and the FreeRTOS-Kernel ports respectively. Use the same ones you used for Week 6 and Week 7.

---

## Suggested timeline

| Day       | Work                                                                  |
|-----------|-----------------------------------------------------------------------|
| Wednesday | Skim the starter; build it without changes; flash; confirm chunks=488/s |
| Thursday  | Probe with the Saleae; capture 10 s; verify 488 IRQs / 10 s, no overruns |
| Friday    | Run a 60 s capture; annotate; commit                                    |
| Friday    | Measure 3V3 current in the three conditions; write POWER.md             |
| Saturday  | Re-architect (optional): try smaller chunks; try hardware ring; observe |
| Saturday  | Write DMA-DESIGN-NOTE.md; commit                                        |
| Sunday    | Polish; re-run to confirm reproducibility; final commit                 |

Total time budget: ~7 hours. The bulk is on the bench (Thursday-Friday). The writing is Saturday.

---

## What can go wrong (and what to do)

The Week 8 mini-project has more moving pieces than Week 7's. Three failure modes are common.

**Mode A: chunks = 0, no IRQ.** The DMA is configured but not started, or the ADC is configured but not running. Confirm: `dma_channel_is_busy(mp_ch_a)` returns true after `dma_channel_start`; `adc_hw->cs & 1` is set after `adc_run(true)`. If both are true and chunks is still zero, the IRQ handler is not installed or the NVIC line is not enabled.

**Mode B: chunks is correct but `fifo_err` is non-zero.** The DMA is not draining the FIFO fast enough. With a correctly-configured channel this is essentially impossible (the DMA can do 20 MS/s, the ADC produces 0.5 MS/s); look for `INCR_READ=true` on the channel, or for a missing `dma_channel_set_irq0_enabled`. The latter does not stop the DMA but it stops the chain-complete signal you depend on for re-arming.

**Mode C: chunks is correct, error counts are zero, but RMS values are wildly different from expectation.** The samples are arriving but the consumer is processing them wrong. The most common cause is that the consumer is reading the *wrong half* of the ring (off-by-one in the notify-bit decode). Confirm by setting GP13 high for the entire `process_chunk` call and reading on a long Saleae capture; you should see GP13 rises strictly alternate between the periods immediately after a GP14 rise that came from CH_A and one that came from CH_B.

A fourth, rarer mode: **chunks correct, both error counts zero, RMS correct, but `cons_ovr` increments occasionally.** This means a higher-priority task (the FreeRTOS tick? a USB-CDC stall on `printf`?) is preempting the consumer for longer than 2 ms. Check the priorities, check whether the gatekeeper's `printf` is taking a long time (USB-CDC can block for >10 ms during host enumeration; do not let the host re-enumerate during the test run), and consider whether the gatekeeper should be at priority 3 or lowered to 2.

---

## A note on building

This mini-project's source is structured to compile under the standard pico-sdk + FreeRTOS-Kernel CMake setup. The course does not test the build on every commit, and on-device build + flash remains a student responsibility, as in Week 6 and Week 7. If the build fails with messages about missing headers, the most common cause is that `FreeRTOS_Kernel_import.cmake` is pointing at the wrong path; check your `git submodule status` for FreeRTOS-Kernel and `git submodule update --init`.

---

## What this prepares you for

Week 9 (Wire Protocols at Speed) takes this DMA architecture and applies it to SPI: an SD-card streamed at 12 MHz with DMA-paced TX, DMA-paced RX, and a chunk-complete IRQ that posts to a FAT16 driver task. The mini-project of Week 9 records the ADC stream of Week 8 to the SD-card at 1 MB/s with zero dropped samples for a 10-minute run. You will reach for the same chain-pair pattern, the same overrun-detection logic, and the same gatekeeper UART.

Week 10 (Audio I²S) goes the other direction — DMA-driven TX into an I²S DAC at 44.1 kS/s stereo. Same architecture, opposite direction.

By the time you reach Week 14 (Ethernet + lwIP), the DMA chain-pair pattern is reflexive. The week after, in Week 15 (USB Device), you discover that the RP2040's USB controller has its own DMA path that bypasses the general DMA controller — and you spend half the week unlearning your reflexes. That is the texture of embedded systems.
