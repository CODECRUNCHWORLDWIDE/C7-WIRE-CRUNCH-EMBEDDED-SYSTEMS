/*
 * Exercise 3 - Channel chaining for ping-pong ring-buffer ADC capture
 *
 * Two DMA channels in chain configuration:
 *   CH_A writes the first 2 KB of the ring.
 *   CH_B writes the second 2 KB.
 *   CH_A.CHAIN_TO = CH_B
 *   CH_B.CHAIN_TO = CH_A
 *
 * On each channel-complete, the chain re-triggers the partner channel
 * AND fires DMA_IRQ_0. The IRQ handler re-arms the just-completed
 * channel's WRITE_ADDR and TRANS_COUNT (chain only re-triggers, it does
 * NOT re-load), then notifies a FreeRTOS task that this half is ready
 * to consume.
 *
 * The consumer task reads the just-completed half, computes a simple
 * running average, and posts a one-line log to the UART once per second.
 * The capture runs forever; the GP12/GP13/GP14 markers let you read
 * IRQ-to-task latency on a Saleae.
 *
 * Hardware:
 *   - Raspberry Pi Pico.
 *   - Analog source on GP26 (mic preamp, function generator, or pot).
 *   - GP14 = chain-complete IRQ marker (Saleae ch0)
 *   - GP13 = consumer-start marker      (Saleae ch1)
 *   - GP12 = consumer-finish marker     (Saleae ch2)
 *   - GP11 = overrun marker             (Saleae ch3, fires on any overrun)
 *
 * Expected measurement (ADC 500 kS/s, 1024-sample chunks):
 *   IRQ rate          : 488.28 Hz (= 500000 / 1024)
 *   consumer wake     : within ~4 us of IRQ entry
 *   consumer finish   : ~120 us after start (RMS over 1024 halfwords)
 *   overrun events    : 0 over a 10 s run
 *
 * Build:
 *   Link: pico_stdlib, hardware_dma, hardware_adc,
 *         FreeRTOS-Kernel, FreeRTOS-Kernel-Heap4
 *   FreeRTOSConfig.h: configUSE_TASK_NOTIFICATIONS=1, configUSE_TIMERS=0
 *
 * RP2040 datasheet references:
 *   2.5.3   CTRL register,        p. 120
 *   2.5.4   DMA interrupts,       pp. 122-124
 *   2.5.6.2 Channel chaining,     pp. 127-129
 *   4.9.2   ADC FIFO,             pp. 562-565
 *
 * FreeRTOS references:
 *   https://www.freertos.org/xTaskNotifyFromISR.html
 *   https://www.freertos.org/ulTaskNotifyTake.html
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/dma.h"
#include "hardware/irq.h"

#include "FreeRTOS.h"
#include "task.h"

#include "dma_ring.h"

/* ---- Ring buffer ------------------------------------------------------- */

/* 4 KB ring, 4 KB-aligned for the optional hardware-ring mode (we do not
 * use hardware ring in this exercise, but the alignment is harmless and
 * the mini-project uses the same layout). Two halves are referenced by
 * pointer arithmetic, not by separate arrays, to keep them contiguous. */
static __attribute__((aligned(RING_BYTES))) uint16_t ring[RING_HALFWORDS];

#define HALF_A_PTR  (&ring[0])
#define HALF_B_PTR  (&ring[SAMPLES_PER_CHUNK])

/* ---- DMA channels ----------------------------------------------------- */

/* The SDK allocates channels at run-time via dma_claim_unused_channel; we
 * cache the allocated indices so the IRQ handler can identify them. */
static uint32_t ch_a = 0u;
static uint32_t ch_b = 0u;

/* ---- FreeRTOS handles ------------------------------------------------- */

static TaskHandle_t  consumer_handle = NULL;

/* ---- Overrun counters ------------------------------------------------- */

static volatile uint32_t fifo_overrun_count     = 0u;
static volatile uint32_t consumer_overrun_count = 0u;
static volatile uint32_t chunks_processed       = 0u;

/* ---- DMA chain-complete IRQ handler ----------------------------------- */

