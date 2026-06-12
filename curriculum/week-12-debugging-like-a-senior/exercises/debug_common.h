/*
 * debug_common.h — Shared declarations for Week 12 exercises and mini-project.
 *
 * Week 12 is about senior-level embedded debugging on the RP2040: SWD/JTAG,
 * GDB + OpenOCD, RTT-style logging, fault handlers and stack unwinding, the
 * DWT cycle counter, logic-analyzer bus decoding, heisenbug reproduction, and
 * postmortem from a captured core image. The constants and structs here mirror
 * the Cortex-M0+ exception model and the RP2040's DWT/SysTick registers.
 *
 * The RP2040 cores are Cortex-M0+ (ARMv6-M). The M0+ is a cut-down profile: no
 * MPU on the Pico's parts beyond the optional one, no FPU, no MemManage/BusFault
 * /UsageFault split — every fault funnels into HardFault. We treat that as a
 * feature this week: there is exactly one fault handler to write, and the
 * stacked frame format is identical across all of them.
 *
 * Citations:
 *   - RP2040 datasheet §2.4 (Cortex-M0+), pp. 16-18; §2.8 (Bootrom).
 *   - ARMv6-M Architecture Reference Manual (DDI0419), §B1.5 (Exceptions),
 *     §B3.2 (System Control Space).
 *   - ARM Cortex-M0+ Technical Reference Manual (DDI0484), DWT chapter.
 *   - SEGGER RTT manual, https://www.segger.com/products/debug-probes/j-link/
 *     technology/about-real-time-transfer/.
 */

#ifndef CC_DEBUG_COMMON_H_
#define CC_DEBUG_COMMON_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Cortex-M0+ exception numbers (ARMv6-M ARM §B1.5.2, Table B1-4).
 *
 * The M0+ has a fixed set of system exceptions plus 32 external IRQs. On a
 * fault, IPSR holds the exception number; reading it tells you which handler
 * is executing. NMI is 2, HardFault is 3; there is no separate MemManage(4),
 * BusFault(5), or UsageFault(6) on ARMv6-M — those are ARMv7-M and up, and the
 * conditions that would raise them on a bigger core all escalate to HardFault
 * here.
 * ----------------------------------------------------------------------- */

#define CC_EXC_THREAD              0u   /* Not in an exception. */
#define CC_EXC_RESET               1u
#define CC_EXC_NMI                 2u
#define CC_EXC_HARDFAULT           3u
#define CC_EXC_SVCALL              11u
#define CC_EXC_PENDSV              14u
#define CC_EXC_SYSTICK             15u
#define CC_EXC_IRQ0                16u  /* External IRQ 0 == exception 16. */

/* -------------------------------------------------------------------------
 * System Control Space register addresses (ARMv6-M ARM §B3.2).
 *
 * We hardcode these because the fault handler must not depend on the SDK's
 * CMSIS headers being linked correctly at the moment of the fault. If your
 * crash dumps stop matching CMSIS macros after an SDK bump, these literals are
 * the ground truth.
 * ----------------------------------------------------------------------- */

#define CC_SCS_BASE                0xE000E000u
#define CC_SCB_BASE                0xE000ED00u

#define CC_SCB_CPUID               (CC_SCB_BASE + 0x00u)
#define CC_SCB_ICSR                (CC_SCB_BASE + 0x04u)  /* Interrupt control/state. */
#define CC_SCB_VTOR                (CC_SCB_BASE + 0x08u)
#define CC_SCB_AIRCR               (CC_SCB_BASE + 0x0Cu)
#define CC_SCB_SHCSR               (CC_SCB_BASE + 0x24u)

/* ICSR fields we read in the fault handler. */
#define CC_ICSR_VECTACTIVE_MASK    0x000001FFu  /* Active exception number. */
#define CC_ICSR_VECTPENDING_MASK   0x001FF000u
#define CC_ICSR_VECTPENDING_SHIFT  12u

