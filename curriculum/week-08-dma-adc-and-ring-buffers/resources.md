# Week 8 — Resources

Every link below is free. Datasheet page numbers refer to the RP2040 datasheet revision dated August 2024 (the current revision as of this writing); if your local PDF is older, the section numbers are stable and the page numbers shift by no more than two.

---

## Primary references (read this week)

### RP2040 datasheet — Raspberry Pi (current revision)

PDF: <https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf>

- **§2.5 — DMA**, pp. 102–144
  - §2.5.1 Overview (pp. 102–108): the 12-channel model, the read/write/count register set, the arbitration policy.
  - §2.5.2 Channel registers (pp. 108–119): per-channel `READ_ADDR`, `WRITE_ADDR`, `TRANS_COUNT`, `CTRL`, `CTRL_TRIG`, `AL1_*` / `AL2_*` / `AL3_*` aliases.
  - §2.5.3 Channel control register (pp. 119–129): every bit of `CTRL` documented — `EN`, `HIGH_PRIORITY`, `DATA_SIZE`, `INCR_READ`, `INCR_WRITE`, `RING_SIZE`, `RING_SEL`, `CHAIN_TO`, `TREQ_SEL`, `IRQ_QUIET`, `BSWAP`, `SNIFF_EN`, `BUSY`, `WRITE_ERROR`, `READ_ERROR`, `AHB_ERROR`.
  - §2.5.3.1 DREQ table (p. 121): the canonical DREQ index list — `DREQ_ADC=36`, the UART/SPI/I²C/PIO indices for later weeks.
  - §2.5.4 Interrupts (pp. 122–124): the two NVIC slots (`DMA_IRQ_0`, `DMA_IRQ_1`), the per-channel `INTE0`/`INTE1` masks, `INTS0`/`INTS1` status, `INTF0`/`INTF1` force.
  - §2.5.6.2 Channel chaining (pp. 127–129): the trigger mechanics, the chain-on-completion timing.
  - §2.5.7 Channel control details (pp. 129–134): field encodings, the alias-register layout for atomic re-arming.

- **§4.9 — ADC and Temperature Sensor**, pp. 558–576
  - §4.9.1 Overview (pp. 558–562): 12-bit SAR, four channels + temp sensor, 48 MHz reference.
  - §4.9.2 ADC FIFO (pp. 562–565): the 8-entry FIFO, the threshold, the DREQ generation, the error bit in bit 15.
  - §4.9.3 Sample rates (pp. 565–567): 500 kS/s single-channel maximum, 96 cycles per conversion.
  - §4.9.4 Round-robin (pp. 567–568): the channel-mask register, the rate per channel when multiple are enabled.
  - §4.9.5 Calibration and noise (pp. 568–571): the INL/DNL specs, the 0.6 LSB noise floor.

- **§2.4.7 — Power management**, pp. 88–92
  - `WFI`, `WFE`, the SLEEP / DORMANT modes, the wake sources. Relevant to `vApplicationIdleHook` and the power measurement in `POWER.md`.

### pico-sdk reference (HTML, current)

- `hardware_dma`: <https://www.raspberrypi.com/documentation/pico-sdk/group__hardware__dma.html>
  - Key functions: `dma_channel_claim`, `dma_channel_get_default_config`, `dma_channel_configure`, `dma_channel_set_read_addr`, `dma_channel_set_write_addr`, `dma_channel_set_trans_count`, `dma_channel_start`, `dma_channel_abort`, `dma_irqn_set_channel_enabled`, `dma_irqn_acknowledge_channel`, `dma_channel_get_irq0_status`.
  - The `dma_channel_config` struct: `channel_config_set_transfer_data_size`, `..._read_increment`, `..._write_increment`, `..._dreq`, `..._chain_to`, `..._ring`, `..._irq_quiet`.

- `hardware_adc`: <https://www.raspberrypi.com/documentation/pico-sdk/group__hardware__adc.html>
  - Key functions: `adc_init`, `adc_gpio_init`, `adc_select_input`, `adc_set_round_robin`, `adc_set_clkdiv`, `adc_fifo_setup`, `adc_fifo_drain`, `adc_fifo_get_blocking`, `adc_run`.
  - The `adc_fifo_setup` parameter list — five booleans plus a 4-bit threshold — is the most-mis-set call in this week's exercises. Read the comment block carefully.

- `pico/stdlib`: <https://www.raspberrypi.com/documentation/pico-sdk/group__pico__stdlib.html>
  - For `stdio_init_all`, `sleep_ms`, GPIO basics.

### FreeRTOS reference (read selectively)

- "Mastering the FreeRTOS Real Time Kernel" v1.1.0, free PDF: <https://www.freertos.org/Documentation/RTOS_book.html>
  - **§6 — Interrupt Management** (~40 pages). Re-read the deferred-interrupt sections; this week we replace the stream-buffer hand-off with a direct-to-task notification on every chain-complete IRQ.
  - **§11 — Task Notifications** (~20 pages). The fast-path for IRQ → task signalling.
- FreeRTOS API ref pages used this week:
  - `xTaskNotifyFromISR`: <https://www.freertos.org/xTaskNotifyFromISR.html>
  - `ulTaskNotifyTake`: <https://www.freertos.org/ulTaskNotifyTake.html>
  - `vTaskGetRunTimeStats`: <https://www.freertos.org/rtos-run-time-stats.html> (for the CPU utilization measurement)

