# Lecture 2 — The ADC Peripheral and DMA Wiring

> *The ADC is a 12-bit successive-approximation converter with a FIFO and a clock divider. The DMA is twelve channels with a DREQ network. The wire between them is two register fields and one bit in the FIFO control register. Get the wire right, and the data flows.*

Datasheet §4.9 (pp. 558–576) is the reference. Read it once end-to-end before this lecture, twice if you have not configured an ADC peripheral before. We will cite section by section as we go.

---

## 2.1 The ADC at a glance

The RP2040 has one ADC (datasheet §4.9.1, p. 558). Single instance, twelve bits of resolution, successive-approximation topology, fed by a 48 MHz clock divided down to set the sample rate.

Inputs are five: ADC0 = GP26, ADC1 = GP27, ADC2 = GP28, ADC3 = GP29 (shared with the Pico's VSYS divider — write your own resistor divider if you need an external signal there), and ADC4 = the internal temperature sensor (datasheet §4.9.5, p. 569). Voltage range is 0.0 V to 3.3 V; the reference is the chip's analog supply pin (`ADC_AVDD`, datasheet §4.9.1.1, p. 559) which is filtered from the digital 3V3 rail.

The conversion takes 96 ADC clock cycles, which at 48 MHz is exactly 2.0 µs. The maximum throughput with one channel selected is therefore 500 kS/s. With round-robin across multiple channels the same 500 kS/s is divided evenly: two channels → 250 kS/s each, three channels → 166.7 kS/s each, four channels → 125 kS/s each (datasheet §4.9.3, p. 565).

The mini-project's "1 MS/s aggregate" target uses two channels at 500 kS/s per channel = 1 MS/s total. This is the practical maximum the chip can deliver.

---

## 2.2 The ADC FIFO

The FIFO is the lifeline. Without it, every sample would need to be read out of the result register before the next conversion starts; with it, the ADC runs free and the consumer reads in bursts.

Datasheet §4.9.2 (pp. 562–565) gives the details:

- **Depth: 8 entries.** Each entry is 16 bits wide. The 12-bit conversion result occupies bits 11:0. Bit 15 is the *error bit* — set if the FIFO overflowed when this entry was written. Bits 14:12 are reserved (read as zero).
- **Threshold: 1 to 8.** The threshold is the FIFO level at which DREQ asserts and the FIFO-IRQ asserts. Setting it to 1 means "DREQ as soon as the FIFO is non-empty"; setting it to 8 means "DREQ only when the FIFO is full". Higher threshold = lower DMA rate (one DMA transfer per threshold-worth of samples) = lower DMA overhead but higher latency to the consumer. We will use threshold=1 in Week 8 because the DMA is fast enough and we want minimum FIFO occupancy.
- **The error bit on overflow.** If the FIFO is full and a new sample arrives, the ADC silently discards the new sample and sets the error bit on the *oldest* still-in-FIFO sample (datasheet §4.9.2.4, p. 564). The error bit propagates with that sample through to the consumer. To preserve it you set `err_in_fifo=true` in `adc_fifo_setup`; otherwise the SDK masks it out.

### The FIFO_CS register

The FIFO control/status is a single 32-bit register at `ADC_BASE + 0x08` (datasheet Table 535, p. 562). Fields:

| Bits  | Field    | Meaning                                                       |
|------:|:---------|:--------------------------------------------------------------|
| 0     | `EN`     | Enable the FIFO. If 0, samples land in the result register only and the FIFO is bypassed. |
| 1     | `SHIFT`  | If 1, results shift right by 4 (12-bit result → byte-aligned in 8-bit). Useful for byte-sized DMA. |
| 2     | `ERR`    | If 1, the error bit is included in bit 15 of FIFO entries. If 0, masked out. |
| 3     | `DREQ_EN`| If 1, FIFO level ≥ threshold asserts the DREQ_ADC line.        |
| 7:4   | (rsv)    | Reserved                                                      |
| 11:8  | `THRESH` | FIFO level threshold for DREQ / IRQ. 1 to 8.                   |
| 23:16 | `LEVEL`  | Current FIFO level (read-only).                                |
| 24    | `EMPTY`  | 1 if FIFO is empty.                                            |
| 25    | `FULL`   | 1 if FIFO is full.                                             |
| 26    | `UNDER`  | Underflow flag — read of an empty FIFO. Write 1 to clear.      |
| 27    | `OVER`   | Overflow flag — write to a full FIFO. Write 1 to clear.        |

The SDK call `adc_fifo_setup(en, dreq_en, thresh, err_in_fifo, byte_shift)` packs five booleans plus a 4-bit threshold into this register. The order of arguments matters; mis-ordering them is the second-most-common ADC bug (after wrong-channel selection).

---

## 2.3 The bring-up sequence

The minimal "ADC samples GP26 at 500 kS/s into a memory buffer via DMA" sequence is six steps. Each step has a precise place in the order; reorder them and the chip starts mid-stream with garbage in the FIFO.

```c
/* Step 1: bring up the ADC peripheral.
 * Powers on the analog block, sets up the clock divider in the SDK's
 * internal state. Idempotent. Datasheet §4.9.1, p. 559. */
adc_init();

/* Step 2: configure GP26 as an analog input.
 * Disables the digital input buffer and the pull-up/pull-down on the pin.
 * Required to get the rated noise floor; without this the pin's digital
 * input buffer leaks ~1 mV of switching noise into the ADC. Datasheet
 * §4.9.1.2, p. 560. */
adc_gpio_init(26u);

/* Step 3: select the input channel.
 * 0=ADC0=GP26, 1=ADC1=GP27, 2=ADC2=GP28, 3=ADC3=GP29, 4=temp sensor.
 * For round-robin, use adc_set_round_robin(mask) and skip this call. */
adc_select_input(0u);

/* Step 4: set the clock divider for full rate.
 * The ADC clock is 48 MHz fixed. The conversion takes 96 cycles. The
 * "free-running rate" is 48e6 / (96 + clkdiv) samples per second. With
 * clkdiv=0, that is 500 kS/s. Datasheet §4.9.3, p. 565. */
adc_set_clkdiv(0.0f);

/* Step 5: configure the FIFO.
 *   en=true            -- enable the FIFO (otherwise samples are not buffered)
 *   dreq_en=true       -- assert DREQ_ADC when threshold met
 *   thresh=1           -- DREQ on every sample
 *   err_in_fifo=true   -- preserve the error bit in bit 15
 *   byte_shift=false   -- 16-bit results, do not right-shift
 * Datasheet §4.9.2, pp. 562-565. */
adc_fifo_setup(true, true, 1u, true, false);

/* Step 6: arm the DMA channel (see Lecture 1) and start the ADC.
 * The DMA must be armed FIRST, because the moment adc_run(true) is called
 * the ADC starts producing samples; if no DMA is configured to drain the
 * FIFO, the FIFO fills in 16 us and starts setting error bits. */
adc_run(true);
```

Step 5's argument order is the one to memorize: `en, dreq_en, thresh, err_in_fifo, byte_shift`. The signature is in the pico-sdk `hardware_adc` header; we will reproduce it in every exercise.

---

## 2.4 The DREQ wire

Datasheet §2.5.3.1 (p. 121) gives DREQ_ADC = 36. The DMA channel that drains the ADC FIFO must have `TREQ_SEL=36` in its CTRL register. The SDK call is:

```c
channel_config_set_dreq(&cfg, DREQ_ADC);
```

`DREQ_ADC` is a macro in `hardware/dma.h` evaluating to 36. Confirm by grepping the SDK header — if you ever see this code with a literal `36`, replace it with the macro for the next reader.

The DREQ asserts when:

- The FIFO is enabled (`FIFO_CS.EN=1`), AND
- `FIFO_CS.DREQ_EN=1`, AND
- The current FIFO level ≥ `FIFO_CS.THRESH`.

It deasserts on the cycle the DMA reads the FIFO and the level drops below the threshold. The deassertion is *combinational* — there is no edge; the DMA arbiter samples the level on every clock. This means there is no race between the DMA's transfer and the next sample arriving: the ADC pushes the next sample into the FIFO independently, and DREQ stays asserted (or re-asserts) for the next DMA transfer.

---

## 2.5 The sample-rate math

The ADC clock is 48 MHz. The conversion takes 96 cycles. The free-running rate with a clock divider of `D` is:

$$f_s = \frac{48 \times 10^6}{96 + D}$$

with `D` in the range 0 to 65535.999 (the divider is fractional; integer part in `DIV.INT`, fractional part in `DIV.FRAC`, 8 bits of fraction). Notable rates:

| Clock divider (`D`) | Sample rate (single channel) | Use case                          |
|--------------------:|------------------------------:|-----------------------------------|
| 0                   | 500 kS/s                      | Maximum, audio-band capture       |
| 47999               | 1 kS/s                        | Slow scan, very low noise         |
| 95999               | 500 S/s                       | Mains-frequency sampling          |
| 47999.999           | ~1 kS/s with fractional       | Lock to non-integer rate          |

The SDK call `adc_set_clkdiv(float)` accepts the divider as a float — the SDK does the integer/fractional split. To set 100 kS/s, you compute `D = 48e6/100e3 - 96 = 384`, call `adc_set_clkdiv(384.0f)`. The mini-project sets `D=0` for the 500 kS/s-per-channel rate.

---

## 2.6 Round-robin across multiple channels

Datasheet §4.9.4 (pp. 567–568): `adc_set_round_robin(mask)` configures the ADC to advance through a bitmask of channels, one per conversion. The mask bits map to ADC channels 0..4. `mask=0` disables round-robin (use the selected input forever); `mask=0x03` rotates between ADC0 and ADC1; `mask=0x0F` rotates ADC0→ADC1→ADC2→ADC3.

When round-robin is enabled, `adc_select_input` is *not used* — the ADC advances on its own. The rate per channel is the free-running rate divided by the number of bits set in the mask. With `clkdiv=0` and `mask=0x03`, each of ADC0 and ADC1 samples at 250 kS/s.

**The mini-project trick.** To hit "1 MS/s aggregate" we use `mask=0x03` at `clkdiv=0`. The aggregate sample rate is 500 kS/s, but each *sample* carries information from one of two channels — so for the purposes of a multi-channel acquisition we get 500 kS/s × 2 channels = 1 MS/s of data. This is a perfectly defensible benchmark; it is the rate the DMA sees on its DREQ line. If your signal has bandwidth ≤ 250 kHz per channel, you have not aliased anything.

The samples arrive in the FIFO interleaved: ADC0 sample 0, ADC1 sample 0, ADC0 sample 1, ADC1 sample 1, ... The consumer de-interleaves by index parity (or by the high bits of the FIFO entry; the ADC does not embed the channel ID, you must track it yourself).

---

## 2.7 Aliasing — the input-filter requirement

The Nyquist rate for a 250 kS/s-per-channel mini-project capture is 125 kHz. Any signal energy above 125 kHz at the ADC input folds into the captured band. For a microphone preamp this is rarely a problem (acoustic energy above 20 kHz is small); for a function-generator test it absolutely is — a 600 kHz square-wave has harmonics at 1.8 MHz, 3.0 MHz, ... and all of them alias into the 0–125 kHz band as crisp false tones.

The fix is an antialiasing filter at the ADC pin. A first-order RC low-pass with corner at 100 kHz (R=1.6 kΩ, C=1 nF, since `f_c = 1/(2πRC)`) attenuates 1 MHz by 20 dB and 10 MHz by 40 dB. Higher-order filters (Sallen-Key, switched-capacitor) give sharper rolloff at the cost of an op-amp.

Challenge 2 walks through the full calculation and a swept-sine validation. The lecture-level fact: **never run the ADC without thinking about what's above Nyquist on the input pin.** Free-running 500 kS/s on a wire that picks up a 5 MHz harmonic from a nearby switching regulator looks like a 250 kHz tone in your capture. You will chase a "noise problem" that is in fact an aliasing problem.

---

## 2.8 Putting Lecture 1 and Lecture 2 together

The single-channel free-running capture into a 4 KB SRAM ring buffer, with one DMA channel and no chaining, takes about 30 lines of C. The annotated sequence:

```c
/* (1) Allocate the ring buffer, aligned for hardware ring wrap. */
__attribute__((aligned(4096)))
static uint16_t ring[2048];  /* 4 KB = 2048 halfwords */

/* (2) Bring up the ADC (steps 1-5 from §2.3). */
adc_init();
adc_gpio_init(26u);
adc_select_input(0u);
adc_set_clkdiv(0.0f);
adc_fifo_setup(true, true, 1u, true, false);

/* (3) Claim and configure a DMA channel. */
const uint32_t ch = (uint32_t)dma_claim_unused_channel(true);
dma_channel_config cfg = dma_channel_get_default_config(ch);
channel_config_set_transfer_data_size(&cfg, DMA_SIZE_16);
channel_config_set_read_increment(&cfg, false);
channel_config_set_write_increment(&cfg, true);
channel_config_set_dreq(&cfg, DREQ_ADC);
channel_config_set_ring(&cfg, true /*write*/, 12 /*log2(4096)*/);
/* RING_SIZE=12 wraps the write pointer at 4096-byte boundaries; the
 * buffer's base must be 4096-aligned (enforced above with __attribute__). */

dma_channel_configure(
    ch,
    &cfg,
    ring,                /* WRITE_ADDR */
    &adc_hw->fifo,       /* READ_ADDR */
    UINT32_MAX,          /* TRANS_COUNT — effectively forever */
    false                /* do not start yet */
);

/* (4) Start the DMA, then the ADC. */
dma_channel_start(ch);
adc_run(true);

/* (5) The DMA writes samples into `ring[]` forever, wrapping at 4 KB.
 * The CPU is uninvolved. To consume, read the channel's hardware
 * write-pointer register and copy out the just-written region. */
```

This is the simplest "ADC streaming with hardware ring" pattern. Exercise 2 builds this; Exercise 3 swaps the hardware ring for chain-pair ping-pong so the consumer gets a clean IRQ on every half-fill. The mini-project uses chain-pair ping-pong with round-robin across two channels.

---

## 2.9 Common ADC-side bugs

Three to recognize:

1. **`adc_fifo_setup` left at defaults.** Symptom: `adc_run(true)` returns but the DMA never sees a DREQ. Cause: `dreq_en=false`, FIFO fills, DMA waits forever. Fix: pass `dreq_en=true` (the second argument).
2. **`adc_gpio_init` skipped.** Symptom: ADC works, but the noise floor is ~5 LSB higher than spec. Cause: the digital input buffer on GP26 is still enabled and is leaking digital activity into the analog input. Fix: always call `adc_gpio_init` for every channel you use.
3. **Round-robin mask without `adc_set_input(0)` first.** Symptom: the first sample of every burst is the wrong channel. Cause: the ADC's "current channel" register is not reset by enabling round-robin; it starts wherever it was. Fix: call `adc_select_input(0)` before `adc_set_round_robin(mask)`, or accept that you may need to discard the first sample.

---

## 2.10 Summary

The ADC is a 12-bit SAR converter with an 8-entry FIFO. It connects to the DMA through DREQ index 36. The six-step bring-up is: `adc_init`, `adc_gpio_init`, `adc_select_input`, `adc_set_clkdiv`, `adc_fifo_setup`, then arm the DMA and call `adc_run`. The maximum throughput is 500 kS/s on one channel or aggregated across two. The FIFO error bit (bit 15) reports overrun and is preserved if `err_in_fifo=true`. Antialiasing is your responsibility on the input pin.

Next lecture: the ring-buffer SPSC pattern and the chain-vs-hardware-ring decision.
