/*
 * imu_common.h — Shared declarations for Week 15 exercises and mini-project.
 *
 * This header pins the sensor register maps, the fixed-point and float math
 * conventions, the quaternion type, and the closed-loop control structs that
 * the exercises and the mini-project share. It compiles both on the Pico
 * (Pico SDK) and on the host (a plain C11 build for the offline replay tools).
 *
 * Platform: Raspberry Pi Pico / RP2040 + Pico SDK (C/C++). The IMU is an
 * InvenSense MPU-9250 (accel + gyro + AK8963 magnetometer) on the Pico's I2C0
 * bus; a hobby servo and a brushed DC motor hang off PWM-capable GPIOs.
 *
 * Citations:
 *   - InvenSense MPU-9250 Register Map and Descriptions, rev 1.6, RM-MPU-9250A-00.
 *   - AsahiKASEI AK8963 3-axis magnetometer datasheet (embedded in the MPU-9250).
 *   - RP2040 datasheet §4.5 (PWM), §4.3 (I2C), pp. 525-548 / 463-510.
 *   - RFC-style quaternion conventions follow Diebel, "Representing Attitude"
 *     (Stanford, 2006), Hamilton convention, scalar-first (w, x, y, z).
 *   - Madgwick, "An efficient orientation filter for inertial and
 *     inertial/magnetic sensor arrays", Univ. of Bristol, 2010.
 */

#ifndef CC_IMU_COMMON_H_
#define CC_IMU_COMMON_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Physical constants.
 * ----------------------------------------------------------------------- */

#define CC_PI                 3.14159265358979323846f
#define CC_DEG_TO_RAD         (CC_PI / 180.0f)
#define CC_RAD_TO_DEG         (180.0f / CC_PI)
#define CC_GRAVITY_MS2        9.80665f      /* standard gravity */

/* -------------------------------------------------------------------------
 * MPU-9250 I2C addresses and registers (InvenSense RM-MPU-9250A-00).
 *
 * The MPU-9250 is a 9-axis IMU: a 3-axis accelerometer, a 3-axis gyroscope,
 * and an AK8963 3-axis magnetometer reachable through an internal I2C master
 * (or, as we use it, through the MPU's "bypass" mode that exposes the AK8963
 * directly on the external bus). We drive the part at I2C 400 kHz.
 * ----------------------------------------------------------------------- */

#define MPU9250_I2C_ADDR          0x68u   /* AD0 tied low */
#define MPU9250_WHO_AM_I          0x75u   /* returns 0x71 on a genuine MPU-9250 */
#define MPU9250_WHO_AM_I_VALUE    0x71u

#define MPU9250_PWR_MGMT_1        0x6Bu
#define MPU9250_PWR_MGMT_2        0x6Cu
#define MPU9250_CONFIG            0x1Au   /* DLPF config */
#define MPU9250_GYRO_CONFIG       0x1Bu   /* full-scale + self-test */
#define MPU9250_ACCEL_CONFIG      0x1Cu
#define MPU9250_ACCEL_CONFIG2     0x1Du   /* accel DLPF */
#define MPU9250_SMPLRT_DIV        0x19u
#define MPU9250_INT_PIN_CFG       0x37u   /* bit1 BYPASS_EN exposes AK8963 */
#define MPU9250_INT_ENABLE        0x38u
#define MPU9250_INT_STATUS        0x3Au   /* bit0 RAW_DATA_RDY_INT */

#define MPU9250_ACCEL_XOUT_H      0x3Bu   /* 6 bytes accel, big-endian, 2's comp */
#define MPU9250_TEMP_OUT_H        0x41u   /* 2 bytes die temperature */
#define MPU9250_GYRO_XOUT_H       0x43u   /* 6 bytes gyro, big-endian, 2's comp */

/* Full-scale selectors written into GYRO_CONFIG[4:3] / ACCEL_CONFIG[4:3]. */
#define MPU9250_GYRO_FS_250DPS    (0u << 3)
#define MPU9250_GYRO_FS_500DPS    (1u << 3)
#define MPU9250_GYRO_FS_1000DPS   (2u << 3)
#define MPU9250_GYRO_FS_2000DPS   (3u << 3)

#define MPU9250_ACCEL_FS_2G       (0u << 3)
#define MPU9250_ACCEL_FS_4G       (1u << 3)
#define MPU9250_ACCEL_FS_8G       (2u << 3)
#define MPU9250_ACCEL_FS_16G      (3u << 3)

/* Sensitivity scale factors (datasheet §3, "Electrical Characteristics").
 * LSB-per-unit, so multiply the raw count by the reciprocal to get SI units. */
#define MPU9250_GYRO_SENS_250DPS   131.0f   /* LSB / (deg/s) */
#define MPU9250_GYRO_SENS_500DPS    65.5f
#define MPU9250_GYRO_SENS_1000DPS   32.8f
#define MPU9250_GYRO_SENS_2000DPS   16.4f

