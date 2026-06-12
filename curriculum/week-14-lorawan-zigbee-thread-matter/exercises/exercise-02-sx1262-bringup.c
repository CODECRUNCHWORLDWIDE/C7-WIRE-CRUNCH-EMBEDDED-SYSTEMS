/*
 * exercise-02-sx1262-bringup.c — Bring up a Semtech SX1262 over SPI from the
 * RP2040 and transmit one raw LoRa packet.
 *
 * This exercise runs on the bench: a Pico wired to an SX1262 module. It is
 * the lowest layer of the week — no LoRaWAN MAC yet, just "configure the
 * radio, put bytes in its FIFO, key the PA, watch TX_DONE assert." Once this
 * works you can see your packet on any second LoRa receiver tuned to the same
 * SF/BW/frequency, or on a $30 RTL-SDR with `gr-lora`.
 *
 * Wiring (SX1262 module <-> Pico):
 *   NSS   -> GP5   (SPI0 CSn, driven by hand, not the hardware CS)
 *   SCK   -> GP2   (SPI0 SCK)
 *   MOSI  -> GP3   (SPI0 TX)
 *   MISO  -> GP4   (SPI0 RX)
 *   BUSY  -> GP6   (input; high while the chip is processing a command)
 *   DIO1  -> GP7   (input; the IRQ line we poll for TX_DONE)
 *   NRST  -> GP8   (output; active-low reset)
 *   3V3 / GND to the Pico's 3V3(OUT) and GND.
 *
 * Builds with the Pico SDK:
 *   add_executable(exercise2 exercise-02-sx1262-bringup.c)
 *   target_link_libraries(exercise2 pico_stdlib hardware_spi hardware_gpio)
 *   pico_add_extra_outputs(exercise2)
 *
 * Citations:
 *   - Semtech SX1261/2 datasheet rev. 2.1, §13 (Commands), §6.1 (Modem
 *     Configuration), §9.2.1 (TX setup sequence).
 */

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

#include "lpwan_common.h"

/* --- pin map --- */
#define PIN_NSS    5
#define PIN_SCK    2
#define PIN_MOSI   3
#define PIN_MISO   4
#define PIN_BUSY   6
#define PIN_DIO1   7
#define PIN_NRST   8

#define SPI_PORT   spi0
#define SPI_HZ     (8u * 1000u * 1000u)   /* 8 MHz; SX126x tolerates up to 16 */

/* SX1262 has a 32 MHz TCXO on most modules; the crystal frequency the
 * frequency word is computed against. Datasheet §13.4.1, SetRfFrequency. */
#define SX126X_XTAL_FREQ   32000000.0
#define SX126X_FREQ_STEP   (SX126X_XTAL_FREQ / (double)(1u << 25))

/* EU868 default uplink channel for raw TX in this exercise. */
#define TX_FREQ_HZ   868100000u

/* -------------------------------------------------------------------------
 * Low-level SPI helpers. The SX126x convention: pull NSS low, send the
 * opcode and parameters, pull NSS high. Before each command you must wait
 * for BUSY to fall (the chip raises BUSY while it digests the previous
 * command). Datasheet §8.3.1.
 * ----------------------------------------------------------------------- */

static void sx_wait_busy(void) {
    /* BUSY can stay high for up to ~3.5 ms after a cold SetStandby. */
    uint32_t guard = 0u;
    while (gpio_get(PIN_BUSY)) {
        sleep_us(1);
        if (++guard > 100000u) {     /* 100 ms hard timeout */
            printf("  WARNING: BUSY stuck high — check wiring / TCXO.\n");
            return;
        }
    }
}

static void sx_cmd(uint8_t opcode, const uint8_t *params, size_t n) {
    sx_wait_busy();
    gpio_put(PIN_NSS, 0);
    spi_write_blocking(SPI_PORT, &opcode, 1);
    if (n > 0u) {
        spi_write_blocking(SPI_PORT, params, n);
    }
    gpio_put(PIN_NSS, 1);
}

static void sx_read_cmd(uint8_t opcode, uint8_t *resp, size_t n) {
    sx_wait_busy();
    gpio_put(PIN_NSS, 0);
    spi_write_blocking(SPI_PORT, &opcode, 1);
    /* The first returned byte after a Get* opcode is the status; the caller
     * sizes `resp` to include it. We clock out zeros to read. */
    uint8_t tx0 = 0x00u;
    for (size_t i = 0u; i < n; i++) {
        spi_write_read_blocking(SPI_PORT, &tx0, &resp[i], 1);
    }
    gpio_put(PIN_NSS, 1);
}

