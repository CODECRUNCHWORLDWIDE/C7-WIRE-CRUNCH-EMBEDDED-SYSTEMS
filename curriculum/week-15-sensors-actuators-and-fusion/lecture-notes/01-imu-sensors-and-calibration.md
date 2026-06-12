# Lecture 1 — IMU Sensors and Calibration

> *An inertial measurement unit is a chip that lies to you in three different, predictable ways, and your job is to characterize each lie precisely enough to subtract it. The gyroscope tells you how fast you are rotating, but it tells you with a constant offset that integrates into unbounded drift. The accelerometer tells you which way is down, but only when you are not moving — the moment you accelerate, "down" tilts toward your acceleration. The magnetometer tells you which way is north, but the steel screw next to it and the current in the wire under it bend its idea of north into an ellipsoid. None of these sensors is usable raw. Every one becomes usable after a calibration that you, the firmware engineer, run on the bench and bake into a struct. This lecture is about what each sensor measures, how each one lies, and the calibration that turns lies into numbers you can fuse.*

## 1. What an IMU is and what it measures

An IMU bundles a three-axis accelerometer and a three-axis gyroscope into one MEMS package; a "9-axis" IMU adds a three-axis magnetometer. The part we use this week is the InvenSense MPU-9250 — a 6-axis MPU-6500 die stacked with an AsahiKASEI AK8963 magnetometer die in one 3×3 mm QFN. It is a 2014 part, end-of-lifed by InvenSense (now TDK), and still the most common IMU on hobby breakouts in 2026 because it is cheap, the register map is fully public, and every embedded codebase already has a driver for it. Its successor, the ICM-20948, is pin-and-register-similar; the calibration math in this lecture applies unchanged.

The MPU-9250 speaks I2C (up to 400 kHz Fast Mode) or SPI (up to 1 MHz for register access, 20 MHz for the sensor-data burst). We use I2C0 on the Pico at 400 kHz because it needs only two wires and the 100–200 Hz sample rates we care about leave the bus 95% idle. The accel and gyro live at I2C address `0x68` (when the AD0 pin is tied low); the magnetometer is a *separate* I2C slave at `0x0C` that sits behind the MPU's internal I2C master. To reach it directly we set the `BYPASS_EN` bit in `INT_PIN_CFG` (register 0x37, bit 1), which electrically connects the AK8963 to the external bus. This is the simplest scheme and the one our `imu_common.h` driver uses; the alternative — programming the MPU's internal I2C master to slurp the AK8963 into the MPU's register bank — is more efficient at high rates but more code, and we do not need it at 100 Hz.

Three measurements, three SI units:

- **Accelerometer** measures **specific force** in g (multiples of standard gravity, 9.80665 m/s²). At rest, the only specific force is the reaction to gravity, so a level board reads (0, 0, +1 g) — note the sign: an accelerometer at rest reads the force holding it *up*, which points up, so the Z axis reads +1 g, not −1 g. Full-scale is selectable: ±2 g, ±4 g, ±8 g, ±16 g. Smaller full-scale means finer resolution (more LSBs per g) but clips sooner. We use ±4 g — fine enough for tilt, with headroom for the bumps a robot hits.
- **Gyroscope** measures **angular rate** in degrees per second. Full-scale ±250, ±500, ±1000, ±2000 dps. We use ±500 dps: a hand-tumbled board rarely exceeds 500 dps and the 65.5 LSB/dps resolution is plenty. The gyro's output is rate, not angle — to get angle you integrate, and integration is where the trouble starts.
- **Magnetometer** measures **magnetic flux density** in microtesla (µT). Earth's field is ~25–65 µT depending on latitude. The AK8963's full-scale is ±4912 µT over a 16-bit signed range, giving ~0.15 µT per count. The magnetometer is the only absolute heading reference an IMU has; without it, yaw drifts because gravity (the accel's reference) is symmetric about the vertical axis and carries no heading information.

## 2. The gyroscope and why integration drifts

A gyro's output is `ω_measured = ω_true + b + n`, where `b` is a slowly-varying **bias** (offset) and `n` is white **noise**. To get angle you integrate: `θ(t) = ∫ ω_measured dt`. Substitute and the bias integrates into a ramp: `θ(t) = θ_true(t) + b·t + ∫n dt`. The `b·t` term is the killer. A consumer MEMS gyro has a bias on the order of 1–5°/s after power-on. Integrate 2°/s for one minute and you have accumulated 120° of error from a board that never moved.

The bias is not constant. It drifts with temperature (the die heats up after power-on; the bias moves with it), with supply voltage, and with mechanical stress on the package. The datasheet's "zero-rate output" spec for the MPU-9250 is ±5°/s initial, with a temperature coefficient around ±0.02°/s/°C. This is why you cannot calibrate the gyro once at the factory and trust it for the life of the device — the bias today is not the bias in an hour, and certainly not the bias next winter.

The noise `n` integrates into a **random walk**: the variance of the integrated angle grows linearly with time. This is the "angle random walk" (ARW) spec, given in °/√hr. For the MPU-9250 it is around 0.1°/√s. Over short intervals (a fraction of a second) the random walk is negligible compared to the bias ramp; over long intervals both matter. The practical takeaway: a raw gyro integration is accurate for a fraction of a second and useless after a minute. The whole point of sensor fusion (Lecture 2) is to use the gyro only over the short intervals where it is good and let another sensor pin the long-term value.

### Measuring the gyro bias

The cheapest useful gyro calibration is a bias measurement: hold the board perfectly still, average a few hundred samples, and the mean is the bias. Subtract it from every future reading. Our `exercise-01` code does exactly this — 512 samples at ~500 Hz, averaged per axis, converted to rad/s. The catch is "perfectly still": any vibration during the measurement corrupts the mean. On a solid bench the residual bias after this calibration is a few thousandths of a rad/s, which the fusion filter further suppresses.

A better calibration re-measures the bias whenever the board is detected to be stationary (gyro magnitude below a threshold for a second), continuously, over the device's life. This tracks the thermal drift. The two-state Kalman filter of `exercise-03` does this implicitly: the bias is a filter state that updates online. For this week's exercises we do a one-shot bias measurement at startup and let the fusion filter handle the residual.

## 3. The accelerometer and its calibration

An accelerometer at rest measures the gravity vector rotated into the body frame. Tilt the board and the gravity vector tilts in the board's coordinates; `atan2(ay, az)` gives the roll, `atan2(-ax, sqrt(ay²+az²))` gives the pitch. This is a perfect long-term tilt reference — gravity does not drift — but it has two problems. First, it carries no yaw information: rotating the board about the vertical axis does not change the gravity vector in the body frame, so the accelerometer cannot tell you which way you are facing. Second, it measures *all* specific force, not just gravity. The moment the board accelerates, the measured vector is `gravity + linear_acceleration`, and the tilt you compute from it is wrong by the angle of the linear acceleration.

The accelerometer's lies are **offset** and **scale** errors, per axis. Offset (also called bias): each axis reads a small non-zero value even when that axis is perpendicular to gravity. Scale (gain): the LSB-per-g is not exactly the datasheet value; a board that should read 1.000 g reads 1.013 g. Both are stable over time (unlike the gyro bias) and can be calibrated once.

### The six-position tumble calibration

The standard accel calibration places the board in six orientations — each axis pointing straight up, then straight down — and records the steady reading. When +Z points up, the Z axis should read exactly +1 g; when it points down, exactly −1 g. From the two readings on each axis you solve for offset and scale:

```text
offset = (reading_up + reading_down) / 2
scale  = (2·g_reference) / (reading_up - reading_down)
```

The offset is the midpoint of the two extremes (where the axis reads when perpendicular to gravity); the scale corrects the span to exactly 2 g. Our `imu_calibration_t` stores `accel_offset[3]` and `accel_scale[3]`, and `cc_imu_apply_calibration` applies them as `(value − offset) · scale`. A careful tumble calibration brings a consumer accelerometer's tilt error under 0.5°, which is the floor the fusion filter inherits.

A more elaborate accel calibration solves for a full 3×3 misalignment matrix (the axes are not perfectly orthogonal) plus offsets, by least-squares fitting many orientations to the constraint "the magnitude must equal g". For this week the per-axis offset+scale is sufficient; the misalignment of a modern MEMS part is under 0.1° and below our other error sources.

## 4. The magnetometer and hard/soft iron

The magnetometer is the most finicky of the three. In a perfect world it measures Earth's field — a roughly constant vector pointing toward magnetic north and down into the ground (the "inclination" angle, ~60° below horizontal at mid-latitudes). In the real world it measures Earth's field *plus* the field of every piece of ferromagnetic material near it *plus* the field induced in nearby conductors by Earth's field. The corruptions split into two categories.

**Hard-iron** distortion is a constant additive offset: a permanent magnet, a magnetized screw, a speaker, or a steel battery clip near the sensor adds a fixed vector to every reading. On a plot of the magnetometer's three axes as you rotate the board through all orientations, the points should lie on a sphere centered at the origin; hard iron shifts the sphere's center away from the origin. The fix is to subtract the center offset: `mag_corrected = mag_raw − hard_iron_offset`.

**Soft-iron** distortion is a multiplicative distortion: a nearby ferromagnetic material that is not itself magnetized but that *channels* Earth's field, bending the response so that the sphere becomes an ellipsoid. The fix is a 3×3 matrix that maps the ellipsoid back to a sphere. The full soft-iron correction is `mag_corrected = M · (mag_raw − hard_iron_offset)` where `M` is the inverse of the ellipsoid's shape matrix. For a cheap approximation — the one our `imu_common.h` uses — we treat `M` as diagonal: just scale each axis by the reciprocal of that axis's ellipsoid semi-axis. The diagonal approximation captures the dominant soft-iron effect (axis-aligned stretching) and ignores the cross-axis shear, which is usually small on a well-laid-out board.

### Measuring hard and soft iron

The calibration procedure: rotate the board slowly through every orientation (the "figure-eight in the air" dance, for 20–30 seconds), recording the min and max reading on each axis. Then:

```text
hard_iron[i] = (max[i] + min[i]) / 2          // ellipsoid center
semi_axis[i] = (max[i] - min[i]) / 2          // ellipsoid semi-axis
avg_radius   = (semi_axis[x] + semi_axis[y] + semi_axis[z]) / 3
soft_iron[i] = avg_radius / semi_axis[i]       // diagonal scale to a sphere
```

The hard-iron offset is the midpoint of the min/max box; the soft-iron diagonal scales each axis so all three have the same span (the average radius). After this correction the magnetometer's locus is a sphere of radius `avg_radius` centered at the origin, and a tilt-compensated heading computed from it is accurate to a few degrees. The mini-project includes a `cc-magcal.py` host tool that ingests a stream of raw magnetometer samples and computes these parameters, so you do not have to compute min/max on the device.

The AK8963 also has a **factory sensitivity adjustment** (ASA), three values stored in fuse ROM that correct for die-level per-axis sensitivity variation. You read these once at startup (`exercise-01`'s `ak8963_init`) and apply them as `µT = counts × 0.15 × ASA_factor` where `ASA_factor = (ASA_byte − 128)/256 + 1`. This is separate from and prior to the hard/soft-iron calibration: ASA corrects the die, hard/soft iron corrects the environment.

## 5. Control-grade versus consumer-grade

A word on what you are *not* getting from a $3 MPU-9250. The IMU in a phone, a drone, or a self-balancing robot is "consumer grade": bias instability in the tens of °/hr, ARW around 0.1°/√s, and a price under $5. The IMU in an aircraft, a missile, or a survey instrument is "tactical" or "navigation" grade: bias instability under 1°/hr (tactical) or under 0.01°/hr (navigation), ARW two or three orders of magnitude better, and a price from hundreds to hundreds of thousands of dollars. The difference is not the algorithm — the same Kalman math runs on both — it is the raw sensor quality, and no amount of fusion recovers signal that the sensor never captured.

For a consumer IMU the practical consequence is: your fused orientation is excellent in roll and pitch (gravity is a strong, always-present reference) and only fair in yaw (the magnetometer is weak and easily corrupted). A self-balancing robot cares about pitch and is happy. A pedestrian dead-reckoning system cares about yaw and fights the magnetometer constantly. Know which axis your application leans on, and calibrate that sensor hardest.

The other consumer-grade reality is **vibration**. A motor spinning near the IMU couples mechanical vibration into the MEMS proof mass, and the accel and gyro both pick it up as broadband noise. The hardware fix is mechanical isolation (foam mounting, away from motors). The firmware fix is the digital low-pass filter (DLPF) built into the MPU — registers `CONFIG` and `ACCEL_CONFIG2` set the gyro and accel bandwidth. We set 41 Hz on the gyro and ~45 Hz on the accel, which rejects motor-frequency vibration while passing the few-Hz dynamics of a tumbling board. Setting the DLPF too aggressively (5 Hz) adds phase lag that the fusion filter sees as the board lagging reality; too loosely (250 Hz) lets vibration through. 40–50 Hz is the sweet spot for a hand-held or slow-robot application.

## 6. Putting it together: the calibrated read path

The `exercise-01` code is the canonical read path and the foundation for everything downstream. Its shape:

1. **Init the bus and the parts.** I2C0 at 400 kHz, pull-ups on SDA/SCL. Reset the MPU, select the PLL clock, set full-scale and the DLPF, enable bypass. Verify the AK8963's WIA, read its ASA, put it in 16-bit continuous mode.
2. **Measure the gyro bias.** 512 stationary samples averaged per axis. Store in the calibration struct.
3. **Load the static calibrations.** Accel offset/scale and mag hard/soft iron come from a prior bench calibration (this week we ship sane defaults and let the fusion filter absorb the residual; the homework asks you to run the full tumble and figure-eight and bake the numbers in).
4. **Read and convert each sample.** Burst-read 14 bytes from the MPU (accel + temp + gyro, big-endian), then the AK8963 (mag, little-endian, with the mandatory ST2 read). Convert raw counts to SI units through `cc_imu_apply_calibration`.

The output of this path is the `imu_sample_t` — accel in m/s², gyro in rad/s, mag in µT, plus the measured `dt` since the last sample. That struct is the input to every fusion filter in Lectures 2 and 3. Get this right and the fusion is easy; get it wrong and no amount of clever filtering recovers.

## 7. Summary

An IMU is three sensors that each fail in a characteristic way: the gyro drifts (bias integrates), the accel is fooled by linear acceleration (it measures specific force, not just gravity), and the magnetometer is bent by nearby iron (hard and soft). Each failure mode has a calibration: gyro bias measurement (continuous if you can), accel six-position tumble (offset and scale), magnetometer figure-eight (hard-iron offset and soft-iron scale, after the AK8963 factory ASA). The calibrated, SI-unit `imu_sample_t` is the contract between this lecture's read path and next lecture's fusion filters. Spend your bench time on the calibration; the fusion is the easy part.

Next lecture: complementary and Kalman filtering — how to combine the gyro's good short-term behavior with the accel's good long-term behavior into a single drift-free orientation estimate.

## References for this lecture

- InvenSense, "MPU-9250 Register Map and Descriptions", RM-MPU-9250A-00, rev 1.6. <https://invensense.tdk.com/wp-content/uploads/2015/02/RM-MPU-9250A-00-v1.6.pdf>
- InvenSense, "MPU-9250 Product Specification", PS-MPU-9250A-01 — sensitivity, bias, and ARW specs in §3.
- AsahiKASEI, "AK8963 3-axis Electronic Compass" datasheet — ASA fuse-ROM values, ST1/ST2 handshake, 16-bit mode.
- RP2040 datasheet §4.3 "I2C", pp. 463–510. <https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf>
- Diebel, "Representing Attitude: Euler Angles, Unit Quaternions, and Rotation Vectors", Stanford, 2006 — the accel-tilt and Euler conventions.
- IEEE Std 952-1997, "Standard Specification Format Guide and Test Procedure for Single-Axis Interferometric Fiber Optic Gyros" — the source of the ARW / bias-instability vocabulary (applies to MEMS too).
