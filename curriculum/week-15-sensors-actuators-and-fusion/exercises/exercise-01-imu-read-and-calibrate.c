/*
 * exercise-01-imu-read-and-calibrate.c — Read the MPU-9250 over I2C, apply a
 * calibration, and convert raw counts to SI units.
 *
 * This is the foundation exercise of Week 15. Everything downstream — the
 * complementary filter, the Kalman filter, the Madgwick filter in the
 * mini-project — consumes the SI-unit `imu_sample_t` that this code produces.
 * Garbage calibration here is garbage orientation everywhere else, so we treat
 * the conversion arithmetic as load-bearing and test it byte-for-byte.
 *
 * The exercise has two halves:
 *   1. A pure function `cc_imu_apply_calibration` that takes raw counts plus a
 *      calibration and emits SI units. This compiles and runs on the host so
 *      you can unit-test it against hand-computed expected values.
 *   2. A Pico main() that brings up I2C0, configures the MPU-9250, measures the
 *      gyro bias over 512 stationary samples, and streams calibrated samples to
 *      the CDC serial port.
 *
 * Build on the Pico (with the Pico SDK):
 *   add_executable(exercise1 exercise-01-imu-read-and-calibrate.c)
 *   target_link_libraries(exercise1 pico_stdlib hardware_i2c)
 *   pico_add_extra_outputs(exercise1)
 *
 * Build on the host (to run the conversion unit test only):
 *   cc -std=c11 -DHOST_TEST -Wall -Wextra -O2 -lm \
 *      -o ex1test exercise-01-imu-read-and-calibrate.c
 *
 * Citations:
 *   - InvenSense MPU-9250 Register Map, RM-MPU-9250A-00, rev 1.6.
 *   - InvenSense MPU-9250 Product Specification §3 (sensitivity scale factors).
 *   - RP2040 datasheet §4.3 (I2C), pp. 463-510.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>

#include "imu_common.h"

/* -------------------------------------------------------------------------
 * The pure conversion function. Host-testable.
 *
 * Raw -> SI:
 *   accel: counts / accel_sens -> g, then * GRAVITY -> m/s^2, then
 *          subtract per-axis offset and multiply per-axis scale.
 *   gyro:  counts / gyro_sens  -> deg/s, then * DEG_TO_RAD -> rad/s,
 *          then subtract gyro bias.
 *   mag:   counts * UT_PER_LSB * factory_ASA -> microtesla, then subtract
 *          hard-iron and apply the diagonal soft-iron scale.
 * ----------------------------------------------------------------------- */

void cc_imu_apply_calibration(const imu_raw_t *raw,
                              const imu_calibration_t *cal,
                              float gyro_sens, float accel_sens,
                              float dt, imu_sample_t *out) {
    /* Accelerometer: counts -> g -> m/s^2, then offset + scale. */
    float ax_g = (float)raw->ax / accel_sens;
    float ay_g = (float)raw->ay / accel_sens;
    float az_g = (float)raw->az / accel_sens;

    out->ax = (ax_g * CC_GRAVITY_MS2 - cal->accel_offset[0]) * cal->accel_scale[0];
    out->ay = (ay_g * CC_GRAVITY_MS2 - cal->accel_offset[1]) * cal->accel_scale[1];
    out->az = (az_g * CC_GRAVITY_MS2 - cal->accel_offset[2]) * cal->accel_scale[2];

    /* Gyroscope: counts -> deg/s -> rad/s, then subtract the measured bias. */
    out->gx = ((float)raw->gx / gyro_sens) * CC_DEG_TO_RAD - cal->gyro_bias[0];
    out->gy = ((float)raw->gy / gyro_sens) * CC_DEG_TO_RAD - cal->gyro_bias[1];
    out->gz = ((float)raw->gz / gyro_sens) * CC_DEG_TO_RAD - cal->gyro_bias[2];

    /* Magnetometer: counts -> microtesla with the AK8963 factory ASA, then
       hard-iron subtraction, then diagonal soft-iron scaling. */
    float mx_ut = (float)raw->mx * AK8963_UT_PER_LSB * cal->mag_sensitivity_adj[0];
    float my_ut = (float)raw->my * AK8963_UT_PER_LSB * cal->mag_sensitivity_adj[1];
    float mz_ut = (float)raw->mz * AK8963_UT_PER_LSB * cal->mag_sensitivity_adj[2];

    out->mx = (mx_ut - cal->mag_hard_iron[0]) * cal->mag_soft_iron[0];
    out->my = (my_ut - cal->mag_hard_iron[1]) * cal->mag_soft_iron[1];
    out->mz = (mz_ut - cal->mag_hard_iron[2]) * cal->mag_soft_iron[2];

    out->dt = dt;
}