static void sx_write_buffer(uint8_t offset, const uint8_t *data, size_t n) {
    sx_wait_busy();
    gpio_put(PIN_NSS, 0);
    uint8_t hdr[2] = { SX126X_CMD_WRITE_BUFFER, offset };
    spi_write_blocking(SPI_PORT, hdr, 2);
    spi_write_blocking(SPI_PORT, data, n);
    gpio_put(PIN_NSS, 1);
}

/* -------------------------------------------------------------------------
 * Reset + bring-up sequence. This follows the datasheet's recommended TX
 * setup (§9.2.1): reset, standby(RC), regulator mode, packet type, RF
 * frequency, PA config, TX params, modulation params, then per-packet the
 * buffer base address, packet params, IRQ params, write buffer, SetTx.
 * ----------------------------------------------------------------------- */

static void sx_reset(void) {
    gpio_put(PIN_NRST, 0);
    sleep_ms(2);
    gpio_put(PIN_NRST, 1);
    sleep_ms(5);
    sx_wait_busy();
}

static void sx_set_rf_frequency(uint32_t freq_hz) {
    uint32_t frf = (uint32_t)((double)freq_hz / SX126X_FREQ_STEP);
    uint8_t p[4] = {
        (uint8_t)(frf >> 24), (uint8_t)(frf >> 16),
        (uint8_t)(frf >> 8),  (uint8_t)(frf),
    };
    sx_cmd(SX126X_CMD_SET_RF_FREQUENCY, p, 4);
}

static void sx_set_modulation(const sx126x_lora_modulation_t *m) {
    uint8_t p[4] = {
        (uint8_t)m->sf, (uint8_t)m->bw, (uint8_t)m->cr,
        m->low_data_rate_optimize,
    };
    sx_cmd(SX126X_CMD_SET_MODULATION_PARAMS, p, 4);
}

static void sx_set_packet_params(uint16_t preamble, uint8_t payload_len) {
    /* LoRa packet params: preamble len(2), header type, payload len, CRC on,
     * IQ standard. Datasheet §13.4.6, Table 13-67. */
    uint8_t p[6] = {
        (uint8_t)(preamble >> 8), (uint8_t)(preamble),
        0x00,            /* variable-length (explicit) header */
        payload_len,
        0x01,            /* CRC on */
        0x00,            /* standard IQ */
    };
    sx_cmd(SX126X_CMD_SET_PACKET_PARAMS, p, 6);
}

static void sx_config_radio(void) {
    sx_reset();

    uint8_t standby_rc = SX126X_STANDBY_RC;
    sx_cmd(SX126X_CMD_SET_STANDBY, &standby_rc, 1);

    /* Use the DC-DC regulator for lower TX current (module-dependent). */
    uint8_t reg_mode = 0x01u;   /* DC-DC + LDO */
    sx_cmd(SX126X_CMD_SET_REGULATOR_MODE, &reg_mode, 1);

    /* DIO2 controls the module's RF switch on Waveshare/RAK boards. */
    uint8_t dio2_rfsw = 0x01u;
    sx_cmd(SX126X_CMD_SET_DIO2_AS_RF_SWITCH_CTRL, &dio2_rfsw, 1);

    uint8_t lora = SX126X_PACKET_TYPE_LORA;
    sx_cmd(SX126X_CMD_SET_PACKET_TYPE, &lora, 1);

    sx_set_rf_frequency(TX_FREQ_HZ);

    /* PA config for SX1262 at +22 dBm (datasheet §13.1.14, Table 13-21). */
    uint8_t pa_cfg[4] = { 0x04, 0x07, 0x00, 0x01 };  /* paDutyCycle, hpMax, deviceSel=SX1262, paLut */
    sx_cmd(SX126X_CMD_SET_PA_CONFIG, pa_cfg, 4);

    /* TX params: power +22 dBm, ramp time 200 us. Datasheet §13.4.4. */
    uint8_t tx_params[2] = { 22, 0x04 };
    sx_cmd(SX126X_CMD_SET_TX_PARAMS, tx_params, 2);

    sx126x_lora_modulation_t mod = {
        .sf = SX126X_LORA_SF7, .bw = SX126X_LORA_BW_125,
        .cr = SX126X_LORA_CR_4_5, .low_data_rate_optimize = 0u,
    };
    sx_set_modulation(&mod);
}

