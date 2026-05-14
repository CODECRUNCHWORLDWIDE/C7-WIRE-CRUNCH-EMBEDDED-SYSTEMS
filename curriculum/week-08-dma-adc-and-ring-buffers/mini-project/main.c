/*
 * Week 8 mini-project - main.c
 *
 * 1 MS/s aggregate ADC capture (two channels in round-robin at 500 kS/s
 * each) into a chained-pair DMA pingpong, with a FreeRTOS consumer task
 * computing RMS over 1024-sample chunks and a UART gatekeeper printing
 * one summary line per second.
 *
 * Tasks (priorities in parentheses):
 *   consumer (4) : wakes on every chain-complete IRQ, drains a half,
 *                  computes RMS and peak.
 *   gatekeep (3) : owns the UART; receives log strings from the
 *                  reporter via a queue and prints them serially.
 *   reporter (2) : every 1 s, builds a summary string and posts it to
 *                  the gatekeeper.
 *   idle     (0) : __WFI in the idle hook for power saving.
 *
 * The capture subsystem (dma_adc_capture.h / dma_adc_capture.c) owns
 * the DMA channels and the ring buffer; main.c owns the tasks and the
 * orchestration.
 *
 * Bench markers (Saleae):
 *   GP14  chain-complete IRQ entry  (channel 0)
 *   GP13  consumer-task in-process  (channel 1)
 *   GP12  consumer-task end-of-chunk (channel 2)
 *   GP11  overrun marker            (channel 3)
 *
 * RP2040 datasheet references:
 *   2.5    DMA,                  pp. 102-144
 *   4.9    ADC,                  pp. 558-576
 *   2.4.7  Power management,     pp. 88-92  (for __WFI)
 *
 * Pass criteria (see README.md):
 *   - 60 s run, 0 overruns (FIFO and consumer).
 *   - Saleae capture (60 s) showing 488.28 Hz +/- 0.1% chain-complete IRQ.
 *   - 3V3 current draw < 60 mA at 125 MHz with __WFI in idle hook.
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
#include "queue.h"

#include "../exercises/dma_ring.h"
#include "dma_adc_capture.h"

/* ---- Ring buffer (owned by dma_adc_capture.c in a full split, inlined
 *      here for the starter to keep one translation unit) ---------------- */

/* The ring buffer must be aligned to its own size (4 KB) for the
 * (optional) hardware-ring-wrap feature. We use chain-pair ping-pong
 * which does not strictly require alignment, but we align anyway to
 * keep the layout consistent across the exercises. */
__attribute__((aligned(RING_BYTES))) uint16_t mp_ring[RING_HALFWORDS];

/* DMA channel indices, allocated at init. */
static uint32_t mp_ch_a = 0u;
static uint32_t mp_ch_b = 0u;

/* Task handles. */
static TaskHandle_t consumer_handle = NULL;
static TaskHandle_t reporter_handle = NULL;
static TaskHandle_t gatekeep_handle = NULL;

/* The gatekeeper's input queue. Each entry is a fixed 80-byte string. */
#define LOG_LINE_MAX        ((uint32_t)80u)
#define LOG_QUEUE_DEPTH     ((uint32_t)4u)
static QueueHandle_t log_queue = NULL;

/* Counters. */
static volatile uint32_t chunks_processed       = 0u;
static volatile uint32_t fifo_overrun_count     = 0u;
static volatile uint32_t consumer_overrun_count = 0u;
static volatile uint32_t irq_count              = 0u;

/* For the per-second RMS report, the consumer accumulates a sum-of-
 * squares and a peak across all chunks seen in the last second. The
 * reporter resets both after reading. */
static volatile uint64_t accum_sum_sq           = 0ull;
static volatile uint32_t accum_n                = 0u;
static volatile uint16_t accum_peak             = 0u;

/* ---- DMA chain-complete IRQ ------------------------------------------ */

