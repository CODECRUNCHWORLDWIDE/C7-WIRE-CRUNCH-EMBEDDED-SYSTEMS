/*
 * exercise-02-complementary-filter.c — Fuse gyro and accelerometer into a
 * stable roll/pitch estimate with a complementary filter.
 *
 * A complementary filter is the cheapest honest sensor fusion you can write.
 * It exploits the fact that the two sensors fail in opposite frequency bands:
 *   - The gyroscope is accurate over short intervals but its integration
 *     drifts without bound over minutes (bias integrates into a ramp).
 *   - The accelerometer measures the gravity vector — a perfect long-term
 *     tilt reference — but is swamped by linear acceleration and vibration
 *     over short intervals.
 * The complementary filter high-passes the gyro and low-passes the accel and
 * sums them. One tuning knob, `alpha`, sets the crossover. The whole filter is
 * one line of arithmetic per axis; there is no matrix, no covariance, no
 * iteration. It runs in nanoseconds on a Cortex-M0+ and it is good enough for
 * a startling number of real products (self-balancing robots, camera gimbals
 * at the cheap end, the level indicator in your phone's compass app).
 *
 * This file builds two ways: HOST_TEST replays a synthetic dataset and checks
 * the fused angle converges; the on-device build streams live angles from the
 * MPU-9250 (reusing the exercise-01 read/calibrate code path).
 *
 * Build on the host:
 *   cc -std=c11 -DHOST_TEST -Wall -Wextra -O2 -lm \
 *      -o ex2test exercise-02-complementary-filter.c
 *
 * Build on the Pico:
 *   add_executable(exercise2 exercise-02-complementary-filter.c)
 *   target_link_libraries(exercise2 pico_stdlib hardware_i2c)
 *   pico_add_extra_outputs(exercise2)
 *
 * Citations:
 *   - Higgins, "A comparison of complementary and Kalman filtering", IEEE
 *     Trans. Aerospace and Electronic Systems, 1975 — the original framing of
 *     the complementary filter as a frequency-domain blend.
 *   - Colton, "The Balance Filter", MIT, 2007 — the canonical embedded write-up.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "imu_common.h"

/* -------------------------------------------------------------------------
 * The filter step. roll and pitch are in radians.
 *
 * The accelerometer angle for roll (rotation about the body X axis) is
 *   roll_accel  = atan2(ay, az)
 * and for pitch (rotation about Y):
 *   pitch_accel = atan2(-ax, sqrt(ay^2 + az^2))
 * The gyro contribution integrates the body rates over dt. We blend:
 *   angle = alpha * (angle + gyro_rate * dt) + (1 - alpha) * angle_accel
 *
 * alpha is the gyro-trust fraction. A common choice: alpha = tau / (tau + dt)
 * where tau is the desired time constant of the high/low-pass crossover. For
 * tau = 0.5 s at dt = 0.01 s, alpha = 0.98. We let the caller set alpha.
 * ----------------------------------------------------------------------- */

void cc_complementary_update(complementary_t *cf, const imu_sample_t *s) {
    float dt = s->dt;

    /* Accelerometer tilt angles. atan2f is well-defined everywhere except the
       free-fall singularity (a == 0), which we guard against below. */
    float roll_accel  = atan2f(s->ay, s->az);
    float pitch_accel = atan2f(-s->ax, sqrtf(s->ay * s->ay + s->az * s->az));

    /* Gyro integration. Body rates are already in rad/s after calibration. */
    float roll_gyro  = cf->roll  + s->gx * dt;
    float pitch_gyro = cf->pitch + s->gy * dt;

    /* If the accelerometer magnitude is far from 1 g, the board is undergoing
       linear acceleration and the accel tilt is untrustworthy this instant;
       lean harder on the gyro by pushing alpha toward 1 for this step. */
    float a_mag = sqrtf(s->ax * s->ax + s->ay * s->ay + s->az * s->az);
    float a_err = fabsf(a_mag - CC_GRAVITY_MS2) / CC_GRAVITY_MS2;
    float alpha = cf->alpha;
    if (a_err > 0.15f) {
        /* Accel disagrees with gravity by >15%; trust gyro almost fully. */
        alpha = 0.999f;
    }

    cf->roll  = alpha * roll_gyro  + (1.0f - alpha) * roll_accel;
    cf->pitch = alpha * pitch_gyro + (1.0f - alpha) * pitch_accel;
}

