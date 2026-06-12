# Exercise Solutions — Week 15

This document walks each exercise's reference solution and lists the canonical bugs students hit. Read it after you have attempted each exercise — not before. Every exercise builds two ways: a `HOST_TEST` build that runs on your laptop against synthetic or hand-computed data (fast iteration, no hardware), and an on-device build that runs against a live MPU-9250 on the Pico's I2C0 bus.

---

## Exercise 1: MPU-9250 Read and Calibrate

### What the exercise asks

Bring up the MPU-9250 over I2C, read raw accel/gyro/mag counts, measure the gyro bias over 512 stationary samples, and convert everything to SI units (m/s², rad/s, microtesla) through a calibration struct. The pure conversion function `cc_imu_apply_calibration` is host-testable; the device main streams calibrated samples over CDC.

### Reference solution

The full source is in `exercise-01-imu-read-and-calibrate.c`. The load-bearing function is `cc_imu_apply_calibration`. The arithmetic chain for each sensor is fixed and worth memorizing:

- **Accel:** `counts / accel_sens` gives g; `× GRAVITY` gives m/s²; then subtract the per-axis offset and multiply the per-axis scale. The offset and scale come from a six-position tumble calibration (see Lecture 1 §3).
- **Gyro:** `counts / gyro_sens` gives deg/s; `× DEG_TO_RAD` gives rad/s; then subtract the measured bias.
- **Mag:** `counts × UT_PER_LSB × ASA` gives microtesla, where `ASA` is the AK8963's factory sensitivity adjustment read once from fuse ROM; then subtract the hard-iron offset and apply the diagonal soft-iron scale.

The byte order is the subtle part: the MPU's accel/gyro registers are **big-endian** (`cc_be16`), but the AK8963 magnetometer registers are **little-endian** (`cc_le16`). Mixing these up is the single most common bug and it produces magnetometer readings that look almost-but-not-quite right, which is worse than obviously broken.

### Expected output

Host test:

```text
=== Exercise 1 host conversion test ===
az: 9.8066 m/s^2 (expect 9.8066)
gx: 0.017453 rad/s (expect 0.017453)
mx: 15.0000 uT (expect 15.0000)
gx after bias subtraction: 0.000000 rad/s (expect ~0)

PASS: 0 failure(s).
```

On-device (board flat on the bench, Z pointing up):

```text
=== Exercise 1: MPU-9250 read + calibrate ===
AK8963 ASA: 1.184 1.188 1.156
Measuring gyro bias over 512 samples — keep the board STILL...
Gyro bias (rad/s): -0.00214 0.00131 -0.00077
a=[ -0.04  0.12  9.79] m/s^2  g=[ 0.001 -0.000  0.002] rad/s  m=[ 23.4 -8.1 -41.2] uT  dt=0.0101
```

The Z accel reading near +9.79 m/s² with X and Y near zero confirms a correct accel decode and a board that is level. The gyro near zero after bias subtraction confirms the bias measurement worked.

### The canonical bugs

1. **Magnetometer read as big-endian.** The AK8963 stores `HXL` (low byte) before `HXH` (high byte) — little-endian, the opposite of the MPU's accel/gyro. Reading it big-endian byte-swaps every axis and the heading is garbage. Use `cc_le16` for mag, `cc_be16` for accel/gyro.
2. **Forgetting the ST2 read on the AK8963.** The AK8963 latches its data until you read `ST2` (register 0x09). If you read only the 6 data bytes and never `ST2`, the magnetometer stops updating after the first sample — you read the same stale value forever. The reference code reads 7 bytes (`HXL`..`ST2`) in one block.
3. **Calibrating the gyro bias while the board moves.** Even a desk tap during the 512-sample bias measurement skews the mean. If your steady-state gyro reads a constant non-zero rate after calibration, redo the bias measurement on a solid surface.
4. **Not enabling bypass before talking to the AK8963.** The AK8963 sits behind the MPU-9250's internal I2C master. Without `INT_PIN_CFG` bit 1 (`BYPASS_EN`) set, the AK8963 is invisible at 0x0C and the WIA read fails. The reference code writes `0x02` to `INT_PIN_CFG`.
5. **Treating ASA as a raw byte instead of the mapped factor.** The datasheet formula is `adj = (ASA - 128) / 256 + 1`. Using the raw ASA byte directly (values around 176) inflates the magnetometer by ~170×.

