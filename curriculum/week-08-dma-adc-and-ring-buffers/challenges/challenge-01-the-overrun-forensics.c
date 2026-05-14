/*
 * Challenge 1 - The overrun forensics
 *
 * Force an overrun on the consumer side, observe it, recover the system
 * without restarting the DMA, and write a one-page post-mortem that
 * answers three questions:
 *
 *   1. Exactly when in time did the overrun begin? (To 1 ms resolution.)
 *   2. How many samples were corrupted before recovery? (To one-sample
 *      resolution.)
 *   3. Could the consumer have detected the overrun without your
 *      explicit instrumentation? (i.e., is there a hardware signal that
 *      reveals it?)
 *
 * The challenge is to *reproduce* the failure cleanly, *detect* it
 * automatically, and *recover* gracefully. You are not done when the
 * code compiles; you are done when you have a Saleae capture annotated
 * with the timestamp of the first dropped chunk and a markdown file
 * (commit as OVERRUN-FORENSICS.md alongside this source file) answering
 * the three questions above.
 *
 * Mechanism for forcing the overrun:
 *
 *   The consumer task has a hidden `sleep_us` call that activates after
 *   the first 100 chunks. The sleep duration (10 ms) is longer than the
 *   chunk period (2.048 ms), so by the time the consumer wakes from the
 *   sleep, four full chunks have arrived and three of them have been
 *   overwritten in the ring. The task-notify-state-already-set check
 *   should detect this; the FIFO error bit should NOT (the FIFO is
 *   drained by the DMA, which is unaffected by the consumer's nap).
 *
 * Recovery:
 *
 *   On detection of overrun, the consumer should:
 *     (a) increment a logged counter,
 *     (b) discard the half it was about to process (since the data has
 *         been overwritten), and
 *     (c) resync to the currently-being-written half by reading the
 *         DMA channel's TRANS_COUNT and skipping to the "other" half.
 *
 *   The DMA itself does not need to be restarted; the chain continues.
 *   This is the architectural advantage of chain-pair ping-pong over a
 *   single-channel ring: recovery is a software state-machine update,
 *   not a hardware re-init.
 *
 * Hardware: same as Exercise 3 (Pico + analog source + 4-channel Saleae).
 *
 * Pass criterion:
 *   - One overrun event recorded in the log.
 *   - Saleae capture shows GP11 (overrun marker) firing exactly once.
 *   - System continues running with chunks=N for some N > 1000 AFTER
 *     the overrun, with no further overruns.
 *   - OVERRUN-FORENSICS.md committed.
 *
 * RP2040 datasheet references:
 *   2.5.3   CTRL register (BUSY bit 24),     p. 120
 *   2.5.4   DMA interrupts,                  pp. 122-124
 *   4.9.2.4 FIFO overflow handling,          p. 564
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/timer.h"

#include "FreeRTOS.h"
#include "task.h"

#include "../exercises/dma_ring.h"

/* ---- Ring buffer ------------------------------------------------------- */

static __attribute__((aligned(RING_BYTES))) uint16_t ring[RING_HALFWORDS];

#define HALF_A_PTR  (&ring[0])
#define HALF_B_PTR  (&ring[SAMPLES_PER_CHUNK])

/* ---- DMA channels ----------------------------------------------------- */

static uint32_t ch_a = 0u;
static uint32_t ch_b = 0u;

/* ---- FreeRTOS handle -------------------------------------------------- */

static TaskHandle_t consumer_handle = NULL;

/* ---- Counters --------------------------------------------------------- */

static volatile uint32_t chunks_processed       = 0u;
static volatile uint32_t consumer_overrun_count = 0u;
static volatile uint32_t fifo_overrun_count     = 0u;
static volatile uint64_t overrun_first_us       = 0u;

/* The "force overrun after N chunks" knob. Set to 100 in the challenge;
 * tune it if you want to reproduce different scenarios. */
#define FORCE_OVERRUN_AFTER_CHUNKS  ((uint32_t)100u)
#define FORCED_SLEEP_US             ((uint32_t)10000u)   /* 10 ms */

/* ---- IRQ handler ------------------------------------------------------ */

