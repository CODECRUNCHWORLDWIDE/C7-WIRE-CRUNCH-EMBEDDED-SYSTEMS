/*
 * sx1262.c — SX1262 LoRa transceiver driver (mini-project).
 *
 * Productised from Exercise 2. Targets the RP2040 + Pico SDK. See sx1262.h
 * for the wiring and the API. Citations: Semtech SX1261/2 datasheet rev. 2.1.
 */

#include "sx1262.h"

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

#define PIN_NSS    5
#define PIN_SCK    2
#define PIN_MOSI   3
#define PIN_MISO   4
#define PIN_BUSY   6
#define PIN_DIO1   7
#define PIN_NRST   8

#define SPI_PORT   spi0
#define SPI_HZ     (8u * 1000u * 1000u)

#define SX126X_XTAL_FREQ  32000000.0
#define SX126X_FREQ_STEP  (SX126X_XTAL_FREQ / (double)(1u << 25))

/* ---- low-level SPI ---- */

static void sx_wait_busy(void) {
    uint32_t guard = 0u;
    while (gpio_get(PIN_BUSY)) {
        sleep_us(1);
        if (++guard > 100000u) return;   /* 100 ms ceiling: TCXO never started */
    }
}

static void sx_cmd(uint8_t opcode, const uint8_t *params, size_t n) {
    sx_wait_busy();
    gpio_put(PIN_NSS, 0);
    spi_write_blocking(SPI_PORT, &opcode, 1);
    if (n > 0u) spi_write_blocking(SPI_PORT, params, n);
    gpio_put(PIN_NSS, 1);
}

static void sx_read_cmd(uint8_t opcode, uint8_t *resp, size_t n) {
    sx_wait_busy();
    gpio_put(PIN_NSS, 0);
    spi_write_blocking(SPI_PORT, &opcode, 1);
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

/* ---- helpers ---- */

static void sx_set_rf_frequency(uint32_t hz) {
    uint32_t frf = (uint32_t)((double)hz / SX126X_FREQ_STEP);
    uint8_t p[4] = { (uint8_t)(frf >> 24), (uint8_t)(frf >> 16),
                     (uint8_t)(frf >> 8),  (uint8_t)(frf) };
    sx_cmd(SX126X_CMD_SET_RF_FREQUENCY, p, 4);
}

static void sx_set_modulation(const sx126x_lora_modulation_t *m) {
    uint8_t p[4] = { (uint8_t)m->sf, (uint8_t)m->bw, (uint8_t)m->cr,
                     m->low_data_rate_optimize };
    sx_cmd(SX126X_CMD_SET_MODULATION_PARAMS, p, 4);
}

static void sx_set_packet_params(uint16_t preamble, uint8_t payload_len) {
    uint8_t p[6] = {
        (uint8_t)(preamble >> 8), (uint8_t)(preamble),
        0x00,          /* explicit header */
        payload_len,
        0x01,          /* CRC on */
        0x00,          /* standard IQ */
    };
    sx_cmd(SX126X_CMD_SET_PACKET_PARAMS, p, 6);
}

/* ---- public API ---- */

void sx1262_init(void) {
    spi_init(SPI_PORT, SPI_HZ);
    gpio_set_function(PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);

    gpio_init(PIN_NSS);  gpio_set_dir(PIN_NSS, GPIO_OUT);  gpio_put(PIN_NSS, 1);
    gpio_init(PIN_NRST); gpio_set_dir(PIN_NRST, GPIO_OUT); gpio_put(PIN_NRST, 1);
    gpio_init(PIN_BUSY); gpio_set_dir(PIN_BUSY, GPIO_IN);
    gpio_init(PIN_DIO1); gpio_set_dir(PIN_DIO1, GPIO_IN);

    /* Reset. */
    gpio_put(PIN_NRST, 0);
    sleep_ms(2);
    gpio_put(PIN_NRST, 1);
    sleep_ms(5);
    sx_wait_busy();

    uint8_t standby = SX126X_STANDBY_RC;
    sx_cmd(SX126X_CMD_SET_STANDBY, &standby, 1);
}

void sx1262_config(uint32_t freq_hz, const sx126x_lora_modulation_t *mod,
                   int8_t power_dbm) {
    uint8_t reg = 0x01u;   /* DC-DC + LDO */
    sx_cmd(SX126X_CMD_SET_REGULATOR_MODE, &reg, 1);

    uint8_t rfsw = 0x01u;  /* DIO2 drives RF switch */
    sx_cmd(SX126X_CMD_SET_DIO2_AS_RF_SWITCH_CTRL, &rfsw, 1);

    uint8_t lora = SX126X_PACKET_TYPE_LORA;
    sx_cmd(SX126X_CMD_SET_PACKET_TYPE, &lora, 1);

    sx_set_rf_frequency(freq_hz);

    /* SX1262 PA config for high-power operation (datasheet §13.1.14). */
    uint8_t pa[4] = { 0x04, 0x07, 0x00, 0x01 };
    sx_cmd(SX126X_CMD_SET_PA_CONFIG, pa, 4);

    uint8_t txp[2] = { (uint8_t)power_dbm, 0x04 };  /* 200 us ramp */
    sx_cmd(SX126X_CMD_SET_TX_PARAMS, txp, 2);

    sx_set_modulation(mod);
}

uint8_t sx1262_status(void) {
    uint8_t resp[2] = { 0, 0 };
    sx_read_cmd(SX126X_CMD_GET_STATUS, resp, 2);
    return resp[0];
}

bool sx1262_transmit(const uint8_t *payload, uint8_t len) {
    uint8_t base[2] = { 0x00, 0x00 };
    sx_cmd(SX126X_CMD_SET_BUFFER_BASE_ADDRESS, base, 2);

    sx_set_packet_params(8u, len);
    sx_write_buffer(0x00, payload, len);

    uint8_t irq[8] = {
        (uint8_t)(SX126X_IRQ_ALL >> 8), (uint8_t)(SX126X_IRQ_ALL),
        (uint8_t)((SX126X_IRQ_TX_DONE | SX126X_IRQ_TIMEOUT) >> 8),
        (uint8_t)( SX126X_IRQ_TX_DONE | SX126X_IRQ_TIMEOUT),
        0x00, 0x00, 0x00, 0x00,
    };
    sx_cmd(SX126X_CMD_SET_DIO_IRQ_PARAMS, irq, 8);

    uint8_t to[3] = { 0x00, 0x00, 0x00 };   /* no timeout */
    sx_cmd(SX126X_CMD_SET_TX, to, 3);

    uint32_t guard = 0u;
    while (gpio_get(PIN_DIO1) == 0) {
        sleep_ms(1);
        if (++guard > 4000u) return false;
    }

    uint8_t irq_status[3] = { 0, 0, 0 };
    sx_read_cmd(SX126X_CMD_GET_IRQ_STATUS, irq_status, 3);
    uint16_t flags = (uint16_t)((irq_status[1] << 8) | irq_status[2]);

    uint8_t clr[2] = { (uint8_t)(SX126X_IRQ_ALL >> 8), (uint8_t)(SX126X_IRQ_ALL) };
    sx_cmd(SX126X_CMD_CLEAR_IRQ_STATUS, clr, 2);

    /* Return to standby so the next config does not run while in TX. */
    uint8_t standby = SX126X_STANDBY_RC;
    sx_cmd(SX126X_CMD_SET_STANDBY, &standby, 1);

    return (flags & SX126X_IRQ_TX_DONE) != 0u;
}

/* End of sx1262.c. */
