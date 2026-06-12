/*
 * sx1262.h — SX1262 LoRa transceiver driver for the Week 14 mini-project.
 *
 * The Exercise 2 bring-up, productised into a small API. Targets the RP2040 +
 * Pico SDK. Wiring (matches Exercise 2):
 *   NSS->GP5  SCK->GP2  MOSI->GP3  MISO->GP4  BUSY->GP6  DIO1->GP7  NRST->GP8
 *
 * Citations:
 *   - Semtech SX1261/2 datasheet rev. 2.1, §13 (Commands), §9.2.1 (TX setup).
 */

#ifndef CC_SX1262_H_
#define CC_SX1262_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lpwan_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise SPI + GPIO and reset the radio into STANDBY_RC. */
void sx1262_init(void);

/* Configure for a LoRaWAN-style channel: frequency in Hz, modulation, and
 * TX power in dBm (LoRaWAN EU868 default is +14 dBm). */
void sx1262_config(uint32_t freq_hz, const sx126x_lora_modulation_t *mod,
                   int8_t power_dbm);

/* Transmit `len` bytes. Returns true on TX_DONE, false on timeout. */
bool sx1262_transmit(const uint8_t *payload, uint8_t len);

/* Read the chip status byte (GetStatus, opcode 0xC0). */
uint8_t sx1262_status(void);

#ifdef __cplusplus
}
#endif

#endif /* CC_SX1262_H_ */
