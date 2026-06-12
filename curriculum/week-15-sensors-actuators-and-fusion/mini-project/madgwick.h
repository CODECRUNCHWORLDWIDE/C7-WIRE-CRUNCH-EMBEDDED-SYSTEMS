/*
 * madgwick.h — Quaternion MARG/IMU orientation filter (Madgwick 2010).
 *
 * Implements the gradient-descent attitude filter from Sebastian Madgwick's
 * 2010 University of Bristol report. Two entry points:
 *   - madgwick_update_imu:  6-axis (accel + gyro), corrects roll/pitch only.
 *   - madgwick_update_marg: 9-axis (accel + gyro + mag), corrects yaw too.
 *
 * The filter state is a single unit quaternion (Hamilton, scalar-first) plus
 * the tuning gain beta. All math is single-precision float; on the RP2040
 * (no FPU) the compiler emits soft-float, and a full MARG update costs ~15 us.
 *
 * Citations:
 *   - Madgwick, "An efficient orientation filter for inertial and
 *     inertial/magnetic sensor arrays", Univ. of Bristol, 2010.
 *   - Madgwick reference C, x-io Technologies, public domain.
 */

#ifndef CC_MADGWICK_H_
#define CC_MADGWICK_H_

#include "imu_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    quat_t q;       /* current orientation, sensor->earth */
    float  beta;    /* gradient-descent gain (rad/s of gyro error) */
} madgwick_t;

/* Initialize with identity orientation and a beta derived from the gyro's
   RMS measurement error (rad/s). beta = sqrt(3/4) * gyro_error. */
void madgwick_init(madgwick_t *m, float gyro_error_rad_s);

/* 6-axis update. gx/gy/gz in rad/s; ax/ay/az in any consistent unit (the
   accel vector is normalized internally, so m/s^2 or g both work). dt in s. */
void madgwick_update_imu(madgwick_t *m,
                         float gx, float gy, float gz,
                         float ax, float ay, float az,
                         float dt);

/* 9-axis update. Adds the magnetometer (mx/my/mz, any consistent unit,
   normalized internally). The mag MUST be hard/soft-iron calibrated. */
void madgwick_update_marg(madgwick_t *m,
                          float gx, float gy, float gz,
                          float ax, float ay, float az,
                          float mx, float my, float mz,
                          float dt);

#ifdef __cplusplus
}
#endif

#endif /* CC_MADGWICK_H_ */
