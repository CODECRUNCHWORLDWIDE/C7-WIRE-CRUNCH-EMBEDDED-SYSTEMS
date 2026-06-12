# Lecture 2 — Fault Handlers and Stack Unwinding on the Cortex-M0+

> *When a desktop program dereferences a null pointer, the kernel catches the fault, writes a core file, and prints a stack trace. When firmware dereferences a null pointer, the chip raises a HardFault, the default handler spins in a `while(1)`, and the device sits there dark and silent with no operating system to write a core file and no terminal to print to. The difference between a junior and a senior at that moment is whether the firmware wrote its own core file. This lecture is about writing that core file: the eight-word frame the Cortex-M0+ stacks on every exception, the registers it does NOT stack that your handler must capture itself, how to recover the faulting instruction's address, how to reconstruct a backtrace with no frame pointers, and how to persist the whole record across the reset so the next boot can print a postmortem you can actually read.*

## 1. The M0+ fault model — one fault to rule them all

The Cortex-M family splits faults differently across architecture versions, and the RP2040's **Cortex-M0+ is ARMv6-M**, the simplest:

- On **ARMv7-M** (Cortex-M3/M4/M7), faults are classified: `MemManage` (exc 4, MPU violations), `BusFault` (exc 5, bad memory access), `UsageFault` (exc 6, undefined instruction, unaligned access, divide-by-zero). Each has a status register — the **Configurable Fault Status Register** `CFSR` at `0xE000ED28`, plus `MMFAR`/`BFAR` fault-address registers — that tells you *exactly* what went wrong and at what address.
- On **ARMv6-M** (Cortex-M0/M0+), there is **only `HardFault`** (exc 3). There is **no `CFSR`, no `MMFAR`, no `BFAR`.** Every illegal load, every unaligned access, every branch to a non-thumb address (the M0+ is thumb-only; a PC with bit 0 clear faults), every `SVC` from a too-high priority, every escalated fault — all of it lands in one `HardFault_Handler`.

This is the single most important platform fact of the week (RP2040 datasheet §2.4, pp. 16–18). It cuts both ways. The bad: you cannot ask the chip "was it a bus fault or a usage fault, and at what address" — there is no register that answers. The good: there is exactly **one** handler to write, **one** stacked-frame layout to memorize, and **one** unwinding strategy. We embrace the simplicity.

What raises a HardFault on the M0+, in practice:

- **Unaligned access.** The M0+ does not support unaligned loads/stores at all (unlike the M3/M4, which can be configured to). A `ldr`/`str` to an address not 4-aligned (for a word) faults. This is the most common "I dereferenced a slightly-wrong pointer" fault.
- **Access to an unmapped or disabled region.** Reading `0x00004000`–`0x0FFFFFFF` (the reserved hole), or a peripheral whose clock is gated off.
- **Execution of a non-thumb address.** Branching to an even address (thumb bit clear) — the classic "function pointer was zeroed" or "the vector table entry is wrong" bug.
- **Stack overflow into a peripheral region.** The stack grows down from the top of SRAM; overflow past `0x20000000` into nothing, or a wild SP into peripheral space, faults on the next push.
- **Undefined instruction.** Executing data as code — a corrupted return address that lands in `.rodata`.

## 2. What the hardware stacks on exception entry

When any exception (including HardFault) is taken, the M0+ automatically pushes **eight 32-bit words** onto the current stack, in this exact order (lowest address first), per ARMv6-M ARM §B1.5.6:

```text
SP+0x00 : R0
SP+0x04 : R1
SP+0x08 : R2
SP+0x0C : R3
SP+0x10 : R12
SP+0x14 : LR    (R14 — the return address INTO the function that was running)
SP+0x18 : PC    (the return address — the faulting instruction, see below)
SP+0x1C : xPSR  (program status; low 9 bits = IPSR = the exception number)
```

This maps directly to the struct in `debug_common.h`:

```c
typedef struct {
    uint32_t r0;
    uint32_t r1;
    uint32_t r2;
    uint32_t r3;
    uint32_t r12;
    uint32_t lr;
    uint32_t pc;
    uint32_t xpsr;
} cc_exception_frame_t;
```

Two subtleties that bite everyone the first time:

**The stacked PC is (approximately) the faulting instruction.** For a *precise* fault — and on the M0+ almost all faults are precise — `frame.pc` is the address of the instruction that caused the fault, or sometimes the next one, depending on the operation. For a load/store fault it is the faulting `ldr`/`str`. Feed it to `addr2line` and you get the source line. Do *not* assume it is exactly right to the byte; assume it is the right *instruction* or one adjacent, which is enough to find the line.

**Eight-word alignment padding.** The M0+ may insert a 4-byte pad before the frame to keep the stack 8-byte aligned, recording that it did so in bit 9 of the stacked `xPSR`. If you ever compute the pre-fault SP by adding `0x20` to the stacked SP, account for that pad: `pre_fault_sp = stacked_sp + 0x20 + ((stacked_xpsr & (1 << 9)) ? 4 : 0)`. For reading the frame itself you do not need this; for walking further up the stack you do.

## 3. The registers the hardware does NOT stack

Here is the trap. The hardware stacks `R0–R3`, `R12`, `LR`, `PC`, `xPSR` — the **caller-saved** registers. It does **not** stack `R4–R11`, the **callee-saved** registers. At the moment your handler runs, `R4–R11` still hold whatever the faulting code left in them (because no function-call boundary has clobbered them yet — the exception entry is not a normal call). If your core dump wants a full register set — and it should, because `R4–R11` often hold the loop counters, the buffer pointers, the `this` pointer of a C++ object — **the handler must capture them itself, before any C code runs and reuses them.**

That is why the handler is written `naked` (no compiler-generated prologue/epilogue) and starts in assembly: capture `R4–R11` while they are still pristine, figure out which stack pointer was active, and hand both to a C function.

## 4. Which stack? MSP vs PSP, and EXC_RETURN

On exception entry the M0+ loads `LR` with a special **`EXC_RETURN`** value whose low bits encode the pre-exception state (ARMv6-M ARM §B1.5.8):

| `EXC_RETURN` | Pre-exception mode | Stack used |
|--------------|--------------------|------------|
| `0xFFFFFFF1` | Handler            | MSP        |
| `0xFFFFFFF9` | Thread             | MSP        |
| `0xFFFFFFFD` | Thread             | PSP        |

Bit 2 (mask `0x4`) is the discriminator: **clear → the frame is on MSP; set → the frame is on PSP.** In a bare-metal program with no RTOS, everything runs on MSP and the answer is always MSP. Under FreeRTOS (Week 6), tasks run on PSP and the kernel on MSP, so a fault in a task stacks onto *that task's* PSP and you must read `EXC_RETURN` to know which pointer points at the frame. Getting this wrong reads garbage and reports a fault in the wrong function — a classic time-waster.

The assembly preamble does this selection. The canonical M0+ idiom (the M0+ cannot conditionally move from MSP/PSP with a single `it`-predicated instruction the way the M3 can, so it branches):

```c
__attribute__((naked)) void isr_hardfault(void) {
    __asm volatile(
        "movs r0, #4            \n"  /* test EXC_RETURN bit 2 */
        "mov  r1, lr            \n"
        "tst  r0, r1            \n"
        "beq  1f                \n"  /* bit clear -> MSP */
        "mrs  r0, psp           \n"  /* bit set   -> PSP */
        "b    2f                \n"
        "1:                     \n"
        "mrs  r0, msp           \n"
        "2:                     \n"
        /* r0 now = the SP that points at the 8-word frame. */
        /* Capture R4-R7 (M0+ can only push low regs directly). */
        "push {r4, r5, r6, r7}  \n"
        "mov  r4, r8            \n"
        "mov  r5, r9            \n"
        "mov  r6, r10           \n"
        "mov  r7, r11           \n"
        "push {r4, r5, r6, r7}  \n"
        "mov  r1, sp            \n"  /* r1 -> our saved R8-R11,R4-R7 block */
        "ldr  r2, =cc_fault_handler_c \n"
        "bx   r2                \n"  /* call C handler: (frame_sp, regs_block) */
        : : : "memory"
    );
}
```