static void __not_in_flash_func(dma_irq0_handler)(void)
{
    BaseType_t higher_prio_woken = pdFALSE;

    /* Marker high: IRQ entry. */
    MARKER_PULSE_HIGH(PIN_MARKER_IRQ);

    /* Did channel A complete? */
    if (dma_channel_get_irq0_status(ch_a)) {
        /* Acknowledge by writing 1 to the channel's bit in INTS0.
         * Failure to acknowledge re-enters the handler immediately. */
        dma_irqn_acknowledge_channel(0u, ch_a);

        /* Re-arm CH_A. Chain only re-triggers; it does not reload the
         * read/write/count registers. Without these two lines, the second
         * traversal of CH_A would start with WRITE_ADDR pointing somewhere
         * inside HALF_B and TRANS_COUNT at zero (channel would idle
         * immediately).  This is canonical DMA bug #5. */
        dma_channel_set_write_addr(ch_a, HALF_A_PTR, false);
        dma_channel_set_trans_count(ch_a, SAMPLES_PER_CHUNK, false);

        /* Notify the consumer. eSetBits accumulates: if the consumer
         * has not yet woken, the bit remains and the second IRQ does
         * not "lose" the event. */
        (void)xTaskNotifyFromISR(consumer_handle,
                                 NOTIFY_HALF_A_READY,
                                 eSetBits,
                                 &higher_prio_woken);
    }

    /* Did channel B complete? */
    if (dma_channel_get_irq0_status(ch_b)) {
        dma_irqn_acknowledge_channel(0u, ch_b);
        dma_channel_set_write_addr(ch_b, HALF_B_PTR, false);
        dma_channel_set_trans_count(ch_b, SAMPLES_PER_CHUNK, false);
        (void)xTaskNotifyFromISR(consumer_handle,
                                 NOTIFY_HALF_B_READY,
                                 eSetBits,
                                 &higher_prio_woken);
    }

    MARKER_PULSE_LOW(PIN_MARKER_IRQ);

    /* Yield to the consumer if it is now the highest-priority ready
     * task. portYIELD_FROM_ISR is a thin wrapper around setting PendSV. */
    portYIELD_FROM_ISR(higher_prio_woken);
}

/* ---- Consumer task ---------------------------------------------------- */

static void process_chunk(const uint16_t * const chunk)
{
    /* Strip the error bit and accumulate sum-of-squares for an RMS
     * estimate. We treat samples as centered around midscale (2048
     * counts) so that DC does not dominate the RMS. */
    uint64_t sum_sq = 0u;
    uint32_t err = 0u;
    for (uint32_t i = 0u; i < SAMPLES_PER_CHUNK; ++i) {
        const uint16_t raw = chunk[i];
        if ((raw & ADC_SAMPLE_ERROR_BIT) != 0u) {
            ++err;
        }
        const int32_t centered = (int32_t)(raw & ADC_SAMPLE_DATA_MASK) - 2048;
        sum_sq += (uint64_t)(centered * centered);
    }

    if (err != 0u) {
        fifo_overrun_count += err;
    }

    ++chunks_processed;

    /* The full sqrt is intentionally not done here; we accumulate the
     * sum-of-squares across many chunks and print a periodic summary. */
    (void)sum_sq;  /* hand to a longer-running aggregator in real code */
}

