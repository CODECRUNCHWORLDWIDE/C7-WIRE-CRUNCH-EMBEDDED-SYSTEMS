/*
 * madgwick.c — Quaternion MARG/IMU orientation filter implementation.
 *
 * This is a from-scratch, fully-commented re-derivation of Madgwick's filter.
 * It does not depend on the original reference's global state; the quaternion
 * and gain live in a madgwick_t the caller owns, so multiple instances can run
 * side by side (useful for comparing tunings on the bench).
 *
 * Conventions: Hamilton quaternion, scalar-first (w,x,y,z), representing the
 * rotation from the sensor body frame to the earth (navigation) frame, where
 * earth Z is up (against gravity) and earth X points toward magnetic north's
 * horizontal projection.
 *
 * Citations:
 *   - Madgwick, Univ. of Bristol 2010, eqns. (11), (25), (42)-(44), (49).
 */

#include <math.h>
#include "madgwick.h"

/* Fast inverse square root. The modern, correct version: just 1/sqrt. The
   original Quake bit-hack saved cycles on FPU-less parts in 2005, but on the
   RP2040 the soft-float sqrtf is fine and bit-hacks invite UB. */
static inline float inv_sqrt(float x) {
    return 1.0f / sqrtf(x);
}

void madgwick_init(madgwick_t *m, float gyro_error_rad_s) {
    m->q = quat_identity();
    /* beta = sqrt(3/4) * gyro measurement error (Madgwick eqn. 50). */
    m->beta = 0.86602540378f * gyro_error_rad_s;  /* sqrt(3)/2 */
}

void madgwick_update_imu(madgwick_t *m,
                         float gx, float gy, float gz,
                         float ax, float ay, float az,
                         float dt) {
    float q0 = m->q.w, q1 = m->q.x, q2 = m->q.y, q3 = m->q.z;

    /* Quaternion rate of change from the gyroscope (eqn. 11):
       q_dot_omega = 0.5 * q (x) (0, gx, gy, gz). */
    float qDot0 = 0.5f * (-q1 * gx - q2 * gy - q3 * gz);
    float qDot1 = 0.5f * ( q0 * gx + q2 * gz - q3 * gy);
    float qDot2 = 0.5f * ( q0 * gy - q1 * gz + q3 * gx);
    float qDot3 = 0.5f * ( q0 * gz + q1 * gy - q2 * gx);

    /* Only apply the accelerometer correction if the accel is non-zero
       (a true zero would mean free-fall or a dead sensor — skip the gradient
       step and integrate the gyro alone). */
    if (!((ax == 0.0f) && (ay == 0.0f) && (az == 0.0f))) {
        /* Normalize the accelerometer measurement. */
        float recipNorm = inv_sqrt(ax * ax + ay * ay + az * az);
        ax *= recipNorm; ay *= recipNorm; az *= recipNorm;

        /* Gradient-descent corrective step (eqns. 25, 30, 42-44).
           The objective f aligns the estimated gravity direction with the
           measured accel; the Jacobian J gives the gradient. The expressions
           below are the hand-simplified product J^T f. */
        float _2q0 = 2.0f * q0, _2q1 = 2.0f * q1;
        float _2q2 = 2.0f * q2, _2q3 = 2.0f * q3;
        float _4q0 = 4.0f * q0, _4q1 = 4.0f * q1, _4q2 = 4.0f * q2;
        float _8q1 = 8.0f * q1, _8q2 = 8.0f * q2;
        float q0q0 = q0 * q0, q1q1 = q1 * q1;
        float q2q2 = q2 * q2, q3q3 = q3 * q3;

        float s0 = _4q0 * q2q2 + _2q2 * ax + _4q0 * q1q1 - _2q1 * ay;
        float s1 = _4q1 * q3q3 - _2q3 * ax + 4.0f * q0q0 * q1 - _2q0 * ay
                 - _4q1 + _8q1 * q1q1 + _8q1 * q2q2 + _4q1 * az;
        float s2 = 4.0f * q0q0 * q2 + _2q0 * ax + _4q2 * q3q3 - _2q3 * ay
                 - _4q2 + _8q2 * q1q1 + _8q2 * q2q2 + _4q2 * az;
        float s3 = 4.0f * q1q1 * q3 - _2q1 * ax + 4.0f * q2q2 * q3 - _2q2 * ay;

        recipNorm = inv_sqrt(s0 * s0 + s1 * s1 + s2 * s2 + s3 * s3);
        s0 *= recipNorm; s1 *= recipNorm; s2 *= recipNorm; s3 *= recipNorm;

        /* Apply the feedback step, scaled by beta. */
        qDot0 -= m->beta * s0;
        qDot1 -= m->beta * s1;
        qDot2 -= m->beta * s2;
        qDot3 -= m->beta * s3;
    }

    /* Integrate the rate of change and renormalize. */
    q0 += qDot0 * dt; q1 += qDot1 * dt;
    q2 += qDot2 * dt; q3 += qDot3 * dt;
    float recipNorm = inv_sqrt(q0 * q0 + q1 * q1 + q2 * q2 + q3 * q3);
    m->q.w = q0 * recipNorm; m->q.x = q1 * recipNorm;
    m->q.y = q2 * recipNorm; m->q.z = q3 * recipNorm;
}

