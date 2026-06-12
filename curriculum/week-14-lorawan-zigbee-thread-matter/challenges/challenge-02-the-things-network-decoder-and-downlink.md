# Challenge 2 — The Things Network: Payload Decoder, Downlink, and a Class-A Round Trip

## Brief

Take the LoRaWAN uplink node from the mini-project and turn it into a complete Class-A round trip on The Things Network: write a JavaScript payload decoder so your sensor bytes show up as named fields in the TTN console, send a **downlink** from the network back to the device, and prove the device receives it in its RX1/RX2 windows. Then characterise the link — RSSI, SNR, airtime, and the duty-cycle headroom — and write it up.

Allocate ~3 hours. The deliverable is the decoder, the device-side downlink handler, and a writeup `TTN-WRITEUP.md`.

## Why this challenge

The mini-project gets a packet *to* the network. A real LoRaWAN device is a two-way citizen: the application server decodes its payload into meaningful fields, and the network occasionally sends it commands (a new reporting interval, an actuator command, an ADR adjustment). This challenge closes the loop and forces you to confront the Class-A timing model — downlinks only arrive in the two short receive windows after an uplink, which is the single most important behavioural fact about a low-power LoRaWAN device.

## Part A — The payload decoder

The TTN console runs a JavaScript "uplink formatter" on every received frame. Write one for your mini-project's payload. If your device sends temperature (int16, centidegrees) and battery (uint8, percent) on FPort 1:

```javascript
// TTN v3 uplink decoder (Payload Formatters -> Custom JavaScript formatter).
function decodeUplink(input) {
  // input.bytes is the decrypted FRMPayload; input.fPort is the port.
  if (input.fPort !== 1) {
    return { errors: ["unexpected FPort " + input.fPort] };
  }
  if (input.bytes.length < 3) {
    return { errors: ["payload too short"] };
  }
  // temperature: int16 big-endian centidegrees -> degrees C
  var raw = (input.bytes[0] << 8) | input.bytes[1];
  if (raw & 0x8000) { raw -= 0x10000; }   // sign-extend
  var tempC = raw / 100.0;
  var battery = input.bytes[2];           // percent
  return {
    data: { temperature_c: tempC, battery_pct: battery },
    warnings: battery < 15 ? ["low battery"] : []
  };
}
```

Paste it into the TTN console under your application's **Payload Formatters → Uplink**, send an uplink, and confirm the live-data view shows `temperature_c` and `battery_pct` as named fields instead of raw hex. Match the byte layout to whatever your mini-project device actually transmits — if you changed the payload, change the decoder.

## Part B — The downlink

### B1 — Queue a downlink from the console

In the TTN console, under your device's **Messaging → Downlink**, queue a downlink: FPort 2, payload `01` (say, "increase reporting rate"), confirmed or unconfirmed. TTN holds it until the device next uplinks (Class A: the network can only transmit in the device's receive windows).

### B2 — Device-side: open the receive windows

This is the part the mini-project skipped. After transmitting an uplink, a Class-A device must open **RX1** (same frequency as the uplink, configurable data rate, 1 second after end-of-uplink for EU868) and, if nothing arrives, **RX2** (a fixed frequency — 869.525 MHz for EU868 — and a fixed data rate, 2 seconds after end-of-uplink). The SX1262 driver puts the radio into RX with a timeout for each window.

