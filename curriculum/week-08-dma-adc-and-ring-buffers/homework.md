# Week 8 — Homework

Six problems. The intent is to consolidate the concepts from the lectures and to give you practical exercises that complement the in-class work without duplicating it. The problems are sized roughly equally; budget about 50 minutes each.

Submit answers in `homework-answers.md` plus any supporting source files in `homework/`. Code that does not compile under the pico-sdk loses half its credit even if the logic is sound — set up your build environment before you start.

---

## Problem 1 — Compute the per-channel rate (45 min)

You configure the ADC with:

```c
adc_init();
adc_gpio_init(26u); adc_gpio_init(27u); adc_gpio_init(28u);
adc_select_input(0u);
adc_set_round_robin(0x07u);  /* channels 0, 1, 2 enabled */
adc_set_clkdiv(95.0f);
adc_fifo_setup(true, true, 4u, true, false);
```

(a) What is the per-channel sample rate? Show the formula and the computation.

(b) What is the aggregate rate at which the FIFO is being written?

(c) At what FIFO level does DREQ_ADC assert? How often does the DMA channel see a DREQ assertion?

(d) If the DMA channel is configured to read 16 halfwords per DREQ (i.e. it processes a "burst" of four DREQ assertions sequentially before the FIFO drops below threshold), is the FIFO ever above 4 entries? Explain.

---

## Problem 2 — Write the channel configuration (60 min)

Write the C code to configure a DMA channel with the following properties, using only the SDK's `channel_config_*` setters:

- Data size: 32-bit word
- Read pointer: fixed at `PIO_FIFO_RX` (a peripheral address; use `(void*)0x50200020` as a placeholder)
- Write pointer: incrementing, into a buffer named `pio_capture[1024]`
- DREQ: PIO0 SM1 RX (= DREQ index 5; verify against the datasheet table)
- Chain-to: itself (no chaining)
- Ring wrap: 4 KB (12 bits), on the write pointer
- IRQ-quiet: false (we want IRQ on completion)
- Transfer count: 1024

Commit as `homework/problem-2.c`. The file should compile (with appropriate stubs for any types it does not have).

---

## Problem 3 — Reverse-engineer the bug (45 min)

The following code snippet is from a colleague's project. It compiles. When run, every sample in `buffer[]` after the first eight is 0xFFFF (with the error bit set in bit 15). Identify the bug and explain in two sentences why it manifests as 0xFFFF specifically.

```c
adc_init();
adc_gpio_init(26u);
adc_select_input(0u);
adc_set_clkdiv(0.0f);
adc_fifo_setup(true, true, 1u, true, false);

dma_channel_config cfg = dma_channel_get_default_config(0);
channel_config_set_transfer_data_size(&cfg, DMA_SIZE_16);
channel_config_set_read_increment(&cfg, true);   /* <- look here */
channel_config_set_write_increment(&cfg, true);
channel_config_set_dreq(&cfg, DREQ_ADC);

dma_channel_configure(0, &cfg, buffer, &adc_hw->fifo, 1024, true);
adc_run(true);
```

Hint: the address space after `ADC_BASE + 0x04` (the FIFO register) is documented in the datasheet's peripheral memory map (§2.1.1).

---

## Problem 4 — Compute the ring-buffer latency (45 min)

In the mini-project's chain-pair pingpong setup:

- ADC: 500 kS/s aggregate
- Chunk size: 1024 halfwords (2 KB) per chain-complete IRQ
- Consumer task: priority 4, processes a chunk in 120 µs

Compute:

(a) The chain-complete IRQ rate.

(b) The end-to-end latency from "a particular sample arrives at the ADC pin" to "the consumer task has finished processing that sample". Decompose into: ADC conversion time, FIFO wait time, DMA transfer time, wait-for-half-complete time, IRQ-to-task time, consumer chunk processing time.

(c) If you halve the chunk size to 512 samples, how does each component of the latency change? What is the new total?

(d) Halving the chunk size doubles the IRQ rate. What is the CPU utilization for IRQ handling at 1024 and at 512 chunk size? (Assume the IRQ handler takes 5 µs.)

---

## Problem 5 — Stack-size budget for the consumer (40 min)

The Week 8 consumer task in Exercise 3 is declared with `configMINIMAL_STACK_SIZE + 256u` words. `configMINIMAL_STACK_SIZE` in the pico-sdk default is 128.

(a) Convert the stack size to bytes (the Cortex-M0+ has 4-byte words).

(b) The consumer's deepest call chain is: `consumer_task` → `xTaskNotifyWait` → kernel internals (queue operations) → `process_chunk` → `__aeabi_uldivmod` (for the 64-bit divide in the RMS). Estimate the per-function stack usage (look at compiled output from `arm-none-eabi-objdump -S`).

(c) Is the budget adequate? What is the safety margin?

(d) What FreeRTOS configuration option enables high-water-mark tracking? What is the cost of enabling it?

---

## Problem 6 — Design a UART logging gatekeeper (60 min)

Sketch (in pseudocode or C) a gatekeeper task that owns the UART and exposes a single API:

```c
void log_send(const char * fmt, ...);
```

The task should:

- Accept log strings from multiple producer tasks without dropping any.
- Print at most one line every 100 ms (rate-limit; if more arrive, queue them).
- Drop log lines silently if the input queue is full (return immediately, no block) — log producers must never block on this call.
- Re-order from priority-1 (e.g. errors) to priority-3 (e.g. debug) if multiple are queued.

You do not need to implement the formatting; assume the caller passes a pre-formatted string.

Justify your choice of FreeRTOS primitive (queue, stream buffer, multiple queues for priority, etc.) and your queue depth. Cite the FreeRTOS reference manual section.

This is a deliberately open problem. There is no single right answer; the grading rewards a design that explicitly addresses each requirement.

---

## Submission

- `homework-answers.md` with answers to Problems 1, 3, 4, 5 (text-based).
- `homework/problem-2.c` (compilable).
- `homework/problem-6-design.md` with the sketch and justification.

Total expected effort: ~5 hours. The homework is graded for engineering reasoning, not for line-by-line correctness; partial credit is liberal.
