# Lecture 3 — Ring Buffers and the Single-Producer-Single-Consumer Pattern

> *Two indices, one buffer, no locks. The reader catches up to the writer or the writer catches up to the reader. Either is fine; we just need to know which has happened, and we need to know it without paying for synchronization on every access.*

This lecture covers (a) the SPSC ring-buffer pattern at a level you can defend in a code review, (b) the two RP2040 implementations — channel chaining vs hardware ring — and the trade-off, (c) overrun detection, (d) the chooser table that you should pin to the wall.

---

## 3.1 The producer/consumer problem in one paragraph

A producer generates data faster than a consumer can immediately process it. Both are concurrent — the producer is the DMA channel (driven by hardware), the consumer is a FreeRTOS task (driven by an IRQ-triggered notification). A ring buffer is the standard solution: an N-element circular array, with one *head* index advanced by the producer and one *tail* index advanced by the consumer. The buffer is empty when `head == tail`, full when `((head + 1) mod N) == tail`, and the producer must check for fullness before writing to avoid overwriting unconsumed data — or accept overwrite-on-overrun and report it.

The classical Lamport (1977) result is that on a sequentially-consistent memory model, no atomic operations are required. The proof: the producer only *writes* `head` and only *reads* `tail`; the consumer only *reads* `head` and *writes* `tail`. There is no cross-write — the two indices live in two different "ownership domains". The empty/full tests are decidable from each side's view of the other's index, and the natural read-after-write ordering on a sequentially-consistent core makes the tests correct.

The Cortex-M0+ is in-order, single-issue, no speculative execution, no store buffer reordering (datasheet §2.4.1, p. 78). It is sequentially consistent. Lamport's preconditions hold. Single-core RP2040 SPSC needs no atomics.

(Note: the RP2040 has *two* cores. If the producer is on core 1 and the consumer is on core 0, the inter-core fabric introduces store-ordering that Lamport's proof does not cover; you need `__dmb()` between the data write and the head update. The DMA, however, lives outside both cores — the data write is on the AHB bus and visible to either core after one cycle. So core 1 → core 0 SPSC needs memory barriers; DMA → core 0 SPSC does not, because the DMA's write reaches the consumer's load via the same AHB fabric whose ordering the consumer's pipeline respects.)

---

## 3.2 The ring buffer in C, annotated

The 24-line implementation:

```c
#include <stdint.h>
#include <stdbool.h>

/* The ring buffer's capacity must be a power of two so the modulo
 * operation reduces to a bitwise AND. */
#define RING_CAPACITY    ((uint32_t)2048u)  /* 2048 halfwords = 4 KB */
#define RING_MASK        ((uint32_t)(RING_CAPACITY - 1u))

typedef struct {
    uint16_t  data[RING_CAPACITY];  /* the buffer itself */
    volatile uint32_t  head;        /* producer writes; consumer reads  */
    volatile uint32_t  tail;        /* consumer writes; producer reads  */
} ring_t;

/* Producer side: write one element. Returns false if the ring is full
 * (the next write would collide with the consumer's tail). */
static inline bool ring_push(ring_t * const r, const uint16_t v)
{
    const uint32_t next_head = (r->head + 1u) & RING_MASK;
    if (next_head == r->tail) {
        return false;  /* full */
    }
    r->data[r->head] = v;
    r->head = next_head;
    return true;
}

/* Consumer side: read one element. Returns false if empty. */
static inline bool ring_pop(ring_t * const r, uint16_t * const out)
{
    if (r->head == r->tail) {
        return false;  /* empty */
    }
    *out = r->data[r->tail];
    r->tail = (r->tail + 1u) & RING_MASK;
    return true;
}
```

`volatile` on the indices is not strictly necessary on the M0+ (the loads/stores in the function bodies cannot be hoisted by the compiler past the data accesses; the dataflow forbids it), but it makes the intent legible and protects against future compilers being more aggressive. Cost is one cycle per access; we will pay it.

This is fine for software producer + software consumer. **It is not what we use this week.** The DMA does not call `ring_push`; the DMA writes directly to `data[]` based on its own `WRITE_ADDR` pointer, and the head update happens implicitly when the DMA channel's `TRANS_COUNT` decrements. The consumer reads from `data[tail]` and updates `tail`. The head is *the DMA's write pointer modulo the ring size*, which the consumer derives by reading `dma_channel_hw_addr(ch)->write_addr` and subtracting the ring base.