static void __not_in_flash_func(dma_irq0_handler)(void)
{
    BaseType_t higher_prio_woken = pdFALSE;
    MARKER_PULSE_HIGH(PIN_MARKER_IRQ);
    ++irq_count;

    if (dma_channel_get_irq0_status(mp_ch_a)) {
        dma_irqn_acknowledge_channel(0u, mp_ch_a);
        dma_channel_set_write_addr(mp_ch_a, &mp_ring[0], false);
        dma_channel_set_trans_count(mp_ch_a, SAMPLES_PER_CHUNK, false);
        (void)xTaskNotifyFromISR(consumer_handle, NOTIFY_HALF_A_READY,
                                 eSetBits, &higher_prio_woken);
    }
    if (dma_channel_get_irq0_status(mp_ch_b)) {
        dma_irqn_acknowledge_channel(0u, mp_ch_b);
        dma_channel_set_write_addr(mp_ch_b, &mp_ring[SAMPLES_PER_CHUNK], false);
        dma_channel_set_trans_count(mp_ch_b, SAMPLES_PER_CHUNK, false);
        (void)xTaskNotifyFromISR(consumer_handle, NOTIFY_HALF_B_READY,
                                 eSetBits, &higher_prio_woken);
    }

    MARKER_PULSE_LOW(PIN_MARKER_IRQ);
    portYIELD_FROM_ISR(higher_prio_woken);
}

/* ---- Consumer task ---------------------------------------------------- */

/* Process one chunk: strip the error bit, accumulate sum-of-squares
 * around midscale, track peak. */
static void process_chunk(const uint16_t * const chunk)
{
    uint64_t local_sum_sq = 0u;
    uint16_t local_peak   = 0u;
    uint32_t err          = 0u;

    for (uint32_t i = 0u; i < SAMPLES_PER_CHUNK; ++i) {
        const uint16_t raw = chunk[i];
        if ((raw & ADC_SAMPLE_ERROR_BIT) != 0u) {
            ++err;
        }
        const uint16_t v = (uint16_t)(raw & ADC_SAMPLE_DATA_MASK);
        const int32_t centered = (int32_t)v - 2048;
        const uint16_t abs_c = (centered < 0) ? (uint16_t)(-centered)
                                              : (uint16_t)centered;
        if (abs_c > local_peak) local_peak = abs_c;
        local_sum_sq += (uint64_t)((int64_t)centered * (int64_t)centered);
    }

    /* Atomically merge into the aggregator. The M0+ has no LDREX/STREX,
     * so we use a critical section. Cost: ~10 cycles per chunk. */
    taskENTER_CRITICAL();
    accum_sum_sq += local_sum_sq;
    accum_n      += SAMPLES_PER_CHUNK;
    if (local_peak > accum_peak) accum_peak = local_peak;
    fifo_overrun_count += err;
    taskEXIT_CRITICAL();
}

static void consumer_task(void * arg)
{
    (void)arg;
    uint32_t notify = 0u;

    for (;;) {
        (void)xTaskNotifyWait(0u, UINT32_MAX, &notify, portMAX_DELAY);
        MARKER_PULSE_HIGH(PIN_MARKER_CONSUME_BEG);

        const uint32_t both = (NOTIFY_HALF_A_READY | NOTIFY_HALF_B_READY);
        if ((notify & both) == both) {
            ++consumer_overrun_count;
            MARKER_PULSE_HIGH(PIN_MARKER_OVERRUN);
            MARKER_PULSE_LOW(PIN_MARKER_OVERRUN);
            /* Process the more recent half; drop the older. */
            process_chunk(&mp_ring[SAMPLES_PER_CHUNK]);
        } else if ((notify & NOTIFY_HALF_A_READY) != 0u) {
            process_chunk(&mp_ring[0]);
        } else if ((notify & NOTIFY_HALF_B_READY) != 0u) {
            process_chunk(&mp_ring[SAMPLES_PER_CHUNK]);
        }

        ++chunks_processed;
        MARKER_PULSE_HIGH(PIN_MARKER_CONSUME_END);
        MARKER_PULSE_LOW(PIN_MARKER_CONSUME_END);
        MARKER_PULSE_LOW(PIN_MARKER_CONSUME_BEG);
    }
}

