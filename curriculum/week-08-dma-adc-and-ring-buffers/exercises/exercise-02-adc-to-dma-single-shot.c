/*
 * Exercise 2 - ADC to DMA, single-shot capture
 *
 * Bring up the ADC on GP26, configure one DMA channel paced by DREQ_ADC,
 * capture 1024 samples into SRAM, stop the ADC, drain the buffer, print
 * a histogram summary over UART. No chaining; no FreeRTOS; bare loop.
 *
 * The point of the exercise is to learn the six-step ADC bring-up
 * (Lecture 2 sec 2.3) and the peripheral-paced DMA channel configuration
 * before adding the complexity of chaining and an RTOS.
 *
 * Hardware:
 *   - Raspberry Pi Pico.
 *   - Analog source on GP26. A 555 oscillator + RC low-pass works; a
 *     function generator at 1 Vpp + 1.65 V offset is ideal; even a
 *     potentiometer between 3V3 and GND (wiper to GP26) gives a static
 *     reading you can verify.
 *   - Optional: GP14 toggles on capture-start and capture-finish for
 *     Saleae timing. Useful but not required.
 *
 * Expected output (potentiometer at midscale ~ 1.65 V):
 *   ADC at 500 kS/s, 1024 samples = 2.048 ms capture
 *   FIFO error bits set : 0
 *   min raw / max raw   : 2040 / 2056
 *   mean (counts)       : 2047.8
 *   mean (volts)        : 1.649 V
 *
 * Build:
 *   Link: pico_stdlib, hardware_dma, hardware_adc, hardware_timer
 *
 * RP2040 datasheet references:
 *   2.5.3   CTRL register,        p. 120 (TREQ_SEL=36 for ADC)
 *   2.5.3.1 DREQ table,           p. 121
 *   4.9.1   ADC overview,         pp. 558-562
 *   4.9.2   ADC FIFO,             pp. 562-565
 *   4.9.3   Sample rates,         pp. 565-567
 *
 * Pico-SDK references:
 *   https://www.raspberrypi.com/documentation/pico-sdk/group__hardware__adc.html
 *   https://www.raspberrypi.com/documentation/pico-sdk/group__hardware__dma.html
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/dma.h"
#include "hardware/timer.h"

#include "dma_ring.h"

/* ---- Capture buffer --------------------------------------------------- */

/* 1024 halfwords = 2 KB. Not ring-aligned; this exercise is a single
 * shot, the DMA stops at 1024 transfers and we read the buffer linearly. */
static uint16_t capture[SAMPLES_PER_CHUNK];

/* ---- ADC bring-up ----------------------------------------------------- */

static void adc_bringup(void)
{
    /* Step 1: power on the analog block. */
    adc_init();

    /* Step 2: configure GP26 as an analog input.
     * Disables the digital input buffer and any pull-up/down on the pin.
     * Datasheet section 4.9.1.2 (p. 560). */
    adc_gpio_init(PIN_ADC_INPUT_0);

    /* Step 3: select ADC channel 0 (= GP26). */
    adc_select_input(0u);

    /* Step 4: full-rate clock divider = 0.0f -> 500 kS/s. */
    adc_set_clkdiv(0.0f);

    /* Step 5: configure the FIFO.
     *   en          = true     enable the FIFO
     *   dreq_en     = true     assert DREQ_ADC when FIFO level >= thresh
     *   thresh      = 1        DREQ on every sample
     *   err_in_fifo = true     preserve bit 15 (the error bit)
     *   byte_shift  = false    16-bit results (not right-shifted to byte) */
    adc_fifo_setup(true, true, 1u, true, false);

    /* Drain anything that may have entered the FIFO during setup. */
    adc_fifo_drain();
}

/* ---- DMA setup -------------------------------------------------------- */

/* Returns the channel index. */
static uint32_t dma_setup_capture(void)
{
    const int32_t ch_signed = dma_claim_unused_channel(true);
    const uint32_t ch = (uint32_t)ch_signed;

    dma_channel_config cfg = dma_channel_get_default_config(ch);

    /* DATA_SIZE = 01 (halfword). ADC FIFO is 16 bits wide. */
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_16);

    /* INCR_READ = 0. Source is the ADC FIFO at one fixed address. */
    channel_config_set_read_increment(&cfg, false);

    /* INCR_WRITE = 1. Destination is the capture[] array; advance. */
    channel_config_set_write_increment(&cfg, true);

    /* TREQ_SEL = 36 = DREQ_ADC. */
    channel_config_set_dreq(&cfg, DREQ_ADC);

    /* No chaining (CHAIN_TO=self), no ring wrap. */

    /* Arm the channel but do not start. We start after adc_run(true). */
    dma_channel_configure(
        ch,
        &cfg,
        capture,                /* WRITE_ADDR */
        &adc_hw->fifo,          /* READ_ADDR  */
        SAMPLES_PER_CHUNK,      /* TRANS_COUNT */
        false                   /* do not trigger yet */
    );

    return ch;
}

