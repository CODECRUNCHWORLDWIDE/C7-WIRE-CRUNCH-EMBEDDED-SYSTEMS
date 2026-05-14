# Lecture 1 — The RP2040 DMA Controller

> *Read the datasheet first. The SDK's C API is a thin wrapper around a register file; the SDK is convenient, but the register file is what is actually moving the bytes. If you only learn the SDK's spelling of the names, the first non-trivial bug — and there will be one — will read as opaque.*

This lecture is structured around the RP2040 datasheet §2.5 (pp. 102–144). Open the PDF. Every claim below has a section + page citation; verify each as you go. The exercises and the mini-project assume the vocabulary in this lecture is automatic.

---

## 1.1 What the DMA controller is

The DMA controller is a piece of silicon whose job is to move data between addresses without involving the CPU. On the RP2040 (datasheet §2.5.1, p. 102) it is twelve independent channels feeding a shared AHB-Lite bus master, with an arbiter resolving simultaneous channel requests.

Each channel has its own register file. The five user-visible registers per channel are:

| Register     | Offset | Purpose                                                                |
|--------------|-------:|------------------------------------------------------------------------|
| `READ_ADDR`  | 0x00   | The source address. Optionally auto-incremented after each transfer.    |
| `WRITE_ADDR` | 0x04   | The destination address. Optionally auto-incremented.                   |
| `TRANS_COUNT`| 0x08   | Remaining transfers. Decremented on each completion. Halts at zero.     |
| `CTRL_TRIG`  | 0x0C   | Channel control word (all the configuration bits) — writing arms the channel and starts it if `EN` is set. |
| `CTRL`       | 0x10   | The same control word, alias that does *not* start the channel.         |

Datasheet §2.5.2, pp. 108–112, gives the full register list including the `AL1_*` through `AL3_*` aliases used for atomic re-arming (you write a single word and it writes `WRITE_ADDR` *and* triggers the channel). The aliases matter when you re-arm from inside an ISR and cannot afford the two-write race.

The CTRL register is the load-bearing part. Section 2.5.3 (pp. 119–129) defines it field by field; the rest of this lecture walks through every field.

---

## 1.2 The CTRL register, field by field

The full layout is datasheet Table 124 (p. 120). The field order below matches the bit positions; the names match the SDK's C constants.

### `EN` (bit 0)

`1` enables the channel; `0` disables. The channel will not transfer with `EN=0` regardless of any DREQ. Writing `0` to a running channel does not abort — use `dma_channel_abort` (datasheet §2.5.5, p. 124) or write the `CHAN_ABORT` register.

### `HIGH_PRIORITY` (bit 1)

The arbiter normally round-robins between channels. With `HIGH_PRIORITY=1` the channel jumps the queue. Useful for time-sensitive sinks (DAC streaming); not useful for memory-to-memory copies. We will not set this in Week 8.

### `DATA_SIZE` (bits 2–3)

`00` = byte (8 bits), `01` = halfword (16 bits), `10` = word (32 bits), `11` = reserved. This must match the peripheral's native FIFO width. The ADC FIFO is 16 bits wide (datasheet §4.9.2, p. 562); we set `DATA_SIZE=01`. Setting it wrong is bug #3 from the README: read a 32-bit word from a 16-bit FIFO and you get one valid sample, one stale half-word per transfer.

### `INCR_READ` (bit 4)

`1` increments `READ_ADDR` by `DATA_SIZE` bytes after each transfer; `0` holds the read pointer fixed. Set `0` for a peripheral source (the ADC FIFO is at one address); set `1` for a memory source (a `memcpy`-style transfer). Bug #2 from the README: `INCR_READ=1` from the ADC FIFO marches the read pointer past `ADC_FIFO`, you read register garbage and the FIFO never drains.

### `INCR_WRITE` (bit 5)

Symmetric to `INCR_READ` on the destination side. `1` for a memory destination, `0` for a peripheral destination (DAC, PIO TX FIFO). Bug #1 from the README: `INCR_WRITE=0` with an SRAM destination means every transfer writes the same address — you see one sample, repeated forever.

### `RING_SIZE` (bits 6–9) and `RING_SEL` (bit 10)