/* ---- Gatekeeper task -------------------------------------------------- */

static void gatekeep_task(void * arg)
{
    (void)arg;
    char line[LOG_LINE_MAX];

    for (;;) {
        if (xQueueReceive(log_queue, line, portMAX_DELAY) == pdTRUE) {
            /* Single owner of the UART; no contention. */
            (void)printf("%s", line);
        }
    }
}

/* Posts a string to the gatekeeper. Truncated if longer than LOG_LINE_MAX.
 * Drops silently if the queue is full (rate-limit graceful degradation). */
static void log_send(const char * const fmt, ...)
{
    char buf[LOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    const int32_t n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    (void)n;
    (void)xQueueSend(log_queue, buf, 0u);  /* non-blocking */
}

/* ---- Reporter task ---------------------------------------------------- */

static void reporter_task(void * arg)
{
    (void)arg;
    TickType_t last = xTaskGetTickCount();

    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(1000u));

        /* Snapshot the accumulators atomically. */
        taskENTER_CRITICAL();
        const uint64_t sum_sq = accum_sum_sq;
        const uint32_t n      = accum_n;
        const uint16_t peak   = accum_peak;
        accum_sum_sq = 0ull;
        accum_n      = 0u;
        accum_peak   = 0u;
        taskEXIT_CRITICAL();

        /* RMS = sqrt(sum_sq / n). The M0+ has no FPU; use an integer
         * approximation. For a 12-bit ADC with samples centered around
         * zero, sum_sq <= n * 2048^2; for n = 488*1024 = 499712 chunks
         * worth per second, the max sum_sq is ~2e12 which fits in u64. */
        uint64_t mean_sq = (n != 0u) ? (sum_sq / (uint64_t)n) : 0ull;

        /* Integer sqrt of mean_sq. mean_sq is bounded by 2048^2 = 4194304,
         * fits in u32. Newton iteration converges in ~12 steps. */
        uint32_t x = 2048u;  /* initial guess */
        for (uint32_t i = 0u; i < 16u; ++i) {
            if (x == 0u) break;
            const uint32_t y = (uint32_t)((mean_sq / x + x) / 2u);
            if (y == x) break;
            x = y;
        }
        const uint32_t rms_counts = x;

        log_send("t=%lus chunks=%lu rms=%lu peak=%u fifo_err=%lu cons_ovr=%lu irq=%lu\r\n",
                 (unsigned long)(xTaskGetTickCount() / configTICK_RATE_HZ),
                 (unsigned long)chunks_processed,
                 (unsigned long)rms_counts,
                 (unsigned)peak,
                 (unsigned long)fifo_overrun_count,
                 (unsigned long)consumer_overrun_count,
                 (unsigned long)irq_count);
    }
}

/* ---- Setup helpers ---------------------------------------------------- */

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
    adc_gpio_init(PIN_ADC_INPUT_0);     /* GP26 = ADC0 */
    adc_gpio_init(PIN_ADC_INPUT_1);     /* GP27 = ADC1 */
    adc_select_input(0u);
    adc_set_round_robin(DAC_RR_CHANNEL_MASK);   /* 0x03 = ADC0+ADC1 */
    adc_set_clkdiv(0.0f);               /* full rate */
    adc_fifo_setup(true, true, 1u, true, false);
    adc_fifo_drain();
}

