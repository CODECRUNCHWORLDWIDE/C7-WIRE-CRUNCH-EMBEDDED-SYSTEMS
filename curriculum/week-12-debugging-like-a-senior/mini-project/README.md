# Mini-Project — The Crash-Dump Server and the Five-Bug Pentathlon

## Brief

Build the full senior-debugging toolchain on the RP2040 and prove it against five bugs you did not write. The deliverable has two halves:

1. **A crash-dump server.** A production-grade naked HardFault handler that captures a CRC-protected core image, persists it across reset in no-init SRAM *and* the watchdog scratch registers, and prints a postmortem on the next clean boot — over both RTT and the CDC console. Plus a host-side `cc-debug.py` that pulls the dump over SWD and maps it to source lines.

2. **The "find the bug" pentathlon.** Five injected bugs — a NULL deref, a stack overflow, a buffer overrun, an ISR jitter overrun, and a heisenbug race — each of which you root-cause with the *right* tool for its symptom (core dump, watchpoint, DWT counter, RTT, the analyzer), under a self-imposed 90-minute-per-bug budget, and document in a runbook. This is the exact format of Lab 12 from the syllabus and the rehearsal for the midterm bench exam.

End-to-end: you flash `debug_app.uf2`, attach the probe, trigger a bug, and either read the postmortem the device prints itself (for the faulting bugs) or root-cause it live over GDB/RTT/DWT (for the non-faulting ones). For the faulting bugs you also pull the dump host-side with `cc-debug.py --gdb` and confirm it decodes to the same source line.

Allocate 6 hours for this mini-project.

## Deliverables

In the `mini-project/` directory of your fork:

- `fault_handler.c` — the naked HardFault handler, the core-dump capture, the CRC/validate/finalize helpers, and the postmortem printer.
- `crash_store.c` and `crash_store.h` — the two-tier persistence (no-init SRAM + watchdog scratch) and the next-boot loader.
- `debug_app.c` — the application: RTT up-channel, DWT timing, the five injected bugs, and the on-boot crash check.
- `debug_common.h` — the shared header (the same file from `exercises/`; symlink or copy).
- `memmap_noinit.ld` — the linker script adding the `.noinit (NOLOAD)` section.
- `cc-debug.py` — the host-side core-dump reader.
- `cc_postmortem.py` — the GDB Python one-keystroke postmortem command (from Homework 1; include your version).
- `CMakeLists.txt` — the build, producing `debug_app.uf2` with a selectable `CC_BUG`.
- `requirements.txt` — host Python deps.
- `RUNBOOK.md` — your graded debugging runbook (see "The runbook" below).
- `PENTATHLON.md` — your five bug write-ups (one short section each).

## The architecture

```text
+----------------------+        SWD        +----------------------+
| Host laptop          |   (debugprobe)    | Target Pico          |
|                      |<----------------->|                      |
| OpenOCD :3333        |                   |  debug_app.c         |
|   GDB server         |                   |   - RTT up-channel   |
|   RTT server :9090   |                   |   - DWT timing       |
|                      |                   |   - 5 injected bugs  |
| gdb-multiarch        |                   |                      |
|   + cc_postmortem.py |                   |  fault_handler.c     |
|                      |                   |   - naked handler    |
| cc-debug.py          |                   |   - core dump        |
|   reads core over    |                   |                      |
|   GDB RSP, addr2line  |                  |  crash_store.c       |
|                      |                   |   - no-init SRAM     |
| nc :9090  (RTT log)  |                   |   - wdog scratch     |
+----------------------+                   +----------------------+
```

## Build prerequisites

You have:

- Pico SDK 1.5.1+ with `PICO_SDK_PATH` set.
- `arm-none-eabi-gcc` 10.3+ and `arm-none-eabi-addr2line`.
- A second Pico flashed as `debugprobe`, wired SWCLK/SWDIO/GND to the target.
- OpenOCD that knows the RP2040, `gdb-multiarch`, `picotool`.
- Python 3.10+ (only the stdlib is needed for `cc-debug.py`; `requirements.txt` is minimal).
- `sigrok-cli`/`pulseview` and a logic analyzer (for the buffer-overrun and heisenbug bugs you may want the wire view; the I²C analyzer work is in Challenge 1).

## Step-by-step

### Step 1 — Add the no-init linker section

Copy `memmap_default.ld` to `memmap_noinit.ld` and insert after `.bss`:

```ld
    .noinit (NOLOAD) : {
        KEEP(*(.noinit))
        KEEP(*(.noinit.*))
    } > RAM
```

Wire it in CMake: `pico_set_linker_script(debug_app ${CMAKE_CURRENT_SOURCE_DIR}/memmap_noinit.ld)`. Verify:

```bash
arm-none-eabi-objdump -h build/debug_app.elf | grep -i noinit
# 16 .noinit  ...  ALLOC          (note: NOT "LOAD" — it is NOLOAD)
```

If the section is missing or shows `LOAD`, the dump will not survive reset — fix this first.

### Step 2 — Build

```bash
mkdir -p build && cd build
cmake -G "Unix Makefiles" ..
make -j8 debug_app
arm-none-eabi-size debug_app.elf
```

To pin a specific bug at build time: `cmake .. -DCC_BUG=BUG_NULL_DEREF` (or `BUG_STACK_OVERFLOW`, `BUG_BUFFER_OVERRUN`, `BUG_ISR_JITTER`, `BUG_RACE`). Leave it unset to pick interactively over the CDC console.

### Step 3 — Bring up the probe and the logs

Three terminals:

```bash
# Terminal A — OpenOCD
openocd -f interface/cmsis-dap.cfg -f target/rp2040.cfg

# Terminal B — GDB, flash, run
gdb-multiarch build/debug_app.elf
(gdb) target extended-remote localhost:3333
(gdb) load
(gdb) source ../cc_postmortem.py
(gdb) monitor rtt setup 0x20000000 0x42000 "SEGGER RTT"
(gdb) monitor rtt start
(gdb) monitor rtt server start 9090 0
(gdb) monitor reset run

# Terminal C — drain the RTT log
nc localhost 9090
```

### Step 4 — The faulting bugs (1, 2): read the core dump

Run BUG_NULL_DEREF. The device faults, the handler captures the dump and resets, and on the next boot it prints the postmortem over RTT and CDC:

```text
*** CRASH DETECTED (previous boot) ***
  source: no-init SRAM (full dump)
  faulted in: thread mode (exc #0)
  PC=0x10001a2e LR=0x10001a11 xPSR=0x61000000
  R0=0x00000000 R1=... R2=... R3=...
  ...
  backtrace (heuristic):
    0x10001a2e
    0x10000c40
  -> arm-none-eabi-addr2line -f -e debug_app.elf 0x10001a2e
*** end postmortem ***
```

Map it:

```bash
arm-none-eabi-addr2line -f -e build/debug_app.elf 0x10001a2e
# bug_null_deref
# debug_app.c:118
```

Then pull the *same* dump host-side over SWD to confirm the round trip:

```bash
python3 ../cc-debug.py --elf build/debug_app.elf --gdb localhost:3333
# cc-debug: found valid dump at 0x2004xxxx
# *** CRASH DUMP DECODED ***
#   faulting line: bug_null_deref @ debug_app.c:118
#   ...
```

Repeat for BUG_STACK_OVERFLOW; the tell-tale is the stacked SP at or below the SRAM floor (`0x20000000`) — read it from the dump and explain in your write-up how the SP value alone distinguishes a stack overflow from a wild pointer.

### Step 5 — The watchpoint bug (3): catch the corruptor

Run BUG_BUFFER_OVERRUN. Set a hardware watchpoint and catch the off-by-one store live:

```text
(gdb) watch g_canary
(gdb) continue
Hardware watchpoint: g_canary
Old value = 0xCAFEF00D
New value = 0xCAFEF0B0
bug_buffer_overrun () at debug_app.c:NNN
(gdb) p i
$1 = 16            # one past the 0..15 valid range
```

### Step 6 — The DWT bug (4): catch the overrun

Run BUG_ISR_JITTER. Watch the RTT stream; the ISR overruns its 50 µs (6250-cycle) deadline every 64th tick. The "[isr] DEADLINE MISS" line and the `isr_max_cycles` jump in the thread heartbeat pinpoint it. Explain why a `printf` in the ISR could not have found this (the print would dominate and change the timing).

### Step 7 — The heisenbug (5): make it deterministic, then fix it

Run BUG_RACE. This is the Challenge 2 method in miniature: the race fires intermittently; the `busy_wait_us(2)` window is already widened so it fires often. Root-cause with `watch g_link_state` and the RTT transition log (never a `printf` in the worker), then fix it with a critical section around the worker's read-decide-act and prove `g_bug_count` stays at 0.

