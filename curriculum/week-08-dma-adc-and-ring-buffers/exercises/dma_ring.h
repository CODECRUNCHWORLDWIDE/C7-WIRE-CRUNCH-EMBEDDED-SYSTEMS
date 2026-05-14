/*
 * dma_ring.h - shared ring-buffer types and pin map for Week 8 exercises
 *
 * This header is included by exercises 01, 02, 03 and by the mini-project.
 * It declares the ring-buffer layout, the pin map for Saleae markers, the
 * canonical chunk size, and a small set of helper macros.
 *
 * All RP2040 register references are to the datasheet revision dated
 * August 2024. Citations in comments are by section + page number.
 */

#ifndef DMA_RING_H
#define DMA_RING_H

#include <stdint.h>
#include <stdbool.h>

/* ----- Pin map ----------------------------------------------------------
 *
 * The mini-project and the exercises share these pin assignments. Any
 * deviation must be documented in the per-file comment block.
 *
 *   GP26   ADC0       analog input #1 (mic preamp or signal generator)
 *   GP27   ADC1       analog input #2 (round-robin partner in mini-project)
 *   GP14   marker     chain-complete IRQ entry (Saleae channel 0)
 *   GP13   marker     consumer-task start of chunk (Saleae channel 1)
 *   GP12   marker     consumer-task end of chunk   (Saleae channel 2)
 *   GP11   marker     overrun event (Saleae channel 3, optional)
 *
 * RP2040 datasheet GPIO pinout: section 1.4.3 (pp. 13-15).
 * ADC channel-to-GPIO mapping: section 4.9.1 (p. 558).
 */

#define PIN_ADC_INPUT_0         ((uint32_t)26u)
#define PIN_ADC_INPUT_1         ((uint32_t)27u)

#define PIN_MARKER_IRQ          ((uint32_t)14u)
#define PIN_MARKER_CONSUME_BEG  ((uint32_t)13u)
#define PIN_MARKER_CONSUME_END  ((uint32_t)12u)
#define PIN_MARKER_OVERRUN      ((uint32_t)11u)

/* ----- Ring-buffer sizing -----------------------------------------------
 *
 * The full ring is 4 KB = 2048 halfwords (uint16_t).
 *
 * The ring is logically two halves of 2 KB each (SAMPLES_PER_CHUNK
 * halfwords). DMA channel A writes the first half; channel B writes the
 * second; the consumer task drains whichever half just completed.
 *
 * RING_BYTES must be a power of two so that the hardware ring-wrap feature
 * (CTRL.RING_SIZE / RING_SEL, datasheet section 2.5.3.2 p. 121) can use
 * a bitwise mask.
 *
 * The buffer's base address must be aligned to RING_BYTES; this is
 * enforced with __attribute__((aligned(RING_BYTES))) at the buffer
 * declaration site (see exercise-03-chain-pingpong.c and
 * mini-project/main.c).
 */

#define RING_BYTES              ((uint32_t)4096u)
#define SAMPLES_PER_CHUNK       ((uint32_t)1024u)       /* halfwords per half */
#define RING_HALFWORDS          ((uint32_t)(RING_BYTES / 2u))
#define LOG2_RING_BYTES         ((uint32_t)12u)         /* 1<<12 == 4096 */

/* Compile-time check that the constants are consistent. */
#if (SAMPLES_PER_CHUNK * 2u) != RING_HALFWORDS
#error "SAMPLES_PER_CHUNK must be exactly half of RING_HALFWORDS"
#endif

#if (1u << LOG2_RING_BYTES) != RING_BYTES
#error "LOG2_RING_BYTES must satisfy (1<<LOG2_RING_BYTES) == RING_BYTES"
#endif

/* ----- Task notification bits ------------------------------------------
 *
 * The DMA chain-complete IRQ posts a task notification to the consumer.
 * Two bits, one per half, are used so that an unusual double-IRQ before
 * the consumer wakes is detectable (both bits set = the consumer missed
 * a beat = overrun on the DMA side).
 *
 * See FreeRTOS xTaskNotifyFromISR / ulTaskNotifyWait reference at
 *   https://www.freertos.org/xTaskNotifyFromISR.html
 *   https://www.freertos.org/ulTaskNotifyTake.html
 */

#define NOTIFY_HALF_A_READY     ((uint32_t)(1u << 0))
#define NOTIFY_HALF_B_READY     ((uint32_t)(1u << 1))
#define NOTIFY_OVERRUN          ((uint32_t)(1u << 2))

/* ----- Sample-rate constants -------------------------------------------
 *
 * The ADC runs from a 48 MHz clock; one conversion takes 96 cycles. With
 * adc_set_clkdiv(0.0f), the per-channel rate is 500 kS/s. With two-channel
 * round-robin (adc_set_round_robin(0x03)), the aggregate rate stays at
 * 500 kS/s of FIFO writes but each physical channel samples at 250 kS/s.
 *
 * The mini-project pitches this as "1 MS/s aggregate" because each FIFO
 * entry carries one channel's data, and we de-interleave in the consumer.
 *
 * Datasheet section 4.9.3 (pp. 565-567).
 */

#define ADC_CLOCK_HZ            ((uint32_t)48000000u)
#define ADC_CYCLES_PER_CONV     ((uint32_t)96u)
#define ADC_MAX_RATE_SPS        ((uint32_t)(ADC_CLOCK_HZ / ADC_CYCLES_PER_CONV))   /* 500000 */

/* IRQ rate at one-half-ring per chunk: 500 kS/s / 1024 samples = 488.28 Hz. */
#define IRQ_RATE_HZ_NUM         ADC_MAX_RATE_SPS
#define IRQ_RATE_HZ_DEN         SAMPLES_PER_CHUNK

/* ----- ADC FIFO bit masks ----------------------------------------------
 *
 * Each FIFO entry is 16 bits:
 *   bit 15      error bit (set if FIFO overflowed when this entry was written)
 *   bits 14:12  reserved
 *   bits 11:0   12-bit conversion result, right-aligned
 *
 * Datasheet section 4.9.2.3 (p. 564).
 */

#define ADC_SAMPLE_ERROR_BIT    ((uint16_t)0x8000u)
#define ADC_SAMPLE_DATA_MASK    ((uint16_t)0x0FFFu)

/* ----- Helper macros ---------------------------------------------------- */

/* Forced-inline GPIO toggle for ISR markers. Cheaper than gpio_xor_mask
 * because it expands to a single SIO_GPIO_OUT_XOR write. The pin must
 * have been configured with gpio_init / gpio_set_dir(out) at startup. */
#define MARKER_PULSE_HIGH(pin)  do { sio_hw->gpio_set = (1u << (pin)); } while (0)
#define MARKER_PULSE_LOW(pin)   do { sio_hw->gpio_clr = (1u << (pin)); } while (0)

/* Build-time guard on the SDK headers the user is expected to include. */
#ifndef PICO_SDK_VERSION_MAJOR
/* If the user did not include "pico/stdlib.h" before this header, the
 * sio_hw definition below will be missing. Surface that as a readable
 * compile error rather than a confusing "undefined" message later. */
/* (Not an #error; some files include this header for type definitions
 *  only and link with code that supplies sio_hw separately.) */
#endif

#endif /* DMA_RING_H */
