# Lecture 2 — Sensor Fusion: Complementary, Kalman, and Madgwick

> *Sensor fusion is the art of combining two sensors that are each wrong in different ways into one estimate that is right. The gyroscope is right over milliseconds and wrong over minutes; the accelerometer is right over minutes and wrong over milliseconds. Stack them so that each is consulted only where it is trustworthy and the combined estimate is right over all timescales. The simplest way to do this is a one-line filter that a 1975 paper called "complementary"; the principled way is a 1960 algorithm called the Kalman filter; the way that took over hobbyist firmware after 2010 is a quaternion gradient-descent scheme called Madgwick. All three are the same idea — trust the gyro short-term, the accel/mag long-term — expressed at three levels of mathematical sophistication. This lecture walks all three, and tells you which to reach for.*

## 1. The fusion problem, stated precisely

You have, at each timestep `k`, a gyro rate `ω_k` (rad/s, with bias and noise) and an accelerometer vector `a_k` (m/s², equal to gravity when stationary, corrupted by linear acceleration when not). You want an orientation estimate `θ_k` (an angle, or a full attitude) that is accurate over all timescales.

Two naive approaches, both bad:

- **Gyro-only:** `θ_k = θ_{k-1} + ω_k·dt`. Accurate instantly, drifts without bound (Lecture 1 §2). Unusable after a minute.
- **Accel-only:** `θ_k = atan2(a_y, a_z)`. Drift-free, but noisy and wrong whenever the board accelerates. Unusable on anything that moves.

Fusion combines them so the estimate inherits the gyro's smoothness and the accel's stability. The three algorithms below differ only in *how* they weight the two sources.

## 2. The complementary filter

The complementary filter is the cheapest honest fusion. Observe that the gyro error lives at *low* frequency (the bias is a slow drift, near DC) and the accel error lives at *high* frequency (vibration and transient linear acceleration). So: high-pass the gyro (keep its fast, accurate part, reject its slow drift) and low-pass the accel (keep its slow, accurate part, reject its fast noise) and add them. The two filters are "complementary" — their transfer functions sum to exactly 1 at every frequency — so the true signal passes through untouched while each sensor's error is filtered out in the band where it dominates.

In the time domain this reduces to a single recursion per axis:

```text
θ_k = α·(θ_{k-1} + ω_k·dt) + (1 - α)·θ_accel_k
```

where `θ_accel = atan2(a_y, a_z)` is the accelerometer tilt and `α ∈ (0,1)` is the one tuning knob. The first term is the gyro-integrated estimate (the high-pass path); the second is the accel tilt (the low-pass path). `α` close to 1 trusts the gyro more (longer time constant, smoother, slower to correct drift); `α` close to 0 trusts the accel more (noisier, faster to correct). The relationship to a time constant: `α = τ / (τ + dt)`, so for a desired crossover `τ = 0.5 s` at `dt = 0.01 s`, `α = 0.98`.

This is `cc_complementary_update` in `exercise-02`. The whole filter is two multiplies and an add per axis. No matrix, no covariance, no division. It runs in tens of nanoseconds and it is genuinely good enough for self-balancing robots, camera gimbals at the low end, and the bubble-level in your phone. Its limitations: `α` is fixed, so the filter cannot adapt its trust dynamically, and it has no notion of gyro bias as a separate quantity — it rejects bias only because the low-pass accel path slowly drags the estimate back to gravity, which means a *changing* bias is tracked sluggishly.

The one refinement worth adding (and which our exercise code includes) is a **linear-acceleration gate**: when `|a| ` deviates from 1 g by more than ~15%, the board is accelerating and the accel tilt is momentarily garbage, so push `α` toward 1 for that step (lean almost entirely on the gyro until the accel settles). This single `if` is the difference between a filter that survives a robot hitting a bump and one that lurches.

## 3. The Kalman filter

The Kalman filter is the principled generalization. Instead of a fixed `α`, it carries an explicit estimate of how uncertain it is (the error covariance `P`) and computes the optimal blend weight (the Kalman gain `K`) at every step from that uncertainty and the known sensor noise. When the filter is confident, it ignores the measurement; when it is uncertain, it leans on the measurement. The gain `K` is the `(1 − α)` of the complementary filter, but recomputed every step from first principles.

For attitude we use the classic **two-state** formulation: the state is `x = [angle, gyro_bias]ᵀ`. This is the headline advantage over the complementary filter — the gyro bias is an explicit state the filter estimates online, so a drifting bias (the thermal creep of Lecture 1 §2) is tracked, not just tolerated.

The filter alternates two steps:

**Predict** (propagate the state forward using the gyro):

```text
angle_k = angle_{k-1} + (ω_k - bias_{k-1})·dt
bias_k  = bias_{k-1}                            // bias is a slow random walk
P_k     = A·P_{k-1}·Aᵀ + Q                      // covariance grows
```

with the system matrix `A = [[1, -dt], [0, 1]]` and the process-noise matrix `Q = diag(Q_angle, Q_bias)`.