Note the M0+ restriction: `push`/`pop` can only directly use the low registers `R0–R7`; to save `R8–R11` you first `mov` them into low registers and then push. That dance is why the assembly is longer than on an M3. The C handler receives a pointer to the stacked frame (`r0`) and a pointer to the saved high registers (`r1`), and from there everything is C.

## 5. The C side: assembling the dump

The C handler casts the frame pointer and fills in a `cc_crash_dump_t`:

```c
void cc_fault_handler_c(uint32_t *frame_sp, uint32_t *saved_regs) {
    cc_crash_dump_t *dump = cc_crash_store_get();   /* no-init SRAM region */

    dump->magic   = CC_CRASH_MAGIC;
    dump->version = CC_CRASH_VERSION;

    /* IPSR (low 9 bits of xPSR) is the active exception number. We are in
       the HardFault handler, so this reads 3 — but reading it from the
       stacked frame would give the PRE-fault exception (0 for thread, or
       an IRQ number if we faulted inside an ISR), which is the more useful
       datum: "we faulted while servicing IRQ N". */
    cc_exception_frame_t *f = (cc_exception_frame_t *)frame_sp;
    dump->frame      = *f;
    dump->stacked_sp = (uint32_t)frame_sp;
    dump->exc_number = f->xpsr & CC_ICSR_VECTACTIVE_MASK;

    /* saved_regs points at [R8,R9,R10,R11, R4,R5,R6,R7] from the preamble. */
    for (int i = 0; i < 6; i++) {
        dump->extra_regs[i] = saved_regs[i];
    }

    /* Heuristic backtrace: scan the stack above the frame for words that
       look like return addresses. (Section 6.) */
    dump->backtrace_count =
        cc_backtrace_scan(frame_sp, dump->backtrace, 8);

    cc_crash_dump_finalize(dump);   /* compute and store CRC32 */

    /* Now we can do something visible: blink an SOS, drop to USB-MSC for
       recovery, or just reset and let the next boot print the dump. */
    cc_crash_store_commit();
    watchdog_reboot(0, 0, 0);       /* reset; the dump survives in no-init SRAM */
    for (;;) { }
}
```

The handler does the minimum at fault time and defers the *printing* to the next boot. That is deliberate: at the moment of a fault the system is in an unknown state — the heap may be corrupt, a peripheral may be wedged, `printf`'s UART may be mid-transmission. Doing as little as possible (fill a struct, CRC it, reset) and printing from a *clean* boot is far more robust than trying to `printf` a backtrace from inside the wreckage.

## 6. Backtrace with no frame pointers

A full call stack needs to walk frames. On x86 with frame pointers you follow the `RBP` chain. Embedded ARM, compiled `-O2`, has **no frame pointers** — the compiler uses every register for data. So we cannot do a precise unwind without DWARF unwind tables (which GDB has, off-target, but the on-chip handler does not). What the on-chip handler *can* do is a **heuristic stack scan**:

```c
size_t cc_backtrace_scan(const uint32_t *sp, uint32_t *out, size_t max) {
    /* Walk up the stack from the fault frame toward the top of SRAM.
       Any word that (a) lies in the .text/flash XIP range and (b) has the
       thumb bit set is PROBABLY a return address that some BL pushed.
       This over-reports (constants that look like addresses) but a human
       reading the postmortem filters those out, and addr2line on each
       candidate quickly shows which are real. */
    extern uint32_t __flash_binary_start, __flash_binary_end;
    uint32_t lo = (uint32_t)&__flash_binary_start;
    uint32_t hi = (uint32_t)&__flash_binary_end;

    const uint32_t *top = (const uint32_t *)0x20042000u;  /* top of SRAM */
    size_t n = 0;
    for (const uint32_t *p = sp; p < top && n < max; p++) {
        uint32_t w = *p;
        if (w >= lo && w < hi && (w & 1u)) {
            out[n++] = w & ~1u;   /* clear thumb bit for addr2line */
        }
    }
    return n;
}
```

