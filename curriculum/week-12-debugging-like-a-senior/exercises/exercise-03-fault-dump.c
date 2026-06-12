/*
 * exercise-03-fault-dump.c — A naked HardFault handler that captures a
 * CRC-checked core dump, persists it across reset in no-init SRAM, and prints
 * a postmortem on the next clean boot.
 *
 * This is the on-device half of the Unix "core file" workflow:
 *   - On a HardFault, capture R0-R12, the stacked PC/LR/xPSR, R4-R11, and a
 *     heuristic backtrace; CRC-protect the record; reset.
 *   - On the next boot, validate the record and print a human-readable dump.
 *
 * It implements cc_crc32, cc_crash_dump_validate, and cc_crash_dump_finalize
 * from debug_common.h, plus the naked handler and the backtrace scan.
 *
 * To SEE it work: this program deliberately faults a few seconds after boot
 * (a store to a deliberately-unaligned, out-of-range address). It reboots,
 * detects the dump, prints the postmortem, then runs cleanly (the fault path
 * is gated behind a counter the dump clears).
 *
 * Build with the Pico SDK:
 *   add_executable(ex3 exercise-03-fault-dump.c)
 *   target_link_libraries(ex3 pico_stdlib hardware_watchdog hardware_exception)
 *   pico_add_extra_outputs(ex3)
 *   # Add a .noinit (NOLOAD) section to your linker script — see SOLUTIONS.md.
 *
 * Then map the faulting PC to a source line off-target:
 *   arm-none-eabi-addr2line -f -e build/ex3.elf <PC-from-postmortem>
 *
 * Citations:
 *   - ARMv6-M ARM §B1.5.6 (stacked frame), §B1.5.8 (EXC_RETURN).
 *   - RP2040 datasheet §2.4 (Cortex-M0+), §4.7 (Watchdog scratch).
 *   - Memfault Interrupt HardFault and coredump posts.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "hardware/exception.h"

#include "debug_common.h"

/* -------------------------------------------------------------------------
 * No-init storage. The linker script must place .noinit in a NOLOAD section
 * the C runtime does NOT clear, so it survives a warm (watchdog) reset.
 * See SOLUTIONS.md for the linker fragment.
 * ----------------------------------------------------------------------- */

__attribute__((section(".noinit"), used))
static cc_crash_dump_t g_crash_dump;

/* A separate no-init counter that gates the deliberate fault: we fault once,
   then the postmortem path clears it so subsequent boots run clean. */
__attribute__((section(".noinit"), used))
static uint32_t g_fault_armed;
__attribute__((section(".noinit"), used))
static uint32_t g_fault_armed_magic;

#define FAULT_ARMED_MAGIC 0x4641554Cu  /* "FAUL" */

cc_crash_dump_t *cc_crash_store_get(void) {
    return &g_crash_dump;
}

/* -------------------------------------------------------------------------
 * CRC32 — IEEE 802.3, reflected, init 0xFFFFFFFF, final XOR 0xFFFFFFFF.
 * Bit-banged (no table) so it has zero data dependencies at fault time.
 * ----------------------------------------------------------------------- */

uint32_t cc_crc32(const void *data, size_t length) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0u; i < length; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

void cc_crash_dump_finalize(cc_crash_dump_t *dump) {
    /* CRC covers everything up to (but not including) the crc32 field. */
    size_t crc_region = offsetof(cc_crash_dump_t, crc32);
    dump->crc32 = cc_crc32(dump, crc_region);
}

cc_dbg_result_t cc_crash_dump_validate(const cc_crash_dump_t *dump) {
    if (dump->magic != CC_CRASH_MAGIC)     return CC_DBG_ERR_BAD_MAGIC;
    if (dump->version != CC_CRASH_VERSION) return CC_DBG_ERR_BAD_VERSION;
    size_t crc_region = offsetof(cc_crash_dump_t, crc32);
    if (cc_crc32(dump, crc_region) != dump->crc32) return CC_DBG_ERR_BAD_CRC;
    return CC_DBG_OK;
}

/* -------------------------------------------------------------------------
 * Heuristic backtrace: scan the stack above the fault frame for words that
 * look like return addresses into the flash .text range with the thumb bit
 * set. Over-reports; a human filters with addr2line.
 * ----------------------------------------------------------------------- */

/* These symbols are defined by the Pico SDK linker script. */
extern uint32_t __flash_binary_start;
extern uint32_t __flash_binary_end;

