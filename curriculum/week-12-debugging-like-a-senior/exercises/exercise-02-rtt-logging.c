/*
 * exercise-02-rtt-logging.c — A minimal SEGGER-compatible RTT up-channel.
 *
 * Implements cc_rtt_init / cc_rtt_write / cc_rtt_write_str from debug_common.h.
 * The control block is placed in SRAM with the "SEGGER RTT" magic so a host
 * RTT viewer (openocd's `rtt` command, or JLinkRTTViewer) can find it by
 * scanning SRAM and drain the ring over SWD without halting the core.
 *
 * The exercise demonstrates three things from Lecture 3:
 *   1. RTT logging from thread context.
 *   2. RTT logging from an ISR (a repeating timer) — which a UART printf could
 *      not safely do — to prove RTT is non-blocking and ISR-safe.
 *   3. A DWT-cycle-counted A/B comparison of the cost of one RTT line vs one
 *      blocking UART printf line, to make the "RTT does not perturb timing"
 *      claim concrete.
 *
 * Build with the Pico SDK:
 *   add_executable(ex2 exercise-02-rtt-logging.c)
 *   target_link_libraries(ex2 pico_stdlib hardware_timer)
 *   pico_add_extra_outputs(ex2)
 *
 * Run RTT host-side (after openocd is up, in the openocd telnet on :4444 or
 * via gdb `monitor`):
 *   > rtt setup 0x20000000 0x42000 "SEGGER RTT"
 *   > rtt start
 *   > rtt server start 9090 0
 * then:  nc localhost 9090
 *
 * Citations:
 *   - SEGGER RTT docs and SEGGER_RTT.h layout.
 *   - Cortex-M0+ TRM, DWT cycle counter.
 *   - OpenOCD user guide, "RTT".
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/timer.h"

#include "debug_common.h"

/* -------------------------------------------------------------------------
 * RTT control block and ring storage.
 *
 * The control block MUST start with the "SEGGER RTT" magic; the host scans
 * SRAM for exactly these bytes. We keep the whole thing in .data (not .bss)
 * so the magic is present in the image and survives the C-runtime bss clear.
 * ----------------------------------------------------------------------- */

static uint8_t g_rtt_up_storage[CC_RTT_UP_BUFFER_SIZE];

/* The control block. The id[] field carries the magic. Note SEGGER scans for
   the magic, so it must be byte-exact: 'S','E','G','G','E','R',' ','R','T','T'. */
static cc_rtt_control_block_t g_rtt_cb = {
    .id = CC_RTT_MAGIC_STR,    /* "SEGGER RTT" + implicit NUL pad to 16 bytes */
    .max_up_channels = 1,
    .max_down_channels = 0,
    .up = {
        {
            .name = "Terminal",
            .buffer = g_rtt_up_storage,
            .size_bytes = CC_RTT_UP_BUFFER_SIZE,
            .write_offset = 0u,
            .read_offset = 0u,
            .flags = CC_RTT_FLAG_SKIP,   /* drop on overflow; never block in an ISR */
        }
    },
};

void cc_rtt_init(void) {
    /* The control block is statically initialized; nothing to do at runtime
       except ensure the storage is in a region the host can read (SRAM) and
       the magic is in place. We re-assert the magic defensively in case a
       prior run left the region dirty (it cannot, it is .data, but the habit
       is cheap and the cost is ten bytes). */
    memcpy(g_rtt_cb.id, CC_RTT_MAGIC_STR, sizeof(CC_RTT_MAGIC_STR));
    g_rtt_cb.up[0].write_offset = 0u;
    g_rtt_cb.up[0].read_offset = 0u;
}

/* The hot path. Non-blocking, ISR-safe, bounded constant time. */
size_t cc_rtt_write(const void *data, size_t length) {
    cc_rtt_ring_t *ring = &g_rtt_cb.up[0];
    const uint8_t *src = (const uint8_t *)data;
    uint32_t wr = ring->write_offset;
    uint32_t rd = ring->read_offset;   /* host advances this over SWD */
    size_t written = 0u;

    while (written < length) {
        uint32_t next = wr + 1u;
        if (next >= ring->size_bytes) {
            next = 0u;
        }
        if (next == rd) {
            /* Ring full. In SKIP mode we drop the rest; never spin in an ISR. */
            if (ring->flags == CC_RTT_FLAG_SKIP) {
                break;
            }
            rd = ring->read_offset;     /* block mode: re-check the host index */
            if (next == rd) {
                continue;
            }
        }
        ring->buffer[wr] = src[written++];
        wr = next;
    }

    /* Publish the data BEFORE the index, so a racing host read never sees an
       advanced write_offset pointing at a byte we have not stored yet. On the
       single-issue in-order M0+ a compiler barrier suffices. */
    __asm volatile("" ::: "memory");
    ring->write_offset = wr;
    return written;
}