#ifdef HOST_TEST

/* -------------------------------------------------------------------------
 * Synthetic test. We simulate a board that is tilted to a true roll of 30 deg
 * and held there, with a noisy gyro that has a constant bias of 2 deg/s and a
 * noisy accelerometer. A correct complementary filter should converge to ~30
 * deg and stay there despite the gyro bias (which a raw gyro integration would
 * ramp away without bound).
 * ----------------------------------------------------------------------- */

static float frand(unsigned *state) {
    /* xorshift32 -> [-1, 1) */
    unsigned x = *state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *state = x;
    return ((float)(x & 0xFFFFFF) / (float)0x800000) - 1.0f;
}

int main(void) {
    const float true_roll = 30.0f * CC_DEG_TO_RAD;
    const float gyro_bias = 2.0f * CC_DEG_TO_RAD;   /* rad/s, constant */
    const float dt = 0.01f;                         /* 100 Hz */
    const int   steps = 6000;                       /* 60 seconds */

    complementary_t cf = { 0.0f, 0.0f, 0.98f };
    unsigned rng = 0xC0FFEEu;

    /* Track a naive gyro-only integration for comparison. */
    float naive_roll = 0.0f;

    printf("=== Exercise 2 host complementary-filter test ===\n");
    printf("true roll = %.1f deg, gyro bias = %.1f deg/s, 60 s @ 100 Hz\n\n",
           true_roll * CC_RAD_TO_DEG, gyro_bias * CC_RAD_TO_DEG);

    for (int i = 0; i < steps; i++) {
        imu_sample_t s;
        memset(&s, 0, sizeof(s));
        s.dt = dt;
        /* The static gravity vector for a 30-deg roll: a = R(roll) * [0,0,g]. */
        s.ax = 0.0f + 0.05f * frand(&rng);
        s.ay = CC_GRAVITY_MS2 * sinf(true_roll) + 0.05f * frand(&rng);
        s.az = CC_GRAVITY_MS2 * cosf(true_roll) + 0.05f * frand(&rng);
        /* The gyro reads ~0 (board is static) plus its constant bias + noise. */
        s.gx = gyro_bias + 0.002f * frand(&rng);

        cc_complementary_update(&cf, &s);
        naive_roll += s.gx * dt;
    }

    float err_deg = (cf.roll - true_roll) * CC_RAD_TO_DEG;
    printf("complementary roll after 60 s: %.2f deg (error %.2f deg)\n",
           cf.roll * CC_RAD_TO_DEG, err_deg);
    printf("naive gyro-only roll after 60 s: %.2f deg (drifted %.1f deg)\n",
           naive_roll * CC_RAD_TO_DEG,
           naive_roll * CC_RAD_TO_DEG);

    int ok = fabsf(err_deg) < 2.0f;     /* should stay within 2 deg of truth */
    printf("\n%s: complementary filter %s the gyro bias.\n",
           ok ? "PASS" : "FAIL", ok ? "rejected" : "did NOT reject");
    return ok ? 0 : 1;
}

#else  /* On-device build. */

#include "pico/stdlib.h"
#include "hardware/i2c.h"

/* The MPU-9250 driver below is the minimal subset from exercise 1; kept here
   so this file builds standalone without cross-file linkage on the bench. */

#define I2C_PORT      i2c0
#define I2C_SDA_PIN   4
#define I2C_SCL_PIN   5
#define I2C_BAUD_HZ   400000

static bool i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t val) {
    uint8_t buf[2] = { reg, val };
    return i2c_write_blocking(I2C_PORT, addr, buf, 2, false) == 2;
}

static bool i2c_read_regs(uint8_t addr, uint8_t reg, uint8_t *dst, size_t len) {
    if (i2c_write_blocking(I2C_PORT, addr, &reg, 1, true) != 1) return false;
    return i2c_read_blocking(I2C_PORT, addr, dst, len, false) == (int)len;
}

