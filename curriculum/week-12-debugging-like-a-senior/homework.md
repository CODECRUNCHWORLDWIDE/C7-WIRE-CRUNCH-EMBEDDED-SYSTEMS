# Homework — Week 12

Six problems. Estimated total time: ~6 hours over the week. Submit answers as commits to your fork of the course repo; each problem has a designated file path under `homework/week-12/`. The midterm follows this week, so treat the homework as exam prep — every problem rehearses a bench skill the reviewer probes.

---

## Problem 1 — Write a GDB Python postmortem command (1 hour)

Extend the `cc_postmortem.py` skeleton from Lecture 1 into a full GDB Python command that, when run after a fault (or at any halt), reads the eight-word stacked frame at `$sp`, unpacks it, decodes the exception number from the stacked `xPSR`, maps the faulting PC and each backtrace candidate to a source line, and prints a formatted postmortem — all in one keystroke.

Deliver `homework/week-12/cc_postmortem.py`. Requirements:

1. Register a `gdb.Command` named `postmortem`.
2. Read 32 bytes at `$sp` with `gdb.selected_inferior().read_memory`, unpack with `struct.unpack("<8I", ...)`.
3. Print `R0–R3`, `R12`, `LR`, `PC`, `xPSR`, and the exception number (`xpsr & 0x1ff`).
4. For the PC, run `info line *0x...` and print the source line; handle the `gdb.error` when there is no line.
5. Bonus: implement the heuristic stack scan in Python — read the stack from `$sp` to `0x20042000`, flag words in the `.text` range with the thumb bit set, and `addr2line` each.

Test it against `exercise-01`'s dispatch trap: let it fault, run `postmortem`, and confirm it names the NULL-pointer call site.

---

## Problem 2 — Decode an I²C capture by hand (1 hour)

You are given (`homework/week-12/i2c-capture.txt`, in the homework directory) a raw timing dump of SDA and SCL — a list of `(time_us, scl, sda)` samples from one I²C transaction. Decode it **by hand** (do not run a decoder), producing the START, the address, the R/W bit, every ACK/NACK, every data byte, and the STOP.

Deliver `homework/week-12/i2c-decode.md` that:

1. Identifies the START (SDA falling while SCL high) and STOP (SDA rising while SCL high) by timestamp.
2. Reads the 7 address bits + R/W bit on the rising SCL edges after START.
3. Reads each ACK/NACK (the 9th clock of each byte).
4. Reads each data byte, MSB first.
5. States what device address was addressed, read or write, and the byte values.
6. Verifies your hand-decode against `sigrok-cli --protocol-decoder i2c` (or PulseView) and notes any discrepancy.

The point is to internalize the I²C framing so that when you read a *live* capture under exam pressure you do not need to think about it.

---

## Problem 3 — Linker work for a no-init crash region (45 minutes)

Take the Pico SDK's `memmap_default.ld`, add a `.noinit (NOLOAD)` section after `.bss`, and confirm a variable placed there survives a watchdog reset.

Deliver `homework/week-12/memmap_noinit.ld` and a short `homework/week-12/noinit-proof.md`:

1. The linker fragment (the `.noinit (NOLOAD)` section in the SECTIONS block, mapped to `RAM`).
2. The CMake line that selects it (`pico_set_linker_script`).
3. `arm-none-eabi-objdump -h build/your.elf | grep noinit` output showing the section is `ALLOC` but **not** `LOAD` (i.e., NOLOAD).
4. A test: write a magic value into a `.noinit` variable, `watchdog_reboot`, and on the next boot confirm the magic is still there. Capture the two-boot serial log.
5. A statement of what reset class this survives (warm) and what it does not (power cycle), with the reasoning.

---

## Problem 4 — Measure ISR jitter with the DWT counter (1 hour)

Write `homework/week-12/isr-jitter.c` that brackets a repeating-timer ISR with `DWT_CYCCNT`, keeps a running min / max / count, and streams the stats over RTT. Then deliberately induce jitter (do a flash read-while-write elsewhere, or run a long DMA) and catch the worst-case overrun.

Deliver the source plus `homework/week-12/isr-jitter.md`:

