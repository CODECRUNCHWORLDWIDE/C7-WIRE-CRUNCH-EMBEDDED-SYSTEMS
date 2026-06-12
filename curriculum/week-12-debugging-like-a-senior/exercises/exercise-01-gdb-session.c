/*
 * exercise-01-gdb-session.c — A target program with planted bugs, for driving
 * a live GDB + OpenOCD session over SWD.
 *
 * This runs ON THE PICO (the target). You attach a second Pico flashed as a
 * debugprobe, run OpenOCD, attach gdb-multiarch, and practice the workflow
 * from Lecture 1: hardware breakpoints, conditional breakpoints, hardware
 * watchpoints, single-stepping, and reading registers/memory.
 *
 * Three planted situations to find with GDB:
 *   1. A "who corrupts g_canary" mystery — solved with a hardware watchpoint.
 *   2. A loop whose worst input only triggers a bug at a specific iteration —
 *      solved with a conditional breakpoint.
 *   3. A function-pointer table with one deliberately-bad entry that, if you
 *      let it run, jumps to a non-thumb address and HardFaults — solved by
 *      stepping up to it and inspecting the table before the jump.
 *
 * Build with the Pico SDK:
 *   add_executable(ex1 exercise-01-gdb-session.c)
 *   target_link_libraries(ex1 pico_stdlib)
 *   pico_add_extra_outputs(ex1)
 *
 * Debug session (see SOLUTIONS.md for the full transcript):
 *   Terminal A:  openocd -f interface/cmsis-dap.cfg -f target/rp2040.cfg
 *   Terminal B:  gdb-multiarch build/ex1.elf
 *                (gdb) target extended-remote localhost:3333
 *                (gdb) load
 *                (gdb) monitor reset halt
 *                (gdb) watch g_canary
 *                (gdb) continue
 *
 * Citations:
 *   - RP2040 datasheet §2.3.4 (SWD), §2.4 (Cortex-M0+).
 *   - GDB user manual, "Watchpoints" and "Breakpoints".
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "debug_common.h"

/* -------------------------------------------------------------------------
 * Situation 1: a canary that "mysteriously" gets corrupted.
 *
 * g_canary sits right after g_buffer in memory. A deliberate off-by-one in
 * fill_buffer() writes one element past the end of g_buffer, landing on
 * g_canary. Find the culprit with:  (gdb) watch g_canary
 * GDB halts on the exact store; `bt` and `info line *$pc` name the line.
 * ----------------------------------------------------------------------- */

#define BUFFER_LEN 16

static volatile uint8_t g_buffer[BUFFER_LEN];
static volatile uint32_t g_canary = 0xCAFEF00Du;

static void fill_buffer(uint8_t seed) {
    /* BUG (planted): the loop condition uses <= instead of <, so the final
       iteration writes g_buffer[BUFFER_LEN], one past the end, clobbering the
       low byte of g_canary. A hardware watchpoint on g_canary catches it. */
    for (size_t i = 0u; i <= BUFFER_LEN; i++) {
        g_buffer[i] = (uint8_t)(seed + (uint8_t)i);
    }
}

/* -------------------------------------------------------------------------
 * Situation 2: a loop whose bug only shows at one iteration.
 *
 * checksum_with_glitch() computes a rolling sum but, at exactly index 100,
 * reads from a deliberately-bad offset. Catch it with a conditional
 * breakpoint:  (gdb) break checksum_with_glitch if i == 100
 * ----------------------------------------------------------------------- */

static uint32_t g_big_table[256];

static uint32_t checksum_with_glitch(size_t count) {
    uint32_t sum = 0u;
    for (size_t i = 0u; i < count; i++) {
        size_t idx = i;
        /* BUG (planted): at i==100 we index 100 positions too far. With
           count<=256 and the table being 256 long, idx+156 overruns. A
           conditional breakpoint on i==100 lets you inspect idx before the
           bad read. */
        if (i == 100u) {
            idx = i + 156u;            /* 256 — out of bounds read */
        }
        sum += g_big_table[idx & 0xFFu]; /* masked so it does not fault, just
                                            returns wrong data — a logic bug,
                                            not a crash. */
    }
    return sum;
}