static void __not_in_flash_func(dma_irq0_handler)(void)
{
    BaseType_t higher_prio_woken = pdFALSE;
    MARKER_PULSE_HIGH(PIN_MARKER_IRQ);

    if (dma_channel_get_irq0_status(ch_a)) {
        dma_irqn_acknowledge_channel(0u, ch_a);
        dma_channel_set_write_addr(ch_a, HALF_A_PTR, false);
        dma_channel_set_trans_count(ch_a, SAMPLES_PER_CHUNK, false);
        (void)xTaskNotifyFromISR(consumer_handle, NOTIFY_HALF_A_READY,
                                 eSetBits, &higher_prio_woken);
    }
    if (dma_channel_get_irq0_status(ch_b)) {
        dma_irqn_acknowledge_channel(0u, ch_b);
        dma_channel_set_write_addr(ch_b, HALF_B_PTR, false);
        dma_channel_set_trans_count(ch_b, SAMPLES_PER_CHUNK, false);
        (void)xTaskNotifyFromISR(consumer_handle, NOTIFY_HALF_B_READY,
                                 eSetBits, &higher_prio_woken);
    }

    MARKER_PULSE_LOW(PIN_MARKER_IRQ);
    portYIELD_FROM_ISR(higher_prio_woken);
}

/* ---- Consumer task ---------------------------------------------------- */

static void process_chunk_inspect(const uint16_t * const chunk, const bool dropped)
{
    /* In a real system you would do an RMS / FFT / threshold here.
     * For the challenge, we just count FIFO error bits to demonstrate
     * that the DMA-side overrun did NOT cause FIFO overflow. */
    if (dropped) {
        /* The data has been overwritten; do not bother scanning. */
        return;
    }
    for (uint32_t i = 0u; i < SAMPLES_PER_CHUNK; ++i) {
        if ((chunk[i] & ADC_SAMPLE_ERROR_BIT) != 0u) {
            ++fifo_overrun_count;
        }
    }
}

static void consumer_task(void * arg)
{
    (void)arg;
    uint32_t notify = 0u;

    for (;;) {
        (void)xTaskNotifyWait(0u, UINT32_MAX, &notify, portMAX_DELAY);
        MARKER_PULSE_HIGH(PIN_MARKER_CONSUME_BEG);

        /* The deliberate stall. After processing N chunks, sleep for
         * longer than a chunk period to force an overrun. */
        if (chunks_processed == FORCE_OVERRUN_AFTER_CHUNKS) {
            (void)printf(">>> deliberate stall: sleeping %lu us\n",
                         (unsigned long)FORCED_SLEEP_US);
            busy_wait_us_32(FORCED_SLEEP_US);
        }

        /* If both halves are notified, we have an overrun. */
        const uint32_t both = (NOTIFY_HALF_A_READY | NOTIFY_HALF_B_READY);
        if ((notify & both) == both) {
            ++consumer_overrun_count;
            if (overrun_first_us == 0u) {
                overrun_first_us = (uint64_t)time_us_64();
            }
            MARKER_PULSE_HIGH(PIN_MARKER_OVERRUN);
            MARKER_PULSE_LOW(PIN_MARKER_OVERRUN);

            /* Resync: the most-recently-written half is the one whose
             * DMA channel has a LARGER remaining TRANS_COUNT (it just
             * restarted after chain trigger). The other half is the one
             * the DMA is currently writing. We process the just-fully-
             * written one (which is the partner of the currently active
             * one) and drop the older. */
            const uint32_t remain_a = dma_channel_hw_addr(ch_a)->transfer_count;
            const uint32_t remain_b = dma_channel_hw_addr(ch_b)->transfer_count;
            if (remain_a > remain_b) {
                /* A just restarted; B is mid-write; the just-completed
                 * one was B's previous run, which has been overwritten.
                 * Process the half that A just finished (HALF_A), but
                 * mark dropped because the order is unreliable. */
                process_chunk_inspect(HALF_A_PTR, true);
            } else {
                process_chunk_inspect(HALF_B_PTR, true);
            }
        } else if ((notify & NOTIFY_HALF_A_READY) != 0u) {
            process_chunk_inspect(HALF_A_PTR, false);
        } else if ((notify & NOTIFY_HALF_B_READY) != 0u) {
            process_chunk_inspect(HALF_B_PTR, false);
        }

        ++chunks_processed;
        MARKER_PULSE_HIGH(PIN_MARKER_CONSUME_END);
        MARKER_PULSE_LOW(PIN_MARKER_CONSUME_END);
        MARKER_PULSE_LOW(PIN_MARKER_CONSUME_BEG);
    }
}

