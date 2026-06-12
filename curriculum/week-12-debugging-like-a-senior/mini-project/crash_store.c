/*
 * crash_store.c — Mini-project: persist a crash dump across reset.
 *
 * Two persistence tiers (Lecture 2 §7):
 *   1. A no-init SRAM region (.noinit, NOLOAD) holding the FULL cc_crash_dump_t.
 *      Survives a warm (watchdog/software) reset; lost on power-off.
 *   2. The watchdog scratch registers (WATCHDOG_SCRATCH0..7) holding a MINIMAL
 *      record (magic, exc, PC, LR). Survives any reset short of power-off, and
 *      is the fallback if the SRAM region was corrupted by the fault.
 *
 * On the next clean boot, cc_crash_store_load() prefers the validated SRAM
 * dump; if that is gone but the scratch record is valid, it reconstructs a
 * minimal dump from the scratch registers.
 *
 * The CRC32 / validate / finalize helpers live in fault_handler.c (so the
 * naked-handler translation unit owns the at-fault-time-safe code path).
 *
 * Citations:
 *   - RP2040 datasheet §4.7 (Watchdog scratch registers), pp. 559-570.
 *   - ARMv6-M ARM §B1.5 (exception model).
 *
 * Build: see CMakeLists.txt; requires the .noinit linker section.
 */

#include <stdint.h>
#include <string.h>

#include "hardware/structs/watchdog.h"

#include "../exercises/debug_common.h"
#include "crash_store.h"

/* -------------------------------------------------------------------------
 * No-init storage. The linker script places .noinit in a NOLOAD region the
 * C runtime does not clear, so it survives a warm reset.
 * ----------------------------------------------------------------------- */

__attribute__((section(".noinit"), used))
static cc_crash_dump_t g_crash_dump;

cc_crash_dump_t *cc_crash_store_get(void) {
    return &g_crash_dump;
}

/* -------------------------------------------------------------------------
 * Watchdog scratch layout for the minimal record. We use four of the eight
 * 32-bit scratch words. SCRATCH0..3 are sometimes used by the SDK/Boot ROM
 * during reboot signalling; we use SCRATCH4..7, which the application owns.
 *
 *   SCRATCH4 : magic (CC_CRASH_MAGIC)
 *   SCRATCH5 : exception number
 *   SCRATCH6 : faulting PC
 *   SCRATCH7 : faulting LR
 *
 * No CRC fits, so the magic alone gates validity. A spurious match is
 * improbable (a 32-bit magic), and the SRAM dump is the authoritative source
 * when both are present.
 * ----------------------------------------------------------------------- */

#define SCRATCH_MAGIC  4
#define SCRATCH_EXC    5
#define SCRATCH_PC     6
#define SCRATCH_LR     7

void cc_crash_store_write_scratch(const cc_crash_dump_t *dump) {
    watchdog_hw->scratch[SCRATCH_MAGIC] = CC_CRASH_MAGIC;
    watchdog_hw->scratch[SCRATCH_EXC]   = dump->exc_number;
    watchdog_hw->scratch[SCRATCH_PC]    = dump->frame.pc;
    watchdog_hw->scratch[SCRATCH_LR]    = dump->frame.lr;
}

static void cc_crash_store_clear_scratch(void) {
    watchdog_hw->scratch[SCRATCH_MAGIC] = 0u;
}

/* -------------------------------------------------------------------------
 * Load the best available record on the next boot.
 *
 * Order of preference:
 *   1. The full SRAM dump, if its magic+CRC validate.
 *   2. A minimal dump reconstructed from the scratch registers.
 *   3. Nothing (clean boot).
 *
 * Returns CC_DBG_OK and fills *out if a record was found; otherwise an error.
 * The caller (the app's early-boot path) prints the postmortem and then calls
 * cc_crash_store_consume() so the record is not reprinted on later boots.
 * ----------------------------------------------------------------------- */

cc_dbg_result_t cc_crash_store_load(cc_crash_dump_t *out) {
    cc_crash_dump_t *sram = cc_crash_store_get();

    if (cc_crash_dump_validate(sram) == CC_DBG_OK) {
        *out = *sram;
        out->source = CC_CRASH_SOURCE_SRAM;
        return CC_DBG_OK;
    }

    /* SRAM dump missing or corrupt — try the scratch record. */
    if (watchdog_hw->scratch[SCRATCH_MAGIC] == CC_CRASH_MAGIC) {
        memset(out, 0, sizeof(*out));
        out->magic       = CC_CRASH_MAGIC;
        out->version     = CC_CRASH_VERSION;
        out->exc_number  = watchdog_hw->scratch[SCRATCH_EXC];
        out->frame.pc    = watchdog_hw->scratch[SCRATCH_PC];
        out->frame.lr    = watchdog_hw->scratch[SCRATCH_LR];
        out->source      = CC_CRASH_SOURCE_SCRATCH;
        out->backtrace_count = 0u;
        return CC_DBG_OK;
    }

    return CC_DBG_ERR_NO_DUMP;
}

void cc_crash_store_consume(void) {
    cc_crash_store_get()->magic = 0u;
    cc_crash_store_clear_scratch();
}

/* End of crash_store.c. */
