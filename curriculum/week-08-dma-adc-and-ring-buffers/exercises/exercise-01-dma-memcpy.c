/*
 * Exercise 1 - DMA memory-to-memory copy
 *
 * The simplest possible DMA configuration. One channel. No DREQ pacing
 * (TREQ_SEL = 0x3F = "permanent / unpaced"). Source and destination are
 * both SRAM. The point of the exercise is to (a) verify your DMA setup
 * compiles and runs at all, before you bring in any peripheral side, and
 * (b) measure the DMA throughput against a `memcpy()` baseline.
 *
 * Hardware:
 *   - Raspberry Pi Pico (W or non-W).
 *   - No external connections required.
 *   - Logic analyzer is optional; this exercise prints timings to UART.
 *
 * Expected measurement (Cortex-M0+ at 125 MHz, 8 KB transfer):
 *   memcpy(),       8 KB:   ~96 us
 *   DMA, DATA_SIZE=word:    ~17 us  (one transfer per 4 bytes -> 2048 transfers)
 *   DMA, DATA_SIZE=halfword: ~33 us  (4096 transfers, slower per-transfer rate)
 *   DMA, DATA_SIZE=byte:    ~65 us  (8192 transfers, AHB arbitration overhead)
 *
 * The lesson: DMA is fastest at the widest data size the source and
 * destination support. For peripheral-to-memory transfers, you are
 * constrained to the peripheral's FIFO width (the ADC FIFO is 16 bits;
 * Exercise 2 uses DATA_SIZE=halfword).
 *
 * Build:
 *   pico-sdk libraries to link:
 *     pico_stdlib, hardware_dma, hardware_timer
 *   Standard CMakeLists snippet (see mini-project/README.md).
 *
 * RP2040 datasheet references:
 *   2.5    DMA overview,         pp. 102-108
 *   2.5.2  Channel registers,    pp. 108-112
 *   2.5.3  CTRL register,        pp. 119-129
 *   2.5.5  Aborting channels,    pp. 124-126
 *
 * Pico-SDK references:
 *   https://www.raspberrypi.com/documentation/pico-sdk/group__hardware__dma.html
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/dma.h"
#include "hardware/timer.h"

#include "dma_ring.h"

/* ---- Buffers ----------------------------------------------------------- */

/* 8 KB worth of test data. The DMA copies the full 8 KB from src[] to
 * dst[]; we fill src[] with a known pattern and verify dst[] after.
 * Word-aligned because we will test DATA_SIZE = word, halfword, byte. */
#define TEST_BYTES              ((uint32_t)8192u)
#define TEST_WORDS              ((uint32_t)(TEST_BYTES / 4u))

static __attribute__((aligned(4))) uint32_t src[TEST_WORDS];
static __attribute__((aligned(4))) uint32_t dst[TEST_WORDS];

/* ---- Forward declarations --------------------------------------------- */

static void fill_pattern(uint32_t * buf, uint32_t n_words);
static bool verify_pattern(const uint32_t * buf, uint32_t n_words);
static uint32_t time_memcpy_us(void);
static uint32_t time_dma_us(enum dma_channel_transfer_size size);
static const char * size_name(enum dma_channel_transfer_size size);

/* ---- Test pattern ------------------------------------------------------ */

static void fill_pattern(uint32_t * const buf, const uint32_t n_words)
{
    /* A linear-congruential sequence keeps the pattern easy to verify
     * while exercising every bit of the data path. */
    uint32_t v = 0x12345678u;
    for (uint32_t i = 0u; i < n_words; ++i) {
        buf[i] = v;
        v = (v * 1103515245u) + 12345u;
    }
}

static bool verify_pattern(const uint32_t * const buf, const uint32_t n_words)
{
    uint32_t v = 0x12345678u;
    for (uint32_t i = 0u; i < n_words; ++i) {
        if (buf[i] != v) {
            (void)printf("verify FAIL at word %lu: got 0x%08lx, expected 0x%08lx\n",
                         (unsigned long)i,
                         (unsigned long)buf[i],
                         (unsigned long)v);
            return false;
        }
        v = (v * 1103515245u) + 12345u;
    }
    return true;
}

/* ---- memcpy baseline --------------------------------------------------- */

static uint32_t time_memcpy_us(void)
{
    /* Zero the destination so we can verify after. */
    (void)memset(dst, 0, sizeof(dst));

    const uint32_t t0 = time_us_32();
    (void)memcpy(dst, src, TEST_BYTES);
    const uint32_t t1 = time_us_32();

    return t1 - t0;
}