---

## Background reading (one-pass, optional but useful)

### Lamport, "Concurrent Reading and Writing" (CACM 1977)

PDF (author's site, free): <https://lamport.azurewebsites.net/pubs/concurrent-reading-and-writing.pdf>

The four-page foundational paper on lock-free SPSC queues. The proof is short. The relevance to Week 8 is direct: our DMA writer + task reader is exactly the SPSC pattern Lamport analyzed, and on the Cortex-M0+'s sequentially-consistent single-issue pipeline the proof's preconditions hold without modification. Read sections 1–3 (the first three pages). The fourth page generalizes to multi-word values; we use only the single-word case.

### Yiu, "The Definitive Guide to ARM Cortex-M0 and Cortex-M0+ Processors" (Newnes, 2nd ed., 2015)

Sample chapters and excerpts free at <https://www.elsevier.com/books/the-definitive-guide-to-arm-cortex-m0-and-cortex-m0-processors/yiu/978-0-12-803277-0> (search for "free chapter download"). The relevant material for Week 8:

- **Chapter 7 — Exceptions and Interrupts** (NVIC details, tail-chaining, the 16-cycle worst-case latency math).
- **Chapter 9 — Low-Power Features** (`WFI`, `WFE`, the SCR register).

Yiu writes for the engineer who actually has the core in front of them. The data the book gives — exception entry cycle counts, the precise priority-mask semantics — is not in the ARMv6-M ARM in the same form, and you will reach for it later in C7 when we get to interrupt nesting in Week 12.

### "RP2040 ADC characterisation" — Raspberry Pi forums + Tom's Hardware test

- Forum thread (free, official-engineer answers): <https://forums.raspberrypi.com/viewtopic.php?t=302933>
- A measured noise-floor and INL paper (community, free): <https://pico-adc.markomo.me/>

The official ADC characterization is sparse; the community has measured it at length. The relevant findings for Week 8: the ADC has a ~0.6 LSB noise floor and a small DNL spike near 1/4 scale; for any DSP application you want to oversample and average (which the mini-project does, implicitly, by computing 1024-sample RMS).

---

## Lab equipment references (free product pages)

- **Saleae Logic 8** (or any clone): <https://www.saleae.com/products/saleae-logic-8>
  - We use 4 channels this week: chain-complete IRQ (GP14), consumer-start (GP13), consumer-finish (GP12), and the ADC GPIO marker (GP11, optional). 25 MS/s is enough at our event rate (~500 Hz).
- **PulseView** (Sigrok, free, runs Saleae clones): <https://sigrok.org/wiki/PulseView>
- **INA219 breakout** (any clone, ~3 USD): for the 3V3 current measurement in `POWER.md`. Place between the Pico's 3V3 pin and the VBUS rail of a USB power source.
- **Function generator** (or 555 oscillator on a breadboard): any 10 kHz–100 kHz sine source with adjustable amplitude. The mini-project tests with a 17 kHz tone; pick a frequency between 5 kHz and 100 kHz for the sweep in Challenge 2.

---

## Quick-reference cheat-sheets you will reach for

These are the four single-page references worth printing or pinning open in a side window. The first three are sections of the datasheet; the fourth is a community card.

1. **Datasheet §2.5.3 CTRL register table** (p. 120) — the field-by-field layout of every DMA channel's control word. Pin this; every exercise and the mini-project references its fields by name.
2. **Datasheet §2.5.3.1 DREQ index table** (p. 121) — DREQ_ADC=36, the UART/SPI/I²C/PIO indices. You will reach for this every time you wire a new peripheral.
3. **Datasheet §4.9.2 ADC FIFO register table** (pp. 562–563) — the FIFO_CS register fields, the threshold, the error-bit policy.
4. **Pico-SDK DMA cheat-sheet** (community, free): <https://github.com/raspberrypi/pico-examples/blob/master/dma/README.md>. The `dma/` directory of `pico-examples` is the single best on-line reference for working DMA-channel code; the four examples (`hello_dma`, `channel_irq`, `control_blocks`, `sniff_crc`) cover 90 % of what you will write this week. Clone the repo, even if you do not build the examples.

---

## What we are not assigning

There are good books and papers on DMA controllers on other MCUs (the STM32 DMA engine, the SAMD51 DMAC, the NXP eDMA). They are not assigned because the architectural details — the DREQ semantics, the chaining mechanism, the ring-wrap support — differ enough that cross-reading is confusing. After Week 8 on the RP2040 you will read the STM32 DMA chapter in an hour and the eDMA chapter in two; this week is RP2040-specific.

Likewise, the broader DSP literature on antialiasing filters (Lyons, "Understanding Digital Signal Processing") is not assigned. Challenge 2 walks through the one filter calculation you need; deeper DSP is C20's job.

---

## A note on revision drift

The RP2040 datasheet has been revised several times since 2021. The page numbers cited in this curriculum reflect the August 2024 revision; the section numbers are stable across revisions. If your PDF reads `Section 2.5 DMA` at a different page number, trust the section number, not the page number. The Raspberry Pi documentation HTML pages (the pico-sdk references) are continuously updated; the function names and parameter lists have been stable since SDK 1.5.0.
