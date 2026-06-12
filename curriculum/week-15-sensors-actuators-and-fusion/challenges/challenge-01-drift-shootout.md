# Challenge 1 — The Drift Shootout

## Brief

Run all three fusion filters from this week — complementary, 1-D Kalman, and
Madgwick MARG — against the *same* logged IMU dataset, plus a gyro-only baseline,
and quantify how much each one drifts over a 10-minute stationary run and how
fast each one tracks a step disturbance. Produce a head-to-head report with
plots. The goal is not to declare a winner (each has its place) but to *feel*,
in numbers, why fusion beats integration and why each filter trades accuracy for
complexity differently.

You should spend ~2 hours on this challenge. The deliverable is a markdown report
`DRIFT-SHOOTOUT.md` plus the plots and the replay code.

## Why a logged dataset and not live runs

If you run each filter live on the bench, you cannot compare them fairly — the
board's motion differs between runs, the temperature drifts, the bias changes.
The honest comparison feeds *one* recorded dataset through *all* filters offline,
so the only variable is the algorithm. This is standard practice for filter
evaluation and it is why every fusion paper publishes against a fixed dataset
(EuRoC, TUM-VI, or the author's own logged IMU stream).

## Setup

You have already completed:

- Exercise 1 (calibrated read path), Exercise 2 (complementary), Exercise 3
  (1-D Kalman), and the mini-project's `madgwick.c`.
- A calibrated MPU-9250 on the bench.

## Procedure

### Phase 1 — Log a dataset

Build a logging firmware that streams the *calibrated SI-unit* sample stream
(`ax,ay,az,gx,gy,gz,mx,my,mz,dt`) over CDC at 100 Hz. Capture two datasets:

1. **Stationary, 10 minutes.** Board flat on a solid surface, untouched. This
   isolates drift — the true orientation is constant, so any change in the
   estimate is error.
2. **Stepped, 2 minutes.** Board flat, then tilted to a known angle (use a
   protractor or a 3-D-printed 30°/45°/60° wedge), held, returned to flat,
   repeated for several angles. This measures tracking accuracy and step
   response.

Record both with `cc-orientation.py --log` adapted to capture the raw calibrated
stream (or a dedicated logging build). Commit the CSVs.

### Phase 2 — Replay through all four estimators

Write a host program (`replay.c`, built with `-DHOST_TEST` against the exercise
sources, or a Python port) that reads a CSV dataset and runs:

- **Gyro-only:** integrate `gx`, `gy` into roll, pitch; integrate `gz` into yaw.
  No correction. The baseline that drifts.
- **Complementary:** `cc_complementary_update` per the exercise.
- **1-D Kalman:** `cc_kalman1d_update` per axis (roll, pitch).
- **Madgwick MARG:** the mini-project's `madgwick_update_marg`, then
  `quat_to_euler`.

Emit, for each estimator, a CSV of `t, roll, pitch, yaw`.

### Phase 3 — Quantify

For the **stationary** dataset, the true orientation is constant. Compute for
each estimator:

- **Roll/pitch drift:** max deviation from the initial value over 10 minutes.
- **Yaw drift:** total yaw change over 10 minutes (deg) and the rate (deg/min).
- **Noise:** standard deviation of the estimate over a 60-second window.

For the **stepped** dataset, compute for each estimator:

- **Steady-state error:** estimated angle minus true wedge angle, once settled.
- **Rise time:** time to reach 90% of the step.
- **Overshoot:** peak excursion past the final value.

Tabulate all of this. A representative result (your numbers will differ):

| Estimator      | Pitch drift (10 min) | Yaw drift (10 min) | Noise (sd) | Step rise | Overshoot |
|----------------|---------------------:|-------------------:|-----------:|----------:|----------:|
| Gyro-only      | 18.4 deg             | 96.0 deg           | 0.02 deg   | instant   | 0%        |
| Complementary  | 0.6 deg              | (no yaw ref)       | 0.35 deg   | 80 ms     | 4%        |
| 1-D Kalman     | 0.4 deg              | (no yaw ref)       | 0.20 deg   | 110 ms    | 2%        |
| Madgwick MARG  | 0.3 deg              | 1.1 deg            | 0.30 deg   | 95 ms     | 5%        |

### Phase 4 — Plot and interpret

Produce two overlay plots: pitch-vs-time for the stepped dataset (all four
estimators on one axis, plus the true wedge angle as a staircase) and yaw-vs-time
for the stationary dataset (gyro-only versus Madgwick, to make the drift gap
visceral). Then write the interpretation:

1. **Why gyro-only drifts so much in yaw.** No yaw reference exists in accel
   (gravity is symmetric about vertical), so only the magnetometer pins yaw —
   and only Madgwick MARG uses it. Quantify the gyro-only yaw drift in deg/min
   and tie it to the measured gyro bias from your calibration.
2. **Why the complementary and Kalman filters have no yaw column.** They are
   2-axis (roll/pitch) here; the magnetometer would need to be fused in
   separately, which is exactly the cross-coupling that pushed the field toward
   quaternion filters.
3. **Why Madgwick's roll/pitch noise is comparable to the complementary
   filter's but its yaw is bounded.** The gradient step is doing the same job as
   the complementary low-pass, just in quaternion space and with the mag adding
   the heading constraint.
4. **When you would still pick the complementary filter.** Minimum code,
   roll/pitch only, no magnetometer, hard real-time on a part with no FPU and no
   cycles to spare.

## Deliverable

`DRIFT-SHOOTOUT.md` (1500–2500 words) plus the CSVs, the replay code, and the
two overlay plots. Required sections:

1. **Datasets** — how you logged them, board, surface, temperature.
2. **Method** — the four estimators, identical input, offline replay.
3. **Results table** — the drift/noise/step metrics for all four.
4. **Plots** — the two overlays with captions.
5. **Interpretation** — the four questions above, answered with your numbers.
6. **References** — Lecture 2, Madgwick, the Allan-variance framing.

Commit with a message like `week-15/challenge-01: drift shootout, 4 estimators`.

## Pass criteria

- All four estimators run against the identical logged dataset (no live re-runs).
- The metrics table is complete and the numbers are internally consistent (the
  gyro-only yaw drift must match the measured gyro bias times the run length).
- The two overlay plots are present and legible.
- The interpretation correctly explains why only Madgwick MARG bounds yaw drift.

## Why this challenge matters

Every fusion paper and every IMU vendor's marketing claims "low drift". The skill
that separates a firmware engineer who can ship attitude estimation from one who
copies a library blindly is the ability to *measure* drift on real hardware, with
a fair offline comparison, and to know which axis your application actually leans
on. Ten minutes of stationary logging and an honest overlay plot tells you more
about your sensor than any datasheet number.

## References

- Lecture 2 (complementary, Kalman, Madgwick).
- Madgwick, Univ. of Bristol 2010.
- Allan-variance framing in Lecture 2 §7.
- EuRoC MAV dataset (Burri et al., IJRR 2016) — the standard public IMU dataset,
  if you want a reference stream to validate your replay code.
