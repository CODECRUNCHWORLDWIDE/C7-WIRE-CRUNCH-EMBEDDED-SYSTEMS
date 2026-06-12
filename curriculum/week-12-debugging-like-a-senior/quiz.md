# Quiz — Week 12

Ten questions. Closed-book on the platform facts (you should be able to recite the M0+ fault model and the stacked-frame layout); open-book on C/GDB syntax. ~45 minutes. Answer key is at the bottom; do not peek before completing. The midterm is right after this week, and several of these questions are exactly the kind the bench reviewer probes.

---

## Question 1

The RP2040's Cortex-M0+ takes a fault. Which fault exception is raised, and which status registers tell you the precise cause?

(A) Whichever of MemManage / BusFault / UsageFault applies; read CFSR and MMFAR/BFAR.
(B) Always HardFault (exception 3); there are no CFSR / MMFAR / BFAR registers on the M0+.
(C) A SysTick exception; read the SysTick CSR.
(D) No exception; the M0+ silently ignores illegal accesses.

## Question 2

On exception entry, the Cortex-M0+ automatically stacks eight 32-bit words. List them in order (lowest address first). Which general-purpose registers does the hardware NOT stack, and what is the consequence for a fault handler that wants a full register set?

## Question 3

You want to trace `printf`-grade logs off a running RP2040 without halting the core and without the timing perturbation of a UART. Which mechanism do you use, and why is SWO/ITM not an option on this chip?

(A) SWO/ITM single-wire trace; it is the M0+'s native trace path.
(B) RTT; the M0+ has no ITM/SWO, so RTT (over plain SWD memory reads) is the printf-free path.
(C) A second UART at a higher baud rate.
(D) JTAG boundary scan.

## Question 4

In a naked HardFault handler, you read `LR` (the `EXC_RETURN` value) and find bit 2 is set (`LR == 0xFFFFFFFD`). Which stack pointer points at the eight-word stacked frame, and in what scenario does this case arise?

## Question 5

You set six breakpoints in flash-resident code with `break` and GDB reports "cannot insert breakpoint." Why, and what is the fix?

(A) GDB ran out of memory; restart it.
(B) The M0+ has only 4 hardware breakpoint comparators, and software breakpoints cannot be placed in XIP flash; use `hbreak` and stay within 4.
(C) OpenOCD needs `monitor reset run` first.
(D) Flash is write-protected; disable the lock bit.

## Question 6

The DWT cycle counter (`DWT_CYCCNT`) reads 0 no matter what you do. You have set `CYCCNTENA` in `DWT_CTRL`. What did you forget, and at the RP2040's default clock, what is the counter's resolution and wrap period?

## Question 7

An I²C bus is hung after a mid-transaction reset: SDA is stuck low, SCL idle high, no edges. Explain (a) why the master cannot recover with a normal START, and (b) the spec-defined recovery, including why it is *nine* clock pulses specifically.

## Question 8

You are chasing a bug that fires once every few hundred cycles but vanishes when you add a `printf` in the suspect function. What does the disappearance tell you about the bug, and what is the senior's next move (name the wrong instinct and the right tools)?

## Question 9

In your crash-dump record, the CRC32 covers all fields *except* the `crc32` field itself. The dump is persisted in a `.noinit` SRAM region. Answer: (a) why exclude the crc32 field from its own CRC; (b) what kind of reset the `.noinit` region survives, and what kind it does not.

## Question 10

Write the GDB commands to: connect to an OpenOCD GDB server on the default port, reset-and-halt the target, set a *hardware* watchpoint on the global `g_state` so the core halts when it is written, and continue. Then, after the watchpoint fires, print the call chain.

---

## Answers

### Q1: B

The RP2040's cores are Cortex-M0+ (ARMv6-M). ARMv6-M has a single fault exception, **HardFault (exception 3)**, and **no** Configurable Fault Status Register, no MMFAR/BFAR. Every illegal access, unaligned access, non-thumb branch, etc., escalates to HardFault. The split fault model with CFSR/MMFAR/BFAR (answer A) is ARMv7-M (M3/M4/M7). RP2040 datasheet §2.4, pp. 16–18. This is the platform fact the exam tests hardest.

### Q2

The eight stacked words, lowest address first: **R0, R1, R2, R3, R12, LR (R14), PC (return address), xPSR** (ARMv6-M ARM §B1.5.6). The hardware does **not** stack **R4–R11** (the callee-saved registers). Consequence: a fault handler that wants a full register set must capture R4–R11 *itself*, in assembly, before any C code runs and clobbers them — which is why the handler is written `naked` with an assembly preamble that pushes R4–R11 while they are still pristine.

### Q3: B