This is exactly what the Memfault Interrupt blog calls a "lazy stack trace" — imperfect but invaluable. The over-reporting (a `const` that happens to look like a thumb code address) is filtered by the human: run `arm-none-eabi-addr2line -e firmware.elf 0x1000xxxx` on each candidate; the real return addresses resolve to plausible call sites, the false positives resolve to nonsense or to data sections. Off-target, GDB does a *precise* unwind using the ELF's DWARF `.debug_frame`, so when you have the probe attached you get a real `bt`; the heuristic scan is for the case where all you have is the persisted dump and no live target.

## 7. Persisting the dump across reset

The dump is useless if the reset wipes it. Three persistence mechanisms, in increasing durability and cost:

**(1) No-init SRAM.** Declare a region the C runtime's startup does not clear. The Pico SDK clears `.bss` on boot; a section marked `NOLOAD` and excluded from that clear survives a *warm* reset (a watchdog reboot, a `SCB->AIRCR` system reset) because SRAM keeps its contents through a warm reset — only the CPU state is reset, not the RAM bits. It does **not** survive a power cycle. Setup: a linker section plus an attribute.

```c
/* In the linker script, after .bss:
   .noinit (NOLOAD) : { KEEP(*(.noinit)) } > RAM   */
__attribute__((section(".noinit"), used))
static cc_crash_dump_t g_crash_dump;
```

The magic + CRC make a stale or garbage region detectable: on the next boot, validate before trusting.

**(2) Watchdog scratch registers.** The RP2040 has eight 32-bit `WATCHDOG_SCRATCH` registers (datasheet §4.7) that survive any reset short of power-off — including the ones the Boot ROM itself uses for `picotool reboot`. Eight words is enough for a *minimal* dump: magic, exception number, faulting PC, faulting LR, and a CRC. When SRAM no-init is not available (e.g., the fault corrupted the RAM region), the scratch registers are the fallback. The crash-store in the mini-project writes both.

**(3) A flash page.** Survives power-off, but writing flash in a fault handler is risky — the chip may be in a state where the XIP-disable dance (Week 10) is unsafe, and an erase takes ~45 ms during which an undervoltage could brick. We do **not** write flash from the fault handler; if power-off survival matters, the next clean boot reads the no-init/scratch dump and *then* archives it to flash from a known-good state.

The next-boot read is the mirror:

```c
/* Very early in main(), before anything else that touches the dump region. */
void cc_check_for_crash(void) {
    cc_crash_dump_t *dump = cc_crash_store_get();
    if (cc_crash_dump_validate(dump) == CC_DBG_OK) {
        cc_print_postmortem(dump);   /* now stdio/RTT is up; print it */
        dump->magic = 0;             /* consume it so we don't reprint */
    }
}
```

## 8. Reading a postmortem

The postmortem print (over RTT or UART, now that we are on a clean boot) looks like:

```text
*** CRASH DETECTED (from previous boot) ***
  exception : 3 (HardFault)   faulted while servicing: thread mode
  PC  = 0x10002a4e   LR  = 0x10002a31   xPSR = 0x61000000
  R0  = 0x00000001   R1  = 0xdeadbeef   R2  = 0x20001a40   R3  = 0x00000000
  R12 = 0x00000000
  R4  = 0x20002000   R5  = 0x00000010   R6  = 0x10004120   R7  = 0x20001a40
  R8..R11 = 0x00000000 0x00000000 0x00000000 0x00000000
  stacked SP = 0x20041f60
  backtrace (heuristic):
    0x10002a4e
    0x100012c8
    0x10000404
```