/* -------------------------------------------------------------------------
 * Situation 3: a dispatch table with one bad entry.
 *
 * handlers[] holds three valid function pointers and one NULL. dispatch()
 * calls handlers[sel](). If sel==2 (the NULL slot) is reached, the call jumps
 * to address 0 (no thumb bit) and HardFaults. Step up to the call, inspect
 * the table with `p/x handlers`, and SEE the NULL before you let it jump.
 * ----------------------------------------------------------------------- */

typedef void (*handler_fn_t)(void);

static volatile uint32_t g_h0_count = 0;
static volatile uint32_t g_h1_count = 0;
static volatile uint32_t g_h3_count = 0;

static void handler0(void) { g_h0_count++; }
static void handler1(void) { g_h1_count++; }
static void handler3(void) { g_h3_count++; }

/* BUG (planted): index 2 is NULL. Calling it branches to address 0. */
static const handler_fn_t handlers[4] = {
    handler0,
    handler1,
    (handler_fn_t)0,   /* the trap */
    handler3,
};

static void dispatch(uint32_t sel) {
    handler_fn_t fn = handlers[sel & 0x3u];
    /* In a real debug session, set `hbreak dispatch`, step to here, then
       `p/x fn` — if it is 0, do NOT `step` into it (that HardFaults). Set
       `set var sel = 3` to route around the bug and keep the session alive. */
    fn();
}

/* -------------------------------------------------------------------------
 * Main: exercise each situation. Without a debugger this program runs,
 * silently corrupts its canary, computes a wrong checksum, and eventually
 * faults on the dispatch trap. WITH a debugger, you observe each step.
 * ----------------------------------------------------------------------- */

int main(void) {
    stdio_init_all();
    sleep_ms(2000);  /* allow CDC enumeration before any printf */

    cc_dwt_enable();

    printf("\n=== Exercise 1: GDB session target ===\n");
    printf("Attach OpenOCD + gdb-multiarch and follow SOLUTIONS.md.\n");

    /* Initialize the lookup table so the checksum has deterministic input. */
    for (size_t i = 0u; i < 256u; i++) {
        g_big_table[i] = (uint32_t)(i * 2654435761u);  /* Knuth multiplicative */
    }

    /* Situation 1 — fill the buffer (corrupts g_canary on the last iteration). */
    printf("Situation 1: filling buffer (watch g_canary)...\n");
    printf("  g_canary before = 0x%08lx\n", (unsigned long)g_canary);
    fill_buffer(0x10u);
    printf("  g_canary after  = 0x%08lx  %s\n",
           (unsigned long)g_canary,
           (g_canary == 0xCAFEF00Du) ? "(intact)" : "(CORRUPTED)");

    /* Situation 2 — checksum with a glitch at i==100. */
    printf("Situation 2: checksum (break if i==100)...\n");
    uint32_t cs = checksum_with_glitch(200u);
    printf("  checksum = 0x%08lx\n", (unsigned long)cs);

    /* Situation 3 — dispatch. sel 0,1,3 are safe; sel 2 is the trap.
       Loop through the safe ones first, then deliberately hit the trap LAST
       so you have time to break on dispatch and inspect the table. */
    printf("Situation 3: dispatch (hbreak dispatch; inspect handlers[])...\n");
    for (uint32_t sel = 0u; sel < 4u; sel++) {
        printf("  dispatch(%lu)\n", (unsigned long)sel);
        sleep_ms(200);
        dispatch(sel);   /* sel==2 HardFaults unless you intervene in GDB */
    }

    printf("If you reach here, you routed around the dispatch trap. Nicely done.\n");

    /* Idle. BOOTSEL still recovers if anything went wrong. */
    while (true) {
        printf("  h0=%lu h1=%lu h3=%lu\n",
               (unsigned long)g_h0_count,
               (unsigned long)g_h1_count,
               (unsigned long)g_h3_count);
        sleep_ms(1000);
    }
    return 0;
}

/* End of exercise-01-gdb-session.c. */
