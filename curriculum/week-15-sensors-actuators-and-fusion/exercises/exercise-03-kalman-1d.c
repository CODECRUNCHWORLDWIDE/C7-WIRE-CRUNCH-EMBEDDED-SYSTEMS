/*
 * exercise-03-kalman-1d.c — A two-state scalar Kalman filter for one tilt axis.
 *
 * The complementary filter of exercise 2 has one fixed knob and no notion of
 * how much to trust each sensor at each instant. The Kalman filter generalizes
 * it: it carries a covariance estimate and adapts the blend dynamically, and
 * crucially it estimates the gyro bias online as a second state, so it tracks a
 * drifting bias that the fixed-alpha complementary filter cannot.
 *
 * The state vector is x = [angle, bias]^T. The process model:
 *   angle_k = angle_{k-1} + (rate - bias) * dt
 *   bias_k  = bias_{k-1}                          (bias is a slow random walk)
 * The measurement is the accelerometer-derived angle z = angle + noise.
 *
 * This is the classic Lauszus / Trammell-Hudson two-state attitude filter. It
 * is exactly seven additions, a handful of multiplies, and one division per
 * step — cheap enough to run per-axis at 1 kHz on a Cortex-M0+ without an FPU
 * (the RP2040 has no hardware float; the compiler emits the soft-float
 * routines, and a Kalman step still costs only a few microseconds).
 *
 * Build on the host:
 *   cc -std=c11 -DHOST_TEST -Wall -Wextra -O2 -lm \
 *      -o ex3test exercise-03-kalman-1d.c
 *
 * Build on the Pico:
 *   add_executable(exercise3 exercise-03-kalman-1d.c)
 *   target_link_libraries(exercise3 pico_stdlib hardware_i2c)
 *   pico_add_extra_outputs(exercise3)
 *
 * Citations:
 *   - Kalman, "A New Approach to Linear Filtering and Prediction Problems",
 *     Trans. ASME J. Basic Eng., 1960.
 *   - Lauszus, "A practical approach to Kalman filter and how to implement it",
 *     TKJ Electronics, 2012 — the embedded two-state formulation we follow.
 *   - Welch & Bishop, "An Introduction to the Kalman Filter", UNC TR 95-041.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "imu_common.h"

/* -------------------------------------------------------------------------
 * Initialize the filter with sane noise parameters. The three Q/R values are
 * the tuning surface:
 *   Q_angle   — how fast we let the angle estimate wander (process noise).
 *   Q_bias    — how fast we let the bias estimate wander.
 *   R_measure — how noisy we believe the accelerometer angle to be.
 * Larger R_measure => trust the accel less => smoother but laggier.
 * ----------------------------------------------------------------------- */

void cc_kalman1d_init(kalman1d_t *kf) {
    kf->angle = 0.0f;
    kf->bias  = 0.0f;
    kf->P[0][0] = 0.0f; kf->P[0][1] = 0.0f;
    kf->P[1][0] = 0.0f; kf->P[1][1] = 0.0f;
    /* Defaults tuned for a consumer MEMS IMU at ~100-200 Hz. */
    kf->Q_angle   = 0.001f;
    kf->Q_bias    = 0.003f;
    kf->R_measure = 0.03f;
}

/* -------------------------------------------------------------------------
 * One predict-update cycle.
 *   new_angle_meas — accelerometer-derived angle (radians).
 *   new_rate_meas  — gyro rate about this axis (rad/s).
 *   dt             — seconds since the previous call.
 * Returns the fused angle estimate (radians).
 * ----------------------------------------------------------------------- */