/* -------------------------------------------------------------------------
 * DWT (Data Watchpoint and Trace) registers.
 *
 * The Cortex-M0+ optionally implements DWT. On the RP2040 the DWT cycle
 * counter (CYCCNT) is present and is the cheapest precise timer we have: a
 * free-running 32-bit counter that increments once per CPU clock. At 125 MHz a
 * 32-bit counter wraps every ~34.4 seconds, which is plenty for instruction-
 * level timing. Enabling it requires setting TRCENA in DEMCR first.
 *
 * Note: the M0+ DWT does NOT implement DWT_LAR/DWT_LSR lock registers the way
 * some M7 parts do; you write CTRL directly. It also has no PC sampling and a
 * reduced comparator set. We use only CYCCNT this week.
 * ----------------------------------------------------------------------- */

#define CC_DWT_BASE                0xE0001000u
#define CC_DWT_CTRL                (CC_DWT_BASE + 0x00u)
#define CC_DWT_CYCCNT              (CC_DWT_BASE + 0x04u)

#define CC_DWT_CTRL_CYCCNTENA      0x00000001u

#define CC_CORE_DEBUG_BASE         0xE000EDF0u
#define CC_DEMCR                   (CC_CORE_DEBUG_BASE + 0x0Cu)
#define CC_DEMCR_TRCENA            0x01000000u

/* -------------------------------------------------------------------------
 * The exception stack frame the Cortex-M0+ pushes on entry.
 *
 * ARMv6-M ARM §B1.5.6: on exception entry the processor stacks eight 32-bit
 * words: R0, R1, R2, R3, R12, LR(R14), the return address (the PC of the
 * faulting instruction, or the next one), and xPSR. The frame is 8-word
 * aligned if bit 9 of the stacked xPSR is set (it usually is on the M0+).
 *
 * This is THE structure you cast the stacked SP to in the fault handler. Get
 * the field order wrong and your postmortem reports the wrong faulting PC,
 * which sends you debugging the wrong function for an hour.
 * ----------------------------------------------------------------------- */

typedef struct {
    uint32_t r0;
    uint32_t r1;
    uint32_t r2;
    uint32_t r3;
    uint32_t r12;
    uint32_t lr;      /* R14 — return address into the function that faulted. */
    uint32_t pc;      /* Return address — the faulting instruction (approx). */
    uint32_t xpsr;
} cc_exception_frame_t;

/* EXC_RETURN values (ARMv6-M ARM §B1.5.8). The low nibble tells us whether the
 * faulting code was using MSP or PSP, which is which-stack-to-unwind. */
#define CC_EXC_RETURN_MSP_MASK     0x00000004u  /* bit 2: 0 => MSP, 1 => PSP. */

/* -------------------------------------------------------------------------
 * Crash dump record. This is what we persist to a reserved SRAM region (or to
 * the watchdog scratch registers, or to a flash page) so the NEXT boot can
 * read it back and print a postmortem. The structure is deliberately small and
 * has a magic + CRC so a half-written record is detectable.
 * ----------------------------------------------------------------------- */

#define CC_CRASH_MAGIC             0xDEADF1A7u   /* "DEAD FIAT" — a dead frame. */
#define CC_CRASH_VERSION           1u

/* Which persistence tier a recovered dump came from (set by the loader). */
typedef enum {
    CC_CRASH_SOURCE_NONE    = 0,
    CC_CRASH_SOURCE_SRAM    = 1,
    CC_CRASH_SOURCE_SCRATCH = 2,
} cc_crash_source_t;

typedef struct {
    uint32_t magic;                 /* CC_CRASH_MAGIC if valid. */
    uint32_t version;               /* CC_CRASH_VERSION. */
    uint32_t exc_number;            /* IPSR VECTACTIVE at fault. */
    cc_exception_frame_t frame;     /* The 8-word stacked frame. */
    uint32_t stacked_sp;            /* SP value that pointed at the frame. */
    uint32_t extra_regs[6];         /* R4-R7, plus SP and a guard word. */
    uint32_t backtrace[8];          /* Heuristic stack-scan return addresses. */
    uint32_t backtrace_count;
    uint32_t crc32;                 /* Over all fields ABOVE this one only. */
    /* Fields below crc32 are NOT covered by the CRC and are filled in by the
       loader on a clean boot (never by the fault handler). `source` records
       which persistence tier a recovered dump came from. */
    uint32_t source;                /* cc_crash_source_t (0 if unset). */
} cc_crash_dump_t;