size_t cc_rtt_write_str(const char *s) {
    return cc_rtt_write(s, strlen(s));
}

/* -------------------------------------------------------------------------
 * A tiny integer-to-decimal helper so the ISR can log a number without
 * pulling in printf (which would not be ISR-safe and would defeat the point).
 * ----------------------------------------------------------------------- */

static void rtt_write_u32(uint32_t v) {
    char buf[11];
    int n = 0;
    if (v == 0u) {
        cc_rtt_write_str("0");
        return;
    }
    while (v > 0u && n < (int)sizeof(buf)) {
        buf[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    char rev[11];
    for (int i = 0; i < n; i++) {
        rev[i] = buf[n - 1 - i];
    }
    cc_rtt_write(rev, (size_t)n);
}

/* -------------------------------------------------------------------------
 * ISR demo: a repeating timer that logs over RTT from interrupt context.
 *
 * A UART printf here would risk a re-entrancy deadlock (the interrupted thread
 * may hold the stdio lock) and would add hundreds of microseconds of busy-wait
 * inside the ISR. RTT is a memcpy + index bump: safe and fast.
 * ----------------------------------------------------------------------- */

static volatile uint32_t g_isr_ticks = 0u;
static volatile uint32_t g_isr_max_cycles = 0u;
static uint32_t g_dwt_overhead = 0u;

static bool timer_isr(repeating_timer_t *rt) {
    (void)rt;
    uint32_t t0 = cc_dwt_cyccnt();

    g_isr_ticks++;
    cc_rtt_write_str("[isr] tick ");
    rtt_write_u32(g_isr_ticks);
    cc_rtt_write_str("\n");

    uint32_t dt = cc_dwt_cyccnt() - t0 - g_dwt_overhead;
    if (dt > g_isr_max_cycles) {
        g_isr_max_cycles = dt;
    }
    return true;  /* keep repeating */
}

/* -------------------------------------------------------------------------
 * Calibrate the DWT measurement overhead once: time an empty region.
 * ----------------------------------------------------------------------- */

static uint32_t calibrate_dwt_overhead(void) {
    uint32_t a = cc_dwt_cyccnt();
    uint32_t b = cc_dwt_cyccnt();
    return b - a;
}

int main(void) {
    stdio_init_all();      /* UART/CDC for the A/B comparison only */
    sleep_ms(2000);

    cc_dwt_enable();
    g_dwt_overhead = calibrate_dwt_overhead();

    cc_rtt_init();
    cc_rtt_write_str("=== Exercise 2: RTT logging ===\n");
    cc_rtt_write_str("RTT up-channel live. Attach the host viewer.\n");

    printf("[uart] DWT overhead calibrated: %lu cycles\n",
           (unsigned long)g_dwt_overhead);

    /* --- A/B: cost of one RTT line vs one blocking UART printf line. --- */
    {
        const char *line = "the quick brown fox jumps over the lazy dog\n";

        uint32_t t0 = cc_dwt_cyccnt();
        cc_rtt_write_str(line);
        uint32_t rtt_cycles = cc_dwt_cyccnt() - t0 - g_dwt_overhead;

        t0 = cc_dwt_cyccnt();
        printf("%s", line);            /* blocking UART/CDC */
        uint32_t uart_cycles = cc_dwt_cyccnt() - t0 - g_dwt_overhead;

        printf("[uart] one RTT line  = %lu cycles\n", (unsigned long)rtt_cycles);
        printf("[uart] one UART line = %lu cycles\n", (unsigned long)uart_cycles);
        printf("[uart] UART is ~%lux costlier than RTT\n",
               (unsigned long)(uart_cycles / (rtt_cycles ? rtt_cycles : 1u)));

        cc_rtt_write_str("[rtt] (same line logged over RTT — note it was cheap)\n");
    }

    /* Start the timer ISR logging over RTT at 1 kHz. */
    static repeating_timer_t rt;
    add_repeating_timer_us(-1000, timer_isr, NULL, &rt);

    /* Thread-context heartbeat over RTT; report the ISR's worst-case cost. */
    uint32_t beat = 0u;
    while (true) {
        cc_rtt_write_str("[thread] heartbeat ");
        rtt_write_u32(beat++);
        cc_rtt_write_str(" isr_max_cycles=");
        rtt_write_u32(g_isr_max_cycles);
        cc_rtt_write_str("\n");
        sleep_ms(1000);
    }
    return 0;
}

/* End of exercise-02-rtt-logging.c. */