Then, off-target with the matching ELF:

```sh
$ arm-none-eabi-addr2line -f -e build/firmware.elf 0x10002a4e 0x100012c8 0x10000404
sensor_read
src/sensor.c:88
process_sample
src/app.c:142
main
src/main.c:31
```

There it is: the fault is at `sensor.c:88` inside `sensor_read`, called from `process_sample` at `app.c:142`, from `main`. `R1 = 0xdeadbeef` (the value being written) plus `R0 = 1` (an unaligned destination address) tells the rest of the story — a store to address 1, the unaligned-write fault from Week 10's bricking challenge, here read out of a persisted core dump instead of caught live. That is the Unix `gdb program core` workflow, transplanted to a chip with no OS.

## 8a. Precise vs imprecise faults

A *precise* fault is one where the stacked PC points at (or immediately after) the instruction that caused it. An *imprecise* fault is one where the offending instruction has already retired and the PC has moved on, because a write was buffered and the bus error surfaced cycles later. On the big Cortex-M cores the distinction matters a lot — an imprecise BusFault sets the `IMPRECISERR` bit in `BFSR` and the stacked PC is useless for finding the store. On the **M0+ it barely matters**: the M0+'s memory interface is simple, it has no store buffer of the kind that produces imprecise faults on a write, and in practice essentially every HardFault you will see has a precise stacked PC. That is a gift — it means `addr2line` on the stacked PC reliably names the faulting line, which is the foundation of the whole postmortem workflow. (If you ever port this handler to an M4 or M7, revisit this: there you must check the imprecise bit and, if set, force a `DSB` early in the fault path or accept that the PC is approximate.)

One M0+ caveat that *does* bite: for a faulting **load**, the stacked PC is the load instruction; for some faulting **branches** (jumping to a non-thumb address), the stacked PC may be the branch *target* (the bad address itself, which is why you sometimes see a stacked PC of `0x00000000` — the firmware branched to a zeroed function pointer and the fault is reported at address 0). Either way the dump tells the story: a stacked PC of 0 plus a stacked LR pointing at the call site is the unmistakable signature of "called a NULL function pointer," which is exactly Exercise 1's dispatch trap and one of the mini-project's five bugs.

## 8b. Faults under FreeRTOS — reading the right stack

Everything above assumed MSP. Under FreeRTOS (Week 6), the kernel runs on MSP and each task runs on its own PSP-based stack. When a task faults, the eight-word frame is stacked on **that task's PSP**, and the `EXC_RETURN` in `LR` will be `0xFFFFFFFD` (thread + PSP). The handler's MSP/PSP selection (Section 4) handles this — but the *backtrace* now needs the task's stack bounds, not the main stack's, and the postmortem is more useful if it also records *which task* faulted. The standard trick: read FreeRTOS's `pxCurrentTCB` (the current task control block) in the C handler and copy the task name into the dump. Then the postmortem reads "HardFault in task `mqtt_worker`, at `tls.c:204`" — which, in a multi-task system, is the difference between a five-minute fix and an afternoon. We do not run the RTOS in this week's mini-project (it is bare-metal, all MSP), but the exam may ask you to reason about the PSP case, and the answer is always: select the stack from `EXC_RETURN`, then unwind *that* stack within *that* task's bounds.

## 8c. Reading the ELF with objdump and nm

The on-chip dump gives you addresses; the ELF turns them into meaning. Three binutils commands you will run constantly:

- `arm-none-eabi-addr2line -f -e firmware.elf 0x10002a4e` — the function and source line for a PC. The `-f` adds the function name. Run it on the faulting PC and on every backtrace candidate.
- `arm-none-eabi-nm -n firmware.elf` — every symbol, sorted by address. When `addr2line` says `??` (an address in a region with no line info, like a library or a vector), `nm -n` plus a manual "which symbol's range contains this address" tells you the function.
- `arm-none-eabi-objdump -d firmware.elf` — full disassembly. Find the faulting PC in the disassembly and you see the exact `ldr`/`str`/`bl` that faulted, with the registers it used — which, cross-referenced against the stacked register values, completes the story ("`str r1, [r0]` where `r0 = 1` and `r1 = 0xdeadbeef`: an unaligned store of `0xdeadbeef` to address 1").

The discipline that makes all of this work: **archive the exact ELF with every build that ships.** An address from a field unit only maps to a line against the ELF that was actually running; a rebuild shifts every address. Tag each release, keep its `.elf` (and the `.map` from `-Wl,-Map`), and a crash dump from the field becomes a five-minute `addr2line` instead of an unsolvable mystery.

## 9. The SDK-supported way to install the handler

The Pico SDK installs a default HardFault handler. To override it cleanly, the SDK gives you `exception_set_exclusive_handler(HARDFAULT_EXCEPTION, my_handler)` (in `hardware_exception`), which patches the RAM vector table the SDK maintains. That is the supported path and what we use; it avoids fighting the SDK's own vector-table setup. (You *can* also override the weak `isr_hardfault` symbol the SDK declares, but `exception_set_exclusive_handler` is cleaner and survives SDK changes.) The handler you register must still be `naked` and do the assembly preamble — the SDK only routes the vector; the register-capture discipline is yours.

## 9a. Detecting stack overflow before it faults

A stack overflow is one of the nastiest memory bugs because it does not always fault at the overflow — it scribbles whatever lives below the stack (other variables, the heap, peripheral shadow regions) and the corruption surfaces *elsewhere*, long after and far away. The fault, when it comes, points at an innocent victim, not the overflowing function. Two techniques catch it closer to the source.

**Stack painting.** Before `main`, fill the entire stack region with a known pattern — the classic value is `0xDEADBEEF` or `0xCAFEBABE`. As the program runs, the stack high-water mark is the lowest address whose pattern got overwritten; everything still showing the pattern was never used. A periodic check (or a one-shot at the end of a stress run) that scans up from the stack bottom counting intact pattern words tells you the *maximum* stack depth the program ever reached:

```c
extern uint32_t __StackLimit;   /* SDK linker symbol: bottom of the stack */
extern uint32_t __StackTop;

#define STACK_PAINT 0xDEADBEEFu

void paint_stack(void) {
    /* Call this VERY early, before deep calls — paint from the limit up to
       just below the current SP. */
    uint32_t sp; __asm volatile("mov %0, sp" : "=r"(sp));
    for (uint32_t *p = &__StackLimit; (uint32_t)p < sp; p++) {
        *p = STACK_PAINT;
    }
}

uint32_t stack_high_water_bytes(void) {
    uint32_t *p = &__StackLimit;
    while (p < &__StackTop && *p == STACK_PAINT) p++;   /* skip untouched */
    return (uint32_t)((uint8_t *)&__StackTop - (uint8_t *)p);
}
```

If `stack_high_water_bytes()` creeps toward your allocated stack size across a long run, you are about to overflow — and you caught it *before* the fault. This is the standard technique FreeRTOS uses (`uxTaskGetStackHighWaterMark`) and it is invaluable: a stack that is 90% used is a latent field crash waiting for the one deeper code path.

**The overflow signature in a dump.** When a stack overflow *does* fault, the tell-tale in the core dump is the **stacked SP value out of range** — at or below the SRAM floor (`0x20000000`), or pointing into a peripheral region. A normal stacked SP is somewhere in the high SRAM where the stack lives (near `0x20042000` on the Pico); an SP of `0x1FFFFFE0` or a wild low value screams "the stack grew past its bottom and the hardware tried to stack the exception frame onto invalid memory, which itself faulted." The mini-project's BUG_STACK_OVERFLOW produces exactly this, and Homework 6 has you read it: the distinguishing evidence is the SP, not the PC.

