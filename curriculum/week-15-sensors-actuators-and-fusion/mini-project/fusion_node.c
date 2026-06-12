/*
 * fusion_node.c — Week 15 mini-project: tilt-stabilized platform.
 *
 * The full week stacked into one program:
 *   1. Read the MPU-9250 (Lecture 1 calibrated read path) at 100 Hz.
 *   2. Fuse accel + gyro + mag into a drift-free quaternion (Lecture 2,
 *      Madgwick MARG, madgwick.c).
 *   3. Convert to Euler; close a PID loop on pitch (Lecture 3) to keep a
 *      platform level by commanding a brushed DC motor, and mirror the roll
 *      onto a servo so a camera mount stays horizon-locked.
 *   4. Stream a CSV telemetry line over USB CDC so the host tool
 *      cc-orientation.py can live-plot orientation and drift.
 *
 * Two safety layers: a hardware watchdog the loop must kick (a stalled loop
 * coasts the motor on the next boot), and a NaN guard that coasts the motor
 * and freezes the servo if the fused angle goes non-finite.
 *
 * Wiring:
 *   I2C0: SDA=GP4, SCL=GP5  -> MPU-9250 @ 0x68, AK8963 @ 0x0C (bypass)
 *   Servo: GP16
 *   Motor (DRV8833): IN1=GP18 (PWM), IN2=GP19
 *   LED: GP25 (heartbeat)
 *
 * Build (Pico SDK):
 *   add_executable(fusion_node fusion_node.c madgwick.c actuators.c)
 *   target_include_directories(fusion_node PRIVATE ../exercises)
 *   target_link_libraries(fusion_node pico_stdlib hardware_i2c hardware_pwm
 *                         hardware_clocks hardware_watchdog)
 *   pico_add_extra_outputs(fusion_node)
 *
 * Citations:
 *   - RP2040 datasheet §4.3 (I2C), §4.5 (PWM), §4.7.6 (watchdog).
 *   - Madgwick 2010; Astrom & Murray (PID).
 */

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/watchdog.h"

#include "imu_common.h"
#include "madgwick.h"
#include "actuators.h"

#define I2C_PORT      i2c0
#define I2C_SDA_PIN   4
#define I2C_SCL_PIN   5
#define I2C_BAUD_HZ   400000

#define SERVO_GPIO    16
#define MOTOR_IN1     18
#define MOTOR_IN2     19
#define LED_GPIO      25

#define LOOP_HZ       100
#define WATCHDOG_MS   500   /* loop must kick within 500 ms or reboot */

/* -------------------------------------------------------------------------
 * Minimal MPU-9250 + AK8963 driver (subset of exercise 1, inlined so the
 * mini-project builds without cross-directory C linkage).
 * ----------------------------------------------------------------------- */

static bool i2c_wr(uint8_t addr, uint8_t reg, uint8_t val) {
    uint8_t b[2] = { reg, val };
    return i2c_write_blocking(I2C_PORT, addr, b, 2, false) == 2;
}
static bool i2c_rd(uint8_t addr, uint8_t reg, uint8_t *dst, size_t len) {
    if (i2c_write_blocking(I2C_PORT, addr, &reg, 1, true) != 1) return false;
    return i2c_read_blocking(I2C_PORT, addr, dst, len, false) == (int)len;
}

static bool mpu_init(void) {
    uint8_t who = 0;
    if (!i2c_rd(MPU9250_I2C_ADDR, MPU9250_WHO_AM_I, &who, 1)) return false;
    if (who != MPU9250_WHO_AM_I_VALUE) return false;
    i2c_wr(MPU9250_I2C_ADDR, MPU9250_PWR_MGMT_1, 0x80u); sleep_ms(100);
    i2c_wr(MPU9250_I2C_ADDR, MPU9250_PWR_MGMT_1, 0x01u); sleep_ms(10);
    i2c_wr(MPU9250_I2C_ADDR, MPU9250_PWR_MGMT_2, 0x00u);
    i2c_wr(MPU9250_I2C_ADDR, MPU9250_CONFIG, 0x03u);
    i2c_wr(MPU9250_I2C_ADDR, MPU9250_SMPLRT_DIV, 0x09u);   /* 100 Hz */
    i2c_wr(MPU9250_I2C_ADDR, MPU9250_GYRO_CONFIG, MPU9250_GYRO_FS_500DPS);
    i2c_wr(MPU9250_I2C_ADDR, MPU9250_ACCEL_CONFIG, MPU9250_ACCEL_FS_4G);
    i2c_wr(MPU9250_I2C_ADDR, MPU9250_ACCEL_CONFIG2, 0x03u);
    i2c_wr(MPU9250_I2C_ADDR, MPU9250_INT_PIN_CFG, 0x02u);  /* bypass on */
    sleep_ms(10);
    return true;
}

