/*
 * debug_app.c — Mini-project application: the crash-dump server plus the
 * five-bug "find the bug" pentathlon harness.
 *
 * On boot it:
 *   1. Brings up RTT and the DWT cycle counter.
 *   2. Installs the production fault handler (fault_handler.c).
 *   3. Checks for a crash dump left by the previous boot and prints the
 *      postmortem if one is found.
 *   4. Selects one of five injected bugs (by a compile-time or no-init-stored
 *      selector) and runs it, so the student can practice rooting it out with
 *      the tools from the week.
 *
 * The five bugs (the pentathlon — see mini-project/README.md):
 *   BUG_NULL_DEREF    : a NULL function-pointer call -> HardFault, core dump.
 *   BUG_STACK_OVERFLOW: unbounded recursion -> HardFault with SP below SRAM.
 *   BUG_BUFFER_OVERRUN: an off-by-one that corrupts a watched canary.
 *   BUG_ISR_JITTER    : an ISR that occasionally overruns its deadline.
 *   BUG_RACE          : a non-atomic shared-state race (a heisenbug).
 *
 * The RTT up-channel is implemented here (cc_rtt_init / cc_rtt_write /
 * cc_rtt_write_str) so fault_handler.c can log over it.
 *
 * Build with the Pico SDK: see CMakeLists.txt. Select a bug at build time:
 *   cmake .. -DCC_BUG=BUG_NULL_DEREF
 * or leave it unset to step through them interactively over the CDC console.
 *
 * Citations:
 *   - All of Week 12's lecture notes.
 *   - RP2040 datasheet §2.4, §4.7.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/timer.h"
#include "hardware/sync.h"

#include "../exercises/debug_common.h"
#include "crash_store.h"

/* ========================================================================
 * RTT up-channel (single channel; matches the SEGGER layout).
 * ===================================================================== */

static uint8_t g_rtt_storage[CC_RTT_UP_BUFFER_SIZE];

static cc_rtt_control_block_t g_rtt_cb = {
    .id = CC_RTT_MAGIC_STR,
    .max_up_channels = 1,
    .max_down_channels = 0,
    .up = {
        {
            .name = "Terminal",
            .buffer = g_rtt_storage,
            .size_bytes = CC_RTT_UP_BUFFER_SIZE,
            .write_offset = 0u,
            .read_offset = 0u,
            .flags = CC_RTT_FLAG_SKIP,
        }
    },
};

void cc_rtt_init(void) {
    memcpy(g_rtt_cb.id, CC_RTT_MAGIC_STR, sizeof(CC_RTT_MAGIC_STR));
    g_rtt_cb.up[0].write_offset = 0u;
    g_rtt_cb.up[0].read_offset = 0u;
}

size_t cc_rtt_write(const void *data, size_t length) {
    cc_rtt_ring_t *ring = &g_rtt_cb.up[0];
    const uint8_t *src = (const uint8_t *)data;
    uint32_t wr = ring->write_offset;
    uint32_t rd = ring->read_offset;
    size_t written = 0u;
    while (written < length) {
        uint32_t next = wr + 1u;
        if (next >= ring->size_bytes) next = 0u;
        if (next == rd) {
            if (ring->flags == CC_RTT_FLAG_SKIP) break;
            rd = ring->read_offset;
            if (next == rd) continue;
        }
        ring->buffer[wr] = src[written++];
        wr = next;
    }
    __asm volatile("" ::: "memory");
    ring->write_offset = wr;
    return written;
}

size_t cc_rtt_write_str(const char *s) {
    return cc_rtt_write(s, strlen(s));
}

/* ========================================================================
 * DWT timing helper state.
 * ===================================================================== */

static uint32_t g_dwt_overhead = 0u;

static uint32_t calibrate_dwt_overhead(void) {
    uint32_t a = cc_dwt_cyccnt();
    uint32_t b = cc_dwt_cyccnt();
    return b - a;
}

/* ========================================================================
 * The five injected bugs.
 * ===================================================================== */

typedef enum {
    BUG_NONE = 0,
    BUG_NULL_DEREF,
    BUG_STACK_OVERFLOW,
    BUG_BUFFER_OVERRUN,
    BUG_ISR_JITTER,
    BUG_RACE,
} bug_id_t;

/* ---- Bug 1: NULL function-pointer call. ---- */

typedef void (*voidfn_t)(void);

static void bug_null_deref(void) {
    cc_rtt_write_str("[bug] BUG_NULL_DEREF: calling a NULL handler...\n");
    printf("[bug] BUG_NULL_DEREF: calling a NULL handler...\n");
    sleep_ms(500);
    voidfn_t fn = (voidfn_t)0;     /* address 0, no thumb bit */
    fn();                          /* -> HardFault; the dump names this site */
}

