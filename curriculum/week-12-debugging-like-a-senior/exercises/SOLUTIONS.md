# Exercise Solutions — Week 12

Read this after you have attempted each exercise on the bench — not before. The whole point of this week is the hands-on workflow; reading the transcript without driving the probe yourself teaches you nothing the midterm rewards.

---

## Exercise 1: GDB Session

### What the exercise asks

Drive a live GDB + OpenOCD session over SWD against `exercise-01-gdb-session.c`, which has three planted situations: a watched-variable corruption, a conditional-breakpoint loop bug, and a dispatch-table trap that HardFaults if you let it run.

### Setup

Two terminals, OpenOCD in one, GDB in the other:

```text
Terminal A:
$ openocd -f interface/cmsis-dap.cfg -f target/rp2040.cfg
Info : [rp2040.core0] Cortex-M0+ r0p1 processor detected
Info : Listening on port 3333 for gdb connections

Terminal B:
$ gdb-multiarch build/ex1.elf
(gdb) target extended-remote localhost:3333
(gdb) load
(gdb) monitor reset halt
```

### Situation 1 — the watched canary

`fill_buffer()` has `for (i = 0; i <= BUFFER_LEN; i++)` — the `<=` writes one element past `g_buffer`, landing on `g_canary`. Catch the exact store with a hardware watchpoint:

```text
(gdb) watch g_canary
Hardware watchpoint 1: g_canary
(gdb) continue

Hardware watchpoint 1: g_canary
Old value = 3405578253     (0xCAFEF00D)
New value = 3405578256     (0xCAFEF010)
fill_buffer (seed=16 '\020') at exercise-01-gdb-session.c:74
74              g_buffer[i] = (uint8_t)(seed + (uint8_t)i);
(gdb) p i
$1 = 16
(gdb) bt
#0  fill_buffer (seed=16) at exercise-01-gdb-session.c:74
#1  main () at exercise-01-gdb-session.c:153
```

The watchpoint halts on the write, `p i` shows `i == 16` (one past the last valid index 15), and `bt` names the call site. This is the canonical "who corrupts this variable" solve — a watchpoint, not a thousand `printf`s. You get **2** hardware watchpoints on the RP2040; this used one.

### Situation 2 — the conditional breakpoint

`checksum_with_glitch()` reads a wrong index only at `i == 100`. A normal breakpoint would halt 200 times; a conditional one halts once:

```text
(gdb) break checksum_with_glitch if i == 100
Breakpoint 2 at 0x10000abc: file exercise-01-gdb-session.c, line 96.
(gdb) continue
Breakpoint 2, checksum_with_glitch (count=200) at exercise-01-gdb-session.c:96
96              if (i == 100u) {
(gdb) p idx
$2 = 100
(gdb) next
(gdb) p idx
$3 = 256          # the bug: idx jumped to i+156
(gdb) p/x g_big_table[idx & 0xff]
```

The condition is evaluated on each hit, so it does cost a halt every iteration internally — but to *you* it halts once, at the iteration that matters.

### Situation 3 — the dispatch trap

`handlers[2]` is `NULL`. Letting `dispatch(2)` run calls address 0 (no thumb bit) and HardFaults. Break before the jump and inspect:

```text
(gdb) hbreak dispatch
(gdb) continue
Breakpoint 3, dispatch (sel=2) at exercise-01-gdb-session.c:130
(gdb) p/x handlers
$4 = {0x10000401, 0x10000409, 0x0, 0x10000411}
(gdb) p/x fn
$5 = 0x0
```

`fn == 0x0` — do NOT `step` into it. Route around the bug to keep the session alive:

```text
(gdb) set var sel = 3
(gdb) p/x handlers[sel & 3]
$6 = 0x10000411     # handler3, safe
(gdb) continue
```

### The canonical bugs (your bugs, while learning)

1. **Plain `break` in flash fails with "cannot insert breakpoint."** Flash code needs `hbreak` (hardware), and you only get 4. Symptom: GDB sets the breakpoint but `continue` errors. Use `hbreak`; `delete` ones you are done with.
2. **Forgetting `monitor reset halt` before `load`.** If the target's firmware reconfigures the SWD pads, OpenOCD loses the port mid-session. Always `monitor reset halt` to catch the core at the reset vector before its code runs.
3. **`step` into a NULL function pointer.** This faults; now you are debugging a fault instead of inspecting the table. Inspect (`p/x fn`) before you step.
4. **Watchpoint on a local variable that goes out of scope.** GDB auto-deletes a watchpoint when its variable's scope exits; `g_canary` is a global so it persists, but a `watch` on a stack local vanishes at the return. Watch globals, or set the watchpoint by address (`watch *(uint32_t*)0x20001234`).
5. **Confusing `step` and `next`.** `step` descends into calls; `next` steps over them. Stepping into a `printf` and getting lost in libc is the universal first-day mistake.