#define MPU9250_ACCEL_SENS_2G    16384.0f   /* LSB / g */
#define MPU9250_ACCEL_SENS_4G     8192.0f
#define MPU9250_ACCEL_SENS_8G     4096.0f
#define MPU9250_ACCEL_SENS_16G    2048.0f

/* -------------------------------------------------------------------------
 * AK8963 magnetometer (inside the MPU-9250 package, reached via bypass).
 * ----------------------------------------------------------------------- */

#define AK8963_I2C_ADDR           0x0Cu
#define AK8963_WIA                0x00u   /* device ID, returns 0x48 */
#define AK8963_WIA_VALUE          0x48u
#define AK8963_ST1                0x02u   /* bit0 DRDY */
#define AK8963_HXL                0x03u   /* 6 bytes mag, LITTLE-endian (!) */
#define AK8963_ST2                0x09u   /* must be read to latch the sample */
#define AK8963_CNTL1              0x0Au
#define AK8963_CNTL2              0x0Bu
#define AK8963_ASAX               0x10u   /* 3 bytes factory sensitivity adj */

#define AK8963_CNTL1_PWR_DOWN     0x00u
#define AK8963_CNTL1_FUSE_ROM     0x0Fu
#define AK8963_CNTL1_CONT_16BIT   0x16u   /* 16-bit, continuous mode 2 (100 Hz) */

/* AK8963 full-scale is +/- 4912 uT mapped over a 16-bit signed range. */
#define AK8963_UT_PER_LSB         0.15f   /* 4912 / 32760 microtesla per count */

/* -------------------------------------------------------------------------
 * Raw and calibrated sample structs.
 * ----------------------------------------------------------------------- */

typedef struct {
    int16_t ax, ay, az;   /* accelerometer counts, big-endian decoded */
    int16_t gx, gy, gz;   /* gyroscope counts */
    int16_t mx, my, mz;   /* magnetometer counts (AK8963) */
    int16_t temperature;  /* die temperature counts */
} imu_raw_t;

/* SI-unit sample: accel in m/s^2, gyro in rad/s, mag in microtesla. */
typedef struct {
    float ax, ay, az;     /* m/s^2 */
    float gx, gy, gz;     /* rad/s */
    float mx, my, mz;     /* microtesla */
    float dt;             /* seconds since previous sample */
} imu_sample_t;

/* -------------------------------------------------------------------------
 * Calibration parameters.
 *
 * Gyro:  bias subtracted from each axis (rad/s), measured while stationary.
 * Accel: per-axis bias (offset) and scale (gain). The "six-position tumble"
 *        calibration solves for offset and scale on each axis.
 * Mag:   hard-iron offset (a 3-vector subtracted) and a soft-iron 3x3 matrix
 *        (applied after the offset). For a cheap calibration we approximate
 *        the soft-iron matrix as a diagonal scale (the ellipsoid's semi-axes).
 * ----------------------------------------------------------------------- */

typedef struct {
    float gyro_bias[3];        /* rad/s, subtracted */

    float accel_offset[3];     /* m/s^2, subtracted */
    float accel_scale[3];      /* dimensionless, multiplied after offset */

    float mag_hard_iron[3];    /* microtesla, subtracted */
    float mag_soft_iron[3];    /* diagonal soft-iron scale (semi-axis recip) */

    float mag_sensitivity_adj[3]; /* AK8963 factory ASA correction (read once) */
} imu_calibration_t;

/* -------------------------------------------------------------------------
 * Quaternion: Hamilton convention, scalar-first (w, x, y, z), unit norm.
 * Represents the rotation from the sensor body frame to the navigation frame.
 * ----------------------------------------------------------------------- */

typedef struct {
    float w, x, y, z;
} quat_t;

static inline quat_t quat_identity(void) {
    quat_t q = { 1.0f, 0.0f, 0.0f, 0.0f };
    return q;
}