static bool ak_init(float asa[3]) {
    uint8_t wia = 0;
    if (!i2c_rd(AK8963_I2C_ADDR, AK8963_WIA, &wia, 1)) return false;
    if (wia != AK8963_WIA_VALUE) return false;
    i2c_wr(AK8963_I2C_ADDR, AK8963_CNTL1, AK8963_CNTL1_PWR_DOWN); sleep_ms(10);
    i2c_wr(AK8963_I2C_ADDR, AK8963_CNTL1, AK8963_CNTL1_FUSE_ROM); sleep_ms(10);
    uint8_t a[3];
    if (!i2c_rd(AK8963_I2C_ADDR, AK8963_ASAX, a, 3)) return false;
    for (int i = 0; i < 3; i++) asa[i] = ((float)a[i] - 128.0f) / 256.0f + 1.0f;
    i2c_wr(AK8963_I2C_ADDR, AK8963_CNTL1, AK8963_CNTL1_PWR_DOWN); sleep_ms(10);
    i2c_wr(AK8963_I2C_ADDR, AK8963_CNTL1, AK8963_CNTL1_CONT_16BIT); sleep_ms(10);
    return true;
}

static bool imu_read(imu_raw_t *r) {
    uint8_t b[14];
    if (!i2c_rd(MPU9250_I2C_ADDR, MPU9250_ACCEL_XOUT_H, b, 14)) return false;
    r->ax = cc_be16(&b[0]); r->ay = cc_be16(&b[2]); r->az = cc_be16(&b[4]);
    r->gx = cc_be16(&b[8]); r->gy = cc_be16(&b[10]); r->gz = cc_be16(&b[12]);

    uint8_t st1 = 0;
    if (i2c_rd(AK8963_I2C_ADDR, AK8963_ST1, &st1, 1) && (st1 & 0x01u)) {
        uint8_t m[7];
        if (i2c_rd(AK8963_I2C_ADDR, AK8963_HXL, m, 7) && !(m[6] & 0x08u)) {
            r->mx = cc_le16(&m[0]); r->my = cc_le16(&m[2]); r->mz = cc_le16(&m[4]);
        }
    }
    return true;
}

static void measure_gyro_bias(int n, float bias[3]) {
    double acc[3] = { 0, 0, 0 }; int got = 0;
    for (int i = 0; i < n; i++) {
        imu_raw_t r; memset(&r, 0, sizeof(r));
        if (imu_read(&r)) { acc[0] += r.gx; acc[1] += r.gy; acc[2] += r.gz; got++; }
        sleep_ms(2);
    }
    for (int i = 0; i < 3; i++) {
        float c = got ? (float)(acc[i] / got) : 0.0f;
        bias[i] = (c / MPU9250_GYRO_SENS_500DPS) * CC_DEG_TO_RAD;
    }
}

/* -------------------------------------------------------------------------
 * Main.
 * ----------------------------------------------------------------------- */