/* -------------------------------------------------------------------------
 * An identity calibration: no offsets, unit scales, unit ASA. Useful as a
 * starting point and for the host unit test.
 * ----------------------------------------------------------------------- */

static imu_calibration_t identity_calibration(void) {
    imu_calibration_t cal;
    memset(&cal, 0, sizeof(cal));
    for (int i = 0; i < 3; i++) {
        cal.accel_scale[i] = 1.0f;
        cal.mag_soft_iron[i] = 1.0f;
        cal.mag_sensitivity_adj[i] = 1.0f;
    }
    return cal;
}

#ifdef HOST_TEST

/* -------------------------------------------------------------------------
 * Host-side unit test of the conversion math. No hardware involved.
 * ----------------------------------------------------------------------- */

static int approx(float a, float b, float tol) {
    return fabsf(a - b) <= tol;
}

int main(void) {
    int failures = 0;

    /* Construct a raw sample where the part reads +1 g on Z and nothing else,
       full-scale +/-2 g (16384 LSB/g), gyro +/-250 dps (131 LSB/dps). */
    imu_raw_t raw;
    memset(&raw, 0, sizeof(raw));
    raw.az = 16384;        /* exactly 1 g */
    raw.gx = 131;          /* exactly 1 deg/s */
    raw.mx = 100;          /* 100 counts of mag */

    imu_calibration_t cal = identity_calibration();
    imu_sample_t s;
    cc_imu_apply_calibration(&raw, &cal,
                             MPU9250_GYRO_SENS_250DPS,
                             MPU9250_ACCEL_SENS_2G,
                             0.01f, &s);

    printf("=== Exercise 1 host conversion test ===\n");
    printf("az: %.4f m/s^2 (expect %.4f)\n", s.az, CC_GRAVITY_MS2);
    printf("gx: %.6f rad/s (expect %.6f)\n", s.gx, CC_DEG_TO_RAD);
    printf("mx: %.4f uT (expect %.4f)\n", s.mx, 100.0f * AK8963_UT_PER_LSB);

    if (!approx(s.az, CC_GRAVITY_MS2, 1e-3f))            { printf("FAIL az\n"); failures++; }
    if (!approx(s.gx, CC_DEG_TO_RAD, 1e-5f))             { printf("FAIL gx\n"); failures++; }
    if (!approx(s.mx, 100.0f * AK8963_UT_PER_LSB, 1e-3f)){ printf("FAIL mx\n"); failures++; }

    /* Now apply a gyro bias of exactly 1 deg/s on X; the calibrated gx should
       become zero. */
    cal.gyro_bias[0] = CC_DEG_TO_RAD;
    cc_imu_apply_calibration(&raw, &cal,
                             MPU9250_GYRO_SENS_250DPS,
                             MPU9250_ACCEL_SENS_2G,
                             0.01f, &s);
    printf("gx after bias subtraction: %.6f rad/s (expect ~0)\n", s.gx);
    if (!approx(s.gx, 0.0f, 1e-5f)) { printf("FAIL gx-bias\n"); failures++; }

    printf("\n%s: %d failure(s).\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}

#else  /* On-device build. */

#include "pico/stdlib.h"
#include "hardware/i2c.h"

#define I2C_PORT      i2c0
#define I2C_SDA_PIN   4
#define I2C_SCL_PIN   5
#define I2C_BAUD_HZ   400000

/* Blocking register write to a 7-bit I2C device. Returns true on success. */
static bool i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t val) {
    uint8_t buf[2] = { reg, val };
    int n = i2c_write_blocking(I2C_PORT, addr, buf, 2, false);
    return n == 2;
}

/* Blocking register-block read. Returns true on success. */
static bool i2c_read_regs(uint8_t addr, uint8_t reg, uint8_t *dst, size_t len) {
    int n = i2c_write_blocking(I2C_PORT, addr, &reg, 1, true /* keep bus */);
    if (n != 1) return false;
    n = i2c_read_blocking(I2C_PORT, addr, dst, len, false);
    return n == (int)len;
}

/* Bring up the MPU-9250: wake it, set full-scale, enable the DLPF, and turn on
   the bypass so the AK8963 magnetometer is visible on the external bus. */
static bool mpu9250_init(void) {
    uint8_t who = 0;
    if (!i2c_read_regs(MPU9250_I2C_ADDR, MPU9250_WHO_AM_I, &who, 1)) return false;
    if (who != MPU9250_WHO_AM_I_VALUE) {
        printf("WHO_AM_I = 0x%02X (expected 0x%02X)\n", who, MPU9250_WHO_AM_I_VALUE);
        return false;
    }

    /* Reset, then select the best clock source (PLL with gyro X reference). */
    i2c_write_reg(MPU9250_I2C_ADDR, MPU9250_PWR_MGMT_1, 0x80u);  /* device reset */
    sleep_ms(100);
    i2c_write_reg(MPU9250_I2C_ADDR, MPU9250_PWR_MGMT_1, 0x01u);  /* clk = PLL */
    sleep_ms(10);
    i2c_write_reg(MPU9250_I2C_ADDR, MPU9250_PWR_MGMT_2, 0x00u);  /* all axes on */

    /* DLPF: gyro/temp bandwidth 41 Hz (CONFIG=3), 1 kHz internal rate. */
    i2c_write_reg(MPU9250_I2C_ADDR, MPU9250_CONFIG, 0x03u);
    /* Sample-rate divider: 1 kHz / (1 + 9) = 100 Hz output. */
    i2c_write_reg(MPU9250_I2C_ADDR, MPU9250_SMPLRT_DIV, 0x09u);

    /* Full-scale: gyro +/-500 dps, accel +/-4 g. */
    i2c_write_reg(MPU9250_I2C_ADDR, MPU9250_GYRO_CONFIG, MPU9250_GYRO_FS_500DPS);
    i2c_write_reg(MPU9250_I2C_ADDR, MPU9250_ACCEL_CONFIG, MPU9250_ACCEL_FS_4G);
    /* Accel DLPF 44.8 Hz. */
    i2c_write_reg(MPU9250_I2C_ADDR, MPU9250_ACCEL_CONFIG2, 0x03u);

    /* Enable bypass so the AK8963 is reachable at 0x0C on the external bus. */
    i2c_write_reg(MPU9250_I2C_ADDR, MPU9250_INT_PIN_CFG, 0x02u);
    sleep_ms(10);
    return true;
}

/* Read the AK8963 factory sensitivity adjustment (ASA) and configure it for
   16-bit continuous mode. Returns the ASA values mapped per the datasheet:
   adj = (ASA - 128) / 256 + 1. */
static bool ak8963_init(float asa_out[3]) {
    uint8_t wia = 0;
    if (!i2c_read_regs(AK8963_I2C_ADDR, AK8963_WIA, &wia, 1)) return false;
    if (wia != AK8963_WIA_VALUE) {
        printf("AK8963 WIA = 0x%02X (expected 0x%02X)\n", wia, AK8963_WIA_VALUE);
        return false;
    }

    /* Read the factory ASA from fuse ROM. */
    i2c_write_reg(AK8963_I2C_ADDR, AK8963_CNTL1, AK8963_CNTL1_PWR_DOWN);
    sleep_ms(10);
    i2c_write_reg(AK8963_I2C_ADDR, AK8963_CNTL1, AK8963_CNTL1_FUSE_ROM);
    sleep_ms(10);

    uint8_t asa[3];
    if (!i2c_read_regs(AK8963_I2C_ADDR, AK8963_ASAX, asa, 3)) return false;
    for (int i = 0; i < 3; i++) {
        asa_out[i] = ((float)asa[i] - 128.0f) / 256.0f + 1.0f;
    }

    /* Back to power-down, then continuous 16-bit mode 2 (100 Hz). */
    i2c_write_reg(AK8963_I2C_ADDR, AK8963_CNTL1, AK8963_CNTL1_PWR_DOWN);
    sleep_ms(10);
    i2c_write_reg(AK8963_I2C_ADDR, AK8963_CNTL1, AK8963_CNTL1_CONT_16BIT);
    sleep_ms(10);
    return true;
}

/* Read one full IMU sample (accel + gyro + mag + temp) into raw counts. */
static bool imu_read_raw(imu_raw_t *raw) {
    uint8_t b[14];
    if (!i2c_read_regs(MPU9250_I2C_ADDR, MPU9250_ACCEL_XOUT_H, b, 14)) return false;
    raw->ax = cc_be16(&b[0]);
    raw->ay = cc_be16(&b[2]);
    raw->az = cc_be16(&b[4]);
    raw->temperature = cc_be16(&b[6]);
    raw->gx = cc_be16(&b[8]);
    raw->gy = cc_be16(&b[10]);
    raw->gz = cc_be16(&b[12]);

    /* Magnetometer: read ST1 (DRDY), then 6 data bytes, then ST2 to release
       the latch. The AK8963 stores its data little-endian (note cc_le16). */
    uint8_t st1 = 0;
    if (i2c_read_regs(AK8963_I2C_ADDR, AK8963_ST1, &st1, 1) && (st1 & 0x01u)) {
        uint8_t m[7];  /* 6 data + ST2 */
        if (i2c_read_regs(AK8963_I2C_ADDR, AK8963_HXL, m, 7)) {
            /* ST2 bit3 (HOFL) set means magnetic overflow; discard if so. */
            if ((m[6] & 0x08u) == 0u) {
                raw->mx = cc_le16(&m[0]);
                raw->my = cc_le16(&m[2]);
                raw->mz = cc_le16(&m[4]);
            }
        }
    }
    return true;
}

/* Measure the gyro bias by averaging N stationary samples. The board MUST be
   still during this — even a desk vibration corrupts the bias estimate. */
static void measure_gyro_bias(int samples, float gyro_sens, float bias_out[3]) {
    double acc[3] = { 0.0, 0.0, 0.0 };
    int got = 0;
    for (int i = 0; i < samples; i++) {
        imu_raw_t raw;
        memset(&raw, 0, sizeof(raw));
        if (imu_read_raw(&raw)) {
            acc[0] += (double)raw.gx;
            acc[1] += (double)raw.gy;
            acc[2] += (double)raw.gz;
            got++;
        }
        sleep_ms(2);
    }
    for (int i = 0; i < 3; i++) {
        float mean_counts = (got > 0) ? (float)(acc[i] / (double)got) : 0.0f;
        bias_out[i] = (mean_counts / gyro_sens) * CC_DEG_TO_RAD;
    }
}

int main(void) {
    stdio_init_all();
    sleep_ms(2000);  /* wait for CDC enumeration */

    printf("\n=== Exercise 1: MPU-9250 read + calibrate ===\n");

    i2c_init(I2C_PORT, I2C_BAUD_HZ);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);

    if (!mpu9250_init()) {
        printf("MPU-9250 init failed; check wiring and AD0.\n");
        while (true) { sleep_ms(1000); }
    }

    imu_calibration_t cal = identity_calibration();
    if (!ak8963_init(cal.mag_sensitivity_adj)) {
        printf("AK8963 init failed; continuing without magnetometer.\n");
    } else {
        printf("AK8963 ASA: %.3f %.3f %.3f\n",
               cal.mag_sensitivity_adj[0],
               cal.mag_sensitivity_adj[1],
               cal.mag_sensitivity_adj[2]);
    }

    printf("Measuring gyro bias over 512 samples — keep the board STILL...\n");
    measure_gyro_bias(512, MPU9250_GYRO_SENS_500DPS, cal.gyro_bias);
    printf("Gyro bias (rad/s): %.5f %.5f %.5f\n",
           cal.gyro_bias[0], cal.gyro_bias[1], cal.gyro_bias[2]);

    /* Stream calibrated samples at ~100 Hz. */
    absolute_time_t prev = get_absolute_time();
    while (true) {
        imu_raw_t raw;
        memset(&raw, 0, sizeof(raw));
        if (!imu_read_raw(&raw)) {
            printf("read error\n");
            sleep_ms(100);
            continue;
        }

        absolute_time_t now = get_absolute_time();
        float dt = (float)absolute_time_diff_us(prev, now) / 1e6f;
        prev = now;

        imu_sample_t s;
        cc_imu_apply_calibration(&raw, &cal,
                                 MPU9250_GYRO_SENS_500DPS,
                                 MPU9250_ACCEL_SENS_4G,
                                 dt, &s);

        printf("a=[% 6.2f % 6.2f % 6.2f] m/s^2  "
               "g=[% 6.3f % 6.3f % 6.3f] rad/s  "
               "m=[% 6.1f % 6.1f % 6.1f] uT  dt=%.4f\n",
               s.ax, s.ay, s.az, s.gx, s.gy, s.gz, s.mx, s.my, s.mz, s.dt);

        sleep_ms(10);
    }
    return 0;
}

#endif /* HOST_TEST */

/* End of exercise-01-imu-read-and-calibrate.c. */
