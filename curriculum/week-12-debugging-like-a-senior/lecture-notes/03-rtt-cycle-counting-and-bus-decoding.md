# Lecture 3 — RTT, Cycle Counting, Bus Decoding, and Heisenbugs

> *The previous two lectures gave you the probe and the core dump — the tools for when the firmware has stopped, halted or crashed. This lecture is about the harder class of bug: the one that only happens when the firmware is running full speed, the one that vanishes the instant you halt the core to look at it, the one where the data leaves your chip correctly but the peripheral disagrees. For these you need tools that observe without perturbing: RTT, which logs without a UART's latency; the DWT cycle counter, which times without a printf's overhead; and the logic analyzer, which reads the wire without trusting any of your code. And you need a methodology — the heisenbug discipline — for the bugs that move when you look at them. This is the senior's toolkit, and it is what the midterm bench exam is actually testing.*

## 1. RTT — logging that does not perturb

You have been using `printf` over a UART (or CDC) all course. It works, and for most bugs it is the right tool. But a UART `printf` has a cost that matters for an entire class of bug: **time**. At 115200 baud, one byte is ~87 µs on the wire; a 40-character log line is ~3.5 ms of the CPU either busy-waiting on the UART FIFO or, if you buffered it, of DMA contention and an interrupt. If the bug you are chasing is a timing bug — an ISR that occasionally overruns its deadline, a race between two tasks — then **adding a `printf` changes the timing you are trying to measure.** That is the observer effect, and it is why `printf` is the wrong tool for timing bugs. It is also unsafe in an ISR (the UART driver may not be re-entrant; printing from an ISR can deadlock on a lock the interrupted code holds) and impossible after a fault (no working stdio).

**RTT (Real-Time Transfer)**, SEGGER's invention, solves the timing problem. The mechanism:

1. The target places a **control block** in SRAM, beginning with the ASCII magic string `"SEGGER RTT"`.
2. The control block describes one or more **ring buffers** (an "up" channel is target→host, a "down" channel is host→target), each with a storage pointer, a size, a write index (owned by the target), and a read index (owned by the host).
3. To log, the target copies bytes into the up-ring and bumps its write index. That is a memcpy and an integer add — **a few CPU cycles per byte, no busy-wait, no I/O.**
4. The debug probe, over SWD, periodically reads the control block and the ring buffer **while the core runs full speed, never halting it**, drains any new bytes (advancing the read index), and shows them in your terminal.

The host finds the control block by scanning SRAM for the `"SEGGER RTT"` magic — which is exactly why the magic string exists and why `openocd`'s `rtt setup <ram_base> <ram_size> "SEGGER RTT"` takes the magic as an argument. Once found, OpenOCD (`monitor rtt start`, then `rtt server start 9090 0`) exposes the up-channel on a TCP port you `nc localhost 9090` or point a viewer at.

The control-block and ring layout (simplified to one up-channel, matching `debug_common.h`):

```c
typedef struct {
    const char *name;
    uint8_t    *buffer;
    uint32_t    size_bytes;
    volatile uint32_t write_offset;   /* target writes */
    volatile uint32_t read_offset;    /* host writes (over SWD) */
    uint32_t    flags;                /* skip-if-full vs block-if-full */
} cc_rtt_ring_t;

typedef struct {
    char id[16];                      /* "SEGGER RTT" + pad; host scans for this */
    int32_t max_up_channels;
    int32_t max_down_channels;
    cc_rtt_ring_t up[1];
} cc_rtt_control_block_t;
```

The write, the entire hot path of RTT:

```c
size_t cc_rtt_write(const void *data, size_t length) {
    cc_rtt_ring_t *ring = &g_rtt_cb.up[0];
    const uint8_t *src = (const uint8_t *)data;
    uint32_t wr = ring->write_offset;
    uint32_t rd = ring->read_offset;      /* host may be advancing this concurrently */
    size_t written = 0;

    while (written < length) {
        uint32_t next = wr + 1;
        if (next >= ring->size_bytes) next = 0;
        if (next == rd) {                 /* ring full */
            if (ring->flags == CC_RTT_FLAG_SKIP) break;   /* drop, don't block */
            /* block mode: re-read rd and spin (rarely used in an ISR) */
            rd = ring->read_offset;
            if (next == rd) continue;
        }
        ring->buffer[wr] = src[written++];
        wr = next;
    }
    /* Publish the new write index LAST, after the data is in the buffer, so a
       host read that races us never sees an advanced index pointing at a byte
       we have not written yet. A compiler barrier (or DMB) enforces the order. */
    __asm volatile("" ::: "memory");
    ring->write_offset = wr;
    return written;
}
```