**Update** (correct using the accelerometer angle measurement `z = θ_accel`):

```text
y = z - angle_k                                 // innovation
S = P[0][0] + R_measure                         // innovation covariance
K = [P[0][0]/S, P[1][0]/S]ᵀ                     // Kalman gain
angle_k += K[0]·y
bias_k  += K[1]·y
P_k     = (I - K·H)·P_k                          // covariance shrinks
```

with the measurement matrix `H = [1, 0]` (we measure angle, not bias). This is exactly `cc_kalman1d_update` in `exercise-03`. The three tuning parameters `Q_angle`, `Q_bias`, `R_measure` replace the complementary filter's single `α`, and they have physical meaning: `Q_angle`/`Q_bias` say how fast you believe the true state can change, `R_measure` says how noisy the accelerometer is. Larger `R_measure` → trust the accel less → smoother but laggier. The defaults (0.001, 0.003, 0.03) work for a consumer MEMS IMU at 100–200 Hz; the homework asks you to tune them against logged data.

The Kalman filter costs a 2×2 covariance propagation per axis per step — about seven adds, a dozen multiplies, and one divide. On the RP2040 (no FPU, soft-float) that is a few microseconds per axis, comfortable at 1 kHz. It is the right choice when you need an honest uncertainty estimate, when the gyro bias drifts meaningfully, or when you want the blend to adapt automatically. It is overkill for a static-`α` application where the complementary filter already works.

A caveat: this two-state-per-axis Kalman filter operates on **Euler angles** and therefore suffers **gimbal lock** at pitch = ±90°, where roll becomes ambiguous. For tilt within ±70° it is fine; for full 3-D attitude (a tumbling drone, a VR headset) you need a quaternion formulation, which is where Madgwick comes in.

## 4. The Madgwick filter

Sebastian Madgwick published, in his 2010 University of Bristol report, an orientation filter that became the default in the hobbyist and academic world because it is quaternion-based (no gimbal lock), fuses all nine axes (accel + gyro + mag, so it estimates yaw too), has a single intuitive tuning parameter (`β`), and costs about the same as a complementary filter. It is not a Kalman filter — it does not carry a covariance — but it achieves Kalman-comparable accuracy through a clever gradient-descent correction.

The idea: the gyro integration gives a quaternion rate `q̇_ω = ½·q ⊗ ω` (the quaternion derivative from the body angular rate). This drifts. To correct the drift, Madgwick poses an **error function**: the measured gravity (and magnetic) direction should match what the current orientation predicts. He computes the gradient of that error with respect to the quaternion (analytically — the Jacobian is worked out in the paper) and takes one gradient-descent step *down* the error surface, scaled by `β`. The corrected quaternion rate is:

```text
q̇ = q̇_ω - β·(∇f / |∇f|)
q_k = normalize(q_{k-1} + q̇·dt)
```

`β` is the only knob: it is the gradient-descent step size, and it has units of rad/s — it represents the gyro's measurement error (its drift rate). A typical value is `β ≈ 0.1` for a consumer IMU; larger `β` corrects drift faster but lets accel/mag noise through, smaller `β` is smoother but drifts more. Madgwick gives a formula `β = sqrt(3/4)·ω_error` where `ω_error` is the gyro's RMS bias in rad/s.

The two flavors:

- **IMU (6-axis):** accel + gyro only. Corrects roll and pitch (gravity reference), leaves yaw to drift (no heading reference). This is `MadgwickAHRSupdateIMU` in the reference code.
- **MARG (9-axis):** accel + gyro + mag. The "AHRS" (Attitude and Heading Reference System) version, which adds the magnetometer to pin yaw. This is `MadgwickAHRSupdate`. It requires the magnetometer to be properly hard/soft-iron calibrated (Lecture 1 §4) or the yaw is worse than useless.

The MARG version also handles the **magnetic distortion compensation**: it rotates the measured magnetic field into the earth frame using the current orientation estimate, then forces the horizontal component to align with a reference direction, which cancels the magnetic *inclination* (the ~60° dip angle) so that tilt does not contaminate heading. This is the "tilt-compensated compass" — the reason you can hold the board at any angle and still get a correct heading, as long as the orientation estimate is good.

Madgwick's reference C implementation is ~130 lines, public domain, and embarrassingly fast: it uses a fast inverse square root (the famous `0x5f3759df` Quake trick, though modern builds just use `1.0f/sqrtf`) and a handful of multiply-adds. It runs in ~15 µs on the RP2040. The mini-project this week implements the full MARG version and fuses the calibrated MPU-9250 stream into a drift-free quaternion.

## 5. Choosing among the three

A decision table, because students always ask:

| Need | Reach for |
|------|-----------|
| Roll/pitch only, static `α` is fine, minimum code | Complementary |
| Gyro bias drifts, want an honest uncertainty estimate, Euler is fine | 1-D Kalman (per axis) |
| Full 3-D attitude, need yaw, want no gimbal lock | Madgwick MARG |
| Full 3-D with rigorous covariance (aerospace) | Quaternion EKF (beyond this week) |