---

## 3.3 Two RP2040 implementations: chain vs hardware ring

### Implementation A: hardware ring on a single DMA channel

One DMA channel. `CTRL.RING_SIZE=12, RING_SEL=1` makes `WRITE_ADDR` auto-wrap at 4 KB boundaries. `TRANS_COUNT = UINT32_MAX` makes the channel run forever. No IRQ.

The consumer's loop:

```c
/* The DMA's current write address, masked to the ring offset. */
const uintptr_t base = (uintptr_t)ring_data;
const uint32_t  dma_head = (uint32_t)(
    (dma_channel_hw_addr(ch)->write_addr - base) / sizeof(uint16_t)
);
/* Bytes available to read: (dma_head - tail) modulo ring size. */
const uint32_t available = (dma_head - tail) & RING_MASK;

if (available >= CHUNK_SIZE) {
    /* Drain CHUNK_SIZE elements. */
    process_chunk(&ring_data[tail], CHUNK_SIZE);
    tail = (tail + CHUNK_SIZE) & RING_MASK;
}
```

**Pros:** simplest hardware setup. One channel claimed. No IRQ. The consumer can poll at any rate.

**Cons:** the consumer must poll (or use a periodic FreeRTOS timer); there is no IRQ on a natural chunk boundary. Overrun detection requires the consumer to compute `(dma_head - tail) > RING_SIZE - some_margin` and react before the DMA's wrap collides with the unread tail. If the consumer is suspended for any reason — a SysTick storm, a long printf — the producer keeps writing and silently overwrites unread data.

**When to use:** low-event-rate streams where the consumer is periodic (e.g., a 1 ms timer reads whatever has arrived in the last millisecond). UART logging is the canonical fit.

### Implementation B: channel chaining for ping-pong

Two DMA channels. Each channel's `TRANS_COUNT` is half the ring (1024 halfwords = 2 KB). Channel A's `WRITE_ADDR` points at the first half; B's at the second. `CHAIN_TO`: A→B and B→A. `IRQ_QUIET=false` on both; the chain-complete IRQ wakes the consumer.

The IRQ handler:

```c
void dma_irq0_handler(void)
{
    BaseType_t higher_prio_woken = pdFALSE;

    /* Channel A completed? */
    if (dma_channel_get_irq0_status((uint32_t)CH_A)) {
        dma_irqn_acknowledge_channel(0u, (uint32_t)CH_A);
        /* Re-arm A's write pointer; chain only re-triggers, not re-loads. */
        dma_channel_set_write_addr((uint32_t)CH_A, ring_half_a, false);
        dma_channel_set_trans_count((uint32_t)CH_A, SAMPLES_PER_CHUNK, false);
        /* Tell the consumer: half A is now full and ready to drain. */
        xTaskNotifyFromISR(consumer_task, BUFFER_A_READY,
                           eSetBits, &higher_prio_woken);
    }
    if (dma_channel_get_irq0_status((uint32_t)CH_B)) {
        dma_irqn_acknowledge_channel(0u, (uint32_t)CH_B);
        dma_channel_set_write_addr((uint32_t)CH_B, ring_half_b, false);
        dma_channel_set_trans_count((uint32_t)CH_B, SAMPLES_PER_CHUNK, false);
        xTaskNotifyFromISR(consumer_task, BUFFER_B_READY,
                           eSetBits, &higher_prio_woken);
    }
    portYIELD_FROM_ISR(higher_prio_woken);
}
```

The consumer's loop:

```c
for (;;) {
    uint32_t notify = 0u;
    xTaskNotifyWait(0u, UINT32_MAX, &notify, portMAX_DELAY);
    if (notify & BUFFER_A_READY) {
        process_chunk(ring_half_a, SAMPLES_PER_CHUNK);
    }
    if (notify & BUFFER_B_READY) {
        process_chunk(ring_half_b, SAMPLES_PER_CHUNK);
    }
}
```