static void dma_setup_pingpong(void)
{
    mp_ch_a = (uint32_t)dma_claim_unused_channel(true);
    mp_ch_b = (uint32_t)dma_claim_unused_channel(true);

    dma_channel_config cfg_a = dma_channel_get_default_config(mp_ch_a);
    channel_config_set_transfer_data_size(&cfg_a, DMA_SIZE_16);
    channel_config_set_read_increment(&cfg_a, false);
    channel_config_set_write_increment(&cfg_a, true);
    channel_config_set_dreq(&cfg_a, DREQ_ADC);
    channel_config_set_chain_to(&cfg_a, mp_ch_b);
    dma_channel_configure(mp_ch_a, &cfg_a,
                          &mp_ring[0],
                          &adc_hw->fifo,
                          SAMPLES_PER_CHUNK, false);

    dma_channel_config cfg_b = dma_channel_get_default_config(mp_ch_b);
    channel_config_set_transfer_data_size(&cfg_b, DMA_SIZE_16);
    channel_config_set_read_increment(&cfg_b, false);
    channel_config_set_write_increment(&cfg_b, true);
    channel_config_set_dreq(&cfg_b, DREQ_ADC);
    channel_config_set_chain_to(&cfg_b, mp_ch_a);
    dma_channel_configure(mp_ch_b, &cfg_b,
                          &mp_ring[SAMPLES_PER_CHUNK],
                          &adc_hw->fifo,
                          SAMPLES_PER_CHUNK, false);

    dma_channel_set_irq0_enabled(mp_ch_a, true);
    dma_channel_set_irq0_enabled(mp_ch_b, true);

    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq0_handler);
    irq_set_enabled(DMA_IRQ_0, true);
}

/* ---- FreeRTOS hooks --------------------------------------------------- */

void vApplicationIdleHook(void)
{
    /* Halt the core until the next interrupt. Saves ~30 mA at 125 MHz.
     * Datasheet section 2.4.7 (power management), p. 88. ARMv6-M ARM
     * section A6.7.74 (WFI instruction).  */
    __asm volatile ("wfi");
}

void vApplicationStackOverflowHook(TaskHandle_t task, char * name)
{
    /* Toggle GP15 to flag the fault and spin so the user can read out
     * the task name on a debugger. Production firmware should reset. */
    (void)task;
    (void)name;
    gpio_init(15u);
    gpio_set_dir(15u, GPIO_OUT);
    for (;;) {
        gpio_put(15u, 1);
        for (volatile uint32_t i = 0u; i < 1000000u; ++i) { }
        gpio_put(15u, 0);
        for (volatile uint32_t i = 0u; i < 1000000u; ++i) { }
    }
}

/* ---- main ------------------------------------------------------------- */

int main(void)
{
    stdio_init_all();
    sleep_ms(2000u);

    gpio_markers_init();

    (void)printf("\nWeek 8 mini-project - 1 MS/s aggregate ADC capture\n");
    (void)printf("Chunk size : %lu samples (%lu bytes)\n",
                 (unsigned long)SAMPLES_PER_CHUNK,
                 (unsigned long)(SAMPLES_PER_CHUNK * 2u));
    (void)printf("Ring size  : %lu bytes\n", (unsigned long)RING_BYTES);
    (void)printf("IRQ rate   : %lu / %lu = ~488 Hz\n",
                 (unsigned long)IRQ_RATE_HZ_NUM,
                 (unsigned long)IRQ_RATE_HZ_DEN);

    adc_bringup();
    dma_setup_pingpong();
    (void)printf("DMA channels: A=%lu B=%lu\n",
                 (unsigned long)mp_ch_a, (unsigned long)mp_ch_b);

    /* Create the log queue (gatekeeper's input). */
    log_queue = xQueueCreate(LOG_QUEUE_DEPTH, LOG_LINE_MAX);
    configASSERT(log_queue != NULL);

    /* Create tasks. Consumer is the highest-priority data path. */
    (void)xTaskCreate(consumer_task, "consumer",
                      configMINIMAL_STACK_SIZE + 256u,
                      NULL, 4u, &consumer_handle);
    (void)xTaskCreate(gatekeep_task, "gatekeep",
                      configMINIMAL_STACK_SIZE + 256u,
                      NULL, 3u, &gatekeep_handle);
    (void)xTaskCreate(reporter_task, "reporter",
                      configMINIMAL_STACK_SIZE + 256u,
                      NULL, 2u, &reporter_handle);

    /* Begin sampling. */
    dma_channel_start(mp_ch_a);
    adc_run(true);

    vTaskStartScheduler();

    /* Unreachable on success. */
    for (;;) { }
    return 0;
}