The ordering at the end is the only subtle part: write the *data* into the ring, then a barrier, then publish the *index*. The probe reading concurrently over SWD must never see `write_offset` point past data the target has not yet stored. Because the M0+ is single-issue and in-order, a compiler barrier suffices for the up-channel (the probe is the only other agent, and it only reads); on a multi-core or store-buffered part you would use a `DMB`.

**Why RTT works in an ISR and `printf` does not:** RTT's write is non-blocking (skip-if-full), touches no peripheral, takes no lock, and completes in bounded constant time. You can call it from a HardFault handler, from a high-priority ISR, from anywhere — it cannot deadlock and it cannot busy-wait. That property is what makes RTT the logging tool for timing-sensitive and ISR-context bugs.

**The RP2040 platform fact:** the M0+ has **no ITM and no SWO**. SWO (Serial Wire Output) and ITM (Instrumentation Trace Macrocell) are the M3/M4+ single-wire `printf`-trace mechanisms; they do not exist on ARMv6-M. So on the RP2040, **RTT is the printf-free tracing story** — there is no SWO to fall back to. This is a frequent wrong answer on the exam: "use SWO/ITM for tracing on the Pico" is incorrect because the silicon lacks it. RTT (which works over plain SWD memory reads, no trace hardware needed) is the answer.

## 2. The DWT cycle counter — timing what you cannot print

For "how long does this region take, exactly," the right tool is the **DWT cycle counter** (`CYCCNT`): a free-running 32-bit counter that increments once per CPU clock. At the RP2040's default 125 MHz, each tick is 8 ns and the counter wraps every ~34.4 seconds — ample for instruction-level timing. Enabling it (per `debug_common.h`'s `cc_dwt_enable`):

```c
static inline void cc_dwt_enable(void) {
    cc_reg_write(CC_DEMCR, cc_reg_read(CC_DEMCR) | CC_DEMCR_TRCENA);     /* arm DWT */
    cc_reg_write(CC_DWT_CYCCNT, 0u);                                     /* clear   */
    cc_reg_write(CC_DWT_CTRL,  cc_reg_read(CC_DWT_CTRL) | CC_DWT_CTRL_CYCCNTENA);
}
```

`TRCENA` in `DEMCR` (`0xE000EDFC`) globally enables the trace/debug subsystem; without it the DWT registers read as zero. `CYCCNTENA` in `DWT_CTRL` starts the counter. Then timing a region is read-region-read-subtract:

```c
uint32_t t0 = cc_dwt_cyccnt();
do_the_thing();
uint32_t t1 = cc_dwt_cyccnt();
uint32_t cycles = t1 - t0 - g_dwt_overhead;   /* subtract calibrated overhead */
```

`g_dwt_overhead` is the cost of the two reads and the subtract themselves, measured once at startup by timing an empty region:

```c
uint32_t a = cc_dwt_cyccnt();
uint32_t b = cc_dwt_cyccnt();
g_dwt_overhead = b - a;                        /* typically a handful of cycles */
```

The DWT counter is **non-perturbing** in the way that matters: reading it is a single `LDR`, it does not touch a peripheral, it does not wait, it does not interrupt. You can bracket an ISR with it and learn the ISR's exact worst-case duration across thousands of invocations — recording the max into a variable RTT then ships off-chip — and find the one-in-ten-thousand invocation that blows the deadline. A `printf` could never find that bug because the `printf` itself would dominate the timing.

The M0+'s DWT is *reduced*: no PC sampling, fewer comparators than an M4. But `CYCCNT` is present on the RP2040 and is the load-bearing feature. (The same two DWT comparators that back GDB's hardware watchpoints are the M0+'s entire DWT comparator budget; do not expect the M4's four.)

A subtlety on the 32-bit wrap: because `CYCCNT` is a free-running 32-bit counter, the subtraction `t1 - t0` is correct *even across a wrap*, as long as the region is shorter than the wrap period (~34.4 s). Unsigned 32-bit subtraction wraps modulo 2^32, which is exactly the arithmetic you want:

```c
/* Correct across a counter wrap because uint32_t subtraction is modulo 2^32.
   t0 = 0xFFFFFFF0, t1 = 0x00000010  ->  t1 - t0 = 0x20 = 32 cycles. */
uint32_t elapsed = t1 - t0;       /* do NOT add a wrap check; it would be wrong */
```

Adding a "did it wrap?" branch is a classic over-engineering bug here — the natural unsigned subtraction already handles it. The only failure is a region longer than ~34 s, which no ISR should ever be.

