# Week 8 Exercises — Solutions and Bench Notes

This file walks through each exercise in detail: what you should see on the UART, what the Saleae should show, the most common ways the exercise breaks, and the diagnostic steps for each. Read this *after* attempting the exercise; the value is in the contrast between what you did and what was expected.

---

## Exercise 1 — DMA memory-to-memory copy

### Expected UART output

```
Exercise 1 - DMA memory-to-memory throughput
Transfer size: 8192 bytes (2048 words)
  memcpy()           :     96 us
  DMA, DATA_SIZE=word    :     17 us
  DMA, DATA_SIZE=halfword:     33 us
  DMA, DATA_SIZE=byte    :     65 us
Done.
```

Numbers will vary +/- a few µs depending on the SRAM bank arrangement of `src[]` and `dst[]` (the linker may place them in the same 32 KB SRAM bank, in which case bus contention adds ~5 µs to the word-mode case). The ratios are stable.

### The "why" of the numbers

- **`memcpy()` at 96 µs.** The pico-sdk's `memcpy` uses a word-at-a-time loop with a load-store dual issue impossible on M0+; effective rate is one word per 4 cycles + loop overhead = ~85 MB/s. 8 KB / 85 MB/s ≈ 96 µs.
- **DMA word at 17 µs.** One transfer per 4 bytes, 2048 transfers, AHB-Lite cycle ~6 ns each = ~12 µs raw; arbiter slot allocation and `dma_channel_wait_for_finish_blocking` overhead bring it to ~17 µs. Effective 480 MB/s.
- **DMA halfword at 33 µs.** 4096 transfers, double the per-transfer cost. The arbiter allocates a slot every transfer regardless of size.
- **DMA byte at 65 µs.** 8192 transfers; the arbiter overhead now dominates.

### How it can fail

- **Verify fails after the first DMA call.** `INCR_READ` or `INCR_WRITE` is wrong. With both `false`, the channel writes one word and stops with `dst[]` mostly zero. With both `true` (correct), the channel marches both pointers.
- **The channel never finishes.** `TREQ_SEL` is not set to 0x3F. The channel waits forever for a DREQ that never comes. Confirm with `dma_channel_hw_addr(ch)->ctrl_trig & (1u << 24)` — `BUSY` is set, no progress.

### Extension

Re-run with `__attribute__((aligned(32)))` on both buffers; you should see the byte case improve by ~5 µs as the AHB arbiter can burst-align.

---

## Exercise 2 — ADC to DMA, single-shot capture

### Expected UART output (potentiometer at midscale)

```
Exercise 2 - ADC to DMA single-shot capture
Using DMA channel 0, DREQ=36 (DREQ_ADC)
Capture: 1024 samples in 2048 us (expected ~2048 us)
  FIFO error bits set : 0
  min / max counts    : 2040 / 2056
  mean counts (x1000) : 2047800
  mean volts (mV)     : 1649

[four more captures, similar numbers]

Done.
```

The captured time should be 2048 µs +/- 1 µs (the SDK rounds the clkdiv setting; 0.0 gives exactly 500 kS/s, but the host timer's resolution is 1 µs). The mean should match your source voltage; min/max should span ~16 LSB (the ADC's noise floor is ~0.6 LSB but the input pin picks up ~3-5 LSB of switching noise from the digital supply unless you have decoupled `ADC_AVDD` carefully).

### The "why" of the FIFO error count

If you see `FIFO error bits set: N` with N > 0, the DMA was not draining the FIFO fast enough. On a single-shot 1024-sample capture, the FIFO can hold 8 entries; the ADC produces a sample every 2 µs; the DMA reads a sample every 48 ns (well below 2 µs). The error count should be exactly zero. If it is not, one of:

- `INCR_READ=true` (you misset it; the read pointer marched past `ADC_FIFO`).
- `DATA_SIZE` wrong (set to byte or word instead of halfword).
- `TREQ_SEL` wrong (not pointing at DREQ_ADC = 36).

Re-check the channel config; the bug is almost certainly in one of these three fields.

### Saleae trace

GP14 goes high for the full capture duration (~2 ms). It is a single rectangle. If you see GP14 high for less than 1 ms or for more than 3 ms, the DMA is finishing early (likely an `INCR_READ=true` bug — channel ran the count to zero in the first few transfers because `READ_ADDR` returned garbage that looked like a valid transfer) or hanging (`TREQ_SEL` wrong).

### Extension

Swap `adc_select_input(0)` for `adc_select_input(4)` (the temperature sensor). Add `adc_set_temp_sensor_enabled(true)` before. You will read about 700-800 counts at room temperature; subtract from 2047 and convert with the datasheet's formula (§4.9.5, p. 569) for an actual temperature in °C. Useful sanity check that the whole path works on a known signal.

---

## Exercise 3 — Channel chaining + FreeRTOS consumer

### Expected UART output

```
Exercise 3 - ADC -> chained DMA ping-pong -> FreeRTOS
DMA channels: A=0, B=1
chunks=488 fifo_err=0 consumer_overrun=0
chunks=977 fifo_err=0 consumer_overrun=0
chunks=1465 fifo_err=0 consumer_overrun=0
chunks=1953 fifo_err=0 consumer_overrun=0
...
```