The hardware ring-buffer feature. `RING_SIZE` selects a power-of-two byte count from 2 (=2^1) up to 32768 (=2^15); the actual wrap size is `1 << RING_SIZE` bytes. `RING_SEL=0` wraps the *read* pointer (rare; useful for reading a sliding window from a peripheral with multiple registers); `RING_SEL=1` wraps the *write* pointer (the common case — a ring buffer in SRAM). Setting `RING_SIZE=0` disables wrapping.

The wrap is a *bitwise mask* on the pointer: the lower `RING_SIZE` bits roll over, the upper bits stay fixed. This means **the buffer must be aligned to its own size**. A 4 KB ring (`RING_SIZE=12`, wrap mask `0xFFF`) requires the buffer's base address to have its low 12 bits zero. The SDK's `aligned` attribute or a careful linker script section enforces this; you cannot just `uint16_t buf[2048]` at file scope and hope the linker picks a 4 KB-aligned address.

Datasheet §2.5.3.2 (p. 121) shows the exact register encoding.

### `CHAIN_TO` (bits 11–14)

The 4-bit channel index of the channel to trigger when this one's `TRANS_COUNT` reaches zero. `CHAIN_TO=self` (i.e., this channel's own index) disables chaining (the documentation calls this the "self-chain disable" idiom).

When chain fires, the target channel's `CTRL_TRIG` is written — which means the target channel must have its read/write/count registers *already loaded*. Chaining only re-triggers; it does not re-load. This is the most common Week 8 bug after CTRL field mistakes: chain B from A, fail to re-arm B's `WRITE_ADDR` after B itself completes, B keeps writing to the same buffer half.

Datasheet §2.5.6.2 (pp. 127–129) covers the chain timing in detail. Key fact: the chain trigger is single-cycle and fires at the moment `TRANS_COUNT` is decremented to zero, *before* the channel goes idle. This is why you must re-arm the *just-completed* channel from its IRQ handler, not from the *next* channel's IRQ handler — the just-completed channel still has stale register values from the run that finished.

### `TREQ_SEL` (bits 15–20)

The 6-bit DREQ index. The DREQ table is datasheet Table 119, p. 121. Highlights:

| DREQ index | Source                                       |
|-----------:|----------------------------------------------|
| 0–11       | DMA channel N self-chain (timer or paced)    |
| 20         | PIO0 SM0 TX                                  |
| 24         | PIO0 SM0 RX                                  |
| 36         | ADC FIFO                                     |
| 38–41      | I²C0/I²C1 TX / RX                            |
| 42–45      | SPI0/SPI1 TX / RX                            |
| 0x3F       | "Permanent" — transfer at full bus rate, no pacing |

For the ADC capture: `TREQ_SEL=36`. The DMA channel will issue exactly one transfer per ADC-FIFO-non-empty assertion.

`TREQ_SEL=0x3F` is the "go as fast as you can" mode — useful for memory-to-memory copies. We use it in Exercise 1.

### `IRQ_QUIET` (bit 21)

`1` suppresses the channel's IRQ-on-completion line. Set this on chained-pair channels where the IRQ should only fire from one of the two; otherwise you get two IRQ assertions per ring-traversal and the kernel wakes the consumer twice.

We will set `IRQ_QUIET=1` on one channel of the ping-pong pair in Exercise 3 and the mini-project.

### `BSWAP` (bit 22)

Byte-swap on transfer. Useful for converting between little-endian and big-endian wire formats. Not used in Week 8; we will use it in Week 14 (Ethernet) and Week 19 (SD-card).

### `SNIFF_EN` (bit 23)

Enables the sniff peripheral — a CRC/sum engine that the DMA can feed in parallel with the transfer. Not used in Week 8; we will use it in Week 11 for SD-card integrity checks.

### `BUSY` (bit 24, read-only)

`1` while the channel is mid-transfer. Useful for polling completion in a tight loop (Exercise 1 does this for the memcpy timing).

### `WRITE_ERROR`, `READ_ERROR`, `AHB_ERROR` (bits 29–31, read-only)

Bus errors. If the DMA attempts a transfer to/from an unmapped address, one of these latches. Useful for catching pointer arithmetic bugs in flight — read in a startup self-test or after `dma_channel_abort`.

---

## 1.3 The DREQ mechanism, end-to-end

Walk a single ADC sample through the DMA, step by step:

1. **ADC** finishes a conversion. The 12-bit result is pushed into the ADC FIFO; the FIFO depth counter increments from 0 to 1.
2. **ADC** asserts `DREQ_ADC` (DREQ index 36) because the FIFO depth is now ≥ the threshold (which we set to 1 in `adc_fifo_setup`).
3. **DMA arbiter** sees DREQ 36 asserted and the channel with `TREQ_SEL=36` ready (not currently mid-transfer). The arbiter grants the channel its slot.
4. **DMA channel** issues a 16-bit read at `READ_ADDR` (= `ADC_FIFO`). The ADC FIFO returns the oldest entry and decrements its depth to 0.
5. **ADC** sees the depth drop and deasserts `DREQ_ADC` (one cycle later).
6. **DMA channel** issues a 16-bit write at `WRITE_ADDR` (= the next slot in the SRAM ring). `WRITE_ADDR` advances by 2 bytes (`DATA_SIZE=halfword`), modulo the ring size if `RING_SIZE/RING_SEL` are set.
7. **DMA channel** decrements `TRANS_COUNT`. If zero, fires the chain trigger and/or asserts the channel's IRQ line.

The whole sequence is six clock cycles in the best case (arbiter grant + AHB-Lite read + AHB-Lite write + count decrement). At 125 MHz this is ~48 ns per sample, i.e., a single channel can sustain ~20 MS/s of throughput on the bus — far above what the ADC can produce. The bottleneck this week is always the ADC's 500 kS/s ceiling, never the DMA.

---

## 1.4 IRQ routing

The 12 channels share two NVIC slots (datasheet §2.5.4, pp. 122–124):

- `DMA_IRQ_0` — fires when any channel with its bit set in `INTE0` completes.
- `DMA_IRQ_1` — same, with `INTE1`.

In a single-handler design you put all channels on `INTE0` and the handler reads `INTS0` to find which channel completed (or `INTS0_RAW` to bypass the mask). In a dual-handler design you split chained pairs across `INTE0` / `INTE1` so two latency-sensitive streams cannot block each other. We use the single-handler pattern this week; Week 14 introduces the dual.

The handler must acknowledge the channel's bit in `INTS0` (write 1 to clear). The SDK call is `dma_irqn_acknowledge_channel(0, channel)`. Failure to acknowledge means the handler is re-entered immediately — the ISR appears to "freeze" the system.

**SDK shortcut:** `dma_channel_set_irq0_enabled(channel, true)` sets the channel's bit in `INTE0`. The companion `dma_channel_get_irq0_status(channel)` is a per-channel mask read; cleaner than `INTS0 & (1u << channel)`.

---

## 1.5 The minimal channel-config recipe

The SDK provides a `dma_channel_config` struct with setters; the recipe for a peripheral-to-memory channel with chaining is:

```c
dma_channel_config cfg = dma_channel_get_default_config(ch);
channel_config_set_transfer_data_size(&cfg, DMA_SIZE_16);    /* DATA_SIZE=01 */
channel_config_set_read_increment(&cfg, false);              /* peripheral source */
channel_config_set_write_increment(&cfg, true);              /* memory destination */
channel_config_set_dreq(&cfg, DREQ_ADC);                     /* TREQ_SEL=36 */
channel_config_set_chain_to(&cfg, other_ch);                 /* ping-pong partner */
channel_config_set_irq_quiet(&cfg, false);                   /* let this one IRQ */

dma_channel_configure(
    ch,
    &cfg,
    write_ptr,                /* WRITE_ADDR — must be aligned for ring */
    &adc_hw->fifo,            /* READ_ADDR — the ADC FIFO MMIO */
    SAMPLES_PER_CHUNK,        /* TRANS_COUNT */
    false                     /* trigger=false; we start manually after both
                                 channels are configured */
);
```

Every field is intentional. The exercises will start from this recipe and walk you through varying one parameter at a time.

---

## 1.6 The ISR-vs-DMA latency math

Here is the comparison that justifies the whole week, with numbers.

**ISR-driven sample reader, single channel, 500 kS/s.** The per-sample budget is 2.0 µs. The ISR sequence on Cortex-M0+ at 125 MHz is:

| Step                                   | Cycles | Time at 125 MHz |
|----------------------------------------|-------:|-----------------|
| NVIC handshake (vector fetch, etc.)    |     16 | 128 ns          |
| Stacking 8 registers to PSP            |     16 | 128 ns          |
| ISR body: read FIFO, store, mask, inc  |     12 | 96 ns           |
| Unstacking 8 registers                 |     16 | 128 ns          |
| Branch back to interrupted code        |      1 |   8 ns          |
| **Subtotal: one ISR**                  | **61** | **488 ns**      |

488 ns vs 2000 ns budget — looks safe with 4× slack. But the budget is the *average* sample interval. The *worst-case* sample interval is the average minus the longest preemption — i.e., minus whatever interferes between this sample and the next. The Cortex-M0+ does not preempt at the same priority; it tail-chains. SysTick fires at 1 kHz (the FreeRTOS tick); SysTick's entry adds ~16 cycles of stack to *its* preceding context, and the OS tick handler is ~400 cycles (~3.2 µs) in the FreeRTOS Pico port.

If SysTick fires between two ADC samples, the second sample's ISR enters 3.7 µs late. The ADC FIFO is 8 deep; one delayed ISR consumes one of those slots; the *next* sample arrives 2 µs after that, the one after 4 µs, etc. A 3.7 µs delay puts you 1.85 samples behind. If two such delays happen within 8 samples, the FIFO overruns.

This is not a theoretical concern. Run an ISR-driven 500 kS/s reader for ten seconds and you will see FIFO-error bits in your stream. The numerical analysis is reproduced in Exercise 1 with measured data.

**DMA-driven sample reader, two-channel ping-pong, 500 kS/s.** The per-sample budget is the same 2.0 µs. The DMA's transfer time per sample is ~48 ns (§1.3). There is no ISR per sample — only one ISR per 1024 samples, i.e., every 2.048 ms. SysTick can fire 2.048 ticks between chunk-complete IRQs; it does not interfere with the DMA pacing because the DMA does not depend on the CPU between DREQ assertions.

The chunk-complete IRQ has the same FreeRTOS overhead as any ISR (~5 µs to notify a task), but it has 2.048 ms of slack before the next one. The ISR-to-task latency budget is now ~1.5 ms (we want the task to drain the just-completed half before the *next-next* IRQ fires and starts overwriting the *currently-being-drained* half). The 1.5 ms is so loose that priority inversion, queue contention, even a long `printf` cannot consume it.

That is the architectural difference. Not "DMA is faster". DMA is isochronous; it decouples the data path from the control path.

---

## 1.7 The five canonical DMA bugs (preview)

The exercises will reproduce all five so you can recognize them on bench. Listed here for foreshadowing:

1. **`INCR_WRITE=0` with memory destination.** Symptom: one sample value repeated through the entire buffer. Cause: write pointer never advances.
2. **`INCR_READ=1` with peripheral source.** Symptom: the FIFO never drains; ADC error bit set on every sample after the eighth. Cause: read pointer marches past `ADC_FIFO` into the next peripheral's register space.
3. **`DATA_SIZE` mismatched to FIFO width.** Symptom: every other sample is zero (or rather, contains the high half-word of the ADC FIFO register, which is reserved). Cause: 32-bit read of a 16-bit FIFO.
4. **Channel's `INTE0` bit not set.** Symptom: DMA finishes its 1024 samples, never IRQs, consumer task never wakes. Cause: `dma_channel_set_irq0_enabled` was never called for that channel.
5. **Forgetting to re-arm `WRITE_ADDR` in the chain-complete IRQ.** Symptom: first ring traversal works; on the second the just-completed channel writes into a stale address (often inside another buffer or in the wrong half). Cause: chain only re-triggers; it does not re-load.

If you write a chain-complete IRQ handler this week and your system fails in a way that is not one of these five, re-read the handler. It is almost certainly one of these five.

---

## 1.8 Summary

The RP2040 DMA controller is twelve channels, each with a read/write/count/control register file, an AHB-Lite master, and a connection to the peripheral DREQ network. The `CTRL` register is the configuration surface: data size, increment directions, ring wrap, chain target, DREQ source, IRQ-quiet. Channels move data without CPU help; they IRQ on completion if you ask them to.

In the next lecture we wire it to the ADC. Read §2.5.3.1 (the DREQ table on p. 121) before you continue.
