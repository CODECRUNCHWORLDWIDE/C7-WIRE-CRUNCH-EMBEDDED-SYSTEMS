# Lecture 3 — Actuators, PWM, and Closed-Loop Control

> *A sensor turns the physical world into numbers; an actuator turns numbers back into the physical world. Between the two sits the loop — the control law that reads the sensor, compares it to what you wanted, and commands the actuator to close the gap. This lecture is about the actuator side: how PWM drives a hobby servo, a brushed DC motor, and a stepper; why an H-bridge exists; what the four quadrants of motor operation are; and how a PID controller turns the orientation estimate from Lecture 2 into a motor command that actually balances a robot. By the end you can take the fused pitch angle from the IMU and use it to drive a wheel that keeps the robot upright — the canonical closed-loop demo, and the mini-project this week.*

## 1. PWM: the universal actuator interface

Pulse-width modulation is how a digital pin commands an analog quantity. You toggle the pin high for a fraction of each fixed-period cycle; the *duty cycle* (high time over period) is the command. A motor's inductance and inertia, or a servo's pulse-decoding circuit, integrate the train of pulses into a smooth response. PWM is everywhere because it is lossless — the switching transistor is either fully on (no voltage across it) or fully off (no current through it), so it dissipates almost no power, unlike a linear regulator that drops voltage as heat.

The RP2040 has eight PWM "slices", each with two channels (A and B), for 16 PWM outputs (datasheet §4.5, pp. 525–548). Each slice has a 16-bit counter, a clock divider, and a wrap value. The output frequency is `f_pwm = f_sys / (divider · (wrap + 1))`. The duty cycle is `level / (wrap + 1)`, where `level` is the compare value. To configure a PWM output you: pick a GPIO, set its function to `GPIO_FUNC_PWM`, find which slice/channel it maps to (`pwm_gpio_to_slice_num`), set the wrap and divider for your frequency, set the level for your duty, and enable the slice. The Pico SDK's `hardware_pwm` wraps all of this.

Two PWM regimes matter this week, and they are very different frequencies:

- **Servo PWM:** 50 Hz (20 ms period), with the *pulse width* — not the duty cycle — carrying the command. A hobby servo decodes the high-time of each pulse: 1.0 ms = full one way, 1.5 ms = center, 2.0 ms = full the other way. The frequency is fixed at 50 Hz by convention; the information is entirely in the 1–2 ms pulse width, which is a 5–10% duty cycle of the 20 ms period.
- **Motor PWM:** 20 kHz, with the *duty cycle* carrying the command. A brushed DC motor's speed is proportional to average voltage, which is proportional to duty cycle. The frequency is chosen above the ~20 kHz human hearing limit so the motor does not whine, and high enough that the motor inductance smooths the current ripple, but not so high that the H-bridge's switching losses dominate. 20 kHz is the standard compromise.

## 2. The hobby servo

A hobby servo is a self-contained closed-loop positioning system: a small DC motor, a gearbox, a potentiometer measuring the output shaft angle, and a control board that drives the motor until the pot matches the commanded position. You command position with the PWM pulse width; the servo's internal loop does the rest. From the firmware's perspective a servo is a one-wire "go to this angle" device.

The math: map a desired angle in [−90°, +90°] to a pulse width in [1000 µs, 2000 µs], linearly. At 50 Hz the period is 20000 µs; the level (compare value) for a pulse of `p` µs is `level = wrap · p / 20000`. Our `imu_common.h` defines `SERVO_MIN_PULSE_US`, `SERVO_MID_PULSE_US`, `SERVO_MAX_PULSE_US` and the mini-project's `servo_set_angle` does the mapping. A subtlety: cheap servos do not honor the full 1–2 ms range; some go ±60° over 1–2 ms and need 0.5–2.5 ms for the full ±90°. Calibrate the endpoints on the bench before trusting the mapping, or you command 90° and get 70°.

