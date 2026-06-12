/*
 * actuators.h — Servo, motor, and PID declarations for the Week 15 mini-project.
 */

#ifndef CC_ACTUATORS_H_
#define CC_ACTUATORS_H_

#include <math.h>
#include "pico/types.h"
#include "imu_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint     gpio;
    uint     slice;
    uint16_t wrap;
} servo_t;

typedef struct {
    uint     in1, in2;
    uint     slice1, slice2;
    uint16_t wrap;
} motor_t;

void servo_init(servo_t *sv, uint gpio);
void servo_set_angle(servo_t *sv, float angle_deg);

void motor_init(motor_t *mo, uint gpio_in1, uint gpio_in2);
void motor_set(motor_t *mo, float command);   /* [-1, +1] */
void motor_coast(motor_t *mo);

#ifdef __cplusplus
}
#endif

#endif /* CC_ACTUATORS_H_ */
