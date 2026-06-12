# Mini-Project — Tilt-Stabilized Platform (IMU Fusion → PID → Actuators)

## Brief

Build a complete sensor-fusion-and-control system on the RP2040. The Pico reads
a 9-axis MPU-9250 at 100 Hz, fuses accel + gyro + magnetometer into a
drift-free orientation quaternion with a Madgwick filter, and closes two control
loops: a PID balance loop that drives a brushed DC motor to keep a platform
upright (the canonical self-balancing demo), and a horizon-lock loop that mirrors
the roll onto a hobby servo so a camera mount stays level. A host tool live-plots
the orientation and reports yaw drift over a 10-minute run.

End-to-end demonstration: power the Pico, hold the board level for the gyro-bias
measurement, then tilt it. The fused pitch/roll/yaw track the physical board
with sub-degree noise and no perceptible drift; the motor command nulls the
pitch; the servo counter-rotates the roll; and `cc-orientation.py --plot` shows
three smooth traces on your laptop. Held still for 10 minutes, yaw drifts under
~2°/min thanks to the magnetometer discipline (versus tens of degrees for a
gyro-only integration).

Allocate 4 hours for this mini-project.

## Deliverables

In `mini-project/` of your fork:

- `fusion_node.c` — the main program: sensor read, fusion, PID, actuation, telemetry.
- `madgwick.c` / `madgwick.h` — the quaternion MARG/IMU orientation filter.
- `actuators.c` / `actuators.h` — servo and H-bridge motor drivers, plus the PID step.
- `imu_common.h` — the shared header (the same file from `exercises/`).
- `CMakeLists.txt` — the build configuration, producing `fusion_node.uf2`.
- `cc-orientation.py` — the host telemetry + live-plot + drift-report tool.
- `cc-magcal.py` — the magnetometer hard/soft-iron calibration helper.
- `requirements.txt` — Python deps.

## The architecture

```text
+----------------------+        +-----------------------------------+
| Host laptop          |        | RP2040 Pico (fusion_node)         |
|                      |        |                                   |
| cc-orientation.py    | <-USB- |  100 Hz control loop:             |
|   reads CSV stream   |  CDC   |   1. read MPU-9250 (I2C, ~0.5ms)  |
|   live-plots r/p/y   |        |   2. calibrate -> SI units        |
|   reports yaw drift  |        |   3. Madgwick MARG -> quaternion  |
|                      |        |   4. quat -> euler (pitch,roll)   |
| cc-magcal.py         |        |   5. PID(pitch) -> motor cmd      |
|   reads mx,my,mz     |        |   6. roll -> servo angle          |
|   fits hard/soft iron|        |   7. CSV telemetry out            |
|   emits mag_cal.h    |        |   8. kick watchdog                |
+----------------------+        |                                   |
                                |  Actuators:                       |
   I2C0 (GP4/GP5)               |   DRV8833 H-bridge -> DC motor    |
   +-- MPU-9250 @ 0x68          |   GP16 PWM -> hobby servo         |
   +-- AK8963   @ 0x0C (bypass) |   GP25 -> heartbeat LED           |
                                +-----------------------------------+
```

## Build prerequisites

You have:

- Pico SDK 1.5.1 or later, with `PICO_SDK_PATH` set.
- `arm-none-eabi-gcc` 10.3 or later, CMake, `picotool`.
- Python 3.10+ with `pyserial>=3.5` (and `matplotlib` for `--plot`).
- An MPU-9250 (or ICM-20948) breakout, a DRV8833 (or TB6612FNG) H-bridge, a
  brushed DC motor, a hobby servo, and a separate motor supply (do NOT power the
  motor from the Pico's 3V3 — use a 5-6 V pack and common-ground it).
- A Pico (W or non-W).

## Wiring

| Signal        | Pico pin | To                              |
|---------------|----------|---------------------------------|
| I2C0 SDA      | GP4      | MPU-9250 SDA                    |
| I2C0 SCL      | GP5      | MPU-9250 SCL                    |
| 3V3           | 3V3(OUT) | MPU-9250 VCC                    |
| Servo PWM     | GP16     | servo signal                    |
| Motor IN1     | GP18     | DRV8833 AIN1                    |
| Motor IN2     | GP19     | DRV8833 AIN2                    |
| LED           | GP25     | (on-board)                      |
| GND           | GND      | common ground (Pico+motor+IMU)  |

The servo's power and the motor's power come from the external pack, not the
Pico. Common-ground everything or the I2C and PWM references float.

## Step-by-step build

### Step 1 — Build the firmware

```bash
cd mini-project
mkdir -p build && cd build
cmake -G "Unix Makefiles" ..
make -j8
arm-none-eabi-size fusion_node.elf
#   text    data     bss     dec     hex filename
#  41200     264    3072   44536    adf8 fusion_node.elf
```

The full fusion + actuators + telemetry image is ~41 KB of text — tiny. The
Madgwick MARG update is the heaviest compute (~15 µs), well within the 10 ms
loop budget.

### Step 2 — Flash via BOOTSEL

Hold BOOTSEL, plug in USB, drag `fusion_node.uf2` onto the `RPI-RP2` drive. The
Pico reboots and begins the gyro-bias measurement.

### Step 3 — First run: confirm the read path

```bash
screen /dev/cu.usbmodem* 115200
```

You should see:

```text
# Week 15 mini-project: tilt-stabilized platform
# magnetometer: present
# measuring gyro bias, keep still...
# gyro bias rad/s: -0.00214 0.00131 -0.00077
# CSV: t_ms,roll_deg,pitch_deg,yaw_deg,motor_cmd,servo_deg
0,0.12,-0.34,87.10,0.001,-0.1
10,0.11,-0.33,87.10,0.001,-0.1
...
```

Tilt the board and confirm roll/pitch track it the right way. If an axis moves
backwards, your IMU is mounted rotated relative to the body frame — fix the axis
permutation in `imu_read`, not in the filter.

### Step 4 — Calibrate the magnetometer

A calibration build of the firmware (set `#define CALIBRATE_MAG 1` and rebuild,
or use the dedicated `magcal` target) prints raw `mx,my,mz` lines. Run:

```bash
python3 cc-magcal.py --seconds 30
# rotate the board through ALL orientations...
# hard-iron offset (uT): 12.40 -8.10 31.20
# soft-iron diagonal:    1.0410 0.9870 0.9760
# wrote mag_cal.h
```

Paste `mag_cal.h`'s constants into the `imu_calibration_t` initializer in
`fusion_node.c`, rebuild, reflash. The yaw is now accurate; without this step,
the magnetometer pulls yaw toward whatever steel is nearest.

### Step 5 — Live-plot and measure drift

```bash
python3 cc-orientation.py --plot --log run.csv --seconds 600
#   t=  60.0s  roll=   0.3  pitch=  -0.2  yaw=  87.4  motor=+0.002
#   ...
# cc-orientation: yaw drift = +1.10 deg over 10.0 min (+0.11 deg/min)
```

Hold the board still for the full 10 minutes. The yaw-drift line is the headline
deliverable: a properly-calibrated MARG fusion drifts under ~2°/min; a gyro-only
integration of the same data drifts tens of degrees (the homework asks you to
log both and overlay them).

### Step 6 — Close the balance loop

Mount the Pico + IMU + motor + wheels on a balancing chassis (or just hold the
motor and watch the command). Tilt the platform forward; `motor_cmd` should go
positive (drive the wheels forward to get under the center of mass), and
negative when you tilt back. Tune the PID:

1. Set `ki = kd = 0`. Raise `kp` until the platform oscillates around upright,
   then halve it.
2. Add `kd` (damping from the gyro-derived rate) until the oscillation settles.
3. Add a small `ki` to trim residual lean.

Starting gains in `fusion_node.c` (`kp=0.04, ki=0.002, kd=0.6`) are a reasonable
launch point for a light chassis; expect to retune for your hardware.

## Pass criteria

- `fusion_node.elf` builds clean with `-Wall -Wextra` and no warnings.
- Roll, pitch, and yaw track the physical board with < 1° steady-state noise.
- Yaw drift over a 10-minute stationary run is under ~2°/min (with the
  magnetometer calibrated).
- The Madgwick filter recovers correctly from a deliberate disturbance: tilt the
  board to 80°, hold 5 s, return to level — the estimate follows without
  gimbal-lock artifacts (the Euler readout near ±90° pitch may wrap, but the
  underlying quaternion stays valid).
- The motor command nulls pitch (positive when tilted forward, negative when
  tilted back) and the servo counter-rotates roll.
- The watchdog works: comment out `watchdog_update()`, reflash, and confirm the
  device reboots every 500 ms (proving the loop-stall safety net).
- A `run.csv` from one 10-minute session plus a drift plot are committed with the
  writeup.

## Common bringup gotchas

1. **Magnetometer reads as big-endian.** The AK8963 is little-endian (`cc_le16`),
   unlike the MPU's big-endian accel/gyro (`cc_be16`). Mixing them up produces a
   yaw that looks plausible but is wrong; the figure-eight calibration will fit a
   bizarre ellipsoid. Decode mag with `cc_le16`.
2. **No ST2 read after the mag data.** The AK8963 latches until you read ST2;
   skip it and the magnetometer freezes after one sample. The driver reads 7
   bytes (`HXL`..`ST2`) in one burst.
3. **Motor powered from the Pico.** A motor's inrush browns out the 3V3 rail and
   resets the Pico mid-loop. Use a separate pack, common-ground it.
4. **Gyro bias measured while moving.** Any vibration during the 512-sample bias
   measurement skews the estimate; the orientation then drifts. Measure on a
   solid surface, board still.
5. **`beta` too high or too low.** Too high (`> 0.5`) and accel/mag noise shakes
   the orientation; too low (`< 0.02`) and it drifts. The default `beta` from a
   5°/s gyro-error assumption is a good start; lower it if the output is jittery.
6. **NaN in the quaternion freezes the motor.** A divide-by-zero `dt` (two reads
   in the same microsecond) or a zero accel vector can produce NaN. The NaN guard
   in `fusion_node.c` catches it and resets the filter; if you see the guard fire
   repeatedly, your `dt` sanitization is wrong.

## Bench session structure

- **Saturday 9–11 AM** — build and flash; confirm the calibrated read path and
  the live CSV stream.
- **Saturday 11 AM–1 PM** — magnetometer figure-eight calibration; bake `mag_cal.h`;
  verify tilt-compensated heading.
- **Saturday 2–4 PM** — Madgwick tuning (`beta`); 10-minute drift run; overlay
  versus gyro-only.
- **Sunday morning** — close the PID balance loop, tune gains, commit the run
  log and plots.

## References

- All of Week 15's lecture notes.
- Madgwick, Univ. of Bristol 2010.
- InvenSense MPU-9250 register map; AK8963 datasheet.
- RP2040 datasheet §4.3 (I2C), §4.5 (PWM), §4.7.6 (watchdog).
- TI DRV8833 datasheet.
- Åström & Murray, *Feedback Systems* (PID).