A worked jitter hunt: suppose a sensor ISR is supposed to finish in under 50 µs (6250 cycles at 125 MHz) but the system occasionally drops samples. Bracket the ISR body with `CYCCNT`, keep a running max in a `volatile uint32_t`, and stream it over RTT:

```c
void __isr sensor_isr(void) {
    uint32_t t0 = cc_dwt_cyccnt();
    handle_sensor();
    uint32_t dt = cc_dwt_cyccnt() - t0 - g_dwt_overhead;
    if (dt > g_isr_max) { g_isr_max = dt; cc_rtt_write_str("NEW MAX\n"); }
}
```

Watch the RTT stream; when `g_isr_max` jumps to 9000 cycles (72 µs) you have caught the overrun, and the *correlation* — it happens right after a "NEW MAX" line that also coincides with a flash write elsewhere — points at the cause (the flash write disabled XIP and stalled the ISR's instruction fetch, exactly the Week 10 hazard). No `printf` finds this; the `printf` *is* the perturbation.

## 3. The logic analyzer — the truth on the wire

When the bug is "the I²C sensor returns zeros" or "the SPI display shows garbage," the question is never really about your code first — it is about **the bytes on the wire**. Did your bytes leave the chip correctly? Did the peripheral answer? The only instrument that answers without trusting any of your firmware is a **logic analyzer** on the signal lines.

The setup (Sparkfun's sigrok tutorial is the friendly version):

1. Wire the analyzer's channels to the bus lines: for I²C, `D0→SDA`, `D1→SCL`, plus a ground. For SPI, `D0→SCLK`, `D1→MOSI`, `D2→MISO`, `D3→CS`, plus ground.
2. **Sample fast enough.** The rule is ≥4× the fastest edge of interest (comfortably above Nyquist for clean decoding). I²C at 400 kHz → sample at ≥2 MHz; SPI at 4 MHz → sample at ≥16 MHz. A cheap FX2 analyzer does 24 MS/s, fine for both.
3. **Trigger** on the start of the transaction: a falling edge on `CS` (SPI) or a START condition on I²C (SDA falling while SCL high). A trigger keeps you from capturing seconds of idle bus.
4. **Apply a protocol decoder.** In PulseView, add the `i2c` decoder, assign `SCL`/`SDA`, and the raw edges become a transcript: `START`, the 7-bit address, the R/W bit, `ACK`/`NACK`, each data byte, `STOP`.

A healthy I²C read of a sensor at address `0x76` register `0xD0` decodes as:

```text
START  0x76 W  ACK  0xD0 ACK  REPEATED-START  0x76 R  ACK  0x60 NACK  STOP
```

— address acked, register written, repeated start, read byte `0x60`, master NACKs the last byte, stop. Now the four questions answer themselves: framing present (START/STOP), slave acked its address (so it is alive and at the address you think), data byte is `0x60` (the BME280 chip-id — correct). If instead the address byte gets `NACK`, the slave is not at that address (wrong address, or not powered, or held in reset) — and you have just saved yourself an hour of re-reading driver code that was never the problem. The analyzer relocates the bug from "somewhere in my 500 lines" to "the wire says the slave never answered," which is a different and far smaller search.

## 4. Case study: the hung I²C bus

The canonical real-world bus bug, and the one the midterm reviewer loves to stage. Symptom: after a reset (a watchdog reboot, a debugger-forced reset, a brownout) the I²C bus is dead — every transaction times out, the master cannot even issue a START.

**The cause** (visible on the analyzer): the reset happened *mid-transaction*, while a slave was clocking out a read. The slave had driven `SDA` low for a data bit and was waiting for the next `SCL` clock to advance. The master reset and forgot the transaction ever happened. Now the slave sits forever holding `SDA` low, waiting for clocks that will never come. And here is the deadlock: a START condition is defined as `SDA` falling **while `SCL` is high** (NXP UM10204 §3.1.10) — but `SDA` is *already* low, held by the slave, so the master cannot generate the falling edge that makes a START. The bus is wedged. The analyzer shows it plainly: `SDA` stuck low, `SCL` idle high, no edges.

**The recovery** is the **nine-clock-pulse bus-clear** (NXP UM10204 §3.1.16, "Bus clear"). The master takes manual control of the pins (bit-bangs them as GPIO, not through the I²C peripheral, because the peripheral cannot START into a stuck bus), releases `SDA` (lets it float high via the pull-up), and toggles `SCL` up to **9 times**. Each `SCL` pulse lets the stuck slave shift out one more bit; after at most 9 clocks the slave finishes its byte, sees the (implicit) NACK, and **releases `SDA`**. Once `SDA` is high again, the master issues a manual STOP (`SDA` rising while `SCL` high) to resynchronize, then re-initializes the I²C peripheral. On the RP2040 (DesignWare I²C IP, datasheet §4.3):

```c
void cc_i2c_bus_clear(uint sda_pin, uint scl_pin) {
    /* Detach the pins from the I2C peripheral; drive them as open-drain GPIO. */
    gpio_set_function(sda_pin, GPIO_FUNC_SIO);
    gpio_set_function(scl_pin, GPIO_FUNC_SIO);
    gpio_set_dir(scl_pin, GPIO_OUT);
    gpio_set_dir(sda_pin, GPIO_IN);           /* release SDA (pull-up takes it high) */

    /* Up to 9 clock pulses to free a stuck slave (NXP UM10204 §3.1.16). */
    for (int i = 0; i < 9; i++) {
        gpio_put(scl_pin, 0); sleep_us(5);
        gpio_put(scl_pin, 1); sleep_us(5);
        if (gpio_get(sda_pin)) break;         /* SDA released -> slave is free */
    }

    /* Manual STOP: SDA low->high while SCL high. */
    gpio_set_dir(sda_pin, GPIO_OUT);
    gpio_put(sda_pin, 0); sleep_us(5);
    gpio_put(scl_pin, 1); sleep_us(5);
    gpio_put(sda_pin, 1); sleep_us(5);        /* the STOP edge */

    /* Hand the pins back to the I2C peripheral and re-init. */
    gpio_set_function(sda_pin, GPIO_FUNC_I2C);
    gpio_set_function(scl_pin, GPIO_FUNC_I2C);
}
```

Challenge 1 has you reproduce this for real (reset the master mid-read with a watchdog), *prove* on the analyzer that the slave is the one holding `SDA`, recover with the nine-clock trick, and write it up. The methodology lesson is bigger than I²C: when a bus is wedged, the analyzer tells you *who* is holding *which* line, and the spec tells you the defined recovery. Guessing is for juniors.

## 4a. Decoding SPI, and the down-channel for input

I²C is the dramatic case because of the hang, but SPI bugs are just as common and the analyzer reads them the same way. SPI has four lines — `SCLK`, `MOSI` (master out), `MISO` (master in), and `CS` (chip select, active low) — and a *mode* (CPOL/CPHA) that decides which clock edge samples data. The single most common SPI bug is a mode mismatch: your driver clocks data on the rising edge, the peripheral latches on the falling edge, and every byte comes back shifted by a bit or simply wrong. The analyzer settles it instantly. Wire `D0→SCLK`, `D1→MOSI`, `D2→MISO`, `D3→CS`; trigger on `CS` falling; apply the `spi` decoder and tell it the CPOL/CPHA you *think* you are using. If the decoded bytes match your intended command, your code is right and the wire is right; if they are garbled, flip CPHA and re-decode — if it cleans up, you had the mode wrong. This is a thirty-second experiment on the analyzer versus an afternoon of re-reading a datasheet's timing diagram.

The decoder stack is also *stackable*: sigrok lets you put a device-specific decoder on top of the `spi` or `i2c` decoder, so a raw `0x76 W 0xD0 ...` becomes "BME280: read chip-id register." For a sensor you debug often, writing (or downloading) a stacked decoder turns the transcript into application-level events, which is how a senior reads a bus capture at a glance instead of byte by byte.

One more RTT detail relevant to bus work: RTT has a **down-channel** (host→target), the mirror of the up-channel. You can send commands *into* the running firmware over SWD — e.g., "dump the I²C driver's last 16 transactions" — without a UART and without halting. The down-channel is a second ring buffer in the same control block; the host writes, the target polls. We do not use it in the exercises (one up-channel is enough for logging) but it is how production RTT-based consoles accept input, and worth knowing exists.

## 4b. A taxonomy of firmware bugs and the tool for each

The syllabus frames Week 12 around five bug families. The discipline is to recognize the family from the *symptom* and reach for the matching tool — not to apply your favorite tool to every bug.

| Family | Symptom | First tool | Why |
|--------|---------|-----------|-----|
| **Timing** | jitter, missed deadlines, intermittent dropouts | DWT cycle counter + RTT | measures without perturbing; `printf` changes the timing |
| **Memory** | corruption, faults, "this variable changes by itself" | hardware watchpoint + the core dump | watchpoint catches the corrupting store; the dump reads the wreckage |
| **Peripheral state** | wrong data from a sensor, a wedged bus | logic analyzer + protocol decoder | reads the truth on the wire, trusts none of your code |
| **Supply / EMI** | brownout resets, edges that glitch, noise-dependent | oscilloscope + near-field probe | analog domain; digital tools cannot see a droop |
| **Heisenbug** | moves or vanishes when observed | non-perturbing tools, pin the schedule | the observation is the perturbation; remove it |

The error a junior makes is universal: reach for `printf` regardless of the family. `printf` is fine for a logic bug you can reason about by watching values flow, but it is the *wrong* tool for four of the five families above — it perturbs timing (kills timing and heisenbug hunts), it cannot run after a fault (kills memory-corruption postmortems), and it tells you nothing about the wire (kills peripheral-state bugs) or the supply (kills EMI bugs). Knowing which tool the symptom calls for is the entire skill, and it is exactly what the bench exam grades.

A worked diagnosis that walks the table: a field unit "randomly reboots every few hours." Family? It faults and reboots → start with the **core dump** (memory family). The dump says exception 3 at `sensor.c:88`, `R0 = 1` (a store to address 1, unaligned), `R5 = 0x10`. That is a memory bug — an off-by-one past a buffer on the 16th iteration. But *why* the 16th iteration only sometimes? The DWT timing log, also persisted, shows an ISR overrun (timing family) right before the crash — the overrun delayed the buffer drain, the buffer filled past its bound, and the overrun-induced overflow scribbled the bad pointer. Two families, one root cause, found by reading the dump *and* the timing — which is why a good crash record carries both.

## 5. Heisenbugs — the bug that moves when you look

A **heisenbug** changes behavior or vanishes when you observe it. It is the most frustrating bug class and the one that most sharply separates seniors from juniors, because the junior's instinct — add a `printf`, set a breakpoint — is exactly the action that hides the bug. The discipline is to understand *why* the bug moves and to choose observation tools that do not move it.

**The two mechanisms a heisenbug moves by:**

1. **Timing perturbation.** A `printf`, a breakpoint, a halt-and-step — each adds time or changes the schedule, and a *race condition* that depended on the original timing no longer fires. Classic: two ISRs that race on a shared flag; adding a `printf` in one slows it enough that the race window closes. The bug is real and still there — your observation just papered over it.
2. **Memory-layout / optimization perturbation.** Adding a `volatile`, changing `-O2` to `-O0` for a debug build, adding a variable that shifts the stack frame — each changes the memory layout or the register allocation, and an *uninitialized-memory read* or a *stack overflow* that happened to read/scribble a particular location now reads/scribbles a different, harmless one. Classic: a function returns a pointer to a stack local; at `-O2` the caller reuses that stack slot and the bug bites, at `-O0` the slot stays untouched and the bug hides. Or: an uninitialized variable that was `0xDEADBEEF` garbage at `-O2` becomes a benign `0` at `-O0` because the debug build zero-fills more.

**The senior's response — observe without perturbing, then pin the system down:**

- **Switch to non-perturbing tools.** RTT instead of UART `printf` (no I/O latency to change the timing). The DWT counter instead of timing-by-print. The logic analyzer instead of a breakpoint (the wire does not care that you are watching). A hardware watchpoint (`watch g_flag`) instead of polling — the watchpoint is silicon comparing the address bus, zero timing cost until it fires.
- **Pin the schedule.** Disable the *other* interrupts so the race has only the two actors you care about. Force the FreeRTOS task order (raise one task's priority, or run the scheduler in a deterministic single-step mode). Run single-core (park core 1) so there is no inter-core race. If the bug survives with the schedule pinned, it is deterministic and you can step it; if it vanishes, you have *proven* it is a scheduling race and narrowed it to those actors.
- **Freeze the variables.** Fix the clock (disable any dynamic frequency scaling). Fix the RNG seed. Fix the input (replay a captured input instead of live data). A deterministic input plus a pinned schedule turns a one-in-a-thousand heisenbug into a reproduce-on-demand bug — and a reproducible bug is a solved bug.
- **Bisect.** With a deterministic reproducer, `git bisect` finds the commit that introduced it; binary-searching the *code* (comment out half, does it still fire?) finds the line. This is Agans's Rule 4 ("Divide and conquer") and Rule 2 ("Make it fail") — make it fail *reliably*, then halve the search space.

Challenge 2 gives you a deliberately flaky firmware (a race in a reconnect state machine, reused from Week 11) and asks you to make it fire on demand, root-cause it with RTT and a watchpoint (never a `printf`), and document the reproduce-then-pin procedure. The lesson is the inversion of instinct: the moment a bug "only happens sometimes" and "goes away when I add a print," stop adding prints and reach for the non-perturbing tools.

A concrete reproduction trick worth internalizing: to *force* a race that needs ISR-fires-inside-a-window, you widen the window deterministically and time the ISR into it. If the worker reads a shared flag and then acts on it, insert a controlled `busy_wait_us(N)` between the read and the act — not a `printf` (which is unpredictable) but a fixed-duration spin — and tune `N` so the periodic ISR lands inside it every cycle. Now the one-in-hundreds race fires on every iteration. The mini-project's BUG_RACE does exactly this (a `busy_wait_us(2)` in the window), turning a true heisenbug into a deterministic one you can step. The deep point: you are not *adding* timing the way a `printf` does (which would close the window); you are adding a *fixed, known* delay that holds the window open long enough to make the race certain. Deterministic perturbation reproduces; unpredictable perturbation hides. That distinction — between a controlled `busy_wait` and an uncontrolled `printf` — is the whole art of reproducing a heisenbug.

One more inversion: sometimes the *fix* is the diagnosis. If wrapping the suspect read-modify-write in a critical section makes the bug vanish *and stay gone under the deterministic reproducer*, you have proven it was a concurrency bug — because a critical section changes nothing except atomicity. A fix that works for a reason you can name is a real fix; a fix that "seems to help" against a still-intermittent bug is a coin flip. Always prove the fix against the deterministic reproducer you built, never against the flaky original.

## 5a. Building a latency histogram with the DWT counter

"What is the worst-case ISR latency" is a min/max question, but "is my ISR's latency *distribution* getting worse under load" is a histogram question, and the DWT counter answers both. A latency histogram is a handful of buckets — say `<1µs`, `1–2µs`, `2–5µs`, `5–10µs`, `>10µs` — each a counter you bump based on the measured cycle delta:

```c
static uint32_t g_hist[5];

static inline void record_latency(uint32_t cycles) {
    uint32_t us = cycles / 125u;          /* 125 MHz -> cycles to microseconds */
    if      (us < 1u)  g_hist[0]++;
    else if (us < 2u)  g_hist[1]++;
    else if (us < 5u)  g_hist[2]++;
    else if (us < 10u) g_hist[3]++;
    else               g_hist[4]++;
}
```

Stream the histogram over RTT every second and you get a live view of the latency *distribution*, not just its extremes. The tell-tale of a creeping problem is a tail that grows — a `>10µs` bucket that was empty at boot and starts ticking up after the WiFi stack comes online tells you the network ISR is stealing cycles from yours. A single min/max would show only that the worst case got worse; the histogram shows you *how often* and *under what conditions*, which is the difference between "there is a problem" and "the problem correlates with WiFi traffic." This is what SEGGER's **SystemView** does graphically over RTT (task switches and ISR timing on a timeline), and if your lab has a J-Link, SystemView is the prettier version of this exact technique. The hand-rolled histogram over OpenOCD's free RTT is the no-cost equivalent and teaches you what SystemView is doing underneath.

## 5b. The nine rules, applied

David Agans's *Debugging* distills decades of practice into nine rules, and this week is several of them in firmware form. The ones that earn their keep at the bench:

- **"Understand the system."** You cannot debug what you do not understand. The eleven weeks of C7 before this one were not just feature-building; they were system-understanding, so that when something breaks you know where the I²C peripheral's clock comes from and what the boot sequence does. The midterm tests exactly this: an *unfamiliar* board, so you must understand it fast from the datasheet.
- **"Make it fail."** A bug you cannot reproduce is a bug you cannot fix. Half of the heisenbug challenge is making the intermittent failure deterministic; the other half is trivial once it is. Never start fixing before you can make it fail on command.
- **"Quit thinking and look."** The single most common junior mistake is to *theorize* about the cause and then code a fix for the theory, without ever confirming the theory against the hardware. The analyzer, the watchpoint, the core dump — all of these are "look" instruments. Look first.
- **"Divide and conquer."** Binary-search the problem space: halt at the midpoint and ask "is the state already wrong here?" If yes, the bug is in the first half; if no, the second. `git bisect` is this rule applied to the commit history.
- **"Change one thing at a time."** The cardinal sin of debugging is changing three things, seeing the bug vanish, and not knowing which change fixed it (or whether you just moved a heisenbug). Change one thing; measure; revert if it did not help.
- **"Keep an audit trail."** This is the runbook. Write down what you tried and what you saw, while you saw it. The audit trail turns this week's bugs into next month's instant diagnoses, and it is why the runbook is a graded deliverable.

These are not platitudes; they are the explicit method behind every exercise this week. The tools (RTT, DWT, the analyzer, the core dump) are how you *execute* "quit thinking and look" and "make it fail" on a chip with no operating system.

## 6. The postmortem workflow, end to end

Tying the week together — the senior's response to "the field unit crashed and rebooted, here is the dump it left":

1. **Pull the dump.** From the device's RTT/UART postmortem print (Lecture 2), or pull a full SRAM image over SWD: `dump binary memory core.bin 0x20000000 0x20042000`.
2. **Identify the fault.** Exception number (3 = HardFault), faulting PC from the stacked frame.
3. **Map PC and backtrace to source.** `arm-none-eabi-addr2line -f -e firmware.elf <pc> <bt...>` against the *exact* ELF that was running (this is why you archive every released ELF with its build).
4. **Read the registers.** The stacked `R0–R3` and the handler-captured `R4–R11` tell you the *values* — the bad pointer in `R0`, the value being written in `R1`, the loop counter in `R5`.
5. **Reconstruct the story.** "Store of `0xDEADBEEF` to address `1`, at `sensor.c:88`, in a call chain from `main` → `process_sample` → `sensor_read`, with `R5=0x10` suggesting the 16th loop iteration." Now you know where to add the bounds check.
6. **Write the runbook entry.** "Symptom: reboot loop with exception 3 at `sensor.c:88`. Cause: off-by-one past the sample buffer. Fix: clamp index. Detection: the DWT timing also showed an ISR overrun just before the crash."

That sequence is the same one a Linux engineer runs with `gdb program core` — and it is exactly what the midterm bench exam grades: not "did you write the driver" but "when it broke, did you reason from what the chip told you."

A final tactical note on reading registers in a dump. The stacked `R0–R3` are the *arguments and scratch* at the fault — `R0` is very often the bad pointer (the first argument to the load/store), `R1` the value being written, `R2`/`R3` the next arguments. The handler-captured `R4–R11` are the *locals the compiler kept in registers* — loop counters, buffer base pointers, the `this` pointer of a C++ method. So a dump where `R0` is a small odd number (an unaligned or near-null address) and `R1` is a recognizable sentinel (`0xDEADBEEF`, `0xA5A5A5A5`, a known struct value) almost always reads as "a store of `R1` to the bad address in `R0`," and you can name the bug before you even run `addr2line`. Learning to read the register values *as a story* — destination, value, counters — is what makes a senior glance at a dump and say "off-by-one on the sample buffer, sixteenth iteration" in ten seconds. That fluency is the deliverable of this week.

## 6a. When RTT is the wrong tool too

RTT is not a panacea, and a senior knows its limits. Three cases where RTT does not help:

- **You have no probe attached.** RTT needs the debug probe to drain the ring; an unattended field unit with nothing on its SWD port logs into a ring that fills and (in skip mode) drops. For the field you need persistence — the crash dump (Lecture 2) for faults, or a flash-backed log for non-fault events. RTT is a *bench* tool; it shines when the probe is connected and you are watching.
- **The bug is in the SWD path itself.** If the firmware reconfigures the `SWCLK`/`SWDIO` pads as GPIO (the RP2040 lets you), the probe loses the port and RTT goes dark mid-session — the same hazard as losing GDB. If your RTT stream freezes exactly when a certain code path runs, suspect a pad reconfiguration, not a logic bug.
- **You need it after the firmware has wedged the bus or the clocks.** RTT's ring lives in SRAM and the probe reads it over SWD, both of which survive a wedged peripheral — *unless* the wedge took out the debug clock domain. In the rare case the core's debug logic is itself stuck, only a `monitor reset halt` (which RTT cannot do) recovers you. RTT observes a running system; it cannot un-stick a stuck one.

The honest framing: RTT replaces UART `printf` for *bench logging of a running, probe-attached system*, which is most of your debugging time. It does not replace the crash dump (for after-the-fault forensics), the analyzer (for the wire), or `monitor reset halt` (for recovery). Reach for the right one.

## 6b. The J-Link comparison, for completeness

The commercial SEGGER J-Link is the gold-standard probe, and knowing what it buys over the free second-Pico debugprobe sharpens your sense of the trade-offs:

- **Native RTT** — the J-Link software (`JLinkRTTViewer`, `JLinkRTTLogger`) implements RTT directly and fast; OpenOCD's RTT works but polls more slowly. For high-rate RTT logging the J-Link is noticeably better.
- **SystemView** — SEGGER's free-for-non-commercial RTOS/ISR timeline visualizer runs only over a J-Link (or a J-Link-compatible probe). It is the prettiest version of this week's DWT-timing work.
- **Faster flash and higher SWD clock** — a J-Link programs flash and steps faster, which matters in a tight edit-flash-debug loop but not for correctness.
- **Unlimited flash breakpoints** — the J-Link's "flash breakpoints" feature patches flash on the fly to give you more than the 4 hardware breakpoints, by managing the erase/reprogram for you. On the bare debugprobe you are stuck at 4.

None of this changes the *methods* this week teaches — the fault model, the stacked frame, RTT's mechanism, the DWT counter, the analyzer workflow — all of which are identical regardless of probe. The free second-Pico debugprobe does everything the graded work needs; the J-Link is a productivity upgrade, not a capability the course requires. Mention it so you know what the next rung up looks like; do not feel you need it.

## 6c. A field-debugging note: when you cannot attach a probe

Most of this lecture assumes a probe on the bench. The hardest debugging — and the most senior — is the field unit that crashed once, three weeks ago, in a customer's basement, and all you have is whatever it persisted. This is where the whole week converges. The crash dump (Lecture 2) is your core file. A flash-backed event log (a ring of recent state transitions, written periodically from a known-good state, *not* from the fault handler) is your `dmesg`. The DWT-derived high-water marks and ISR-max timings, snapshotted into that log, are your performance counters. None of it needs a probe at fault time; all of it is read back later, on the bench, with the probe attached or even just over the unit's normal serial/network channel.

The design discipline that makes field debugging possible is *built in before the crash*: reserve the no-init region, install the fault handler, instrument the timing, log the state transitions — all of it in the shipping firmware, costing a few hundred bytes of SRAM and a few cycles per event. A product that ships without this is a product whose field failures are unsolvable; a product that ships with it turns a basement crash into a five-minute `addr2line`. That is the senior's foresight: you do not debug the field unit *after* it crashes; you build the instrumentation *before* it ships, so that when it crashes it tells you why. Everything this week — the dump, RTT, the DWT counter, the runbook — is in service of that one outcome.

This is also the bridge to where C7 goes next. Production telemetry (services like Memfault) is exactly this idea industrialized: the device captures coredumps and metrics, batches them, and uploads them so a fleet's crashes aggregate into a dashboard where you see "37 units hit the same fault at `tls.c:204` after the last OTA." The mechanism is identical to what you build this week — a fault handler, a persisted dump, an `addr2line`-grade symbol archive — scaled to a fleet and automated. Master it on one Pico on the bench and the fleet version is just plumbing. The hard part, the reasoning-from-the-dump, is the same whether it is one device in front of you or ten thousand in the field.

## 7. Summary

RTT moves logs off-chip in a few cycles per byte with no UART latency, works in an ISR and after a fault, and is the RP2040's only printf-free trace path (the M0+ has no SWO/ITM). The DWT cycle counter times regions to 8 ns resolution without perturbing the timing the way a `printf` would, and is how you catch jitter and ISR overruns. The logic analyzer reads the truth on the wire — protocol-decoded into START/ACK/data — and answers "did my bytes leave and did the peripheral reply" without trusting any of your code; it is how you diagnose and recover a hung I²C bus with the nine-clock bus-clear. And the heisenbug discipline — observe without perturbing, pin the schedule, freeze the variables, bisect — is the methodology that turns a bug-that-moves-when-you-look into a reproduce-on-demand bug you can actually fix. Tomorrow is the mini-project: a crash-dump server plus a five-bug pentathlon where you apply all of this against firmware someone else broke on purpose. Allocate 6 hours and keep your runbook open.

## References for this lecture

- SEGGER RTT documentation and the BSD `SEGGER_RTT.c`/`.h` reference sources. <https://www.segger.com/products/debug-probes/j-link/technology/about-real-time-transfer/>
- OpenOCD user guide, "RTT" chapter (`rtt setup`, `rtt start`, `rtt server`). <https://openocd.org/doc/html/index.html>
- Cortex-M0+ Technical Reference Manual, DWT chapter (the cycle counter), DDI0484. <https://developer.arm.com/documentation/ddi0484/latest/>
- RP2040 datasheet §4.3 "I²C", pp. 446–490; §2.4 (no ITM/SWO on the M0+). <https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf>
- NXP UM10204 "I²C-bus specification and user manual" rev. 7.0, §3.1.10 (START/STOP), §3.1.16 (Bus clear). <https://www.nxp.com/docs/en/user-guide/UM10204.pdf>
- sigrok protocol decoders. <https://sigrok.org/wiki/Protocol_decoders>
- David J. Agans, *Debugging: The 9 Indispensable Rules*, AMACOM, 2002.
- Memfault Interrupt, "How to debug a HardFault" and the heisenbug-adjacent timing posts. <https://interrupt.memfault.com/>