## 9b. What the handler should NOT do

The temptation, at fault time, is to do *more* — print a full backtrace, blink an elaborate error code, attempt to recover the failed subsystem. Resist it. At the moment of a fault the system is in an unknown, possibly corrupt state: the heap may be inconsistent, a lock may be held, a peripheral may be mid-transaction, the very stack you are running on may be the one that overflowed. Every additional thing the handler does is a chance to fault *again* (a double fault, which on the M0+ escalates to a lockup the watchdog must rescue) or to hang. The discipline: the handler does the **minimum** that captures the evidence — fill a struct, CRC it, copy four words to scratch, reset — and defers everything fallible (printing, decoding, recovery decisions) to the next clean boot where the system state is known-good. A handler that tries to `printf` a backtrace from inside the wreckage is a handler that frequently hangs before it finishes, leaving you with no dump at all. Small, fast, paranoid: the same three adjectives as a bootloader, for the same reason.

One concession that *is* safe and worth it: a watchdog. Before doing anything else, the handler can pet/reconfigure the watchdog so that if the handler itself hangs (a double fault, a wild jump), the watchdog still resets the chip after its timeout instead of leaving a dark brick. The reset path then reads the dump (if the handler got far enough to write it) or, failing that, at least returns the device to a running state. Belt and suspenders.

A related discipline: keep the fault handler's own code and the crash-store in regions that cannot themselves be the corruption. The handler is `.text` in flash (read-only XIP, hard to corrupt), the dump is in a reserved no-init SRAM region away from the heap and the main stack, and the CRC primitive is table-free so it has no `.data` dependency that a wild write could have scribbled. The whole point is that the evidence-capture path is the *most* robust code in the firmware — because it runs precisely when everything else has gone wrong. Design it that way deliberately; do not let the handler call into the same subsystems that might have caused the fault.

## 10. Summary

The M0+ has one fault, `HardFault`, and no `CFSR`/`MMFAR`/`BFAR` — embrace the simplicity. On exception entry the hardware stacks eight words (`R0–R3`, `R12`, `LR`, `PC`, `xPSR`) but **not** `R4–R11`, so a `naked` handler captures those itself in assembly, selects MSP vs PSP from `EXC_RETURN` bit 2, and hands a frame pointer plus the saved high registers to C. The C side fills a CRC-protected `cc_crash_dump_t`, does a heuristic stack-scan backtrace (words in the `.text` range with the thumb bit set), persists it to no-init SRAM and the watchdog scratch registers, and resets. The next clean boot validates and prints the postmortem, which `addr2line` turns into source lines — your own core dump, your own `gdb -c core`, on a chip with no OS. Next lecture: RTT for non-perturbing logging, the DWT cycle counter for timing the bugs you cannot print, the logic analyzer for the truth on the wire, and the heisenbug discipline that ties it all together.

## References for this lecture

- ARMv6-M Architecture Reference Manual §B1.5 "Exceptions", DDI0419: §B1.5.2 (exception numbers), §B1.5.6 (stacked frame), §B1.5.8 (EXC_RETURN). <https://developer.arm.com/documentation/ddi0419/latest/>
- RP2040 datasheet §2.4 "Cortex-M0+ Processor", pp. 16–18 (the no-CFSR fault model); §4.7 "Watchdog", pp. 559–570 (scratch registers). <https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf>
- Memfault Interrupt, "How to debug a HardFault on an ARM Cortex-M MCU". <https://interrupt.memfault.com/blog/cortex-m-hardfault-debug>
- Memfault Interrupt, coredump series. <https://interrupt.memfault.com/blog/a-deep-dive-into-coredumps-with-gdb>
- Pico SDK `hardware_exception` (`exception_set_exclusive_handler`) and `pico_runtime`. <https://github.com/raspberrypi/pico-sdk>
- `arm-none-eabi-addr2line`, part of GNU binutils. <https://sourceware.org/binutils/docs/binutils/addr2line.html>