---

## Exercise 2: Complementary Filter

### What the exercise asks

Fuse the calibrated gyro and accelerometer into a stable roll/pitch estimate with a one-line-per-axis complementary filter. The synthetic host test holds a true 30° roll with a 2°/s gyro bias and confirms the filter stays within 2° of truth for 60 seconds — while a naive gyro integration drifts ~120° away.

### Reference solution

The full source is in `exercise-02-complementary-filter.c`. The core is `cc_complementary_update`:

```c
float roll_accel  = atan2f(s->ay, s->az);
float pitch_accel = atan2f(-s->ax, sqrtf(s->ay*s->ay + s->az*s->az));
float roll_gyro   = cf->roll  + s->gx * dt;
float pitch_gyro  = cf->pitch + s->gy * dt;
cf->roll  = alpha * roll_gyro  + (1.0f - alpha) * roll_accel;
cf->pitch = alpha * pitch_gyro + (1.0f - alpha) * pitch_accel;
```

The one refinement over the textbook filter: when the accelerometer magnitude deviates from 1 g by more than 15%, the board is accelerating linearly and the accel tilt is momentarily untrustworthy, so we push `alpha` toward 0.999 for that step (lean almost entirely on the gyro). This single guard is the difference between a filter that survives a robot driving over a bump and one that lurches every time it hits anything.

### Expected output

Host test:

```text
=== Exercise 2 host complementary-filter test ===
true roll = 30.0 deg, gyro bias = 2.0 deg/s, 60 s @ 100 Hz

complementary roll after 60 s: 30.04 deg (error 0.04 deg)
naive gyro-only roll after 60 s: 30.08 deg (drifted 120.1 deg)

PASS: complementary filter rejected the gyro bias.
```

The naive-integration line is the lesson: 2°/s of bias over 60 s is 120° of drift. The complementary filter's low-pass accel path pins the estimate to gravity and the drift never accumulates.

### The canonical bugs

1. **`atan2` argument order swapped.** `atan2(az, ay)` instead of `atan2(ay, az)` rotates your roll reference by 90°. The board reads level when it is on its side. Always `atan2(ay, az)` for roll, `atan2(-ax, hypot(ay,az))` for pitch.
2. **Using degrees in the gyro integration but radians in the accel angle (or vice versa).** Mixed units make the blend nonsensical — the gyro term and the accel term are in different scales. Keep everything in radians internally; convert to degrees only at the print statement.
3. **`alpha` too low.** With `alpha = 0.5`, the filter trusts the accel as much as the gyro and every vibration shakes the estimate. For a 100 Hz loop, `alpha` between 0.95 and 0.99 is the usable range; below 0.9 the output is unusably noisy.
4. **No linear-acceleration guard.** Without the magnitude check, a robot that accelerates forward tips its estimated pitch because the accel now reads gravity + forward acceleration. The 15% magnitude gate is cheap insurance.
5. **Integrating with a wrong or stale `dt`.** If you hard-code `dt = 0.01` but your loop actually runs at 80 Hz, every gyro integration is 20% short and the filter slowly biases. Always measure `dt` from a real timer (`absolute_time_diff_us`), never assume the nominal rate.

---

## Exercise 3: 1-D Kalman Filter

### What the exercise asks

Replace the fixed-`alpha` complementary blend with a two-state Kalman filter that estimates both the angle and the gyro bias online. The host test ramps the true angle 0→45° while the gyro bias creeps 0→3°/s, and confirms the filter tracks the angle within 2° and recovers the bias within 1°/s — something the fixed-gain complementary filter cannot do, because it has no bias state.

### Reference solution