void madgwick_update_marg(madgwick_t *m,
                          float gx, float gy, float gz,
                          float ax, float ay, float az,
                          float mx, float my, float mz,
                          float dt) {
    /* If the magnetometer is dead this step, fall back to the 6-axis path so
       we never feed NaNs from a zero-vector normalize. */
    if ((mx == 0.0f) && (my == 0.0f) && (mz == 0.0f)) {
        madgwick_update_imu(m, gx, gy, gz, ax, ay, az, dt);
        return;
    }

    float q0 = m->q.w, q1 = m->q.x, q2 = m->q.y, q3 = m->q.z;

    /* Gyro rate of change (eqn. 11). */
    float qDot0 = 0.5f * (-q1 * gx - q2 * gy - q3 * gz);
    float qDot1 = 0.5f * ( q0 * gx + q2 * gz - q3 * gy);
    float qDot2 = 0.5f * ( q0 * gy - q1 * gz + q3 * gx);
    float qDot3 = 0.5f * ( q0 * gz + q1 * gy - q2 * gx);

    if (!((ax == 0.0f) && (ay == 0.0f) && (az == 0.0f))) {
        float recipNorm = inv_sqrt(ax * ax + ay * ay + az * az);
        ax *= recipNorm; ay *= recipNorm; az *= recipNorm;
        recipNorm = inv_sqrt(mx * mx + my * my + mz * mz);
        mx *= recipNorm; my *= recipNorm; mz *= recipNorm;

        /* Auxiliary precomputations. */
        float _2q0mx = 2.0f * q0 * mx, _2q0my = 2.0f * q0 * my;
        float _2q0mz = 2.0f * q0 * mz, _2q1mx = 2.0f * q1 * mx;
        float _2q0 = 2.0f * q0, _2q1 = 2.0f * q1;
        float _2q2 = 2.0f * q2, _2q3 = 2.0f * q3;
        float q0q0 = q0 * q0, q0q1 = q0 * q1, q0q2 = q0 * q2, q0q3 = q0 * q3;
        float q1q1 = q1 * q1, q1q2 = q1 * q2, q1q3 = q1 * q3;
        float q2q2 = q2 * q2, q2q3 = q2 * q3, q3q3 = q3 * q3;

        /* Reference direction of Earth's magnetic field (rotate the measured
           field into the earth frame, then force the horizontal component
           into the X-Z plane to cancel declination). Eqns. 45-48. */
        float hx = mx * q0q0 - _2q0my * q3 + _2q0mz * q2 + mx * q1q1
                 + _2q1 * my * q2 + _2q1 * mz * q3 - mx * q2q2 - mx * q3q3;
        float hy = _2q0mx * q3 + my * q0q0 - _2q0mz * q1 + _2q1mx * q2
                 - my * q1q1 + my * q2q2 + _2q2 * mz * q3 - my * q3q3;
        float _2bx = sqrtf(hx * hx + hy * hy);
        float _2bz = -_2q0mx * q2 + _2q0my * q1 + mz * q0q0 + _2q1mx * q3
                 - mz * q1q1 + _2q2 * my * q3 - mz * q2q2 + mz * q3q3;
        float _4bx = 2.0f * _2bx, _4bz = 2.0f * _2bz;

        /* Gradient-descent corrective step (eqn. 44 for MARG). */
        float s0 = -_2q2 * (2.0f * q1q3 - 2.0f * q0q2 - ax)
                 + _2q1 * (2.0f * q0q1 + 2.0f * q2q3 - ay)
                 - _2bz * q2 * (_2bx * (0.5f - q2q2 - q3q3) + _2bz * (q1q3 - q0q2) - mx)
                 + (-_2bx * q3 + _2bz * q1) * (_2bx * (q1q2 - q0q3) + _2bz * (q0q1 + q2q3) - my)
                 + _2bx * q2 * (_2bx * (q0q2 + q1q3) + _2bz * (0.5f - q1q1 - q2q2) - mz);
        float s1 = _2q3 * (2.0f * q1q3 - 2.0f * q0q2 - ax)
                 + _2q0 * (2.0f * q0q1 + 2.0f * q2q3 - ay)
                 - 4.0f * q1 * (1.0f - 2.0f * q1q1 - 2.0f * q2q2 - az)
                 + _2bz * q3 * (_2bx * (0.5f - q2q2 - q3q3) + _2bz * (q1q3 - q0q2) - mx)
                 + (_2bx * q2 + _2bz * q0) * (_2bx * (q1q2 - q0q3) + _2bz * (q0q1 + q2q3) - my)
                 + (_2bx * q3 - _4bz * q1) * (_2bx * (q0q2 + q1q3) + _2bz * (0.5f - q1q1 - q2q2) - mz);
        float s2 = -_2q0 * (2.0f * q1q3 - 2.0f * q0q2 - ax)
                 + _2q3 * (2.0f * q0q1 + 2.0f * q2q3 - ay)
                 - 4.0f * q2 * (1.0f - 2.0f * q1q1 - 2.0f * q2q2 - az)
                 + (-_4bx * q2 - _2bz * q0) * (_2bx * (0.5f - q2q2 - q3q3) + _2bz * (q1q3 - q0q2) - mx)
                 + (_2bx * q1 + _2bz * q3) * (_2bx * (q1q2 - q0q3) + _2bz * (q0q1 + q2q3) - my)
                 + (_2bx * q0 - _4bz * q2) * (_2bx * (q0q2 + q1q3) + _2bz * (0.5f - q1q1 - q2q2) - mz);
        float s3 = _2q1 * (2.0f * q1q3 - 2.0f * q0q2 - ax)
                 + _2q2 * (2.0f * q0q1 + 2.0f * q2q3 - ay)
                 + (-_4bx * q3 + _2bz * q1) * (_2bx * (0.5f - q2q2 - q3q3) + _2bz * (q1q3 - q0q2) - mx)
                 + (-_2bx * q0 + _2bz * q2) * (_2bx * (q1q2 - q0q3) + _2bz * (q0q1 + q2q3) - my)
                 + _2bx * q1 * (_2bx * (q0q2 + q1q3) + _2bz * (0.5f - q1q1 - q2q2) - mz);

        recipNorm = inv_sqrt(s0 * s0 + s1 * s1 + s2 * s2 + s3 * s3);
        s0 *= recipNorm; s1 *= recipNorm; s2 *= recipNorm; s3 *= recipNorm;

        qDot0 -= m->beta * s0;
        qDot1 -= m->beta * s1;
        qDot2 -= m->beta * s2;
        qDot3 -= m->beta * s3;
    }

    q0 += qDot0 * dt; q1 += qDot1 * dt;
    q2 += qDot2 * dt; q3 += qDot3 * dt;
    float recipNorm = inv_sqrt(q0 * q0 + q1 * q1 + q2 * q2 + q3 * q3);
    m->q.w = q0 * recipNorm; m->q.x = q1 * recipNorm;
    m->q.y = q2 * recipNorm; m->q.z = q3 * recipNorm;
}

/* End of madgwick.c. */