---

## Exercise 2: RTT Logging

### What the exercise asks

Implement a single SEGGER-compatible RTT up-channel, log from thread context and from an ISR, and use the DWT cycle counter to compare the cost of one RTT line against one blocking UART `printf` line.

### Reference solution

The implementation is in `exercise-02-rtt-logging.c`. The two load-bearing pieces:

- The **control block** with the `"SEGGER RTT"` magic in `.data` so the host can scan SRAM for it.
- `cc_rtt_write`, which copies bytes into the ring, then a compiler barrier, then publishes `write_offset` — data-before-index ordering so a racing host SWD read never sees an index pointing at unwritten data.

### Host-side bring-up

After OpenOCD is running, set up RTT from the GDB `monitor` passthrough (or the OpenOCD telnet on :4444):

```text
(gdb) monitor rtt setup 0x20000000 0x42000 "SEGGER RTT"
(gdb) monitor rtt start
rtt: Searching for control block 'SEGGER RTT'
rtt: Control block found at 0x2000xxxx
(gdb) monitor rtt server start 9090 0
```

Then in a third terminal drain channel 0:

```text
$ nc localhost 9090
=== Exercise 2: RTT logging ===
RTT up-channel live. Attach the host viewer.
[rtt] (same line logged over RTT — note it was cheap)
[isr] tick 1
[isr] tick 2
[thread] heartbeat 0 isr_max_cycles=312
[isr] tick 3
...
```

The ISR lines interleave with the thread lines — proof that RTT logged from interrupt context without deadlock or perturbation.

### Expected DWT comparison (on the UART/CDC side)

```text
[uart] DWT overhead calibrated: 4 cycles
[uart] one RTT line  = 211 cycles
[uart] one UART line = 47230 cycles
[uart] UART is ~223x costlier than RTT
```

The exact numbers depend on the line length and the CDC vs hardware-UART path, but the ratio is always two-to-three orders of magnitude. At 125 MHz, 211 cycles is ~1.7 µs (a memcpy of 44 bytes plus index bumps); 47230 cycles is ~378 µs (the blocking wait for the bytes to clock out). That ~376 µs of difference is exactly the perturbation that hides a timing bug — which is why you log timing-sensitive code over RTT, never over UART.

### The canonical bugs

1. **Magic string not byte-exact.** The host scans for `"SEGGER RTT"` (capital S-E-G-G-E-R, space, R-T-T). A typo, or a `.bss`-placed control block (zeroed at boot before the magic is set), and `rtt setup` never finds it. Keep the control block in `.data` and double-check the literal.
2. **Publishing the index before the data.** If you bump `write_offset` first and copy the byte second, a host read in between sees a phantom byte. The barrier-then-index order in `cc_rtt_write` is mandatory.
3. **Block-mode RTT in an ISR.** `CC_RTT_FLAG_BLOCK` spins until the host drains the ring; in an ISR with the host not attached, that hangs forever. Use `CC_RTT_FLAG_SKIP` (drop on full) in interrupt context. The exercise sets SKIP for exactly this reason.
4. **Off-by-one ring-full test.** A ring of size N holds N-1 bytes (the `next == read_offset` test reserves one slot to distinguish full from empty). Trying to use all N bytes makes full and empty indistinguishable and corrupts the stream.
5. **Forgetting `cc_dwt_enable()` before timing.** Without `TRCENA` set in `DEMCR`, `DWT_CYCCNT` reads 0, and every measurement is 0 cycles. The calibration step (`g_dwt_overhead`) catching a plausible small value is your sanity check that DWT is live.

---

## Exercise 3: Fault Dump

### What the exercise asks

Write a naked HardFault handler that captures a CRC-protected core dump, persist it across a warm reset in no-init SRAM, and print a postmortem on the next clean boot. Then map the faulting PC to a source line with `addr2line`.

### The linker fragment you must add

The no-init region needs a `NOLOAD` section the C runtime does not clear. Add to a custom linker script (copy `memmap_default.ld` and insert after `.bss`):

```ld
    .noinit (NOLOAD) : {
        KEEP(*(.noinit))
        KEEP(*(.noinit.*))
    } > RAM
```

and wire it with `pico_set_linker_script(ex3 ${CMAKE_CURRENT_SOURCE_DIR}/memmap_noinit.ld)`. Without this, the SDK's `.bss` clear zeroes `g_crash_dump` on every boot and the dump never survives. (You can verify with `arm-none-eabi-objdump -h build/ex3.elf | grep noinit` — the section should exist with `NOLOAD`/`ALLOC` and *not* `LOAD`.)

