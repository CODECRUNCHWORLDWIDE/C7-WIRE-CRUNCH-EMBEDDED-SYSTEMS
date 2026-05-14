/*
 * dma_adc_capture.h - Public API for the Week 8 mini-project's
 *                     ADC + DMA + ring-buffer capture subsystem.
 *
 * The capture subsystem owns:
 *   - The ADC peripheral (in round-robin across ADC0 + ADC1 by default).
 *   - Two DMA channels in chain-pair ping-pong.
 *   - A 4 KB ring buffer aligned to its own size.
 *   - The DMA_IRQ_0 NVIC slot.
 *
 * Consumers of this API:
 *   - main.c: calls dma_adc_capture_init / start / stop.
 *   - The consumer task (in main.c): waits on the notification handle
 *     returned by dma_adc_capture_init and reads chunks via the
 *     dma_adc_capture_get_ready_half function.
 *
 * Thread-safety model:
 *   - Init / start / stop are single-threaded (called from main).
 *   - get_ready_half is the consumer-side API; SPSC with the IRQ.
 *   - All counters are volatile and may be read from any context.
 *
 * The driver uses ADC channels 0 (GP26) and 1 (GP27) in round-robin
 * by default; the channel mask is fixed at compile time.
 *
 * RP2040 datasheet references:
 *   2.5     DMA controller,     pp. 102-144
 *   4.9     ADC peripheral,     pp. 558-576
 */

#ifndef DMA_ADC_CAPTURE_H
#define DMA_ADC_CAPTURE_H

#include <stdint.h>
#include <stdbool.h>

#include "FreeRTOS.h"
#include "task.h"

#include "../exercises/dma_ring.h"  /* re-uses constants from the exercises */

/* ---- Configuration ---------------------------------------------------- */

/* Number of ADC channels in round-robin. 1 = single-channel @ 500 kS/s;
 * 2 = two channels @ 250 kS/s each (aggregate 500 kS/s of FIFO writes).
 * Datasheet section 4.9.4 (round-robin), pp. 567-568. */
#define DAC_RR_CHANNEL_COUNT    ((uint32_t)2u)
#define DAC_RR_CHANNEL_MASK     ((uint32_t)0x03u)   /* ADC0 + ADC1 */

/* ---- The half-ready code -- consumer asks which half is ready --------- */

typedef enum {
    DMA_ADC_HALF_NONE = 0,
    DMA_ADC_HALF_A    = 1,
    DMA_ADC_HALF_B    = 2,
    DMA_ADC_HALF_BOTH = 3   /* overrun: consumer missed a beat */
} dma_adc_half_t;

/* ---- Counters (read-only by consumers) --------------------------------- */

typedef struct {
    uint32_t chunks_processed;
    uint32_t fifo_overrun_count;        /* sum of FIFO error bits seen */
    uint32_t consumer_overrun_count;    /* "both halves" wakeups */
    uint32_t irq_count;                 /* total chain-complete IRQs */
} dma_adc_counters_t;

/* ---- Lifecycle API ----------------------------------------------------- */

/* Initialize the ADC and the two DMA channels. Does NOT start the capture.
 *
 * Arguments:
 *   consumer       FreeRTOS task handle to notify on each chain-complete.
 *                  Must be non-NULL and the task must already be created.
 *   sample_rate_hz Desired total FIFO write rate. 500000 is the maximum;
 *                  lower values are achieved by raising the ADC clkdiv.
 *
 * Returns true on success, false if the channel claim fails or the rate
 * is unachievable.
 */
bool dma_adc_capture_init(TaskHandle_t consumer, uint32_t sample_rate_hz);

/* Begin sampling. Arms both DMA channels, starts CH_A (which chains to
 * B and back), then enables the ADC. After this call returns, chain-
 * complete IRQs begin firing at sample_rate_hz / SAMPLES_PER_CHUNK Hz. */
void dma_adc_capture_start(void);

/* Stop sampling. Disables the ADC, aborts both DMA channels, drains the
 * FIFO. The consumer task should be drained of pending notifications
 * after this call before being deleted. */
void dma_adc_capture_stop(void);

/* ---- Consumer-side API ------------------------------------------------- */

/* Returns a pointer to the just-completed half of the ring buffer, given
 * a task-notification value. Pointer is valid until the next chain-
 * complete IRQ for that channel (~2 ms at 500 kS/s, 1024-sample chunks).
 *
 * which: DMA_ADC_HALF_A or DMA_ADC_HALF_B. Returns NULL for NONE/BOTH.
 *
 * The returned buffer contains 16-bit ADC FIFO entries; the consumer is
 * responsible for masking off the error bit (bit 15) before treating the
 * value as a 12-bit sample.
 */
const uint16_t * dma_adc_capture_get_half(dma_adc_half_t which);

/* Decode a task-notification value (as returned by xTaskNotifyWait)
 * into a dma_adc_half_t. */
dma_adc_half_t dma_adc_capture_decode_notify(uint32_t notify_value);

/* Read a snapshot of the counters. Atomic on the M0+ (each field is a
 * single word). */
void dma_adc_capture_get_counters(dma_adc_counters_t * out);

/* ---- Testing hooks (used by Challenge 1) ------------------------------ */

/* Allow the test harness to read the live DMA channel transfer-count
 * registers, for the overrun-resync logic. */
uint32_t dma_adc_capture_remain_a(void);
uint32_t dma_adc_capture_remain_b(void);

#endif /* DMA_ADC_CAPTURE_H */