/* ---- Reporter task ---------------------------------------------------- */

static void reporter_task(void * arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(1000u));
        (void)printf("chunks=%lu fifo_err=%lu consumer_overrun=%lu first_overrun_us=%llu\n",
                     (unsigned long)chunks_processed,
                     (unsigned long)fifo_overrun_count,
                     (unsigned long)consumer_overrun_count,
                     (unsigned long long)overrun_first_us);
    }
}

/* ---- Setup ----------------------------------------------------------- */

static void gpio_markers_init(void)
{
    const uint32_t pins[] = {
        PIN_MARKER_IRQ,
        PIN_MARKER_CONSUME_BEG,
        PIN_MARKER_CONSUME_END,
        PIN_MARKER_OVERRUN
    };
    for (uint32_t i = 0u; i < (sizeof(pins) / sizeof(pins[0])); ++i) {
        gpio_init(pins[i]);
        gpio_set_dir(pins[i], GPIO_OUT);
        gpio_put(pins[i], 0);
    }
}

static void adc_bringup(void)
{
    adc_init();
    adc_gpio_init(PIN_ADC_INPUT_0);
    adc_select_input(0u);
    adc_set_clkdiv(0.0f);
    adc_fifo_setup(true, true, 1u, true, false);
    adc_fifo_drain();
}

static void dma_setup_pingpong(void)
{
    ch_a = (uint32_t)dma_claim_unused_channel(true);
    ch_b = (uint32_t)dma_claim_unused_channel(true);

    dma_channel_config cfg_a = dma_channel_get_default_config(ch_a);
    channel_config_set_transfer_data_size(&cfg_a, DMA_SIZE_16);
    channel_config_set_read_increment(&cfg_a, false);
    channel_config_set_write_increment(&cfg_a, true);
    channel_config_set_dreq(&cfg_a, DREQ_ADC);
    channel_config_set_chain_to(&cfg_a, ch_b);
    dma_channel_configure(ch_a, &cfg_a,
                          HALF_A_PTR, &adc_hw->fifo,
                          SAMPLES_PER_CHUNK, false);

    dma_channel_config cfg_b = dma_channel_get_default_config(ch_b);
    channel_config_set_transfer_data_size(&cfg_b, DMA_SIZE_16);
    channel_config_set_read_increment(&cfg_b, false);
    channel_config_set_write_increment(&cfg_b, true);
    channel_config_set_dreq(&cfg_b, DREQ_ADC);
    channel_config_set_chain_to(&cfg_b, ch_a);
    dma_channel_configure(ch_b, &cfg_b,
                          HALF_B_PTR, &adc_hw->fifo,
                          SAMPLES_PER_CHUNK, false);

    dma_channel_set_irq0_enabled(ch_a, true);
    dma_channel_set_irq0_enabled(ch_b, true);

    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq0_handler);
    irq_set_enabled(DMA_IRQ_0, true);
}

/* ---- main ------------------------------------------------------------- */

int main(void)
{
    stdio_init_all();
    sleep_ms(2000u);

    gpio_markers_init();
    (void)printf("\nChallenge 1 - the overrun forensics\n");

    adc_bringup();
    dma_setup_pingpong();

    (void)xTaskCreate(consumer_task, "consumer",
                      configMINIMAL_STACK_SIZE + 256u,
                      NULL, 4u, &consumer_handle);
    (void)xTaskCreate(reporter_task, "reporter",
                      configMINIMAL_STACK_SIZE + 256u,
                      NULL, 2u, NULL);

    dma_channel_start(ch_a);
    adc_run(true);

    vTaskStartScheduler();

    for (;;) { }
    return 0;
}