The source is in `exercise-03-kalman-1d.c`. The predict/update cycle is `cc_kalman1d_update`. The predict step integrates the unbiased rate and projects the 2×2 covariance with the system matrix `A = [[1, -dt], [0, 1]]`; the update step computes the innovation `y = measured - predicted`, the innovation covariance `S`, the Kalman gain `K`, and corrects state and covariance. The three tunable parameters are `Q_angle`, `Q_bias`, and `R_measure`; their defaults (0.001, 0.003, 0.03) are reasonable for a consumer MEMS IMU at 100–200 Hz.

### Expected output

Host test:

```text
=== Exercise 3 host Kalman 1-D test ===
true angle ramps 0->45 deg over 10 s; gyro bias creeps 0->3 deg/s

final true angle:  45.00 deg
final est angle:   44.93 deg
final true bias:   3.000 deg/s
final est bias:    2.94 deg/s
worst tracking err (post warm-up): 0.71 deg

PASS: Kalman correctly tracked the angle and estimated the bias.
```

The recovered bias (2.94°/s vs the true 3.0°/s) is the headline result. A complementary filter with no bias state would have lagged the ramp and never have produced a bias number at all.

### The canonical bugs

1. **Covariance update written in the wrong order.** The `P[0][0]` and `P[0][1]` corrections in the update step must use the *pre-correction* values of `P[0][0]` and `P[0][1]`; if you overwrite `P[0][0]` first and then use the new value for `P[0][1]`, the covariance becomes inconsistent and the filter slowly diverges. The reference code saves `P00` and `P01` before modifying anything.
2. **Initializing `P` to large values and never letting it settle.** Some tutorials seed `P` with the identity scaled by a big number. Starting from zero (as we do) and seeding the angle from the first accel reading converges faster and avoids a startup transient.
3. **Tuning `R_measure` to zero.** If you tell the filter the accelerometer is noiseless, it follows every vibration spike. `R_measure` must reflect real accel noise; 0.03 rad² is a sane starting point, raise it if the output is jittery.
4. **Feeding the gyro rate already bias-corrected.** The whole point of the Kalman filter is that it estimates the bias itself. Pass the *raw* (calibrated-to-rad/s but bias-uncorrected) gyro rate; the filter subtracts its own bias estimate internally. Pre-subtracting a separate bias double-counts.
5. **Pitch gimbal lock near ±90°.** At pitch = ±90° the roll axis becomes ambiguous. The 1-D-per-axis filter does not handle this — it is a limitation of Euler-angle filtering, and it is exactly why the mini-project moves to a quaternion Madgwick filter. For tilt within ±70° the 1-D Kalman is fine.

---

## A general note on "the math is right but the orientation is wrong"

For all three exercises, mathematically-correct code can still produce a wrong-looking orientation because of:

- **Axis convention mismatch.** The IMU's physical X/Y/Z axes (silk-screened on the breakout) may not match your robot's body frame. A 90° mounting rotation means you must permute or negate axes before the filter. Decide your body frame once, write the permutation in the read function, and never touch it again.
- **Magnetometer axis frame differs from accel/gyro.** On the MPU-9250, the AK8963 is mounted rotated relative to the accel/gyro die — its X and Y are swapped and Z is negated relative to the MPU frame (datasheet §8). The mini-project's Madgwick code applies this correction; the per-axis exercises here ignore the magnetometer, so it does not bite until fusion.
- **Soft-float cost on the RP2040.** The RP2040 has no FPU. Every `atan2f`, `sqrtf`, and `sinf` is a soft-float library call costing 1–10 µs. At 1 kHz this adds up; profile with a GPIO toggle and consider a fixed-point or small-angle approximation if you are tight on cycles. For the 100–200 Hz loops in these exercises, soft-float is comfortable.

When in doubt, hold the board level and confirm roll ≈ 0, pitch ≈ 0; then tilt it 90° about each axis in turn and confirm the right angle moves in the right direction. Five minutes of this on the bench catches every axis-convention bug.
