/*
 * fault_handler.c — Mini-project: the production naked HardFault handler,
 * core-dump capture, and postmortem printer.
 *
 * This is the souped-up version of exercise-03's handler:
 *   - Naked preamble selects MSP/PSP from EXC_RETURN bit 2 and captures
 *     R4-R11 while pristine.
 *   - The C side fills a CRC-protected cc_crash_dump_t, runs a heuristic
 *     backtrace, writes BOTH the no-init SRAM dump and the watchdog-scratch
 *     minimal record, and resets.
 *   - On the next clean boot, cc_print_postmortem() prints a human-readable
 *     dump over RTT and stdio.
 *
 * The CRC / validate / finalize helpers live here (not in crash_store.c) so
 * the at-fault-time code path is in one translation unit and has no library
 * dependencies that might be unsafe to call mid-fault.
 *
 * Citations:
 *   - ARMv6-M ARM §B1.5.6 (stacked frame), §B1.5.8 (EXC_RETURN).
 *   - RP2040 datasheet §2.4 (Cortex-M0+), §4.7 (Watchdog).
 *   - Memfault Interrupt HardFault + coredump posts.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "hardware/exception.h"

#include "../exercises/debug_common.h"
#include "crash_store.h"

/* The SDK linker provides these. */
extern uint32_t __flash_binary_start;
extern uint32_t __flash_binary_end;

/* -------------------------------------------------------------------------
 * CRC32 (IEEE 802.3, reflected, init/final 0xFFFFFFFF). Table-free so it has
 * no data dependencies that could be corrupt at fault time.
 * ----------------------------------------------------------------------- */