float cc_kalman1d_update(kalman1d_t *kf, float new_angle_meas,
                         float new_rate_meas, float dt) {
    /* ---- Predict ---- */
    /* The unbiased rate, then integrate into the angle. */
    float rate = new_rate_meas - kf->bias;
    kf->angle += dt * rate;

    /* Project the error covariance ahead: P = A P A^T + Q. With
       A = [[1, -dt], [0, 1]] this expands to the four updates below. */
    kf->P[0][0] += dt * (dt * kf->P[1][1] - kf->P[0][1] - kf->P[1][0] + kf->Q_angle);
    kf->P[0][1] -= dt * kf->P[1][1];
    kf->P[1][0] -= dt * kf->P[1][1];
    kf->P[1][1] += kf->Q_bias * dt;

    /* ---- Update (measurement) ---- */
    /* Innovation: difference between measured and predicted angle. */
    float y = new_angle_meas - kf->angle;

    /* Innovation covariance S and Kalman gain K. */
    float S = kf->P[0][0] + kf->R_measure;
    float K0 = kf->P[0][0] / S;
    float K1 = kf->P[1][0] / S;

    /* Correct the state. */
    kf->angle += K0 * y;
    kf->bias  += K1 * y;

    /* Correct the covariance. */
    float P00 = kf->P[0][0];
    float P01 = kf->P[0][1];
    kf->P[0][0] -= K0 * P00;
    kf->P[0][1] -= K0 * P01;
    kf->P[1][0] -= K1 * P00;
    kf->P[1][1] -= K1 * P01;

    return kf->angle;
}

#ifdef HOST_TEST

/* -------------------------------------------------------------------------
 * Synthetic test: a true angle that ramps from 0 to 45 deg over 10 s, a gyro
 * whose bias starts at 0 and creeps to 3 deg/s (a slow thermal drift), and a
 * noisy accelerometer. The Kalman filter should track the true angle AND
 * recover the bias, whereas a fixed-gain complementary filter would lag the
 * bias change.
 * ----------------------------------------------------------------------- */

static float frand(unsigned *st) {
    unsigned x = *st; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *st = x;
    return ((float)(x & 0xFFFFFF) / (float)0x800000) - 1.0f;
}

int main(void) {
    const float dt = 0.005f;       /* 200 Hz */
    const int   steps = 2000;      /* 10 seconds */
    unsigned rng = 0x1234ABCDu;

    kalman1d_t kf;
    cc_kalman1d_init(&kf);

    printf("=== Exercise 3 host Kalman 1-D test ===\n");
    printf("true angle ramps 0->45 deg over 10 s; gyro bias creeps 0->3 deg/s\n\n");

    float worst_err = 0.0f;
    float final_true = 0.0f, final_est = 0.0f, final_bias_true = 0.0f;

    for (int i = 0; i < steps; i++) {
        float t = (float)i * dt;
        float true_angle = (45.0f * CC_DEG_TO_RAD) * (t / 10.0f);  /* ramp */
        float true_rate  = (45.0f * CC_DEG_TO_RAD) / 10.0f;        /* deriv */
        float true_bias  = (3.0f * CC_DEG_TO_RAD) * (t / 10.0f);   /* creep */

        /* The gyro reports the true rate plus its bias plus noise. */
        float gyro = true_rate + true_bias + 0.002f * frand(&rng);
        /* The accel reports the true angle plus heavier noise. */
        float accel_angle = true_angle + 0.02f * frand(&rng);

        float est = cc_kalman1d_update(&kf, accel_angle, gyro, dt);

        float err = fabsf(est - true_angle);
        if (t > 2.0f && err > worst_err) worst_err = err;  /* skip warm-up */

        final_true = true_angle; final_est = est; final_bias_true = true_bias;
    }

    printf("final true angle:  %.2f deg\n", final_true * CC_RAD_TO_DEG);
    printf("final est angle:   %.2f deg\n", final_est * CC_RAD_TO_DEG);
    printf("final true bias:   %.3f deg/s\n", final_bias_true * CC_RAD_TO_DEG);
    printf("final est bias:    %.3f deg/s\n", kf.bias * CC_RAD_TO_DEG);
    printf("worst tracking err (post warm-up): %.2f deg\n",
           worst_err * CC_RAD_TO_DEG);

    int ok = (worst_err * CC_RAD_TO_DEG) < 2.0f &&
             fabsf((kf.bias - final_bias_true) * CC_RAD_TO_DEG) < 1.0f;
    printf("\n%s: Kalman %s tracked the angle and estimated the bias.\n",
           ok ? "PASS" : "FAIL", ok ? "correctly" : "did NOT correctly");
    return ok ? 0 : 1;
}

