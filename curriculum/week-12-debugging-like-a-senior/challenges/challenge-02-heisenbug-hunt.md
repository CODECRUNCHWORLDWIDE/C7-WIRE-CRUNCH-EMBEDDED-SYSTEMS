# Challenge 2 — Heisenbug Hunt: Make It Fail On Demand

## Brief

Take a deliberately flaky firmware — a race in a reconnect state machine — and turn a one-in-hundreds intermittent failure into a reproduce-on-demand bug, root-cause it using only **non-perturbing** tools (RTT, a hardware watchpoint, the DWT counter — never a `printf` or a breakpoint that changes the timing), and document the reproduce-then-pin procedure. This is the conceptual capstone of the week and the single most senior skill on the syllabus: a bug that moves when you look at it is not magic, it is timing- or layout-sensitive, and the discipline is to observe without moving it.

You should spend ~3 hours on this challenge. The deliverable is your modified firmware plus a writeup `HEISENBUG-HUNT.md`.

## The flaky firmware

You are given (`heisenbug_target.c`, in the challenge directory of the mini-project) a state machine modeled on the Week 11 WiFi reconnect logic. It has a shared `volatile` connection-state variable updated by two contexts: a periodic timer ISR (the "link supervisor" that marks the link down on a timeout) and the main loop (the "reconnect worker" that drives the association). There is a planted race: under a specific interleaving, the worker reads `state == DOWN`, the ISR then sets `state == UP` (a spurious recovery), and the worker — having already decided to reconnect — tears down a connection that just came up, leaving a leaked socket handle and a state machine stuck in `RECONNECTING` forever.

The bug fires roughly once every few hundred reconnect cycles. It is a textbook heisenbug:

- Add a `printf` in the worker → the bug nearly vanishes (the print slows the worker so the ISR's window closes).
- Set a breakpoint in the ISR → the bug never reproduces (the halt completely reorders the timing).
- Build at `-O0` for "easier debugging" → the bug rate changes (different register allocation, different timing).

Your job is to defeat all three of those traps.

## Procedure

### Phase 1 — Confirm the heisenbug behavior

First, *experience* the observer effect so your writeup describes it from data, not theory.

1. Run the firmware as-is and let it churn reconnect cycles. Instrument a *non-perturbing* counter (a plain `volatile uint32_t g_bug_count` incremented when the stuck state is detected, read out later) and record the failure rate over, say, 5 minutes — e.g., "37 failures in 12,000 cycles."
2. Now add a `printf("worker: state=%d\n", state)` in the worker loop. Re-measure the rate. Record how much it drops (often to near zero).
3. Set a GDB breakpoint in the ISR, `continue`, and try to reproduce. Record that it does not.

You now have three data points proving the bug is timing-sensitive. **Remove the `printf` and the breakpoint** — they are the enemy. Everything from here uses non-perturbing observation.

### Phase 2 — Make it fail on demand (pin the schedule)

Agans's Rule 2 is "Make it fail." A one-in-hundreds bug is unfixable; a reproduce-on-demand bug is half-solved. Pin the system down until the race fires every time:

1. **Narrow the actors.** Disable every interrupt except the link-supervisor timer. Now only two contexts touch `state`: that one ISR and the main loop. If the bug survives, it is genuinely between those two; if it vanishes, a third actor was involved (record which).
2. **Force the interleaving.** You know the race needs the ISR to fire in a specific window of the worker. Widen that window deterministically: insert a *controlled* delay (a `busy_wait_us`, not a `printf`) at the exact point between the worker's read of `state` and its act on it. Tune the delay so the timer ISR lands inside it every cycle. Now the race fires every reconnect cycle — you have made it deterministic.
3. **Fix the clock and the input.** Disable any frequency scaling, fix the reconnect period, replay the same "link up/down" sequence each cycle. A deterministic input plus a pinned schedule = a reproducible bug.

Document the exact configuration that makes it fire 100% of the time. That configuration *is* the bug report.

### Phase 3 — Root-cause with non-perturbing tools

Now find the corrupting interleaving using only tools that do not move the bug:

1. **Hardware watchpoint on `state`.** `watch state` (using one of the two RP2040 DWT watchpoints). It halts the core the instant *anything* writes `state`, with zero timing cost until it fires (it is silicon comparing the address bus). Each halt, record who wrote it (`bt` shows ISR vs worker) and the value. The corrupting sequence — worker reads DOWN, ISR writes UP, worker acts on its stale DOWN read — shows up as a specific write-ordering across the watchpoint halts. Note: the watchpoint *does* halt on the write, which perturbs timing after that point — but you have already pinned the schedule so the race fires deterministically up to the watchpoint, so the halt no longer hides it. This is the key trick: pin first, then a watchpoint is safe.
2. **RTT trace of the state transitions.** Instead of `printf`, log every state transition over RTT (Exercise 2) with a timestamp from the DWT counter. RTT's ~200-cycle cost is two orders of magnitude below the UART `printf` that hid the bug, so the trace barely perturbs the timing. The RTT log shows the exact transition sequence: `DOWN -> (worker decides reconnect) -> UP (ISR) -> teardown (worker) -> stuck`.
3. **DWT-timestamped sequence.** Correlate the RTT transitions with DWT cycle counts to *measure* the race window — how many cycles between the worker's read and the ISR's write. This quantifies the bug and proves your fix closes the window.

### Phase 4 — Fix it and prove the fix

The bug is a non-atomic read-modify-write on shared state. The correct fix is one of:

- **A critical section** around the worker's read-decide-act on `state` (mask the supervisor interrupt for the few instructions, or use the SDK's `critical_section_t`). This makes the worker's decision atomic with respect to the ISR.
- **A proper synchronization primitive** — if this were under FreeRTOS (Week 6), a mutex or a queue instead of a shared `volatile`. The `volatile` keyword guarantees the *compiler* re-reads memory; it guarantees *nothing* about atomicity across contexts, which is the bug's root.