The progression is also pedagogical: the complementary filter teaches the frequency-domain intuition, the Kalman filter teaches the covariance/gain machinery, and Madgwick teaches the quaternion/gradient approach that production hobby firmware actually ships. We implement all three so you can feel the trade-offs on the bench, then the mini-project ships Madgwick because it is the one that produces a stable, drift-free, gimbal-lock-free orientation from a $3 IMU.

## 6. Quaternions in 90 seconds

Because Madgwick is quaternion-based, a minimal quaternion primer. A unit quaternion `q = (w, x, y, z)` with `w² + x² + y² + z² = 1` represents a rotation. The scalar part `w = cos(θ/2)` and the vector part `(x, y, z) = sin(θ/2)·axis` encode a rotation of angle `θ` about a unit `axis`. Quaternions compose rotations by the Hamilton product (`quat_mul` in `imu_common.h`), which is non-commutative — order matters, just like rotation matrices. They have three advantages over Euler angles: no gimbal lock (the singularity that froze our 1-D Kalman at pitch ±90° simply does not exist), no trigonometry in the update (the quaternion derivative is a linear function of the angular rate), and numerical stability (you renormalize to unit length each step, which is one square root, versus the trig soup of Euler integration).

The quaternion derivative from the body angular rate `ω = (ω_x, ω_y, ω_z)` is:

```text
q̇ = ½·q ⊗ (0, ω_x, ω_y, ω_z)
```

— half the Hamilton product of the current quaternion with the pure-vector quaternion of the angular rate. Integrate `q_k = normalize(q_{k-1} + q̇·dt)` and you have a gyro-only attitude that drifts; Madgwick's gradient term corrects the drift. To display the result you convert back to Euler with `quat_to_euler` (in `imu_common.h`), which is the only place trig appears.

## 7. The Allan-variance digression (why you cannot fuse your way out of bad sensors)

Students often ask: if fusion is so good, why does it matter how good the raw sensor is? The answer is the **Allan variance** — a log-log plot of a sensor's noise versus averaging time. At short averaging times the gyro is dominated by white noise (angle random walk, slope −½); at long averaging times by bias instability (the flat floor) and then rate random walk (slope +½). Fusion can suppress the white-noise region (the accel pins it) and track the slow bias (the Kalman/Madgwick bias estimate), but it cannot do better than the sensor's bias-instability floor — the irreducible flat minimum of the Allan deviation curve. A consumer IMU's floor is tens of °/hr; a navigation IMU's is hundredths. No filter crosses that gap. Know your sensor's Allan floor (it is on the datasheet, or you measure it from a multi-hour stationary log) and you know the best your fused estimate can be. Fusion gets you *to* the floor; nothing gets you *below* it.

## 8. Summary

Three filters, one idea: trust the gyro short-term and the accel/mag long-term. The complementary filter does it with one fixed coefficient and two lines of code. The Kalman filter does it with an adaptive gain computed from an explicit covariance, and it estimates the gyro bias as a state — at the cost of a per-axis 2×2 propagation and a gimbal-lock limit from its Euler-angle basis. The Madgwick filter does it in quaternion space with a single gradient-descent step, fusing all nine axes for a drift-free, gimbal-lock-free, yaw-aware attitude at complementary-filter cost — which is why the mini-project ships it. Whichever you pick, the calibration of Lecture 1 is the foundation; no filter fuses its way out of an uncalibrated sensor, and no filter beats the sensor's Allan-variance floor.

Next lecture: actuators — driving servos and motors with PWM, and closing the loop with a PID controller so the orientation estimate you just built can actually *steer* something.

## References for this lecture

- Madgwick, "An efficient orientation filter for inertial and inertial/magnetic sensor arrays", University of Bristol technical report, 30 April 2010. <https://x-io.co.uk/downloads/madgwick_internal_report.pdf>
- Kalman, R.E., "A New Approach to Linear Filtering and Prediction Problems", Trans. ASME, J. Basic Eng., 82(1):35–45, 1960.
- Higgins, W.T., "A Comparison of Complementary and Kalman Filtering", IEEE Trans. Aerospace and Electronic Systems, AES-11(3):321–325, 1975.
- Welch & Bishop, "An Introduction to the Kalman Filter", UNC-Chapel Hill TR 95-041. <https://www.cs.unc.edu/~welch/kalman/>
- Lauszus, K., "A practical approach to Kalman filter and how to implement it", TKJ Electronics blog, 2012 — the two-state embedded formulation.
- Diebel, "Representing Attitude", Stanford 2006 — quaternion conventions and the q̇ = ½q⊗ω derivative.
- IEEE Std 952-1997 / Allan, D.W., "Statistics of Atomic Frequency Standards", Proc. IEEE 54(2), 1966 — the Allan-variance method.