/* ---- Capture-and-analyze ---------------------------------------------- */

static void run_one_capture(const uint32_t ch)
{
    /* Zero the buffer so a partial fill is visible. */
    (void)memset(capture, 0, sizeof(capture));

    /* Make sure the ADC is stopped and the FIFO is empty before we arm. */
    adc_run(false);
    adc_fifo_drain();

    /* Mark capture start on GP14 (Saleae). */
    gpio_put(PIN_MARKER_IRQ, 1);

    const uint32_t t0 = time_us_32();

    /* Start the DMA, then the ADC. Order matters: if we start the ADC
     * first, the FIFO begins filling before the DMA can drain it, and
     * the first samples sit in the FIFO until the DMA arms. */
    dma_channel_start(ch);
    adc_run(true);

    /* Wait for the DMA to finish all 1024 transfers. */
    dma_channel_wait_for_finish_blocking(ch);

    /* Stop the ADC so no further samples accumulate. */
    adc_run(false);

    const uint32_t t1 = time_us_32();

    gpio_put(PIN_MARKER_IRQ, 0);

    /* Analyze the buffer. */
    uint32_t err_count = 0u;
    uint16_t min_raw = 0x0FFFu;
    uint16_t max_raw = 0x0000u;
    uint32_t sum = 0u;
    for (uint32_t i = 0u; i < SAMPLES_PER_CHUNK; ++i) {
        const uint16_t s = capture[i];
        if ((s & ADC_SAMPLE_ERROR_BIT) != 0u) {
            ++err_count;
        }
        const uint16_t v = (uint16_t)(s & ADC_SAMPLE_DATA_MASK);
        if (v < min_raw) min_raw = v;
        if (v > max_raw) max_raw = v;
        sum += (uint32_t)v;
    }

    /* Convert mean counts to volts using ADC midscale 2048 counts = 1.65 V.
     * Full scale 3.3 V / 4096 counts = ~0.806 mV/count. */
    const uint32_t mean_x1000 = (sum * 1000u) / SAMPLES_PER_CHUNK;  /* 0.001 counts */
    const uint32_t volts_x1000 = (mean_x1000 * 3300u) / (4096u * 1000u);
    /* volts_x1000 is millivolts; we will print it with three digits. */

    (void)printf("Capture: %lu samples in %lu us (expected ~%lu us)\n",
                 (unsigned long)SAMPLES_PER_CHUNK,
                 (unsigned long)(t1 - t0),
                 (unsigned long)((SAMPLES_PER_CHUNK * 1000000u) / ADC_MAX_RATE_SPS));
    (void)printf("  FIFO error bits set : %lu\n", (unsigned long)err_count);
    (void)printf("  min / max counts    : %u / %u\n",
                 (unsigned)min_raw, (unsigned)max_raw);
    (void)printf("  mean counts (x1000) : %lu\n",
                 (unsigned long)mean_x1000);
    (void)printf("  mean volts (mV)     : %lu\n",
                 (unsigned long)volts_x1000);
    (void)printf("\n");
}

/* ---- main ------------------------------------------------------------- */

int main(void)
{
    stdio_init_all();
    sleep_ms(2000u);

    /* GP14 marker (capture-start / capture-finish). */
    gpio_init(PIN_MARKER_IRQ);
    gpio_set_dir(PIN_MARKER_IRQ, GPIO_OUT);
    gpio_put(PIN_MARKER_IRQ, 0);

    (void)printf("\nExercise 2 - ADC to DMA single-shot capture\n");

    adc_bringup();
    const uint32_t ch = dma_setup_capture();
    (void)printf("Using DMA channel %lu, DREQ=%u (DREQ_ADC)\n",
                 (unsigned long)ch, (unsigned)DREQ_ADC);

    /* Take five captures so you can compare across them. */
    for (uint32_t i = 0u; i < 5u; ++i) {
        /* Re-arm the channel for the next shot. The previous run consumed
         * TRANS_COUNT down to zero and advanced WRITE_ADDR by 2 KB; reset
         * both before triggering again. */
        dma_channel_set_write_addr(ch, capture, false);
        dma_channel_set_trans_count(ch, SAMPLES_PER_CHUNK, false);

        run_one_capture(ch);

        sleep_ms(500u);
    }

    (void)printf("Done.\n");
    for (;;) {
        sleep_ms(1000u);
    }
}