static uint8_t sx_status(void) {
    uint8_t resp[2] = { 0, 0 };
    sx_read_cmd(SX126X_CMD_GET_STATUS, resp, 2);
    return resp[0];   /* status byte: bits 6:4 chip mode, bits 3:1 cmd status */
}

static bool sx_transmit(const uint8_t *payload, uint8_t len) {
    /* base addresses: TX at 0x00, RX at 0x00. */
    uint8_t base[2] = { 0x00, 0x00 };
    sx_cmd(SX126X_CMD_SET_BUFFER_BASE_ADDRESS, base, 2);

    sx_set_packet_params(8u, len);
    sx_write_buffer(0x00, payload, len);

    /* Map TX_DONE and TIMEOUT to the IRQ register and to DIO1. */
    uint8_t irq[8] = {
        (uint8_t)(SX126X_IRQ_ALL >> 8), (uint8_t)(SX126X_IRQ_ALL),       /* enable mask */
        (uint8_t)((SX126X_IRQ_TX_DONE | SX126X_IRQ_TIMEOUT) >> 8),
        (uint8_t)( SX126X_IRQ_TX_DONE | SX126X_IRQ_TIMEOUT),             /* DIO1 mask */
        0x00, 0x00,                                                      /* DIO2 mask */
        0x00, 0x00,                                                      /* DIO3 mask */
    };
    sx_cmd(SX126X_CMD_SET_DIO_IRQ_PARAMS, irq, 8);

    /* SetTx with a 0 timeout means "TX until done, no timeout". */
    uint8_t tx_to[3] = { 0x00, 0x00, 0x00 };
    sx_cmd(SX126X_CMD_SET_TX, tx_to, 3);

    /* Poll DIO1 for TX_DONE. At SF7/BW125 a 12-byte packet is ~40 ms airtime. */
    uint32_t guard = 0u;
    while (gpio_get(PIN_DIO1) == 0) {
        sleep_ms(1);
        if (++guard > 2000u) {
            printf("  TX timed out waiting for DIO1.\n");
            return false;
        }
    }

    /* Read and clear the IRQ status. */
    uint8_t irq_status[3] = { 0, 0, 0 };
    sx_read_cmd(SX126X_CMD_GET_IRQ_STATUS, irq_status, 3);
    uint16_t flags = (uint16_t)((irq_status[1] << 8) | irq_status[2]);

    uint8_t clr[2] = { (uint8_t)(SX126X_IRQ_ALL >> 8), (uint8_t)(SX126X_IRQ_ALL) };
    sx_cmd(SX126X_CMD_CLEAR_IRQ_STATUS, clr, 2);

    return (flags & SX126X_IRQ_TX_DONE) != 0u;
}

int main(void) {
    stdio_init_all();
    sleep_ms(2000);

    printf("\n=== Exercise 2: SX1262 bring-up ===\n");

    /* SPI + GPIO init. */
    spi_init(SPI_PORT, SPI_HZ);
    gpio_set_function(PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);

    gpio_init(PIN_NSS);  gpio_set_dir(PIN_NSS, GPIO_OUT);  gpio_put(PIN_NSS, 1);
    gpio_init(PIN_NRST); gpio_set_dir(PIN_NRST, GPIO_OUT); gpio_put(PIN_NRST, 1);
    gpio_init(PIN_BUSY); gpio_set_dir(PIN_BUSY, GPIO_IN);
    gpio_init(PIN_DIO1); gpio_set_dir(PIN_DIO1, GPIO_IN);

    printf("Configuring radio: 868.1 MHz, SF7, BW125, CR 4/5, +22 dBm.\n");
    sx_config_radio();

    uint8_t st = sx_status();
    printf("GetStatus after config: 0x%02X (chip mode %u, cmd status %u).\n",
           st, (st >> 4) & 0x7u, (st >> 1) & 0x7u);

    /* Transmit a one-per-5-seconds heartbeat forever. */
    uint32_t counter = 0u;
    while (true) {
        uint8_t payload[16];
        int n = snprintf((char *)payload, sizeof payload, "CC7 #%lu",
                         (unsigned long)counter);
        bool ok = sx_transmit(payload, (uint8_t)n);
        printf("TX #%lu (%d bytes): %s\n",
               (unsigned long)counter, n, ok ? "TX_DONE" : "FAILED");
        counter++;
        sleep_ms(5000);
    }
    return 0;
}

/* End of exercise-02-sx1262-bringup.c. */