```c
// After SetTx completes (TX_DONE), schedule the two RX windows.
// RX1_DELAY for EU868 Class A is 1 s after end-of-uplink; RX2 is at 2 s.
static bool sx_rx_window(uint32_t freq_hz, const sx126x_lora_modulation_t *mod,
                         uint32_t timeout_symbols, uint8_t *out, uint8_t *out_len) {
    sx_set_rf_frequency(freq_hz);
    sx_set_modulation(mod);

    // RX packet params: same preamble, explicit header, CRC on.
    sx_set_packet_params(8u, 255u);

    // Map RX_DONE + TIMEOUT to DIO1.
    uint8_t irq[8] = {
        0x03, 0xFF,
        (uint8_t)((SX126X_IRQ_RX_DONE | SX126X_IRQ_TIMEOUT) >> 8),
        (uint8_t)( SX126X_IRQ_RX_DONE | SX126X_IRQ_TIMEOUT),
        0, 0, 0, 0,
    };
    sx_cmd(SX126X_CMD_SET_DIO_IRQ_PARAMS, irq, 8);

    // SetRx with a symbol-count timeout (the chip converts to ticks internally).
    uint8_t to[3] = {
        (uint8_t)(timeout_symbols >> 16),
        (uint8_t)(timeout_symbols >> 8),
        (uint8_t)(timeout_symbols),
    };
    sx_cmd(SX126X_CMD_SET_RX, to, 3);

    // Poll DIO1 for the window's duration.
    uint32_t guard = 0u;
    while (gpio_get(PIN_DIO1) == 0) {
        sleep_ms(1);
        if (++guard > 3000u) return false;
    }

    uint8_t irq_status[3] = { 0, 0, 0 };
    sx_read_cmd(SX126X_CMD_GET_IRQ_STATUS, irq_status, 3);
    uint16_t flags = (uint16_t)((irq_status[1] << 8) | irq_status[2]);

    uint8_t clr[2] = { 0x03, 0xFF };
    sx_cmd(SX126X_CMD_CLEAR_IRQ_STATUS, clr, 2);

    if (flags & SX126X_IRQ_RX_DONE) {
        // GetRxBufferStatus gives the payload length and start offset.
        uint8_t st[4] = { 0 };
        sx_read_cmd(SX126X_CMD_GET_RX_BUFFER_STATUS, st, 4);
        uint8_t len = st[1];      // PayloadLengthRx
        uint8_t start = st[2];    // RxStartBufferPointer
        sx_read_buffer(start, out, len);
        *out_len = len;
        return true;
    }
    return false;  // TIMEOUT: nothing in this window.
}
```

The downlink, when it arrives, is itself a LoRaWAN frame (MType `011` unconfirmed-down or `101` confirmed-down) whose MIC you verify and whose FRMPayload you decrypt with AppSKey — the inverse of the uplink path you built in the mini-project. Decode the `01` and act on it (print it, or change the reporting interval).

### B3 — Prove the round trip

Queue the downlink, trigger an uplink, and watch the device receive `01` in RX1 (or RX2 if RX1 missed). Print it over the CDC console with which window caught it.

## Part C — Characterise the link

From the TTN console's live-data metadata for your uplinks, and from your own airtime computation (Lecture 2 §1.4), build a table:

- **RSSI and SNR** at the gateway (from the console) across SF7 and SF12 — show how the SNR margin grows with SF.
- **Airtime** for your payload at SF7 and SF12 (compute it).
- **Duty-cycle headroom**: at SF12, your airtime / 1% duty cycle = the minimum legal interval. Confirm your uplink interval respects it.
- **The data-rate / range trade**: argue, from your numbers, which SF you would pin this device to and why.

## Deliverable

1. `uplink_decoder.js` — the TTN payload formatter.
2. `downlink_handler.c` — the device-side RX-window code and downlink decode.
3. `TTN-WRITEUP.md` (1500–2500 words) — the round-trip demonstration, the link characterisation table, and the SF-selection argument.

## Pass criteria

- The TTN console shows decoded named fields, not raw hex.
- A downlink queued in the console is received by the device and printed, with the receive window (RX1/RX2) identified.
- The link-characterisation table has real RSSI/SNR numbers from the console and correct airtime computations.
- The writeup correctly explains why a Class-A device cannot receive a downlink except right after an uplink, and what that means for command latency.

## References

- LoRaWAN L2 1.0.4 §3.3 (Receive windows), §4.3.1 (downlink frames).
- LoRaWAN Regional Parameters RP002-1.0.4, EU868 RX1/RX2 timing and the RX2 default (869.525 MHz, DR0).
- Semtech SX1261/2 datasheet §13.1.6 (SetRx), §13.5.2 (GetRxBufferStatus).
- The Things Network documentation — payload formatters and downlink scheduling. <https://www.thethingsindustries.com/docs/integrations/payload-formatters/>.
