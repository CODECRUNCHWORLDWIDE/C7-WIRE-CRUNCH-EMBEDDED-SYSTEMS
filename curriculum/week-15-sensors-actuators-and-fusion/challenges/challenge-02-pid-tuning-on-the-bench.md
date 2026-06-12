# Challenge 2 — PID Tuning on the Bench

## Brief

Take the closed-loop balance controller from the mini-project and tune its PID
gains *methodically* — not by random twiddling — using a step-response procedure
and the Ziegler-Nichols ultimate-gain method. Capture the step responses,
characterize the loop (rise time, overshoot, settling time, steady-state error),
and document the tuning so someone else could reproduce your gains. Then
deliberately break the loop in three ways and explain the failure of each.

You should spend ~3 hours on this challenge. The deliverable is the tuning data,
the step-response plots, and a writeup `PID-TUNING.md`.

## Why methodical tuning

"Twiddle the knobs until it balances" works once, on your chassis, on your floor,
and never again. A methodical tuning gives you gains with a known stability
margin and a documented procedure that survives a hardware change. Every control
engineer learns one systematic method; this challenge teaches you the simplest
one (Ziegler-Nichols) and the discipline of measuring the response instead of
eyeballing it.

## Setup

You have:

- The mini-project's `fusion_node.c` building and flashing.
- A balancing chassis (motor + wheels), OR a test rig where you can apply a known
  angular step (a hinged platform with a stop at a known angle works) and read the
  motor command without the motor actually driving (open the motor leads, log the
  commanded duty).
- `cc-orientation.py` logging the CSV stream (pitch and motor_cmd).

## Procedure

### Phase 1 — Characterize the open-loop plant

Before tuning the controller, understand the thing you are controlling. With the
control loop disabled (force `motor_cmd = 0`), tilt the platform to a small angle
and release it. Log pitch-vs-time. The platform falls — measure how fast. The
natural fall time (time to go from 5° to 45°, say) sets the timescale your
controller must beat. A controller slower than the plant's fall time can never
stabilize it. Record the fall time; your loop at 100 Hz has ~100 corrections
inside that window, which is plenty.

### Phase 2 — Find the ultimate gain (Ziegler-Nichols)

1. Set `ki = 0`, `kd = 0`. Pure proportional.
2. Start `kp` small. Increase it in steps (double each time, then fine-tune)
   until the platform oscillates with a *constant* amplitude — neither growing
   nor decaying. This `kp` is the **ultimate gain** `Ku`.
3. Measure the **oscillation period** `Tu` from the logged pitch trace (peak to
   peak time).

Record `Ku` and `Tu`. The Ziegler-Nichols PID rule gives a starting point:

```text
Kp = 0.6 * Ku
Ki = 1.2 * Ku / Tu
Kd = 0.075 * Ku * Tu
```

(These are the classic Z-N "PID" coefficients; the "no-overshoot" and
"Pessen-integral" variants trade overshoot for settling time — try them too.)

### Phase 3 — Apply and measure the step response

Set the Z-N gains. Apply a step disturbance (tap the platform to a small angle,
or change the setpoint by 5°) and log the pitch response. Characterize:

- **Rise time** — time from 10% to 90% of the step.
- **Overshoot** — peak excursion past the final value, as a percentage.
- **Settling time** — time to stay within ±2% of the final value.
- **Steady-state error** — residual offset after settling (should be ~0 with the
  integral term).

A representative well-tuned response (your numbers will differ with chassis):

```text
Ku = 0.11, Tu = 0.42 s
Z-N gains: Kp = 0.066, Ki = 0.314, Kd = 0.0035

step response (5 deg disturbance):
  rise time:          120 ms
  overshoot:          12%
  settling time (2%): 480 ms
  steady-state error: 0.1 deg
```

### Phase 4 — Refine

Z-N is a starting point, usually a bit aggressive (it targets ~25% overshoot).
Refine by hand from there:

- Too much overshoot → raise `Kd` or lower `Kp`.
- Sluggish → raise `Kp`.
- Slow residual lean → raise `Ki` (watch for windup-induced overshoot).

Re-measure the step response after each change and tabulate. The point is to show
the *trajectory* of your tuning, not just the final gains.

### Phase 5 — Break it three ways

Deliberately misconfigure the loop and document each failure from the logged
trace:

1. **Integrator windup.** Remove the anti-windup clamp (set `integ_min/max` to
   ±1e9). Apply a large, sustained disturbance that saturates the motor. Observe
   the long overshoot when the disturbance is removed — the integrator unwinds
   slowly. Show the trace; explain why the clamp fixes it.
2. **Derivative on noise.** Compute the D term on the *error* (and thus on the
   noisy fused angle) instead of on the gyro rate / measurement. Observe the
   motor command chattering as the derivative amplifies sensor noise. Show the
   trace; explain why "derivative on measurement" (and the gyro as a clean rate
   source) avoids it.
3. **Loop too slow.** Drop the loop rate to 20 Hz (insert a delay). Observe the
   loop losing phase margin — oscillation grows, the platform falls. Show the
   trace; explain the relationship between loop rate, plant fall time, and phase
   margin.

## Deliverable

`PID-TUNING.md` (1500–2500 words) plus the logged CSVs and the step-response
plots. Required sections:

1. **Plant characterization** — the open-loop fall time and what it implies.
2. **Ultimate-gain method** — `Ku`, `Tu`, and how you found them, with the
   constant-amplitude oscillation trace.
3. **Tuning trajectory** — the sequence of gains you tried and the step-response
   metrics for each, in a table.
4. **Final gains** — the tuned values and the step response, with a plot.
5. **Three failures** — windup, derivative-on-noise, slow-loop, each with a trace
   and an explanation.
6. **References** — Lecture 3, Ziegler-Nichols, Åström & Murray.

Commit with a message like `week-15/challenge-02: PID tuning, Z-N + three failures`.

## Pass criteria

- `Ku` and `Tu` are measured from real logged traces, not guessed.
- The step-response metrics (rise, overshoot, settling, steady-state) are
  reported for at least three gain sets, showing the tuning trajectory.
- All three deliberate failures are demonstrated with traces and correctly
  explained (windup → clamp, derivative-on-error → chatter, slow loop → phase
  margin).
- The final gains produce a step response with < 20% overshoot and < 1 s settling.

## Why this challenge matters

PID is the most-deployed control law on Earth and the most-misunderstood. Every
junior engineer twiddles gains until something works; every senior engineer
measures the step response and knows their stability margin. The three failures
in Phase 5 are the three bugs you will see in every under-engineered control loop
in the wild — a motor that overshoots after a saturation, a command that chatters
on noise, a loop that oscillates because it runs too slowly. Recognizing them
from a trace, on the bench, is the skill this challenge builds.

## References

- Lecture 3 (PID, anti-windup, derivative-on-measurement, loop-rate/phase-margin).
- Ziegler & Nichols, "Optimum Settings for Automatic Controllers", Trans. ASME, 1942.
- Åström & Murray, *Feedback Systems*, Princeton 2008 — Ch. 10 (PID), the
  anti-windup and derivative-filtering sections.