static inline float quat_norm(quat_t q) {
    return sqrtf(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
}

static inline quat_t quat_normalize(quat_t q) {
    float n = quat_norm(q);
    if (n < 1e-9f) {
        return quat_identity();   /* degenerate; fall back to identity */
    }
    float inv = 1.0f / n;
    quat_t r = { q.w * inv, q.x * inv, q.y * inv, q.z * inv };
    return r;
}

/* Hamilton product r = a (x) b. Non-commutative; order matters. */
static inline quat_t quat_mul(quat_t a, quat_t b) {
    quat_t r;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    return r;
}

/* Euler angles (roll about x, pitch about y, yaw about z) in radians,
 * extracted from a unit quaternion. Aerospace ZYX sequence. */
typedef struct {
    float roll;
    float pitch;
    float yaw;
} euler_t;

static inline euler_t quat_to_euler(quat_t q) {
    euler_t e;
    /* roll (x-axis rotation) */
    float sinr_cosp = 2.0f * (q.w * q.x + q.y * q.z);
    float cosr_cosp = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
    e.roll = atan2f(sinr_cosp, cosr_cosp);

    /* pitch (y-axis rotation), clamped to avoid NaN at the gimbal poles */
    float sinp = 2.0f * (q.w * q.y - q.z * q.x);
    if (sinp > 1.0f)  sinp = 1.0f;
    if (sinp < -1.0f) sinp = -1.0f;
    e.pitch = asinf(sinp);

    /* yaw (z-axis rotation) */
    float siny_cosp = 2.0f * (q.w * q.z + q.x * q.y);
    float cosy_cosp = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
    e.yaw = atan2f(siny_cosp, cosy_cosp);
    return e;
}

/* -------------------------------------------------------------------------
 * Complementary filter state.
 *
 * The complementary filter blends a high-pass-filtered gyro integration with
 * a low-pass-filtered accelerometer tilt estimate:
 *   angle = alpha * (angle + gyro * dt) + (1 - alpha) * accel_angle
 * alpha is close to 1.0 (we trust the gyro short-term, the accel long-term).
 * ----------------------------------------------------------------------- */

typedef struct {
    float roll;          /* radians */
    float pitch;         /* radians */
    float alpha;         /* blend coefficient in [0, 1] */
} complementary_t;

/* -------------------------------------------------------------------------
 * 1-D scalar Kalman filter state (used per-axis in exercise 3).
 *
 * State: angle and gyro bias. Measurement: accelerometer-derived angle.
 * This is the classic "two-state" attitude Kalman filter from the Trammell
 * Hudson / Kristian Lauszus formulation; it estimates the gyro bias online so
 * the integration does not drift.
 * ----------------------------------------------------------------------- */

typedef struct {
    float angle;         /* estimated angle, radians */
    float bias;          /* estimated gyro bias, rad/s */
    float P[2][2];       /* 2x2 error covariance */
    float Q_angle;       /* process noise: angle */
    float Q_bias;        /* process noise: gyro bias */
    float R_measure;     /* measurement noise: accelerometer angle */
} kalman1d_t;

/* -------------------------------------------------------------------------
 * Actuator: hobby servo (50 Hz PWM, 1.0-2.0 ms pulse) and brushed DC motor
 * (fast PWM with an H-bridge). RP2040 PWM math constants.
 * ----------------------------------------------------------------------- */

#define SERVO_PWM_FREQ_HZ          50u       /* 20 ms period */
#define SERVO_MIN_PULSE_US         1000u     /* -90 deg / full reverse */
#define SERVO_MID_PULSE_US         1500u     /* center */
#define SERVO_MAX_PULSE_US         2000u     /* +90 deg / full forward */

#define MOTOR_PWM_FREQ_HZ          20000u    /* 20 kHz, above audible range */

/* PID controller for the closed-loop control exercise/mini-project. */
typedef struct {
    float kp, ki, kd;
    float integrator;
    float prev_error;
    float out_min, out_max;     /* saturation limits */
    float integ_min, integ_max; /* anti-windup clamp on the integrator */
} pid_t;

/* -------------------------------------------------------------------------
 * Inline helpers shared across exercises.
 * ----------------------------------------------------------------------- */

/* Decode a big-endian signed 16-bit value (MPU accel/gyro byte order). */
static inline int16_t cc_be16(const uint8_t *p) {
    return (int16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/* Decode a little-endian signed 16-bit value (AK8963 mag byte order). */
static inline int16_t cc_le16(const uint8_t *p) {
    return (int16_t)(((uint16_t)p[1] << 8) | (uint16_t)p[0]);
}

static inline float cc_clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* -------------------------------------------------------------------------
 * Public APIs implemented by the exercises (so the mini-project can reuse
 * the exact same code paths the students wrote).
 * ----------------------------------------------------------------------- */

/* exercise-01: convert raw counts to SI units using a calibration. */
void cc_imu_apply_calibration(const imu_raw_t *raw,
                              const imu_calibration_t *cal,
                              float gyro_sens, float accel_sens,
                              float dt, imu_sample_t *out);

/* exercise-02: complementary filter step. Updates roll/pitch in radians. */
void cc_complementary_update(complementary_t *cf, const imu_sample_t *s);

/* exercise-03: 1-D Kalman filter step. Returns the fused angle (radians). */
float cc_kalman1d_update(kalman1d_t *kf, float new_angle_meas,
                         float new_rate_meas, float dt);

/* PID step shared by the actuator code. */
float cc_pid_update(pid_t *pid, float setpoint, float measured, float dt);

#ifdef __cplusplus
}
#endif

#endif /* CC_IMU_COMMON_H_ */