/* ---- Bug 2: unbounded recursion -> stack overflow. ---- */

static uint32_t recurse(uint32_t depth) {
    /* A volatile array per frame so the optimizer cannot tail-call this away;
       each frame consumes real stack, so the depth needed to overflow is
       small and deterministic. */
    volatile uint32_t scratch[16];
    for (int i = 0; i < 16; i++) scratch[i] = depth + (uint32_t)i;
    return recurse(depth + 1u) + scratch[depth & 0xFu];   /* no base case */
}

static void bug_stack_overflow(void) {
    cc_rtt_write_str("[bug] BUG_STACK_OVERFLOW: recursing without bound...\n");
    printf("[bug] BUG_STACK_OVERFLOW: recursing without bound...\n");
    sleep_ms(500);
    volatile uint32_t sink = recurse(0u);   /* SP walks off the SRAM floor */
    (void)sink;
}

/* ---- Bug 3: off-by-one buffer overrun corrupting a watched canary. ---- */

#define CANARY_BUF_LEN 16
static volatile uint8_t  g_canary_buf[CANARY_BUF_LEN];
static volatile uint32_t g_canary = 0xCAFEF00Du;

static void bug_buffer_overrun(void) {
    cc_rtt_write_str("[bug] BUG_BUFFER_OVERRUN: set `watch g_canary` in GDB\n");
    printf("[bug] BUG_BUFFER_OVERRUN: set `watch g_canary` in GDB.\n");
    printf("  g_canary before = 0x%08lx\n", (unsigned long)g_canary);
    /* off-by-one: <= writes g_canary_buf[CANARY_BUF_LEN], hitting g_canary. */
    for (size_t i = 0u; i <= CANARY_BUF_LEN; i++) {
        g_canary_buf[i] = (uint8_t)(0xA0u + i);
    }
    printf("  g_canary after  = 0x%08lx  %s\n", (unsigned long)g_canary,
           (g_canary == 0xCAFEF00Du) ? "(intact)" : "(CORRUPTED)");
}

/* ---- Bug 4: an ISR that occasionally overruns its deadline. ---- */

static volatile uint32_t g_isr_ticks = 0u;
static volatile uint32_t g_isr_max_cycles = 0u;
#define ISR_DEADLINE_CYCLES 6250u   /* 50 us at 125 MHz */

static bool jitter_isr(repeating_timer_t *rt) {
    (void)rt;
    uint32_t t0 = cc_dwt_cyccnt();
    g_isr_ticks++;
    /* Most ticks are short; every 64th tick does extra work that blows the
       deadline — the planted jitter. A printf could never catch this because
       printing dominates the timing. */
    volatile uint32_t acc = 0u;
    uint32_t spins = (g_isr_ticks % 64u == 0u) ? 4000u : 40u;
    for (uint32_t i = 0u; i < spins; i++) acc += i * 2654435761u;
    uint32_t dt = cc_dwt_cyccnt() - t0 - g_dwt_overhead;
    if (dt > g_isr_max_cycles) {
        g_isr_max_cycles = dt;
        if (dt > ISR_DEADLINE_CYCLES) {
            cc_rtt_write_str("[isr] DEADLINE MISS\n");
        }
    }
    return true;
}

static void bug_isr_jitter(void) {
    cc_rtt_write_str("[bug] BUG_ISR_JITTER: watch isr_max_cycles over RTT\n");
    printf("[bug] BUG_ISR_JITTER: stream isr_max_cycles, find the overrun.\n");
    static repeating_timer_t rt;
    add_repeating_timer_us(-1000, jitter_isr, NULL, &rt);
    while (true) {
        printf("[thread] isr_ticks=%lu isr_max_cycles=%lu (deadline=%u)\n",
               (unsigned long)g_isr_ticks, (unsigned long)g_isr_max_cycles,
               (unsigned)ISR_DEADLINE_CYCLES);
        sleep_ms(1000);
    }
}

/* ---- Bug 5: a non-atomic shared-state race (heisenbug). ---- */

typedef enum { LINK_DOWN = 0, LINK_UP = 1, LINK_RECONNECTING = 2 } link_state_t;
static volatile link_state_t g_link_state = LINK_DOWN;
static volatile uint32_t g_bug_count = 0u;
static volatile uint32_t g_cycles = 0u;

static bool supervisor_isr(repeating_timer_t *rt) {
    (void)rt;
    /* The supervisor flips the link UP at a timing that can land inside the
       worker's read-decide-act window — the race. */
    if (g_link_state == LINK_DOWN) {
        g_link_state = LINK_UP;       /* spurious recovery */
    }
    return true;
}