/* ---- DMA timed transfer ------------------------------------------------ */

static uint32_t time_dma_us(const enum dma_channel_transfer_size size)
{
    (void)memset(dst, 0, sizeof(dst));

    /* Claim a free channel. The SDK panics if none are available; with
     * 12 channels and nothing else running, claim is guaranteed. */
    const int32_t ch = dma_claim_unused_channel(true);

    /* Compute the transfer count: bytes / element-size. */
    uint32_t count;
    switch (size) {
        case DMA_SIZE_8:    count = TEST_BYTES;          break;
        case DMA_SIZE_16:   count = TEST_BYTES / 2u;     break;
        case DMA_SIZE_32:   count = TEST_BYTES / 4u;     break;
        default:            count = 0u;                  break;
    }

    /* Build the channel configuration.
     *
     * CTRL fields set (datasheet 2.5.3, p. 120):
     *   EN              implicit, set by dma_channel_configure last arg
     *   DATA_SIZE       parameter
     *   INCR_READ       1: source is a moving memory pointer
     *   INCR_WRITE      1: destination is a moving memory pointer
     *   RING_SIZE       0: no ring wrap
     *   CHAIN_TO        self: no chaining
     *   TREQ_SEL        0x3F: unpaced ("permanent" - go as fast as the bus)
     *   IRQ_QUIET       0: not relevant, we are polling for completion
     */
    dma_channel_config cfg = dma_channel_get_default_config((uint32_t)ch);
    channel_config_set_transfer_data_size(&cfg, size);
    channel_config_set_read_increment(&cfg, true);
    channel_config_set_write_increment(&cfg, true);
    channel_config_set_dreq(&cfg, (uint32_t)0x3Fu);

    const uint32_t t0 = time_us_32();

    dma_channel_configure(
        (uint32_t)ch,
        &cfg,
        dst,            /* WRITE_ADDR */
        src,            /* READ_ADDR */
        count,          /* TRANS_COUNT */
        true            /* start immediately (writes CTRL_TRIG) */
    );

    /* Spin until the channel finishes. dma_channel_wait_for_finish_blocking
     * polls CTRL.BUSY (datasheet 2.5.3, bit 24). */
    dma_channel_wait_for_finish_blocking((uint32_t)ch);

    const uint32_t t1 = time_us_32();

    /* Release the channel back to the SDK's free pool. */
    dma_channel_unclaim((uint32_t)ch);

    return t1 - t0;
}

/* ---- Pretty-printing -------------------------------------------------- */

static const char * size_name(const enum dma_channel_transfer_size size)
{
    switch (size) {
        case DMA_SIZE_8:    return "byte    ";
        case DMA_SIZE_16:   return "halfword";
        case DMA_SIZE_32:   return "word    ";
        default:            return "????    ";
    }
}

/* ---- main ------------------------------------------------------------- */

int main(void)
{
    stdio_init_all();
    sleep_ms(2000u);  /* give the host's USB terminal time to attach */

    (void)printf("\nExercise 1 - DMA memory-to-memory throughput\n");
    (void)printf("Transfer size: %lu bytes (%lu words)\n",
                 (unsigned long)TEST_BYTES, (unsigned long)TEST_WORDS);

    fill_pattern(src, TEST_WORDS);

    /* memcpy baseline */
    const uint32_t memcpy_us = time_memcpy_us();
    if (!verify_pattern(dst, TEST_WORDS)) {
        (void)printf("memcpy verify failed; aborting\n");
        return 1;
    }
    (void)printf("  memcpy()           : %6lu us\n", (unsigned long)memcpy_us);

    /* DMA at each data size. */
    static const enum dma_channel_transfer_size sizes[] = {
        DMA_SIZE_32,
        DMA_SIZE_16,
        DMA_SIZE_8
    };
    for (uint32_t i = 0u; i < (sizeof(sizes) / sizeof(sizes[0])); ++i) {
        const uint32_t us = time_dma_us(sizes[i]);
        if (!verify_pattern(dst, TEST_WORDS)) {
            (void)printf("DMA(%s) verify failed; aborting\n",
                         size_name(sizes[i]));
            return 1;
        }
        (void)printf("  DMA, DATA_SIZE=%s: %6lu us\n",
                     size_name(sizes[i]),
                     (unsigned long)us);
    }

    (void)printf("Done.\n");

    /* Loop forever so the USB serial connection stays up. */
    for (;;) {
        sleep_ms(1000u);
    }
}