static void consumer_task(void * arg)
{
    (void)arg;
    uint32_t notify = 0u;

    for (;;) {
        /* Block until any notification bit is set. The bits clear on
         * read (third arg = UINT32_MAX). */
        (void)xTaskNotifyWait(0u, UINT32_MAX, &notify, portMAX_DELAY);

        MARKER_PULSE_HIGH(PIN_MARKER_CONSUME_BEG);

        /* If both bits are set, the IRQ posted twice before we woke -
         * we missed a chunk. Increment the overrun counter and decide
         * which half to drop (we drop the older one and process the
         * newer; either policy is defensible). */
        if ((notify & (NOTIFY_HALF_A_READY | NOTIFY_HALF_B_READY))
            == (NOTIFY_HALF_A_READY | NOTIFY_HALF_B_READY)) {
            ++consumer_overrun_count;
            MARKER_PULSE_HIGH(PIN_MARKER_OVERRUN);
            MARKER_PULSE_LOW(PIN_MARKER_OVERRUN);
            /* Process the most recently filled half (B is newer if both
             * IRQs landed in the same wake; in practice the order varies). */
            process_chunk(HALF_B_PTR);
        } else if ((notify & NOTIFY_HALF_A_READY) != 0u) {
            process_chunk(HALF_A_PTR);
        } else if ((notify & NOTIFY_HALF_B_READY) != 0u) {
            process_chunk(HALF_B_PTR);
        } else {
            /* No known bit set; treat as spurious. */
        }

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
        (void)printf("chunks=%lu fifo_err=%lu consumer_overrun=%lu\n",
                     (unsigned long)chunks_processed,
                     (unsigned long)fifo_overrun_count,
                     (unsigned long)consumer_overrun_count);
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
    /* en=true, dreq_en=true, thresh=1, err_in_fifo=true, byte_shift=false */
    adc_fifo_setup(true, true, 1u, true, false);
    adc_fifo_drain();
}

static void dma_setup_pingpong(void)
{
    ch_a = (uint32_t)dma_claim_unused_channel(true);
    ch_b = (uint32_t)dma_claim_unused_channel(true);

    /* Configure CH_A. */
    dma_channel_config cfg_a = dma_channel_get_default_config(ch_a);
    channel_config_set_transfer_data_size(&cfg_a, DMA_SIZE_16);
    channel_config_set_read_increment(&cfg_a, false);
    channel_config_set_write_increment(&cfg_a, true);
    channel_config_set_dreq(&cfg_a, DREQ_ADC);
    channel_config_set_chain_to(&cfg_a, ch_b);
    channel_config_set_irq_quiet(&cfg_a, false);
    dma_channel_configure(ch_a, &cfg_a,
                          HALF_A_PTR,
                          &adc_hw->fifo,
                          SAMPLES_PER_CHUNK,
                          false);

    /* Configure CH_B. */
    dma_channel_config cfg_b = dma_channel_get_default_config(ch_b);
    channel_config_set_transfer_data_size(&cfg_b, DMA_SIZE_16);
    channel_config_set_read_increment(&cfg_b, false);
    channel_config_set_write_increment(&cfg_b, true);
    channel_config_set_dreq(&cfg_b, DREQ_ADC);
    channel_config_set_chain_to(&cfg_b, ch_a);
    channel_config_set_irq_quiet(&cfg_b, false);
    dma_channel_configure(ch_b, &cfg_b,
                          HALF_B_PTR,
                          &adc_hw->fifo,
                          SAMPLES_PER_CHUNK,
                          false);

    /* Enable per-channel IRQs in INTE0. */
    dma_channel_set_irq0_enabled(ch_a, true);
    dma_channel_set_irq0_enabled(ch_b, true);

    /* Install the handler and enable the NVIC slot. */
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq0_handler);
    irq_set_enabled(DMA_IRQ_0, true);
}

/* ---- main ------------------------------------------------------------- */

int main(void)
{
    stdio_init_all();
    sleep_ms(2000u);

    gpio_markers_init();

    (void)printf("\nExercise 3 - ADC -> chained DMA ping-pong -> FreeRTOS\n");

    adc_bringup();
    dma_setup_pingpong();
    (void)printf("DMA channels: A=%lu, B=%lu\n",
                 (unsigned long)ch_a, (unsigned long)ch_b);

    /* Create the consumer task before starting the DMA: the IRQ posts
     * task notifications, and the handle must exist before the first
     * notification fires. */
    (void)xTaskCreate(consumer_task, "consumer",
                      configMINIMAL_STACK_SIZE + 256u,
                      NULL, 4u, &consumer_handle);
    (void)xTaskCreate(reporter_task, "reporter",
                      configMINIMAL_STACK_SIZE + 256u,
                      NULL, 2u, NULL);

    /* Start CH_A; it will chain to CH_B on completion, which will chain
     * back to A, forever. */
    dma_channel_start(ch_a);
    adc_run(true);

    /* Hand control to the scheduler. */
    vTaskStartScheduler();

    /* Unreachable. */
    for (;;) { }
    return 0;
}
