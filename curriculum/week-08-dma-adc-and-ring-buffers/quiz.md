# Week 8 — Quiz

Ten questions. Open-book, open-datasheet, open-pico-sdk. The intent is to confirm you can answer technical questions from the references on the desk, not to test recall.

Time limit: 60 minutes. Write your answers in `quiz-answers.md` and commit alongside the rest of the week's deliverables.

---

## Question 1 — CTRL register field identification (5 points)

For a DMA channel that reads from the ADC FIFO into an SRAM buffer (no chaining, no hardware ring wrap), state the value of each of the following CTRL register fields and cite the datasheet section that justifies each:

- `DATA_SIZE`
- `INCR_READ`
- `INCR_WRITE`
- `TREQ_SEL`
- `CHAIN_TO`

A correct answer cites datasheet section + page for each field.

---

## Question 2 — DREQ index (3 points)

What is the numeric DREQ index for the ADC FIFO on the RP2040? Which datasheet section table lists all DREQ indices? Give the page number.

---

## Question 3 — Why DMA over ISR-driven sampling (5 points)

State two distinct architectural reasons (i.e., not "DMA is faster" — give the underlying mechanisms) why a DMA-driven ADC capture is preferable to an ISR-driven ADC capture at 500 kS/s on the Cortex-M0+ at 125 MHz. Reference at least one quantitative figure (cycle count, current draw, latency) for each reason.

---

## Question 4 — Chain vs hardware ring decision (5 points)

You are designing a system that captures from a single SPI sensor at 50 kS/s and writes the samples to a SD-card. The SD-card write task is event-driven (writes 512-byte blocks when buffered).

Should you use:
(a) one DMA channel with `RING_SIZE`/`RING_SEL` hardware wrap, or
(b) two DMA channels in chain-pair ping-pong, or
(c) something else?

Justify your choice using at least three of the criteria from the chooser table in Lecture 3.

---

## Question 5 — Re-arming chained channels (4 points)

In a chain-pair ping-pong setup, channel A's `CHAIN_TO` is B and B's is A. The chain-complete IRQ fires when A finishes its 1024 transfers.

Inside the IRQ handler, you must re-arm channel A's `WRITE_ADDR` and `TRANS_COUNT` before the *next* time channel B's completion chains back to A.

Why? What goes wrong if you skip this step? Cite the datasheet section that explains the chain mechanism.

---

## Question 6 — FIFO error bit (4 points)

The ADC FIFO has an error bit (bit 15 of each entry). Under what condition is this bit set? Where in the FIFO does it appear — on the offending sample or on a different one? What `adc_fifo_setup` parameter controls whether it is preserved in the output stream?

Cite datasheet section.

---

## Question 7 — Sample rate math (6 points)

(a) With `adc_set_clkdiv(0.0f)` and one channel selected, what is the sample rate? Give the formula.

(b) With `adc_set_clkdiv(0.0f)` and `adc_set_round_robin(0x07)` (three channels enabled), what is the per-channel sample rate? What is the aggregate FIFO write rate?

(c) The mini-project pitches its target as "1 MS/s aggregate" using `adc_set_round_robin(0x03)` and `adc_set_clkdiv(0.0f)`. Is this a fair description of the rate? Why might one argue it is misleading?

---

## Question 8 — SPSC ring-buffer correctness (5 points)

State, in your own words, the Lamport (1977) result on lock-free SPSC ring buffers. Explain why the Cortex-M0+ satisfies the proof's preconditions but a dual-core RP2040 SPSC (producer on core 1, consumer on core 0) might not.

For full credit, identify the one synchronization primitive needed in the dual-core case and the SDK function that provides it.

---

## Question 9 — Overrun detection on both sides (4 points)

The week-8 system has two distinct overrun conditions:

(a) **ADC-side overrun**: the FIFO overflows because the DMA is not draining it fast enough.
(b) **DMA-side overrun**: the ring buffer overflows because the consumer task is not draining it fast enough.

For each, state:
- How the hardware (or software) signals it.
- One realistic root cause.
- The fix.

---

## Question 10 — Aliasing (4 points)

Given an ADC sampling at 500 kS/s with no input antialiasing filter, at what frequency in the captured spectrum (0 to 250 kHz) would a 600 kHz signal appear? Show the computation. What about a 1.1 MHz signal? A 2.5 MHz signal?

---

## Scoring

| Range  | Grade                  |
|-------:|------------------------|
| 40-45  | A — full pass          |
| 35-39  | B — pass with notes    |
| 30-34  | C — re-read lectures   |
| < 30   | re-do the week         |

The "with notes" range means you can move on but should commit a short reflection (one paragraph in your `LESSONS.md`) on the questions you got wrong.

---

## Hint sheet (after you have attempted on your own)

- Question 1: datasheet §2.5.3 (CTRL register), particularly table on p. 120.
- Question 2: datasheet §2.5.3.1 (DREQ table), p. 121.
- Question 3: Lecture 1 §1.6, the latency math table.
- Question 4: Lecture 3 §3.6, the chooser table.
- Question 5: datasheet §2.5.6.2 (chain timing), pp. 127-129.
- Question 6: datasheet §4.9.2.3 (FIFO entry format), p. 564.
- Question 7: datasheet §4.9.3 (sample rates), pp. 565-567.
- Question 8: Lecture 3 §3.1 and the dual-core note in the parenthetical.
- Question 9: Lecture 3 §3.5.
- Question 10: Lecture 2 §2.7.