### Reference solution

The full source is `exercise-03-fault-dump.c`. The pieces:

- `cc_crc32` — table-free, zero-data-dependency CRC so the validator works even if memory is partly corrupt.
- The **naked handler** `cc_hardfault_handler` — selects MSP/PSP from `EXC_RETURN` bit 2, pushes `R4-R11` (M0+ two-step: `mov` highs to lows, then push), passes the frame SP and the saved-reg block to C.
- `cc_fault_handler_c` — fills the dump, runs the heuristic backtrace, finalizes the CRC, resets.
- `print_postmortem` — runs on the clean boot after validation passes.

### Expected output

Boot 1 (the deliberate fault):

```text
=== Exercise 3: fault dump ===
No prior crash dump found (this is a clean boot).
Armed. Faulting in 3 seconds to demonstrate the dump path...
  3...
  2...
  1...
(device resets here — the fault, the handler, the watchdog reboot)
```

Boot 2 (the postmortem):

```text
=== Exercise 3: fault dump ===

*** CRASH DETECTED (from previous boot) ***
  faulted while servicing: thread mode (no active exception) (exc #0)
  PC  = 0x10000c84  LR  = 0x10000c61  xPSR = 0x61000000
  R0  = 0x00000003  R1  = 0xdeadbeef  R2  = 0x00000000  R3  = 0x00000000
  R12 = 0x00000000
  R8..R11 = 0x00000000 0x00000000 0x00000000 0x00000000
  R4..R7  = 0x20000100 0x00000000 ...
  stacked SP = 0x20041f70
  backtrace (heuristic, 3 entries):
    0x10000c84
    0x100009d0
    0x10000420
  -> arm-none-eabi-addr2line -f -e ex3.elf 0x10000c84
*** end of postmortem ***
Fault already demonstrated this power cycle. Running clean.
alive
alive
```

`R0 = 0x00000003` is the bad address and `R1 = 0xdeadbeef` is the value being stored — the unaligned out-of-range write. Mapping the PC:

```text
$ arm-none-eabi-addr2line -f -e build/ex3.elf 0x10000c84
trigger_deliberate_fault
exercise-03-fault-dump.c:251
```

Exactly the line of the planted `*bad = 0xDEADBEEFu`. You just read a core dump on a chip with no operating system.

### The canonical bugs

1. **Missing the `.noinit` linker section.** The most common failure. The dump is captured, the reset happens, and the next boot finds the region zeroed because `.bss` clearing wiped it. Verify the section exists and is `NOLOAD`.
2. **A non-naked handler.** A normal C handler emits a prologue that pushes registers and shifts SP *before* your code reads it, so the frame pointer you compute is wrong and the postmortem reports a bogus PC. The handler must be `naked` and start in assembly.
3. **Forgetting to capture R4-R11.** They are callee-saved and NOT in the hardware-stacked frame. If you skip the preamble's push, those slots in the dump are garbage. (Symptom: the backtrace and the low registers look right but R4-R11 are nonsense.)
4. **Reading the frame from the wrong stack.** In a bare-metal build everything is on MSP, so a handler that always reads MSP works — until you add FreeRTOS and a task faults on PSP, and suddenly the dump points at the kernel stack. Always select from `EXC_RETURN` bit 2.
5. **CRC over the wrong region.** The CRC must cover everything *except* the `crc32` field itself (a CRC cannot cover itself). `offsetof(cc_crash_dump_t, crc32)` is the exact byte count. Including the crc32 field makes validation always fail.
6. **`addr2line` against the wrong ELF.** The PC only maps to a line if you use the *exact* ELF that was running. A rebuild between the crash and the `addr2line` shifts addresses and gives you the wrong line (or "??"). Archive the ELF with every build that ships.

---

## A general note on "the bug went away"

For all three exercises, and for the whole week: when a bug disappears the moment you observe it, you have a heisenbug (Lecture 3), and the disappearance is *information*, not relief. It tells you the bug is timing- or layout-sensitive. The senior response is to switch to the non-perturbing tools you built this week:

- **A hardware watchpoint** (Exercise 1) instead of a `printf` — silicon comparing the address bus, zero timing cost until it fires.
- **RTT** (Exercise 2) instead of a UART `printf` — two orders of magnitude cheaper, so it perturbs the timing two orders of magnitude less.
- **The DWT counter** (Exercise 2) instead of timing-by-print — measures without printing.
- **The persisted core dump** (Exercise 3) instead of trying to catch the fault live — the crash records itself, no observer present to change it.

The instruments you built this week are precisely the ones that do not move the bug. That is not a coincidence; it is the entire design goal of senior debugging tooling.