#else  /* On-device build. */

#include "pico/stdlib.h"
#include "hardware/i2c.h"

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
    i2c_write_reg(MPU9250_I2C_ADDR, MPU9250_SMPLRT_DIV, 0x04u);   /* 200 Hz */
    i2c_write_reg(MPU9250_I2C_ADDR, MPU9250_GYRO_CONFIG, MPU9250_GYRO_FS_500DPS);
    i2c_write_reg(MPU9250_I2C_ADDR, MPU9250_ACCEL_CONFIG, MPU9250_ACCEL_FS_4G);
    i2c_write_reg(MPU9250_I2C_ADDR, MPU9250_ACCEL_CONFIG2, 0x03u);
    sleep_ms(10);
    return true;
}
static bool imu_read(imu_raw_t *r) {
    uint8_t b[14];
    if (!i2c_read_regs(MPU9250_I2C_ADDR, MPU9250_ACCEL_XOUT_H, b, 14)) return false;
    r->ax = cc_be16(&b[0]); r->ay = cc_be16(&b[2]); r->az = cc_be16(&b[4]);
    r->gx = cc_be16(&b[8]); r->gy = cc_be16(&b[10]); r->gz = cc_be16(&b[12]);
    return true;
}

int main(void) {
    stdio_init_all();
    sleep_ms(2000);
    printf("\n=== Exercise 3: 1-D Kalman roll filter (live) ===\n");

    i2c_init(I2C_PORT, I2C_BAUD_HZ);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);

    if (!mpu9250_init()) {
        printf("MPU-9250 init failed.\n");
        while (true) sleep_ms(1000);
    }

    kalman1d_t kf_roll, kf_pitch;
    cc_kalman1d_init(&kf_roll);
    cc_kalman1d_init(&kf_pitch);

    /* Seed the filters with the first accelerometer reading so they do not
       have to ramp from zero. */
    imu_raw_t r; memset(&r, 0, sizeof(r));
    imu_read(&r);
    float ax = (float)r.ax / MPU9250_ACCEL_SENS_4G * CC_GRAVITY_MS2;
    float ay = (float)r.ay / MPU9250_ACCEL_SENS_4G * CC_GRAVITY_MS2;
    float az = (float)r.az / MPU9250_ACCEL_SENS_4G * CC_GRAVITY_MS2;
    kf_roll.angle  = atan2f(ay, az);
    kf_pitch.angle = atan2f(-ax, sqrtf(ay * ay + az * az));

    absolute_time_t prev = get_absolute_time();
    while (true) {
        if (!imu_read(&r)) { sleep_ms(5); continue; }
        absolute_time_t now = get_absolute_time();
        float dt = (float)absolute_time_diff_us(prev, now) / 1e6f;
        prev = now;

        ax = (float)r.ax / MPU9250_ACCEL_SENS_4G * CC_GRAVITY_MS2;
        ay = (float)r.ay / MPU9250_ACCEL_SENS_4G * CC_GRAVITY_MS2;
        az = (float)r.az / MPU9250_ACCEL_SENS_4G * CC_GRAVITY_MS2;
        float gx = (float)r.gx / MPU9250_GYRO_SENS_500DPS * CC_DEG_TO_RAD;
        float gy = (float)r.gy / MPU9250_GYRO_SENS_500DPS * CC_DEG_TO_RAD;

        float roll_acc  = atan2f(ay, az);
        float pitch_acc = atan2f(-ax, sqrtf(ay * ay + az * az));

        float roll  = cc_kalman1d_update(&kf_roll,  roll_acc,  gx, dt);
        float pitch = cc_kalman1d_update(&kf_pitch, pitch_acc, gy, dt);

        printf("roll=% 6.1f deg pitch=% 6.1f deg gyro_bias_roll=% .3f deg/s dt=%.4f\n",
               roll * CC_RAD_TO_DEG, pitch * CC_RAD_TO_DEG,
               kf_roll.bias * CC_RAD_TO_DEG, dt);
        sleep_ms(5);
    }
    return 0;
}

#endif /* HOST_TEST */

/* End of exercise-03-kalman-1d.c. */