uint32_t cc_crc32(const void *data, size_t length) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0u; i < length; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = (uint32_t)(-(int32_t)(crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

void cc_crash_dump_finalize(cc_crash_dump_t *dump) {
    dump->crc32 = cc_crc32(dump, offsetof(cc_crash_dump_t, crc32));
}

cc_dbg_result_t cc_crash_dump_validate(const cc_crash_dump_t *dump) {
    if (dump->magic != CC_CRASH_MAGIC)     return CC_DBG_ERR_BAD_MAGIC;
    if (dump->version != CC_CRASH_VERSION) return CC_DBG_ERR_BAD_VERSION;
    if (cc_crc32(dump, offsetof(cc_crash_dump_t, crc32)) != dump->crc32) {
        return CC_DBG_ERR_BAD_CRC;
    }
    return CC_DBG_OK;
}

/* -------------------------------------------------------------------------
 * Heuristic backtrace by stack scan.
 * ----------------------------------------------------------------------- */

static size_t backtrace_scan(const uint32_t *sp, uint32_t *out, size_t max) {
    uint32_t lo = (uint32_t)&__flash_binary_start;
    uint32_t hi = (uint32_t)&__flash_binary_end;
    const uint32_t *top = (const uint32_t *)0x20042000u;  /* top of SRAM */
    size_t n = 0u;
    for (const uint32_t *q = sp; q < top && n < max; q++) {
        uint32_t w = *q;
        if (w >= lo && w < hi && (w & 1u) != 0u) {
            out[n++] = w & ~1u;
        }
    }
    return n;
}

/* -------------------------------------------------------------------------
 * The C side of the fault handler.
 * ----------------------------------------------------------------------- */

void cc_fault_handler_c(uint32_t *frame_sp, uint32_t *saved_regs) {
    cc_crash_dump_t *dump = cc_crash_store_get();

    dump->magic   = CC_CRASH_MAGIC;
    dump->version = CC_CRASH_VERSION;

    cc_exception_frame_t *f = (cc_exception_frame_t *)frame_sp;
    dump->frame      = *f;
    dump->stacked_sp = (uint32_t)frame_sp;
    dump->exc_number = f->xpsr & CC_ICSR_VECTACTIVE_MASK;

    /* saved_regs layout from the preamble: [R8,R9,R10,R11, R4,R5,R6,R7]. */
    for (int i = 0; i < 6; i++) {
        dump->extra_regs[i] = saved_regs[i];
    }

    dump->backtrace_count =
        (uint32_t)backtrace_scan(frame_sp, dump->backtrace, 8u);

    cc_crash_dump_finalize(dump);          /* CRC the SRAM dump */
    cc_crash_store_write_scratch(dump);    /* minimal scratch fallback */

    /* Reset; the no-init dump survives the warm reset. */
    watchdog_reboot(0u, 0u, 0u);
    for (;;) { __asm volatile("wfi"); }
}

/* -------------------------------------------------------------------------
 * The naked HardFault handler.
 * ----------------------------------------------------------------------- */

__attribute__((naked)) void cc_hardfault_handler(void) {
    __asm volatile(
        "movs r0, #4                  \n"
        "mov  r1, lr                  \n"
        "tst  r0, r1                  \n"
        "beq  1f                      \n"
        "mrs  r0, psp                 \n"
        "b    2f                      \n"
        "1:                           \n"
        "mrs  r0, msp                 \n"
        "2:                           \n"
        "push {r4, r5, r6, r7}        \n"   /* save R4-R7 */
        "mov  r4, r8                  \n"
        "mov  r5, r9                  \n"
        "mov  r6, r10                 \n"
        "mov  r7, r11                 \n"
        "push {r4, r5, r6, r7}        \n"   /* save R8-R11 (now in R4-R7) */
        "mov  r1, sp                  \n"   /* r1 -> [R8..R11, R4..R7] */
        "ldr  r2, =cc_fault_handler_c \n"
        "bx   r2                      \n"
        :
        :
        : "memory"
    );
}

void cc_install_fault_handler(void) {
    exception_set_exclusive_handler(HARDFAULT_EXCEPTION, cc_hardfault_handler);
}

/* -------------------------------------------------------------------------
 * Postmortem printer. Prints over both RTT (if up) and stdio.
 * ----------------------------------------------------------------------- */

static const char *exc_name(uint32_t n) {
    switch (n) {
        case CC_EXC_THREAD:    return "thread mode";
        case CC_EXC_NMI:       return "NMI";
        case CC_EXC_HARDFAULT: return "HardFault";
        case CC_EXC_SVCALL:    return "SVCall";
        case CC_EXC_PENDSV:    return "PendSV";
        case CC_EXC_SYSTICK:   return "SysTick";
        default:
            return (n >= CC_EXC_IRQ0) ? "external IRQ" : "unknown";
    }
}

static const char *source_name(uint32_t s) {
    switch (s) {
        case CC_CRASH_SOURCE_SRAM:    return "no-init SRAM (full dump)";
        case CC_CRASH_SOURCE_SCRATCH: return "watchdog scratch (minimal)";
        default:                      return "unknown";
    }
}

void cc_print_postmortem(const cc_crash_dump_t *d) {
    /* Build the lines once; emit to both transports. cc_rtt_write_str is the
       non-perturbing path (works even if stdio is not up). printf is the
       convenient one for a developer on the CDC port. */
    char line[128];

    cc_rtt_write_str("\n*** CRASH DETECTED (previous boot) ***\n");
    printf("\n*** CRASH DETECTED (previous boot) ***\n");

    snprintf(line, sizeof(line),
             "  source: %s\n  faulted in: %s (exc #%lu)\n",
             source_name(d->source),
             exc_name(d->exc_number), (unsigned long)d->exc_number);
    cc_rtt_write_str(line); printf("%s", line);

    snprintf(line, sizeof(line),
             "  PC=0x%08lx LR=0x%08lx xPSR=0x%08lx\n",
             (unsigned long)d->frame.pc, (unsigned long)d->frame.lr,
             (unsigned long)d->frame.xpsr);
    cc_rtt_write_str(line); printf("%s", line);

    if (d->source == CC_CRASH_SOURCE_SRAM) {
        snprintf(line, sizeof(line),
                 "  R0=0x%08lx R1=0x%08lx R2=0x%08lx R3=0x%08lx\n",
                 (unsigned long)d->frame.r0, (unsigned long)d->frame.r1,
                 (unsigned long)d->frame.r2, (unsigned long)d->frame.r3);
        cc_rtt_write_str(line); printf("%s", line);

        snprintf(line, sizeof(line),
                 "  R12=0x%08lx  stacked SP=0x%08lx\n",
                 (unsigned long)d->frame.r12, (unsigned long)d->stacked_sp);
        cc_rtt_write_str(line); printf("%s", line);

        snprintf(line, sizeof(line),
                 "  R8..R11=0x%08lx 0x%08lx 0x%08lx 0x%08lx\n",
                 (unsigned long)d->extra_regs[0], (unsigned long)d->extra_regs[1],
                 (unsigned long)d->extra_regs[2], (unsigned long)d->extra_regs[3]);
        cc_rtt_write_str(line); printf("%s", line);

        snprintf(line, sizeof(line),
                 "  R4..R7 =0x%08lx 0x%08lx (R6,R7 follow)\n",
                 (unsigned long)d->extra_regs[4], (unsigned long)d->extra_regs[5]);
        cc_rtt_write_str(line); printf("%s", line);

        cc_rtt_write_str("  backtrace (heuristic):\n");
        printf("  backtrace (heuristic):\n");
        for (uint32_t i = 0u; i < d->backtrace_count; i++) {
            snprintf(line, sizeof(line), "    0x%08lx\n",
                     (unsigned long)d->backtrace[i]);
            cc_rtt_write_str(line); printf("%s", line);
        }
    }

    snprintf(line, sizeof(line),
             "  -> arm-none-eabi-addr2line -f -e debug_app.elf 0x%08lx\n",
             (unsigned long)d->frame.pc);
    cc_rtt_write_str(line); printf("%s", line);
    cc_rtt_write_str("*** end postmortem ***\n\n");
    printf("*** end postmortem ***\n\n");
}

/* End of fault_handler.c. */