**RTT** (Real-Time Transfer). The target writes log bytes into an SRAM ring buffer; the probe reads that SRAM over SWD while the core runs full speed, no halt, no UART latency. SWO/ITM (answer A) is the M3/M4+ single-wire trace path and **does not exist on the ARMv6-M M0+** — there is no Instrumentation Trace Macrocell on this core. So on the RP2040, RTT is the printf-free tracing story. Claiming SWO/ITM works here is the canonical wrong answer.

### Q4: PSP, under an RTOS.

`EXC_RETURN` bit 2 set (`0xFFFFFFFD`) means the pre-exception code was in **thread mode using the Process Stack Pointer (PSP)**, so the eight-word frame is on **PSP** — read it with `mrs r0, psp`. This arises when a task running on its own PSP faults — i.e., under FreeRTOS (or any RTOS that uses PSP for tasks, MSP for the kernel/handlers). In a bare-metal program with no RTOS, everything runs on MSP and `EXC_RETURN` is `0xFFFFFFF9`; the frame is on MSP. ARMv6-M ARM §B1.5.8.

### Q5: B

The M0+ has only **4 hardware breakpoint comparators** (the RP2040's configuration; Cortex-M0+ TRM). Plain `break` tries to place a *software* breakpoint — overwriting the instruction with `BKPT` — which is impossible in XIP flash without an erase. So flash code must use **hardware** breakpoints (`hbreak`), and you are capped at 4 at once. The fix: use `hbreak`, and `delete` breakpoints you no longer need. RAM-resident code (`__not_in_flash_func`) can take software breakpoints.

### Q6

You forgot to set **`TRCENA` in `DEMCR`** (`0xE000EDFC`, bit 24). `TRCENA` globally enables the trace/debug subsystem; without it the DWT registers — including `CYCCNT` — read as zero regardless of `CYCCNTENA`. The correct sequence: set `DEMCR.TRCENA`, then clear `DWT_CYCCNT`, then set `DWT_CTRL.CYCCNTENA`. At the RP2040's default **125 MHz**, each tick is **8 ns** (1/125 MHz) and the 32-bit counter wraps every **2^32 / 125e6 ≈ 34.4 seconds**.

### Q7

(a) **Why a normal START fails:** A START condition is defined as SDA *falling* while SCL is high (NXP UM10204 §3.1.10). With the stuck slave already holding SDA low, there is no high-to-low SDA edge available — SDA is already low — so the master physically cannot generate a START. The bus is wedged.

(b) **The recovery** (UM10204 §3.1.16, "Bus clear"): the master takes manual control of the pins, releases SDA (lets the pull-up take it high), and toggles SCL up to **nine** times, then issues a manual STOP. Why nine: the stuck slave is mid-byte, waiting to clock out the rest of a data byte — at most 8 remaining data bits plus 1 ACK/NACK clock = **9** clocks to finish the byte, after which the slave releases SDA. Once SDA is free, a manual STOP resynchronizes the bus.

### Q8

The disappearance tells you the bug is a **heisenbug** — timing- (or layout-) sensitive — because adding the `printf` changed the timing and closed the race window. The `printf` did not *fix* the bug; it *hid* it. The **wrong instinct** is to add more `printf`s or set breakpoints (both perturb the very timing that triggers the bug). The **right tools** are the non-perturbing ones: a **hardware watchpoint** on the contended variable (silicon, zero timing cost until it fires), **RTT** logging (two orders of magnitude cheaper than UART, so it barely moves the timing), and the **DWT counter** to measure the race window. And the right *method*: pin the schedule (disable other IRQs, force the interleaving with a controlled delay) until the bug fires deterministically, then observe. (Lecture 3 §5; Challenge 2.)

### Q9

(a) **Why exclude the crc32 field from its own CRC:** a CRC cannot cover itself — including the field in its own computation is circular (you would have to know the CRC before computing it). The CRC covers everything from `magic` up to (but not including) `crc32`; `offsetof(cc_crash_dump_t, crc32)` is the exact region length.

(b) A `.noinit` (NOLOAD) SRAM region survives a **warm reset** — a watchdog reboot, a software `SCB->AIRCR` system reset — because SRAM retains its bits through a warm reset (only the CPU state is reset, not the RAM). It does **not** survive a **power cycle** (power-off clears SRAM). For power-off survival you need the watchdog scratch registers (survive any reset short of power-off) or a flash page (survives power-off, but writing flash in a fault handler is risky).

### Q10

```text
(gdb) target extended-remote localhost:3333
(gdb) monitor reset halt
(gdb) watch g_state
Hardware watchpoint 1: g_state
(gdb) continue
... (core halts when g_state is written) ...
(gdb) bt
```

`watch` sets a hardware watchpoint that halts on a *write* (use `rwatch` for reads, `awatch` for either); it is backed by one of the M0+'s 2 DWT watchpoint comparators. `bt` prints the call chain at the halt — showing whether the write came from the ISR or the main loop, the exact "who corrupts this variable" answer.

(End of quiz.)