static void bug_race(void) {
    cc_rtt_write_str("[bug] BUG_RACE: heisenbug; do NOT add printf in worker\n");
    printf("[bug] BUG_RACE: reconnect worker race. See Challenge 2 method.\n");
    static repeating_timer_t rt;
    add_repeating_timer_us(-997, supervisor_isr, NULL, &rt);  /* odd period -> drift */

    while (true) {
        g_cycles++;
        /* Worker read-decide-act on shared state, NON-ATOMIC (the bug). */
        link_state_t seen = g_link_state;
        if (seen == LINK_DOWN) {
            /* Decide to reconnect. WINDOW: the ISR may set UP right here. */
            busy_wait_us(2);                 /* widens the window deterministically */
            if (g_link_state == LINK_UP) {
                /* The link came up while we were deciding; tearing it down now
                   leaks the connection and wedges the machine. */
                g_link_state = LINK_RECONNECTING;
                g_bug_count++;
            } else {
                g_link_state = LINK_RECONNECTING;
            }
        } else if (seen == LINK_UP) {
            g_link_state = LINK_DOWN;          /* normal churn */
        } else {
            g_link_state = LINK_DOWN;          /* recover from RECONNECTING */
        }
        if ((g_cycles % 2000u) == 0u) {
            printf("[thread] cycles=%lu bug_count=%lu\n",
                   (unsigned long)g_cycles, (unsigned long)g_bug_count);
        }
    }
}

/* ========================================================================
 * Bug selection. CC_BUG can be set at build time; otherwise we present a
 * menu over the CDC console and the student picks one.
 * ===================================================================== */

#ifndef CC_BUG
#define CC_BUG BUG_NONE
#endif

static bug_id_t select_bug_interactive(void) {
    printf("\nSelect a bug to run:\n");
    printf("  1) NULL deref      (HardFault -> core dump)\n");
    printf("  2) stack overflow  (HardFault, SP below SRAM)\n");
    printf("  3) buffer overrun  (watch g_canary)\n");
    printf("  4) ISR jitter      (DWT + RTT)\n");
    printf("  5) race/heisenbug  (Challenge 2 method)\n");
    printf("Enter 1-5: ");
    fflush(stdout);
    int c = getchar_timeout_us(30u * 1000u * 1000u);
    switch (c) {
        case '1': return BUG_NULL_DEREF;
        case '2': return BUG_STACK_OVERFLOW;
        case '3': return BUG_BUFFER_OVERRUN;
        case '4': return BUG_ISR_JITTER;
        case '5': return BUG_RACE;
        default:  return BUG_NONE;
    }
}

static void run_bug(bug_id_t bug) {
    switch (bug) {
        case BUG_NULL_DEREF:     bug_null_deref();     break;
        case BUG_STACK_OVERFLOW: bug_stack_overflow(); break;
        case BUG_BUFFER_OVERRUN: bug_buffer_overrun(); break;
        case BUG_ISR_JITTER:     bug_isr_jitter();     break;  /* loops forever */
        case BUG_RACE:           bug_race();           break;  /* loops forever */
        default:
            printf("No bug selected. Idling.\n");
            break;
    }
}

int main(void) {
    stdio_init_all();
    sleep_ms(2000);

    cc_rtt_init();
    cc_dwt_enable();
    g_dwt_overhead = calibrate_dwt_overhead();
    cc_install_fault_handler();

    cc_rtt_write_str("=== cc-debug pentathlon ===\n");
    printf("\n=== cc-debug pentathlon (Week 12 mini-project) ===\n");
    printf("DWT overhead = %lu cycles\n", (unsigned long)g_dwt_overhead);

    /* Did the previous boot crash? If so, print the postmortem. */
    cc_crash_dump_t recovered;
    if (cc_crash_store_load(&recovered) == CC_DBG_OK) {
        cc_print_postmortem(&recovered);
        cc_crash_store_consume();
    } else {
        printf("Clean boot (no prior crash dump).\n");
    }

    bug_id_t bug = (CC_BUG != BUG_NONE) ? (bug_id_t)CC_BUG
                                        : select_bug_interactive();
    printf("Running bug id %d.\n", (int)bug);
    run_bug(bug);

    /* For the non-looping bugs (1,2,3) we reach here. */
    while (true) {
        cc_rtt_write_str("[thread] idle (bug ran; recover via BOOTSEL if needed)\n");
        printf("[thread] idle\n");
        sleep_ms(2000);
    }
    return 0;
}

/* End of debug_app.c. */
