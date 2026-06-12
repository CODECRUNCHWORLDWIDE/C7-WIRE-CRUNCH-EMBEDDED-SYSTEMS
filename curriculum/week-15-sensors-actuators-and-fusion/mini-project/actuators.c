/*
 * actuators.c — Servo and brushed-DC-motor drivers for the Week 15 mini-project,
 * plus the shared PID controller.
 *
 * Hardware assumed:
 *   - One hobby servo on GP16 (50 Hz PWM, 1.0-2.0 ms pulse).
 *   - One brushed DC motor via a DRV8833 H-bridge: GP18 = IN1 (PWM, speed),
 *     GP19 = IN2 (direction polarity). Sign-magnitude drive.
 * Both build with the Pico SDK's hardware_pwm.
 *
 * The PID (cc_pid_update) is platform-independent and host-testable; it is
 * declared in imu_common.h and used by the balance loop in fusion_node.c.
 *
 * Citations:
 *   - RP2040 datasheet §4.5 (PWM), pp. 525-548.
 *   - TI DRV8833 datasheet (H-bridge, sign-magnitude truth table).
 *   - Astrom & Murray, Feedback Systems, PID + anti-windup.
 */

#include "actuators.h"

#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"

/* -------------------------------------------------------------------------
 * The PID step. Platform-independent (no Pico SDK), declared in imu_common.h.
 *
 * Computes output = Kp*e + Ki*integral(e) + Kd*d(measured)/dt, with:
 *   - integrator anti-windup clamp (integ_min/integ_max),
 *   - derivative on the MEASUREMENT (not the error) to avoid the derivative
 *     kick when the setpoint changes,
 *   - output saturation (out_min/out_max).
 * ----------------------------------------------------------------------- */

float cc_pid_update(pid_t *pid, float setpoint, float measured, float dt) {
    if (dt <= 0.0f) {
        return cc_clampf(0.0f, pid->out_min, pid->out_max);
    }
    float error = setpoint - measured;

    /* Integral with anti-windup clamp. */
    pid->integrator += error * dt;
    pid->integrator = cc_clampf(pid->integrator, pid->integ_min, pid->integ_max);

    /* Derivative on measurement: d(measured)/dt, negated so it damps. We
       reuse prev_error to stash the previous measurement. */
    float d_meas = (measured - pid->prev_error) / dt;
    pid->prev_error = measured;

    float out = pid->kp * error
              + pid->ki * pid->integrator
              - pid->kd * d_meas;

    return cc_clampf(out, pid->out_min, pid->out_max);
}

/* -------------------------------------------------------------------------
 * PWM helpers.
 * ----------------------------------------------------------------------- */

/* Configure a GPIO as a PWM output at the requested frequency. Returns the
   slice number and writes the wrap value to *wrap_out so callers can compute
   compare levels. */
static uint pwm_setup_gpio(uint gpio, uint32_t freq_hz, uint16_t *wrap_out) {
    gpio_set_function(gpio, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(gpio);

    uint32_t f_sys = clock_get_hz(clk_sys);   /* default 125 MHz */
    /* Choose a divider so wrap fits in 16 bits. wrap = f_sys/(div*freq) - 1.
       For 50 Hz at 125 MHz, pick div such that wrap <= 65535. */
    float divider = (float)f_sys / ((float)freq_hz * 65536.0f);
    if (divider < 1.0f) divider = 1.0f;
    uint16_t wrap = (uint16_t)((float)f_sys / (divider * (float)freq_hz) - 1.0f);

    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_clkdiv(&cfg, divider);
    pwm_config_set_wrap(&cfg, wrap);
    pwm_init(slice, &cfg, true);

    *wrap_out = wrap;
    return slice;
}

/* -------------------------------------------------------------------------
 * Servo.
 * ----------------------------------------------------------------------- */

static uint16_t s_servo_wrap;

void servo_init(servo_t *sv, uint gpio) {
    sv->gpio = gpio;
    sv->slice = pwm_setup_gpio(gpio, SERVO_PWM_FREQ_HZ, &s_servo_wrap);
    sv->wrap = s_servo_wrap;
    servo_set_angle(sv, 0.0f);   /* center */
}

/* angle in [-90, +90] degrees -> pulse in [1000, 2000] us -> compare level. */
void servo_set_angle(servo_t *sv, float angle_deg) {
    angle_deg = cc_clampf(angle_deg, -90.0f, 90.0f);
    float frac = (angle_deg + 90.0f) / 180.0f;     /* 0..1 */
    float pulse_us = (float)SERVO_MIN_PULSE_US
                   + frac * (float)(SERVO_MAX_PULSE_US - SERVO_MIN_PULSE_US);
    /* period for 50 Hz is 20000 us. level = wrap * pulse / period. */
    float period_us = 1.0e6f / (float)SERVO_PWM_FREQ_HZ;
    uint16_t level = (uint16_t)(((float)sv->wrap + 1.0f) * pulse_us / period_us);
    pwm_set_gpio_level(sv->gpio, level);
}

/* -------------------------------------------------------------------------
 * Brushed DC motor via DRV8833 (sign-magnitude).
 *
 * Truth table for the active diagonal:
 *   command > 0 : IN1 = PWM(|cmd|), IN2 = 0   -> forward
 *   command < 0 : IN1 = 0,          IN2 = PWM(|cmd|) -> reverse
 *   command = 0 : IN1 = 0,          IN2 = 0    -> coast
 * Both pins driven high simultaneously would be brake; we never do that here.
 * ----------------------------------------------------------------------- */

static uint16_t s_motor_wrap;

void motor_init(motor_t *mo, uint gpio_in1, uint gpio_in2) {
    mo->in1 = gpio_in1;
    mo->in2 = gpio_in2;
    mo->slice1 = pwm_setup_gpio(gpio_in1, MOTOR_PWM_FREQ_HZ, &s_motor_wrap);
    mo->slice2 = pwm_setup_gpio(gpio_in2, MOTOR_PWM_FREQ_HZ, &s_motor_wrap);
    mo->wrap = s_motor_wrap;
    motor_set(mo, 0.0f);
}

/* command in [-1.0, +1.0]. Sign sets direction, magnitude sets duty. */
void motor_set(motor_t *mo, float command) {
    command = cc_clampf(command, -1.0f, 1.0f);
    float mag = fabsf(command);
    uint16_t level = (uint16_t)(mag * (float)mo->wrap);

    if (command > 0.0f) {
        pwm_set_gpio_level(mo->in1, level);
        pwm_set_gpio_level(mo->in2, 0);
    } else if (command < 0.0f) {
        pwm_set_gpio_level(mo->in1, 0);
        pwm_set_gpio_level(mo->in2, level);
    } else {
        pwm_set_gpio_level(mo->in1, 0);
        pwm_set_gpio_level(mo->in2, 0);   /* coast */
    }
}

/* Hard stop: coast both sides. Called by the fault path. */
void motor_coast(motor_t *mo) {
    pwm_set_gpio_level(mo->in1, 0);
    pwm_set_gpio_level(mo->in2, 0);
}

/* End of actuators.c. */