int main(void) {
    stdio_init_all();
    sleep_ms(2000);

    gpio_init(LED_GPIO);
    gpio_set_dir(LED_GPIO, GPIO_OUT);

    printf("\n# Week 15 mini-project: tilt-stabilized platform\n");

    /* I2C + sensors. */
    i2c_init(I2C_PORT, I2C_BAUD_HZ);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);

    if (!mpu_init()) {
        printf("# FATAL: MPU-9250 not found\n");
        while (true) { gpio_xor_mask(1u << LED_GPIO); sleep_ms(100); }
    }

    /* The mag hard/soft-iron calibration would come from cc-magcal.py; we
       ship identity here and let the figure-eight populate it in homework. */
    imu_calibration_t cal;
    memset(&cal, 0, sizeof(cal));
    for (int i = 0; i < 3; i++) {
        cal.accel_scale[i] = 1.0f;
        cal.mag_soft_iron[i] = 1.0f;
        cal.mag_sensitivity_adj[i] = 1.0f;
    }
    bool have_mag = ak_init(cal.mag_sensitivity_adj);
    printf("# magnetometer: %s\n", have_mag ? "present" : "absent (6-axis mode)");

    printf("# measuring gyro bias, keep still...\n");
    measure_gyro_bias(512, cal.gyro_bias);
    printf("# gyro bias rad/s: %.5f %.5f %.5f\n",
           cal.gyro_bias[0], cal.gyro_bias[1], cal.gyro_bias[2]);

    /* Fusion filter. beta from an assumed 5 deg/s gyro error. */
    madgwick_t mad;
    madgwick_init(&mad, 5.0f * CC_DEG_TO_RAD);

    /* Actuators. */
    servo_t servo;
    motor_t motor;
    servo_init(&servo, SERVO_GPIO);
    motor_init(&motor, MOTOR_IN1, MOTOR_IN2);

    /* Balance PID on pitch. Setpoint is upright (pitch = 0). Starting gains;
       tune on hardware (homework). Output is a motor command in [-1, 1]. */
    pid_t balance_pid = {
        .kp = 0.04f, .ki = 0.002f, .kd = 0.6f,
        .integrator = 0.0f, .prev_error = 0.0f,
        .out_min = -1.0f, .out_max = 1.0f,
        .integ_min = -200.0f, .integ_max = 200.0f,
    };

    /* Arm the watchdog AFTER the slow init (bias measurement) so we do not
       trip it during the 1-second-plus calibration. */
    watchdog_enable(WATCHDOG_MS, true /* pause on debug */);

    printf("# CSV: t_ms,roll_deg,pitch_deg,yaw_deg,motor_cmd,servo_deg\n");

    const float loop_dt_nominal = 1.0f / (float)LOOP_HZ;
    absolute_time_t prev = get_absolute_time();
    absolute_time_t t_start = prev;
    uint32_t tick = 0;

    while (true) {
        watchdog_update();   /* kick the dog */

        imu_raw_t raw; memset(&raw, 0, sizeof(raw));
        if (!imu_read(&raw)) {
            /* A read failure is a fault: coast the motor and retry. */
            motor_coast(&motor);
            sleep_ms(5);
            continue;
        }

        absolute_time_t now = get_absolute_time();
        float dt = (float)absolute_time_diff_us(prev, now) / 1e6f;
        prev = now;
        if (dt <= 0.0f || dt > 0.2f) dt = loop_dt_nominal;  /* sanitize */

        imu_sample_t s;
        cc_imu_apply_calibration(&raw, &cal,
                                 MPU9250_GYRO_SENS_500DPS,
                                 MPU9250_ACCEL_SENS_4G, dt, &s);

        /* Fuse. Use MARG if the magnetometer is calibrated and present. */
        if (have_mag) {
            madgwick_update_marg(&mad, s.gx, s.gy, s.gz,
                                 s.ax, s.ay, s.az, s.mx, s.my, s.mz, dt);
        } else {
            madgwick_update_imu(&mad, s.gx, s.gy, s.gz, s.ax, s.ay, s.az, dt);
        }

        euler_t e = quat_to_euler(mad.q);

        /* NaN guard: if the fusion produced a non-finite angle, coast and
           freeze before a NaN propagates into a full-scale motor command. */
        if (!isfinite(e.pitch) || !isfinite(e.roll)) {
            motor_coast(&motor);
            madgwick_init(&mad, 5.0f * CC_DEG_TO_RAD);  /* reset the filter */
            balance_pid.integrator = 0.0f;
            continue;
        }

        /* Balance: drive the motor to null the pitch. */
        float motor_cmd = cc_pid_update(&balance_pid, 0.0f, e.pitch, dt);
        motor_set(&motor, motor_cmd);

        /* Horizon-lock: mirror the roll onto the servo so a camera mount
           stays level (servo angle = -roll, clamped to the servo range). */
        float servo_deg = cc_clampf(-e.roll * CC_RAD_TO_DEG, -90.0f, 90.0f);
        servo_set_angle(&servo, servo_deg);

        /* Telemetry at the full loop rate; CSV for cc-orientation.py. */
        uint32_t t_ms = (uint32_t)(absolute_time_diff_us(t_start, now) / 1000);
        printf("%lu,%.2f,%.2f,%.2f,%.3f,%.1f\n",
               (unsigned long)t_ms,
               e.roll * CC_RAD_TO_DEG,
               e.pitch * CC_RAD_TO_DEG,
               e.yaw * CC_RAD_TO_DEG,
               motor_cmd, servo_deg);

        /* Heartbeat LED at ~2 Hz. */
        if ((tick % (LOOP_HZ / 2)) == 0) gpio_xor_mask(1u << LED_GPIO);
        tick++;

        /* Pace the loop to LOOP_HZ. */
        int64_t spent_us = absolute_time_diff_us(now, get_absolute_time());
        int64_t budget_us = 1000000 / LOOP_HZ;
        if (spent_us < budget_us) {
            sleep_us((uint32_t)(budget_us - spent_us));
        }
    }
    return 0;
}

/* End of fusion_node.c. */