1. The DWT enable + overhead-calibration code.
2. The ISR instrumentation (min/max/count, RTT stream).
3. A baseline measurement (ISR with nothing else running): report min/max/mean cycles and the equivalent µs at 125 MHz.
4. The jitter-induced measurement: show the worst-case spike and explain the cause (XIP stall during the flash write, or DMA bus contention).
5. A one-paragraph argument for why a `printf`-based measurement could not have found this.

---

## Problem 5 — Build a tool-per-symptom debugging runbook (1.5 hours)

This is the graded artifact you carry into your career. Author `homework/week-12/runbook.md`: a one-to-two-page decision tree from symptom to tool to likely cause, covering the five bug families from the README (timing, memory, peripheral-state, supply/EMI, heisenbug).

Required sections:

1. **Decision tree.** Start from observable symptoms: *Does the core fault and reboot?* → core dump (Lecture 2). *Does it hang silently?* → halt over SWD, read `$pc`/`$sp`, `bt`. *Is the data wrong on the wire?* → logic analyzer + protocol decoder. *Is the timing wrong / jittery?* → DWT counter + RTT. *Does the bug move when you observe it?* → non-perturbing tools, pin the schedule.
2. **Tool-per-symptom table.** Columns: symptom, first tool, what it shows, likely cause class.
3. **The non-perturbing-tools rule.** State explicitly which tools perturb (printf, breakpoints, halts) and which do not (RTT, DWT, watchpoints, the analyzer), and when each matters.
4. **The "platform facts" cheat sheet.** The M0+ has one fault (HardFault), no CFSR, no SWO/ITM, 4 hardware breakpoints, 2 watchpoints, DWT CYCCNT present. These are the facts the bench exam tests.
5. **Your own war story.** One bug from this week's exercises/challenges, in runbook form (symptom → tool → cause → fix → detection).

The deliverable is ~600–800 words of dense, scannable reference — the kind of document you would actually pin above your bench.

---

## Problem 6 — Reproduce a stack-overflow fault and read it from the dump (45 minutes)

Write `homework/week-12/stack-overflow.c` that deliberately recurses without bound (or allocates a large stack array in a deep call) until the stack overflows past the bottom of SRAM and faults. Capture the fault with your Exercise 3 crash-dump handler and read the postmortem.

Deliver the source plus `homework/week-12/stack-overflow.md`:

1. The recursion/allocation that overflows the stack.
2. The captured postmortem: the faulting PC, the stacked SP (which should be at or below the SRAM floor `0x20000000`, or pointing at a peripheral region — the tell-tale of a stack overflow).
3. The `addr2line` mapping of the PC.
4. An explanation of *how you knew from the dump* that it was a stack overflow and not a wild pointer: the SP value out of the normal stack range, the deep/repeating backtrace, the faulting access being a `push`/store at the frame boundary.
5. A note on the real fix (bound the recursion, size the stack, or add a stack guard / the SDK's stack-overflow check).

---

## Submission

Commit all six problems to your fork under `homework/week-12/`. Tag the commit `week-12-homework`.

```bash
git add homework/week-12/
git commit -m "week 12 homework: gdb postmortem, i2c decode, noinit linker, isr jitter, runbook, stack overflow"
git tag week-12-homework
git push origin main --tags
```

The teaching team reviews homework asynchronously; expect feedback within ~5 business days. Note that Problem 5 (the runbook) is the one the midterm reviewer will ask you to *use* live — invest in it.

---

## References for the homework set

- RP2040 datasheet §2.3.4 (SWD), §2.4 (Cortex-M0+), §4.3 (I²C), §4.7 (Watchdog).
- ARMv6-M ARM §B1.5 (exceptions, stacked frame, EXC_RETURN). <https://developer.arm.com/documentation/ddi0419/latest/>
- GDB user manual, "Extending GDB" (Python API). <https://sourceware.org/gdb/current/onlinedocs/gdb/>
- NXP UM10204 (I²C), §3.1 (framing). <https://www.nxp.com/docs/en/user-guide/UM10204.pdf>
- SEGGER RTT docs. <https://www.segger.com/products/debug-probes/j-link/technology/about-real-time-transfer/>
- Cortex-M0+ TRM (DWT). <https://developer.arm.com/documentation/ddi0484/latest/>
- Memfault Interrupt, HardFault and coredump posts. <https://interrupt.memfault.com/>
