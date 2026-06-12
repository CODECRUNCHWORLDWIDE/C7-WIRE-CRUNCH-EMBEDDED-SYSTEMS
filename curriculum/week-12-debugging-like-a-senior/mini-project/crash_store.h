/*
 * crash_store.h — Mini-project crash-persistence API.
 *
 * Wraps the no-init SRAM dump and the watchdog-scratch minimal record from
 * crash_store.c, and the naked fault handler plus dump helpers from
 * fault_handler.c. Included by debug_app.c.
 *
 * The dump struct itself (cc_crash_dump_t) and the CRC/validate/finalize
 * prototypes are in ../exercises/debug_common.h. Here we add the persistence
 * layer and a "source" tag recording WHERE a recovered dump came from.
 */

#ifndef CC_CRASH_STORE_H_
#define CC_CRASH_STORE_H_

#include <stdint.h>

#include "../exercises/debug_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The `source` field on cc_crash_dump_t (defined in debug_common.h, appended
 * AFTER crc32 so it is never part of the CRC region) records which persistence
 * tier a recovered dump came from. The cc_crash_source_t enum is in
 * debug_common.h. The loader (crash_store.c) sets it; the fault handler never
 * touches it. */

/* Persistence API (crash_store.c). */
cc_crash_dump_t *cc_crash_store_get(void);
void            cc_crash_store_write_scratch(const cc_crash_dump_t *dump);
cc_dbg_result_t cc_crash_store_load(cc_crash_dump_t *out);
void            cc_crash_store_consume(void);

/* Fault-handler entry points (fault_handler.c). */
void cc_hardfault_handler(void);                  /* naked; register with the SDK */
void cc_fault_handler_c(uint32_t *frame_sp, uint32_t *saved_regs);
void cc_install_fault_handler(void);              /* exception_set_exclusive_handler */

/* Postmortem printer (fault_handler.c) — prints over RTT and/or stdio. */
void cc_print_postmortem(const cc_crash_dump_t *dump);

#ifdef __cplusplus
}
#endif

#endif /* CC_CRASH_STORE_H_ */