static size_t cc_backtrace_scan(const uint32_t *sp, uint32_t *out, size_t max) {
    uint32_t lo = (uint32_t)&__flash_binary_start;
    uint32_t hi = (uint32_t)&__flash_binary_end;
    const uint32_t *top = (const uint32_t *)0x20042000u;  /* top of 264 KB SRAM */
    size_t n = 0u;
    for (const uint32_t *q = sp; q < top && n < max; q++) {
        uint32_t w = *q;
        if (w >= lo && w < hi && (w & 1u) != 0u) {
            out[n++] = w & ~1u;     /* strip thumb bit for addr2line */
        }
    }
    return n;
}

/* -------------------------------------------------------------------------
 * The C side of the fault handler. Receives the frame SP and a pointer to the
 * preamble-saved high registers ([R8,R9,R10,R11,R4,R5,R6,R7]).
 * ----------------------------------------------------------------------- */

void cc_fault_handler_c(uint32_t *frame_sp, uint32_t *saved_regs) {
    cc_crash_dump_t *dump = cc_crash_store_get();

    dump->magic   = CC_CRASH_MAGIC;
    dump->version = CC_CRASH_VERSION;

    cc_exception_frame_t *f = (cc_exception_frame_t *)frame_sp;
    dump->frame      = *f;
    dump->stacked_sp = (uint32_t)frame_sp;
    /* IPSR of the stacked xPSR: the exception we were IN when we faulted
       (0 = thread, or an IRQ number). The fact that WE are HardFault is
       implicit (this is the HardFault handler). */
    dump->exc_number = f->xpsr & CC_ICSR_VECTACTIVE_MASK;

    for (int i = 0; i < 6; i++) {
        dump->extra_regs[i] = saved_regs[i];
    }

    dump->backtrace_count =
        (uint32_t)cc_backtrace_scan(frame_sp, dump->backtrace, 8u);

    cc_crash_dump_finalize(dump);

    /* Disarm the deliberate fault so the next boot runs clean. */
    g_fault_armed = 0u;
    g_fault_armed_magic = FAULT_ARMED_MAGIC;

    /* Reset; the dump survives in no-init SRAM through the warm reset. */
    watchdog_reboot(0u, 0u, 0u);
    for (;;) { __asm volatile("wfi"); }
}

/* -------------------------------------------------------------------------
 * The naked HardFault handler. Selects MSP vs PSP from EXC_RETURN bit 2,
 * captures R4-R11 while still pristine, and calls cc_fault_handler_c.
 * ----------------------------------------------------------------------- */

__attribute__((naked)) void cc_hardfault_handler(void) {
    __asm volatile(
        "movs r0, #4              \n"  /* EXC_RETURN bit 2 mask */
        "mov  r1, lr              \n"
        "tst  r0, r1              \n"
        "beq  1f                  \n"  /* bit clear -> frame on MSP */
        "mrs  r0, psp             \n"  /* bit set   -> frame on PSP */
        "b    2f                  \n"
        "1:                       \n"
        "mrs  r0, msp             \n"
        "2:                       \n"
        /* Save R8-R11 then R4-R7 (M0+ push only takes low regs). The order in
           memory becomes [R8,R9,R10,R11,R4,R5,R6,R7] (lowest address first). */
        "push {r4, r5, r6, r7}    \n"
        "mov  r4, r8              \n"
        "mov  r5, r9              \n"
        "mov  r6, r10             \n"
        "mov  r7, r11             \n"
        "push {r4, r5, r6, r7}    \n"
        "mov  r1, sp              \n"  /* r1 -> saved high-reg block */
        "ldr  r2, =cc_fault_handler_c \n"
        "bx   r2                  \n"
        :
        :
        : "memory"
    );
}

/* -------------------------------------------------------------------------
 * Postmortem printer. Runs on a clean boot once stdio is up.
 * ----------------------------------------------------------------------- */

static const char *exc_name(uint32_t n) {
    switch (n) {
        case CC_EXC_THREAD:  return "thread mode (no active exception)";
        case CC_EXC_NMI:     return "NMI";
        case CC_EXC_HARDFAULT: return "HardFault";
        case CC_EXC_SVCALL:  return "SVCall";
        case CC_EXC_PENDSV:  return "PendSV";
        case CC_EXC_SYSTICK: return "SysTick";
        default:
            if (n >= CC_EXC_IRQ0) return "external IRQ";
            return "unknown";
    }
}

