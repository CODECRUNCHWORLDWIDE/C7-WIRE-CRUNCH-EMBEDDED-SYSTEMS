/*
 * main.c — A LoRaWAN Class-A telemetry node (Week 14 mini-project).
 *
 * Reads the RP2040 on-die temperature sensor and an estimated battery
 * percentage, encodes them into a 3-byte payload, builds a LoRaWAN uplink
 * (FRMPayload encrypted with AppSKey, MIC with NwkSKey), drives the SX1262,
 * and transmits on EU868 868.1 MHz / SF7 / BW125. The frame appears in the
 * TTN console.
 *
 * Duty-cycle discipline: the node computes each uplink's airtime and refuses
 * to transmit early if doing so would breach the EU868 1% per-sub-band limit.
 *
 * Build with the Pico SDK (see CMakeLists.txt). Fill secrets.h from your TTN
 * console first (copy secrets.h.example -> secrets.h).
 *
 * Citations:
 *   - LoRaWAN L2 1.0.4, Regional Parameters RP002-1.0.4 (EU868).
 *   - Semtech SX1261/2 datasheet; AN1200.13 (airtime).
 *   - RP2040 datasheet §4.9 (ADC + temperature sensor).
 */

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/adc.h"

#include "lpwan_common.h"
#include "lorawan.h"
#include "sx1262.h"
#include "secrets.h"

#define EU868_CH0_HZ        868100000u
#define TX_POWER_DBM        14            /* EU868 default max EIRP region */
#define UPLINK_PERIOD_MS    30000u        /* 30 s -> ~0.19% duty at SF7    */
#define DUTY_CYCLE_LIMIT    0.01          /* EU868 1% per sub-band         */

/* -------------------------------------------------------------------------
 * Airtime (Semtech AN1200.13). Used for the duty-cycle accounting.
 * ----------------------------------------------------------------------- */
static double lora_airtime_ms(uint8_t sf, uint32_t bw_hz, uint8_t cr_denom_minus4,
                              uint8_t preamble, uint8_t payload,
                              int explicit_hdr, int low_dr_opt) {
    double rs = (double)bw_hz / (double)(1u << sf);
    double ts = 1000.0 / rs;
    double t_preamble = (preamble + 4.25) * ts;
    int de = low_dr_opt ? 1 : 0;
    int ih = explicit_hdr ? 0 : 1;
    double num = 8.0 * payload - 4.0 * sf + 28 + 16 - 20.0 * ih;
    double den = 4.0 * (sf - 2 * de);
    double ceil_term = (num > 0) ? (double)((int)((num / den) + 0.999999)) : 0.0;
    double n_payload = 8 + ceil_term * (cr_denom_minus4 + 4);
    if (n_payload < 8) n_payload = 8;
    return t_preamble + n_payload * ts;
}

/* -------------------------------------------------------------------------
 * RP2040 on-die temperature sensor (ADC channel 4). Datasheet §4.9.5.
 * Returns centidegrees Celsius as an int16.
 * ----------------------------------------------------------------------- */
static int16_t read_temperature_centi(void) {
    adc_select_input(4);
    uint16_t raw = adc_read();
    /* 12-bit ADC, 3.3 V reference. T = 27 - (V - 0.706) / 0.001721. */
    double v = (double)raw * 3.3 / 4096.0;
    double tc = 27.0 - (v - 0.706) / 0.001721;
    return (int16_t)(tc * 100.0);
}

/* A fixed battery estimate for the teaching node. A production node reads a
 * resistor divider on VSYS through an ADC channel and maps voltage to percent;
 * we return a constant so the payload carries a second field to decode. */
static uint8_t read_battery_pct(void) {
    return 92u;
}

int main(void) {
    stdio_init_all();
    sleep_ms(2500);

    adc_init();
    adc_set_temp_sensor_enabled(true);

    sx1262_init();

    sx126x_lora_modulation_t mod = {
        .sf = SX126X_LORA_SF7, .bw = SX126X_LORA_BW_125,
        .cr = SX126X_LORA_CR_4_5, .low_data_rate_optimize = 0u,
    };
    sx1262_config(EU868_CH0_HZ, &mod, TX_POWER_DBM);

    /* Session from secrets.h (ABP). DevAddr is already little-endian there. */
    lorawan_session_t session;
    memcpy(session.devaddr,  SECRET_DEVADDR_LE, 4);
    memcpy(session.nwk_skey, SECRET_NWKSKEY,    16);
    memcpy(session.app_skey, SECRET_APPSKEY,    16);
    session.fcnt_up = 0u;

    uint8_t st = sx1262_status();
    printf("\n=== LoRaWAN Class-A node ===\n");
    printf("DevAddr %02X%02X%02X%02X  EU868  SF7 BW125 CR4/5  +%d dBm\n",
           session.devaddr[3], session.devaddr[2],
           session.devaddr[1], session.devaddr[0], TX_POWER_DBM);
    printf("SX1262 status after config: 0x%02X\n", st);

    while (true) {
        int16_t temp = read_temperature_centi();
        uint8_t batt = read_battery_pct();

        /* Payload: temp int16 big-endian centidegrees, battery uint8 percent. */
        uint8_t payload[3];
        payload[0] = (uint8_t)((uint16_t)temp >> 8);
        payload[1] = (uint8_t)((uint16_t)temp & 0xFFu);
        payload[2] = batt;

        lorawan_uplink_t up;
        lorawan_result_t r = lorawan_build_uplink(&session, 1u, payload,
                                                  sizeof payload, &up);
        if (r != LORAWAN_OK) {
            printf("build failed: %d\n", (int)r);
            sleep_ms(UPLINK_PERIOD_MS);
            continue;
        }

        double air = lora_airtime_ms(7u, 125000u, 1u, 8u,
                                     (uint8_t)up.phy_len, 1, 0);
        double duty = air / (double)UPLINK_PERIOD_MS;

        printf("uplink #%lu FCnt=%lu: temp=%.2fC vbat=%u%%  payload=0x%02X %02X %02X\n",
               (unsigned long)session.fcnt_up, (unsigned long)session.fcnt_up,
               temp / 100.0, batt, payload[0], payload[1], payload[2]);
        printf("  PHYPayload (%u B): ", (unsigned)up.phy_len);
        for (size_t i = 0; i < up.phy_len; i++) printf("%02X ", up.phy[i]);
        printf("\n  airtime %.1f ms  duty %.2f%%\n", air, duty * 100.0);

        if (duty > DUTY_CYCLE_LIMIT) {
            printf("  HOLD: would breach 1%% duty cycle; skipping this slot.\n");
        } else {
            bool ok = sx1262_transmit(up.phy, (uint8_t)up.phy_len);
            printf("  TX %s\n", ok ? "TX_DONE" : "FAILED");
            if (ok) session.fcnt_up++;   /* never reuse a counter */
        }

        sleep_ms(UPLINK_PERIOD_MS);
    }
    return 0;
}

/* End of main.c. */