The servo's internal loop has a deadband (it stops correcting when the error is small, to avoid buzzing) and a slew rate (it cannot move instantly). For our closed-loop demo the servo is fast enough (a typical 0.1 s/60° servo slews 90° in 150 ms) that we treat it as an ideal angle command and let our outer loop run at 100 Hz.

## 3. The brushed DC motor and the H-bridge

A brushed DC motor spins at a speed proportional to applied voltage and produces torque proportional to current. To drive it from a microcontroller you cannot connect the GPIO directly — a GPIO sources a few milliamps, a motor draws hundreds. You need a **driver**: at minimum a single transistor (for one-direction, on/off control), or an **H-bridge** for bidirectional control.

An H-bridge is four switches (MOSFETs) arranged in an "H" with the motor as the crossbar. Close the top-left and bottom-right switches and current flows one way (motor spins forward); close the top-right and bottom-left and current flows the other way (reverse). The four switches give you the **four quadrants** of motor operation:

- **Forward drive:** current and rotation both forward — the motor accelerates.
- **Forward braking (regeneration):** rotation forward, current reversed — the motor decelerates, and if the driver allows, dumps energy back to the supply.
- **Reverse drive** and **reverse braking** — the mirror images.

PWM the high-side switch of the active diagonal and the average voltage — hence speed — follows the duty cycle. There are two PWM schemes: **sign-magnitude** (one PWM pin for speed, one direction pin for sign — simple, but braking is passive) and **locked-antiphase** (a single PWM where 50% duty is stop, >50% is forward, <50% is reverse — smoother, supports active braking, but always switching). We use sign-magnitude in the mini-project: two GPIOs to a DRV8833 H-bridge driver IC, one PWM'd for speed, the pair's polarity setting direction. The DRV8833 (a $1 TI part, 2 channels, 1.5 A) is the canonical hobby H-bridge; the TB6612FNG is the other common choice.

A critical safety rule: **never close both switches on one side of the H-bridge at once** — that shorts the supply through the two transistors ("shoot-through") and destroys them. Integrated driver ICs like the DRV8833 prevent shoot-through internally with dead-time; if you build a discrete H-bridge you must insert dead-time yourself. This is why everyone uses a driver IC for hobby work.

Three more motor realities the firmware must respect:

- **Back-EMF.** A spinning motor generates a voltage opposing the drive (proportional to speed). When you cut the drive, the motor coasts and the collapsing field produces a voltage spike; the H-bridge's body diodes (or external flyback diodes) clamp it. This is why you never drive an inductive load from a bare GPIO — the spike kills the pin.
- **Inrush current.** A stalled or starting motor draws several times its running current (no back-EMF to limit it). Your supply and driver must handle the stall current, or current-limit in firmware by ramping the duty cycle.
- **Current sensing.** To close a torque or current loop you measure motor current, typically as the voltage across a low-side shunt resistor read by the ADC (Week 8's territory). The mini-project's closed loop is a *position/angle* loop, not a current loop, so we do not need current sensing — but a real motor controller layers a fast inner current loop under the slower position loop.

## 4. The stepper motor

A stepper moves in discrete steps — typically 1.8° per full step (200 steps per revolution) — by energizing its coils in sequence. It holds position without feedback (open-loop positioning), which makes it the actuator of choice for 3-D printers and CNC machines where you command "move 1000 steps" and trust it got there. The cost: it can *lose steps* if overloaded (it skips silently, and now your position estimate is wrong with no way to know), and it draws full current even when holding still.

Driving a stepper means energizing the coils in the right sequence at the right time. The naive scheme (full-step, energize one coil pair at a time) is simple but rough; **microstepping** (PWM the coil currents to interpolate between full steps, e.g. 16 microsteps per step) is smooth and quiet and what every modern driver does. You almost never bit-bang a stepper sequence today — you use a driver IC like the A4988 or the DRV8825 (step/direction interface: one pin pulses to step, one pin sets direction, the IC handles the coil sequencing and microstepping) or a smart driver like the TMC2209 (adds stall detection and silent operation). The firmware's job reduces to generating a clean step pulse train at the right rate, which on the RP2040 is a perfect job for a PWM slice or a PIO state machine. We do not build a stepper demo in the mini-project (the IMU-driven balance demo uses a brushed motor and a servo), but the homework includes a step/direction generator so you have the pattern.

## 5. The control loop: P, PI, PID

An actuator alone is open-loop: you command and hope. Closing the loop means measuring the result, comparing it to the target, and adjusting the command to drive the error to zero. The workhorse control law is **PID** — Proportional, Integral, Derivative — and 95% of industrial control loops are some form of it. The law:

```text
error      = setpoint - measured
output     = Kp·error + Ki·∫error dt + Kd·d(error)/dt
```

Each term has a job:

- **Proportional (`Kp·error`):** push harder the further you are from the target. The dominant term. Too little `Kp` and the loop is sluggish; too much and it oscillates. A pure-P loop leaves a **steady-state error** — it stops pushing before reaching the target because the proportional term goes to zero as the error does, but a constant disturbance (gravity on a balancing robot) needs a constant output to counter it.
- **Integral (`Ki·∫error`):** accumulate the error over time, so a small persistent error eventually builds a large enough output to eliminate it. The integral term kills steady-state error. Its danger is **windup**: if the actuator saturates (motor at 100%) and the error persists, the integrator keeps accumulating to a huge value, and when the error finally reverses, the integrator has to unwind before the output responds — a long overshoot. The fix is **anti-windup**: clamp the integrator to a sane range (our `pid_t` has `integ_min`/`integ_max`), or stop integrating when the output is saturated.
- **Derivative (`Kd·d(error)/dt`):** respond to the *rate* of change of error, providing damping that anticipates overshoot. It is the term that lets you turn `Kp` up without oscillating. Its danger is noise: differentiating a noisy measurement amplifies the noise, so the derivative term is usually computed on the *measurement* (not the error) and low-pass filtered. For a balancing robot the gyro rate is itself a clean derivative of the angle, so you can feed it directly as the D term — a "PD on measurement" trick that avoids differentiating noise.

Our `cc_pid_update` implements all three with output saturation and integrator anti-windup. The implementation notes that matter in practice:

- **Sample time.** PID assumes a fixed `dt`. If your loop jitters, the integral and derivative are wrong. Run the loop on a timer interrupt or measure `dt` and scale (we measure `dt`).
- **Output saturation.** Clamp the output to the actuator's range (`out_min`/`out_max`) — a motor cannot go past 100%. Always clamp *after* summing the three terms.
- **Bumpless tuning.** When you change gains live, the integral term can jump. Not a concern for our bench demo, but a real product re-initializes the integrator on a gain change.

Tuning a PID by hand: start with `Ki = Kd = 0`, raise `Kp` until the system oscillates, then back off to ~half. Add `Kd` to damp the residual oscillation. Add a small `Ki` last to kill steady-state error. This is the Ziegler-Nichols flavor; for a balancing robot expect `Kp` in the tens, `Kd` a few, `Ki` near zero or small. The mini-project ships starting gains and the homework asks you to tune them on real hardware.

## 6. The full closed loop: IMU → fusion → PID → motor

Now stack the whole week. The self-balancing demo (the mini-project) runs this loop at 100 Hz:

1. **Read** the MPU-9250 (Lecture 1's calibrated read path) → calibrated accel + gyro.
2. **Fuse** into a pitch angle (Lecture 2's Madgwick or complementary filter) → the robot's lean angle, drift-free.
3. **Control:** the setpoint is "upright" (pitch = 0). `error = 0 − pitch`. Run `cc_pid_update` → a motor command in [−1, +1].
4. **Actuate:** map the command to an H-bridge PWM duty and direction → the wheels drive in the direction that reduces the lean.
5. **Repeat** every 10 ms.

The physics: when the robot leans forward, the PID commands the wheels forward, which drives the base under the center of mass, righting it — exactly how you balance a broom on your palm by moving your hand under it. The gyro rate (the lean's angular velocity) is a natural derivative term, so the controller is effectively PD on the fused angle with the gyro as the D source. The integral term trims out a constant offset (a slightly-off center of mass, an uneven floor).

The latency budget is what makes or breaks it. The total loop delay — sensor read (I2C burst, ~0.5 ms) + fusion (~15 µs) + PID (~1 µs) + PWM update (immediate) — must be small compared to the robot's natural fall time (a few hundred ms for a typical chassis). At 100 Hz with sub-millisecond compute, we have 9 ms of slack per cycle, which is comfortable. Run the loop too slowly (20 Hz) and the robot falls between corrections; run the sensor read on the same core as a blocking USB print and the jitter eats your phase margin. Keep the control loop tight and deterministic — ideally on a dedicated timer interrupt or core, with logging deferred to the other core. This is exactly the RTOS-task and DMA discipline from Weeks 6–8 paying off.

## 7. Failure modes and safety

Actuators move things, and moving things hurt people and break hardware. The firmware must fail safe:

- **Watchdog the loop.** If the control loop stalls (a sensor read hangs, a task deadlocks), the motor must stop, not hold its last command. Arm a hardware watchdog that the loop kicks; on timeout, the reset handler (or a dedicated fault routine) drives the H-bridge to coast/brake. Week 10's watchdog discipline applies directly.
- **Bound the output.** Clamp the PWM duty so a runaway integral or a NaN in the fusion math cannot command full power. Check for NaN/Inf in the fused angle and freeze the output if you see one (a degenerate quaternion or a divide-by-zero `dt` produces NaN, and NaN propagates into a full-scale motor command).
- **Soft-start.** Ramp the motor from zero rather than slamming to the commanded duty, to limit inrush and mechanical shock.
- **E-stop.** A physical kill switch in the motor supply, independent of firmware. When the demo goes wrong on the bench — and it will — you want a button that cuts power without trusting the code that just misbehaved.

## 8. Summary

PWM is the universal actuator interface: 50 Hz pulse-width for servos, 20 kHz duty-cycle for motors. A servo is a self-contained angle command; a brushed DC motor needs an H-bridge for bidirectional four-quadrant drive, with shoot-through, back-EMF, and inrush as the hazards a driver IC handles for you; a stepper positions open-loop via a step/direction driver. The PID controller closes the loop: proportional for responsiveness, integral to kill steady-state error (with anti-windup), derivative for damping (computed on the measurement to avoid noise). Stacked together — IMU read, fusion to angle, PID to command, H-bridge to wheels, at 100 Hz with sub-millisecond compute — they balance a robot. Mind the latency budget and fail safe, because actuators move things and the bench is unforgiving.

That completes the week's theory: sensors (Lecture 1), fusion (Lecture 2), actuators and control (this lecture). The mini-project welds all three into a tilt-stabilized platform.

## References for this lecture

- RP2040 datasheet §4.5 "PWM", pp. 525–548. <https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf>
- Texas Instruments, "DRV8833 Dual H-Bridge Motor Driver" datasheet, SLVSAR1. <https://www.ti.com/lit/ds/symlink/drv8833.pdf>
- Allegro MicroSystems, "A4988 DMOS Microstepping Driver with Translator" datasheet — step/direction stepper interface.
- Åström & Murray, *Feedback Systems: An Introduction for Scientists and Engineers*, Princeton, 2008 — the PID chapter and anti-windup. <https://fbswiki.org/>
- Ziegler & Nichols, "Optimum Settings for Automatic Controllers", Trans. ASME, 1942 — the classic tuning rules.
- Pico SDK `hardware_pwm` API: <https://www.raspberrypi.com/documentation/pico-sdk/hardware.html#hardware_pwm>
- Beningo, J., *Reusable Firmware Development*, Apress 2017 — Ch. 9 on motor-control state machines (library reference, not free online).