**Pros:** clean per-half IRQ on a natural chunk boundary. The consumer knows exactly which half to drain (no pointer arithmetic). The processing time per chunk is bounded — the consumer must finish before the *other* channel completes, giving 2 ms of slack at 500 kS/s and 1024-sample chunks. Overrun detection is the IRQ: if the consumer is still draining A when B's IRQ fires for the *second* time (i.e., A's IRQ fired and B's IRQ fired before the consumer woke for A), the consumer has missed a beat and we can log it.

**Cons:** two channels claimed instead of one. IRQ overhead on every half-fill (~5 µs of FreeRTOS notify, 488 times per second at 500 kS/s = 2.5 ms/s of CPU = 0.25 % utilization). The re-arm-in-IRQ logic is a place to put a bug.

**When to use:** DSP-style fixed-chunk processing where the consumer wants whole frames. ADC capture is the canonical fit. Audio I²S, SPI sensor streaming, any sample-by-sample pipeline with chunk-based downstream processing.

---

## 3.4 The chunk-size trade-off

Smaller chunks = lower IRQ-to-consume latency (the consumer sees data sooner) but higher IRQ rate and higher CPU overhead per second.

At 500 kS/s with 16-bit samples:

| Chunk size (samples) | Bytes per chunk | IRQ rate | CPU overhead at 5 µs / IRQ |
|---------------------:|----------------:|---------:|---------------------------:|
| 64                   | 128             | 7.8 kHz  | 3.9 % (39 ms/s)            |
| 128                  | 256             | 3.9 kHz  | 2.0 %                      |
| 256                  | 512             | 1.95 kHz | 1.0 %                      |
| 512                  | 1024            | 977 Hz   | 0.5 %                      |
| 1024                 | 2048            | 488 Hz   | 0.25 %                     |
| 2048                 | 4096            | 244 Hz   | 0.12 %                     |

The ADC's FIFO is 8 samples, so the *minimum* useful chunk is 8 (below that you would be wasting DMA transfers on a partially-filled FIFO). Real chunk sizes for audio/DSP are 256 to 1024 — large enough to support a meaningful FFT, small enough that the latency-to-output is < 5 ms.

The mini-project uses 1024. This gives 488 IRQs/s, 0.25 % CPU on the IRQ alone, and a 2.05 ms chunk-period over which the consumer must complete its RMS/peak compute. Measured per-chunk compute (Cortex-M0+ at 125 MHz, 1024 halfword RMS): ~120 µs. CPU utilization for processing: 5.9 %. Net CPU: ~6.2 %. Idle: 93.8 %. The remaining headroom is for the UART gatekeeper, the FreeRTOS tick, and future work.

---

## 3.5 Overrun detection — the bench check

Overrun on the ADC side: the FIFO error bit (Lecture 2 §2.2). The consumer masks bit 15 of every sample and increments a counter if set:

```c
uint32_t fifo_overrun_count = 0u;
for (uint32_t i = 0u; i < CHUNK_SIZE; ++i) {
    if ((chunk[i] >> 15) & 1u) {
        ++fifo_overrun_count;
    }
    chunk[i] &= 0x0FFFu;  /* mask to 12 bits of data */
}
```

A passing 60 s run shows `fifo_overrun_count == 0`. Any non-zero value means the DMA is not keeping up with the ADC — which on the RP2040 is impossible unless you have misconfigured the channel (e.g., `INCR_READ=1`), so a non-zero count is a configuration bug.

Overrun on the DMA side: the consumer is too slow. The signal is that *two* chain-complete IRQs fire between consumer wakeups — the kernel sees two task-notify-set-bits and the consumer's `notify` value comes back with both `BUFFER_A_READY` and `BUFFER_B_READY` set, *and* the channel that is currently writing has advanced past the half the consumer is about to drain. The detection is in the IRQ handler:

```c
static uint32_t consumer_overrun_count = 0u;
/* In the IRQ, after dma_irqn_acknowledge_channel: */
if (xTaskNotifyStateClearFromISR(consumer_task) == pdTRUE) {
    /* The notification was already pending when we arrived — the
     * consumer has not yet drained the previous chunk. Count it. */
    ++consumer_overrun_count;
}
```