static void print_postmortem(const cc_crash_dump_t *d) {
    printf("\n*** CRASH DETECTED (from previous boot) ***\n");
    printf("  faulted while servicing: %s (exc #%lu)\n",
           exc_name(d->exc_number), (unsigned long)d->exc_number);
    printf("  PC  = 0x%08lx  LR  = 0x%08lx  xPSR = 0x%08lx\n",
           (unsigned long)d->frame.pc, (unsigned long)d->frame.lr,
           (unsigned long)d->frame.xpsr);
    printf("  R0  = 0x%08lx  R1  = 0x%08lx  R2  = 0x%08lx  R3  = 0x%08lx\n",
           (unsigned long)d->frame.r0, (unsigned long)d->frame.r1,
           (unsigned long)d->frame.r2, (unsigned long)d->frame.r3);
    printf("  R12 = 0x%08lx\n", (unsigned long)d->frame.r12);
    /* extra_regs layout from the preamble: [R8,R9,R10,R11,R4,R5,R6,R7]. */
    printf("  R8..R11 = 0x%08lx 0x%08lx 0x%08lx 0x%08lx\n",
           (unsigned long)d->extra_regs[0], (unsigned long)d->extra_regs[1],
           (unsigned long)d->extra_regs[2], (unsigned long)d->extra_regs[3]);
    printf("  R4..R7  = 0x%08lx 0x%08lx ...\n",
           (unsigned long)d->extra_regs[4], (unsigned long)d->extra_regs[5]);
    printf("  stacked SP = 0x%08lx\n", (unsigned long)d->stacked_sp);
    printf("  backtrace (heuristic, %lu entries):\n",
           (unsigned long)d->backtrace_count);
    for (uint32_t i = 0u; i < d->backtrace_count; i++) {
        printf("    0x%08lx\n", (unsigned long)d->backtrace[i]);
    }
    printf("  -> arm-none-eabi-addr2line -f -e ex3.elf 0x%08lx\n",
           (unsigned long)d->frame.pc);
    printf("*** end of postmortem ***\n\n");
}

/* -------------------------------------------------------------------------
 * The deliberate fault, gated behind the no-init armed flag.
 * ----------------------------------------------------------------------- */

static void trigger_deliberate_fault(void) {
    /* A store to an unaligned, out-of-range address. The M0+ does not support
       unaligned access at all, so this is a precise HardFault: the stacked PC
       will point at this str, and R0 will hold the bad address (3), R1 the
       value (0xDEADBEEF). */
    volatile uint32_t *bad = (volatile uint32_t *)0x00000003u;
    *bad = 0xDEADBEEFu;
}

int main(void) {
    stdio_init_all();
    sleep_ms(2000);

    cc_dwt_enable();

    /* Install our HardFault handler the SDK-supported way. */
    exception_set_exclusive_handler(HARDFAULT_EXCEPTION, cc_hardfault_handler);

    printf("\n=== Exercise 3: fault dump ===\n");

    /* First, check whether the PREVIOUS boot left a crash dump. */
    if (cc_crash_dump_validate(&g_crash_dump) == CC_DBG_OK) {
        print_postmortem(&g_crash_dump);
        g_crash_dump.magic = 0u;   /* consume it so we do not reprint forever */
    } else {
        printf("No prior crash dump found (this is a clean boot).\n");
    }

    /* Decide whether to arm the deliberate fault. We arm it exactly once, on
       a cold boot where the armed-magic is not yet set. After the fault path
       runs, the handler clears g_fault_armed and sets the magic, so we never
       fault again until a true power cycle re-randomizes the no-init region. */
    if (g_fault_armed_magic != FAULT_ARMED_MAGIC) {
        g_fault_armed = 1u;
        g_fault_armed_magic = FAULT_ARMED_MAGIC;
    }

    if (g_fault_armed) {
        printf("Armed. Faulting in 3 seconds to demonstrate the dump path...\n");
        for (int i = 3; i > 0; i--) {
            printf("  %d...\n", i);
            sleep_ms(1000);
        }
        trigger_deliberate_fault();   /* HardFault -> handler -> reset -> dump */
        printf("(unreachable)\n");
    } else {
        printf("Fault already demonstrated this power cycle. Running clean.\n");
    }

    while (true) {
        printf("alive\n");
        sleep_ms(2000);
    }
    return 0;
}

/* End of exercise-03-fault-dump.c. */