static bool mpu9250_init(void) {
    uint8_t who = 0;
    if (!i2c_read_regs(MPU9250_I2C_ADDR, MPU9250_WHO_AM_I, &who, 1)) return false;
    if (who != MPU9250_WHO_AM_I_VALUE) return false;
    i2c_write_reg(MPU9250_I2C_ADDR, MPU9250_PWR_MGMT_1, 0x80u); sleep_ms(100);
    i2c_write_reg(MPU9250_I2C_ADDR, MPU9250_PWR_MGMT_1, 0x01u); sleep_ms(10);
    i2c_write_reg(MPU9250_I2C_ADDR, MPU9250_PWR_MGMT_2, 0x00u);
    i2c_write_reg(MPU9250_I2C_ADDR, MPU9250_CONFIG, 0x03u);
    i2c_write_reg(MPU9250_I2C_ADDR, MPU9250_SMPLRT_DIV, 0x09u);
    i2c_write_reg(MPU9250_I2C_ADDR, MPU9250_GYRO_CONFIG, MPU9250_GYRO_FS_500DPS);
    i2c_write_reg(MPU9250_I2C_ADDR, MPU9250_ACCEL_CONFIG, MPU9250_ACCEL_FS_4G);
    i2c_write_reg(MPU9250_I2C_ADDR, MPU9250_ACCEL_CONFIG2, 0x03u);
    sleep_ms(10);
    return true;
}

static bool imu_read_accel_gyro(imu_raw_t *raw) {
    uint8_t b[14];
    if (!i2c_read_regs(MPU9250_I2C_ADDR, MPU9250_ACCEL_XOUT_H, b, 14)) return false;
    raw->ax = cc_be16(&b[0]); raw->ay = cc_be16(&b[2]); raw->az = cc_be16(&b[4]);
    raw->gx = cc_be16(&b[8]); raw->gy = cc_be16(&b[10]); raw->gz = cc_be16(&b[12]);
    return true;
}

static void measure_gyro_bias(int samples, float bias[3]) {
    double acc[3] = { 0, 0, 0 }; int got = 0;
    for (int i = 0; i < samples; i++) {
        imu_raw_t r; memset(&r, 0, sizeof(r));
        if (imu_read_accel_gyro(&r)) {
            acc[0] += r.gx; acc[1] += r.gy; acc[2] += r.gz; got++;
        }
        sleep_ms(2);
    }
    for (int i = 0; i < 3; i++) {
        float c = got ? (float)(acc[i] / got) : 0.0f;
        bias[i] = (c / MPU9250_GYRO_SENS_500DPS) * CC_DEG_TO_RAD;
    }
}

int main(void) {
    stdio_init_all();
    sleep_ms(2000);
    printf("\n=== Exercise 2: complementary filter (live) ===\n");

    i2c_init(I2C_PORT, I2C_BAUD_HZ);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);

    if (!mpu9250_init()) {
        printf("MPU-9250 init failed.\n");
        while (true) sleep_ms(1000);
    }

    float bias[3];
    printf("measuring gyro bias (keep still)...\n");
    measure_gyro_bias(512, bias);
    printf("bias rad/s: %.5f %.5f %.5f\n", bias[0], bias[1], bias[2]);

    complementary_t cf = { 0.0f, 0.0f, 0.98f };
    absolute_time_t prev = get_absolute_time();

    while (true) {
        imu_raw_t raw; memset(&raw, 0, sizeof(raw));
        if (!imu_read_accel_gyro(&raw)) { sleep_ms(10); continue; }

        absolute_time_t now = get_absolute_time();
        float dt = (float)absolute_time_diff_us(prev, now) / 1e6f;
        prev = now;

        imu_sample_t s;
        memset(&s, 0, sizeof(s));
        s.ax = ((float)raw.ax / MPU9250_ACCEL_SENS_4G) * CC_GRAVITY_MS2;
        s.ay = ((float)raw.ay / MPU9250_ACCEL_SENS_4G) * CC_GRAVITY_MS2;
        s.az = ((float)raw.az / MPU9250_ACCEL_SENS_4G) * CC_GRAVITY_MS2;
        s.gx = ((float)raw.gx / MPU9250_GYRO_SENS_500DPS) * CC_DEG_TO_RAD - bias[0];
        s.gy = ((float)raw.gy / MPU9250_GYRO_SENS_500DPS) * CC_DEG_TO_RAD - bias[1];
        s.dt = dt;

        cc_complementary_update(&cf, &s);

        printf("roll=% 6.1f deg  pitch=% 6.1f deg  dt=%.4f\n",
               cf.roll * CC_RAD_TO_DEG, cf.pitch * CC_RAD_TO_DEG, dt);
        sleep_ms(10);
    }
    return 0;
}

#endif /* HOST_TEST */

/* End of exercise-02-complementary-filter.c. */