A passing run shows `consumer_overrun_count == 0`. A non-zero value means the consumer's chunk-processing time exceeded the chunk period — at 1024 samples / 500 kS/s = 2.048 ms — and you need either a smaller chunk, a faster consumer, or to elevate the consumer's priority.

Both counters are committed in `OVERRUN-COUNTERS.md` at the end of the mini-project run.

---

## 3.6 The chooser table

The single table that justifies the whole lecture. Pin it.

| Question                                    | Use single channel + ring wrap | Use channel chaining + ping-pong |
|---------------------------------------------|-------------------------------|----------------------------------|
| Do you want an IRQ per chunk?               | No                            | Yes                              |
| Do you process in fixed chunks?             | Optional                      | Yes                              |
| Is your consumer periodic (timer-driven)?   | Yes                           | No (event-driven)                |
| Is your data rate < 50 kS/s?                | Either works                  | Either works                     |
| Is your data rate > 200 kS/s?               | Either works, ring is lighter | Chaining is conventional         |
| Do you need power-of-two ring alignment?    | Yes (`__attribute__((aligned)))`) | Each half aligned; pair contiguous |
| How many DMA channels does it cost?         | 1                             | 2                                |
| Per-second CPU overhead at 1024-chunk rate? | 0 % (no IRQ)                  | 0.25 % (one IRQ per 1024 samples)|
| Cleanest overrun detection?                 | Read TRANS_COUNT vs tail in poll | Compare task-notify state at IRQ entry |
| Canonical use                               | UART log streaming            | ADC capture for DSP              |

The mini-project's choice — chain-pair + ping-pong — is in the "ADC capture for DSP" cell. The hardware-ring exercise (Exercise 2) lives in the "UART log streaming" cell, just used here for the ADC for pedagogy.

---

## 3.7 What the consumer task actually does

The consumer is the part the curriculum is least prescriptive about, because it depends on what you want to *do* with the samples. Three useful patterns:

1. **RMS / peak detector.** For each chunk, compute the mean-square value and the maximum absolute deviation from midscale. Output one line per chunk to UART. This is what the mini-project does.
2. **Threshold trigger.** For each chunk, check if any sample exceeds a configurable threshold; if so, latch the timestamp and the surrounding samples to a separate buffer for inspection. This is the classic "scope trigger" mode.
3. **Buffered FFT.** Accumulate samples across multiple chunks until you have enough for an N-point FFT (typically N = 256, 512, or 1024 — match to your chunk size). Run the FFT. This is what the optional extension in Challenge 2 walks through.

All three can be done in < 500 µs per chunk on the M0+ at 125 MHz; the consumer's budget is ~1.5 ms (well under the 2.05 ms chunk period). The RMS calculation in the mini-project is ~120 µs; we have plenty of headroom for the gatekeeper UART send and the tick interrupt.

---

## 3.8 What "correctness" means here

The system is correct when:

1. The DMA never stops. `TRANS_COUNT` is re-armed in every IRQ; the channels chain forever.
2. The ADC never overruns its FIFO. `fifo_overrun_count == 0` over the full run.
3. The consumer never overruns the producer. `consumer_overrun_count == 0` over the full run.
4. The samples are not corrupted. The RMS output is within 1 % of the expected value for a known input (a sine of known amplitude).
5. The CPU is largely idle. `vTaskGetRunTimeStats` reports < 10 % utilization summed across all tasks.

The bench artifacts that prove these are the Saleae capture (chain-complete IRQs at exactly 488.28 Hz, no missing edges), the `OVERRUN-COUNTERS.md` file, the UART log showing RMS values matching the input, and the `vTaskGetRunTimeStats` dump in `CPU-UTILIZATION.md`.

---

## 3.9 Summary

The SPSC ring-buffer pattern needs no atomics on the M0+ thanks to Lamport. The DMA writes; the FreeRTOS task reads; the indices are managed by hardware (the chain-complete IRQ) or by polling (the hardware-ring write-pointer). Channel chaining is the right answer for fixed-chunk DSP-style processing; hardware ring is the right answer for telemetry streams with periodic consumers. Overrun is detected on both the ADC side (FIFO error bit) and the DMA side (task-notify-state-already-set), and both counters must read zero at the end of a passing run.

You have the tools now. The exercises walk you through each piece in isolation; the mini-project ties them together.