`chunks` should increment by 488 per second (= 500000 / 1024, rounded; the rate is exactly 488.28125 Hz, so over 60 s you see 29297 chunks). Both error counters must read zero throughout. If `fifo_err` is non-zero, the ADC FIFO is overflowing — this should not happen on a correct setup. If `consumer_overrun` is non-zero, the consumer task is taking longer than 2.048 ms per chunk; reduce the chunk processing time or raise the consumer's priority.

### Expected Saleae trace (4 channels)

| Channel | Pin  | Signal                                  | Rate      |
|--------:|:-----|:----------------------------------------|:----------|
| 0       | GP14 | Chain-complete IRQ pulse (high for ~5 µs) | 488.28 Hz |
| 1       | GP13 | Consumer-task in-process (high for ~120 µs) | 488.28 Hz |
| 2       | GP12 | Consumer-task chunk-finish pulse        | 488.28 Hz |
| 3       | GP11 | Overrun marker (silent)                 | 0 Hz      |

The IRQ-to-consumer-start latency (GP14 falling edge to GP13 rising edge) should be 3-5 µs. The consumer's processing time (GP13 high to GP12 pulse) is ~120 µs. The slack until the next IRQ is ~1.9 ms.

### How it can fail (the five canonical bugs)

1. **One sample value repeated through the buffer.** `INCR_WRITE` is false on one of the channels.
2. **FIFO error bit set on every sample after the eighth.** `INCR_READ` is true on one of the channels; the read pointer is marching past `ADC_FIFO`.
3. **Every other sample is zero (or 0xF000).** `DATA_SIZE` is set to `DMA_SIZE_32` instead of `DMA_SIZE_16`; the high half-word of the FIFO register (reserved bits) is being copied.
4. **DMA finishes its first half, never IRQs.** `dma_channel_set_irq0_enabled` was not called for that channel. Confirm with `dma_hw->ints0` — bit should be set when the channel completes, even if the NVIC line is disabled.
5. **First ring traversal works; second traversal corrupts data.** You forgot to re-arm `WRITE_ADDR` and/or `TRANS_COUNT` in the IRQ handler. The chain re-triggers but does not reload register state.

If your system fails in a way that does not match these five, re-read your IRQ handler. The bug is almost always there.

### How to confirm the consumer's priority

Run the system with the consumer at priority 1 (below the reporter at priority 2 and below the idle task at 0). Expected behavior: `consumer_overrun` increments every few seconds because the reporter's `printf` (which can block on UART for several ms with default config) preempts the consumer. Then raise the consumer to priority 4. The overrun count stays at zero.

This is the priority-inversion pathology from Week 7 manifesting on a DMA pipeline. The fix is the same: the consumer must be the highest-priority data-handling task.

### Saleae annotation suggestions

In your committed `.sal` file, annotate at least three transitions:

1. The first IRQ after `adc_run(true)`. Annotate "first chain-complete; latency from adc_run is ~2.05 ms".
2. A mid-run IRQ. Annotate "steady-state; period 2048 µs, IRQ-to-task 4 µs".
3. The last IRQ before you stopped the capture. Annotate "no edge missed in N seconds".

The committed `.sal` is one of two pass-criterion artifacts for this exercise.

---

## The five canonical DMA bugs — diagnostic flowchart

Use this when something is wrong and you do not know where to start.

```
Is the DMA channel running at all? (dma_hw->ch[c].ctrl_trig & (1<<24))
├── No.
│   ├── Is TREQ_SEL correct? (DREQ_ADC=36 for ADC, 0x3F for memcpy)
│   ├── Is the ADC actually running? (adc_hw->cs & 1) — adc_run(true) called?
│   ├── Is the FIFO DREQ_EN set? (adc_hw->fcs & (1<<3)) — adc_fifo_setup args?
│   └── Did the previous run drain TRANS_COUNT to zero without re-arm?
└── Yes.
    ├── Are samples landing in the right buffer? Look at WRITE_ADDR.
    │   ├── WRITE_ADDR not advancing -> INCR_WRITE=0, bug #1.
    │   └── WRITE_ADDR advancing past the half boundary -> ring/chain
    │       wrap not set, or chain partner's WRITE_ADDR not re-armed.
    ├── Are sample values correct?
    │   ├── Same value repeated -> bug #1 (INCR_WRITE=0).
    │   ├── Every-other value zero -> bug #3 (DATA_SIZE wrong).
    │   └── Values look random/garbage -> bug #2 (INCR_READ=1) or you
    │       are reading the wrong address (verify dma_channel_hw_addr).
    └── Is the IRQ firing?
        ├── No -> bug #4 (dma_channel_set_irq0_enabled not called).
        └── Yes, but consumer never wakes -> task handle wrong; or
            FreeRTOS scheduler not started; or NVIC priority above
            configMAX_SYSCALL_INTERRUPT_PRIORITY (rare on Pico SDK port).
```

---

## A note on Cortex-M0+ stack-overflow detection

The exercises do not enable FreeRTOS stack-overflow checking by default for keep-things-simple reasons. For the mini-project you should enable `configCHECK_FOR_STACK_OVERFLOW = 2` and provide a `vApplicationStackOverflowHook` that toggles GP15 and prints the offending task's name. Even though Week 8 tasks have small stacks (256-word consumer, 256-word reporter), the ISR-driven re-entry into FreeRTOS notify can grow the consumer's stack unexpectedly during high IRQ bursts. The hook costs ~20 cycles per context switch but is worth it for the first week of DMA work.