### Step 8 — Write the runbook and the pentathlon log

Fill `RUNBOOK.md` and `PENTATHLON.md` (below). These are the graded artifacts.

## The runbook (graded)

`RUNBOOK.md` is the document the midterm reviewer will hand back to you and say "use this." It must contain:

1. **A symptom → tool → cause decision tree.** Faults+reboots → core dump. Hangs silently → halt over SWD, read `$pc`. Wrong data on the wire → analyzer. Wrong/jittery timing → DWT. Moves when observed → non-perturbing tools, pin the schedule.
2. **A tool-per-symptom table.**
3. **The platform-facts cheat sheet.** M0+: one fault (HardFault), no CFSR, no SWO/ITM, 4 hardware breakpoints, 2 watchpoints, DWT CYCCNT present, RTT is the printf-free path.
4. **The non-perturbing-tools rule**, stated explicitly.
5. **Five war stories** — one per pentathlon bug, in runbook form.

## The pentathlon log

`PENTATHLON.md`: for each of the five bugs, a short section with: the symptom as observed, the tool you reached for *first* and why, the root cause, the fix, and how you would *detect* it in the field (which tool would have caught it earliest). Budget 90 minutes per bug; record your actual time. The time pressure is the point — the midterm is timed.

## Pass criteria

- The `.noinit` section is `NOLOAD`; a crash dump survives a watchdog reset and prints on the next boot.
- BUG_NULL_DEREF and BUG_STACK_OVERFLOW both produce a valid, CRC-checked dump that decodes (via both the on-device postmortem and `cc-debug.py`) to the correct source line.
- The stack-overflow write-up correctly identifies the out-of-range SP as the distinguishing evidence.
- BUG_BUFFER_OVERRUN is caught with a hardware watchpoint, not a `printf`.
- BUG_ISR_JITTER's overrun is measured with the DWT counter and streamed over RTT.
- BUG_RACE is made deterministic, root-caused with non-perturbing tools, fixed with a critical section, and the fix is proven (`g_bug_count == 0` over the previously-failing cycle count).
- `RUNBOOK.md` and `PENTATHLON.md` are complete and committed.

## Common bringup gotchas

1. **The dump never survives reset.** The `.noinit` section is missing, is `LOAD` instead of `NOLOAD`, or the variable is not in `.noinit`. Verify with `objdump -h`. This is the number-one failure.
2. **RTT viewer never finds the control block.** The `"SEGGER RTT"` magic is not byte-exact, or the control block landed in `.bss` (zeroed before the magic is set). Keep it in `.data`; check the literal.
3. **The fault handler reports a bogus PC.** The handler is not `naked` (its prologue shifted SP before you read the frame), or you read MSP when the frame was on PSP. Use the assembly preamble and select from `EXC_RETURN` bit 2.
4. **`addr2line` says "??".** You used a different ELF than the one running. Rebuild, reflash, and re-pull, or archive the exact ELF.
5. **Hardware breakpoints exhausted.** You have 4. `delete` unused ones; use `hbreak` for flash code.
6. **The heisenbug "fixed itself" when you added a print.** It did not — you hid it. Remove the print and use the watchpoint/RTT/DWT path.

## Bench session structure

- **Saturday 9 AM – 11 AM** — wire the probe, build, get OpenOCD+GDB+RTT all talking; verify the `.noinit` section.
- **Saturday 11 AM – 1 PM** — bugs 1 and 2: the core dump server, on-device postmortem, `cc-debug.py` round trip.
- **Saturday 2 PM – 4 PM** — bugs 3 and 4: the watchpoint and the DWT jitter hunt.
- **Saturday 4 PM – 6 PM** — bug 5: the heisenbug — make it deterministic, fix it, prove it.
- **Sunday morning** — write `RUNBOOK.md` and `PENTATHLON.md`; commit; review for the midterm.

## References

- All of Week 12's lecture notes.
- RP2040 datasheet §2.3.4 (SWD), §2.4 (Cortex-M0+), §4.7 (Watchdog).
- ARMv6-M ARM §B1.5 (exceptions). <https://developer.arm.com/documentation/ddi0419/latest/>
- SEGGER RTT docs; OpenOCD RTT chapter.
- Memfault Interrupt HardFault and coredump posts. <https://interrupt.memfault.com/>