/* -------------------------------------------------------------------------
 * RTT-style logging control block (SEGGER RTT layout, simplified).
 *
 * SEGGER RTT works by placing a known-magic control block in SRAM. The debug
 * probe, over SWD, periodically reads that SRAM region while the target runs
 * — no halt required. The target writes log bytes into a ring buffer; the host
 * drains the ring by following the read/write indices in the control block.
 * The magic string "SEGGER RTT" is what the host's RTT viewer scans SRAM for.
 *
 * We implement a single up-channel (target->host). The real SEGGER block
 * supports multiple up and down channels; one up-channel is enough for printf.
 * ----------------------------------------------------------------------- */

#define CC_RTT_MAGIC_STR           "SEGGER RTT"     /* 10 chars + NUL + pad. */
#define CC_RTT_UP_BUFFER_SIZE      1024u

typedef struct {
    const char *name;
    uint8_t    *buffer;             /* Ring buffer storage. */
    uint32_t    size_bytes;
    volatile uint32_t write_offset; /* Written by target. */
    volatile uint32_t read_offset;  /* Written by host probe. */
    uint32_t    flags;              /* 0 = skip if full, 1 = block if full. */
} cc_rtt_ring_t;

typedef struct {
    char         id[16];            /* "SEGGER RTT" + pad. Scanned by the host. */
    int32_t      max_up_channels;
    int32_t      max_down_channels;
    cc_rtt_ring_t up[1];            /* One up-channel. */
    /* Real SEGGER blocks append down-channels here; we omit them. */
} cc_rtt_control_block_t;

#define CC_RTT_FLAG_SKIP           0u
#define CC_RTT_FLAG_BLOCK          1u

/* -------------------------------------------------------------------------
 * Result codes for the exercises' helper APIs.
 * ----------------------------------------------------------------------- */

typedef enum {
    CC_DBG_OK                  = 0,
    CC_DBG_ERR_NO_DUMP         = 1,
    CC_DBG_ERR_BAD_MAGIC       = 2,
    CC_DBG_ERR_BAD_CRC         = 3,
    CC_DBG_ERR_BAD_VERSION     = 4,
    CC_DBG_ERR_RING_FULL       = 5,
} cc_dbg_result_t;

/* -------------------------------------------------------------------------
 * Small inline helpers shared by exercises.
 * ----------------------------------------------------------------------- */

/* Memory-mapped 32-bit register access. volatile so the compiler never
 * caches or reorders these across a fault. */
static inline uint32_t cc_reg_read(uint32_t addr) {
    return *(volatile uint32_t *)addr;
}

static inline void cc_reg_write(uint32_t addr, uint32_t value) {
    *(volatile uint32_t *)addr = value;
}

/* Read the DWT cycle counter. Caller must have enabled it via cc_dwt_enable().
 * On the M0+ this is a single LDR; the overhead of the call itself is a handful
 * of cycles, which you subtract out when timing tight regions. */
static inline uint32_t cc_dwt_cyccnt(void) {
    return cc_reg_read(CC_DWT_CYCCNT);
}

static inline void cc_dwt_enable(void) {
    cc_reg_write(CC_DEMCR, cc_reg_read(CC_DEMCR) | CC_DEMCR_TRCENA);
    cc_reg_write(CC_DWT_CYCCNT, 0u);
    cc_reg_write(CC_DWT_CTRL, cc_reg_read(CC_DWT_CTRL) | CC_DWT_CTRL_CYCCNTENA);
}

/* CRC32 (IEEE 802.3, reflected, init 0xFFFFFFFF, final XOR 0xFFFFFFFF) — the
 * same parameterization zlib uses. We carry our own table-free bit-banged
 * version so the crash-dump validator does not depend on any library being
 * intact at fault time. Declared here, defined in exercise-03. */
uint32_t cc_crc32(const void *data, size_t length);

/* RTT API, implemented in exercise-02. */
void   cc_rtt_init(void);
size_t cc_rtt_write(const void *data, size_t length);
size_t cc_rtt_write_str(const char *s);

/* Crash-dump API, implemented in exercise-03 and the mini-project. */
cc_dbg_result_t cc_crash_dump_validate(const cc_crash_dump_t *dump);
void            cc_crash_dump_finalize(cc_crash_dump_t *dump);

#ifdef __cplusplus
}
#endif

#endif /* CC_DEBUG_COMMON_H_ */