Implement your fix, then **prove** it: re-run the pinned, deterministic, fires-every-cycle configuration from Phase 2. The bug must now fire **zero** times across the same cycle count that previously produced a failure every cycle. A fix you cannot prove against a deterministic reproducer is a guess.

### Phase 5 — Write the runbook entry

Distill the hunt into a runbook entry a teammate could follow:

> **Symptom:** reconnect state machine occasionally stuck in RECONNECTING; rate drops when logging is added.
> **First reach for:** the fact that it moves under observation → it is a timing race. Stop adding prints.
> **Reproduce:** pin the schedule (disable other IRQs, insert a controlled delay in the race window, fix the clock/input) until it fires every cycle.
> **Root-cause:** `watch state` (hardware watchpoint) + RTT-with-DWT-timestamps to capture the corrupting write order.
> **Cause:** non-atomic read-modify-write on shared `volatile` state across ISR and main loop. `volatile` ≠ atomic.
> **Fix:** critical section (or mutex/queue under an RTOS) around the read-decide-act.
> **Verify:** the pinned reproducer fires zero times after the fix.

## Deliverable

1. `heisenbug_fixed.c` — your modified firmware: the pinning harness (Phase 2), the RTT/DWT instrumentation (Phase 3), and the fix (Phase 4), each clearly delimited so the grader can see all four.
2. `HEISENBUG-HUNT.md` (1500–2500 words) with:
   - **The observer effect, measured** — the three data points from Phase 1 (baseline rate, rate-with-printf, no-repro-with-breakpoint).
   - **Making it deterministic** — the exact pinned configuration that fires every cycle.
   - **The corrupting interleaving** — the watchpoint + RTT evidence of the bad write order, with the DWT-measured race window.
   - **The fix and its proof** — zero failures over the previously-failing cycle count.
   - **The runbook entry** — the distilled procedure above, in your words.

Commit with a message like `week-12/challenge-02: heisenbug reproduce, root-cause, fix, prove`.

## Pass criteria

- Phase 1 shows, with numbers, that the bug rate changes under perturbing observation.
- Phase 2 produces a configuration that fires the bug 100% of the time (deterministic reproducer).
- The root-cause uses **only** non-perturbing tools (watchpoint, RTT, DWT). A `printf`-based root-cause does not pass — the whole point is that `printf` hides this bug.
- The fix drives the failure count to **zero** over the deterministic reproducer's cycle count, demonstrated.
- The runbook entry correctly identifies "`volatile` is not atomicity" as the root cause class.

## Why this challenge matters

Every engineer who has shipped firmware has lost a day to a bug that vanished when they added a `printf`. The junior concludes "it's flaky, ship it and hope." The senior recognizes the disappearance as *information* — the bug is timing-sensitive — and inverts their instinct: stop perturbing, pin the system down until it is deterministic, then observe with tools that do not move it. This is the highest-leverage debugging skill there is, and it is the one the midterm bench exam most wants to see, because an unfamiliar board under time pressure is exactly where heisenbugs breed. Make the bug fail on demand, and you have already won.

## References

- David J. Agans, *Debugging: The 9 Indispensable Rules*, AMACOM, 2002 — Rule 2 ("Make it fail"), Rule 4 ("Divide and conquer"), Rule 5 ("Change one thing at a time").
- Lecture 3, §5 (heisenbugs) and §2 (DWT cycle counter), §1 (RTT).
- "volatile considered harmful" (LWN / kernel docs on why `volatile` is not a concurrency primitive). <https://www.kernel.org/doc/html/latest/process/volatile-considered-harmful.html>
- RP2040 datasheet §2.4 (the two DWT watchpoint comparators backing `watch`).
