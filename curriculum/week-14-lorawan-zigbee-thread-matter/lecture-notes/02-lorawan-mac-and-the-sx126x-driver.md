# Lecture 2 — The LoRaWAN MAC and the SX126x Driver

> *LoRa is the modulation; LoRaWAN is the etiquette. The SX1262 chip will happily key its PA and chirp a payload into the æther the moment you ask — that part is a fifteen-command SPI sequence and you will have it working by lunchtime. The hard part, the part that determines whether your packet shows up in the The Things Network console or vanishes into the noise floor, is the LoRaWAN MAC: the frame format, the frame counter, the message integrity code, the channel plan, the duty-cycle discipline, and the receive-window timing. This lecture is bottom-up: first the chip, then the MAC, then the two stitched together into a Class-A uplink. By the end you will understand every byte of the PHYPayload you transmit in the mini-project.*

## 1. The SX1262 as a state machine driven over SPI

The SX1262 is not a register-mapped peripheral in the way the RP2040's UART is. It is a small co-processor with its own state machine, and you drive it by sending **commands** over SPI and reading **responses**. Semtech's datasheet (SX1261/2 rev. 2.1) §13 is the command catalogue; §8 is the chip's own state machine. The states that matter:

- **SLEEP** — lowest power (~600 nA cold, ~1.5 µA with retention). The radio remembers nothing in cold sleep; you reconfigure on wake.
- **STANDBY_RC** — the chip is awake on its internal 13 MHz RC oscillator. You issue most configuration commands here. This is where you land after reset and where you return between transmissions.
- **STANDBY_XOSC** — awake on the 32 MHz TCXO/crystal. Needed before frequency synthesis.
- **FS (frequency synthesis)** — the PLL is locking to the target frequency.
- **TX** — transmitting. The chip leaves TX automatically when the packet is done and raises the `TX_DONE` interrupt.
- **RX** — receiving. Raises `RX_DONE` (or `TIMEOUT`, or `CRC_ERR`).

The transitions are not free-form: you cannot jump from SLEEP to TX. The datasheet's §9.2.1 gives the canonical TX sequence, and our driver follows it exactly.

### 1.1 The SPI command framing and the BUSY line

Every command is: assert NSS low, clock out the opcode byte, clock out (or in) the parameters, deassert NSS. Between commands you **must** wait for the `BUSY` line to go low. The chip raises BUSY while it digests a command; if you start the next command while BUSY is high, the chip drops it. This is the single most common SX126x driver bug and it is invisible without a logic analyzer — the chip just silently ignores half your configuration.

```c
static void sx_wait_busy(void) {
    uint32_t guard = 0u;
    while (gpio_get(PIN_BUSY)) {
        sleep_us(1);
        if (++guard > 100000u) {   /* 100 ms hard ceiling */
            /* BUSY stuck high almost always means the TCXO never started:
               you set SetDio3AsTcxoCtrl with the wrong voltage, or the
               module has no TCXO and you told the chip it does. */
            return;
        }
    }
}

static void sx_cmd(uint8_t opcode, const uint8_t *params, size_t n) {
    sx_wait_busy();
    gpio_put(PIN_NSS, 0);
    spi_write_blocking(spi0, &opcode, 1);
    if (n > 0u) spi_write_blocking(spi0, params, n);
    gpio_put(PIN_NSS, 1);
}
```

That `sx_wait_busy()` guard timeout is doing real diagnostic work. On a Waveshare SX1262 module the chip has a TCXO powered from DIO3, and if you forget the `SetDio3AsTcxoCtrl` command (or pass the wrong voltage code) the TCXO never starts, BUSY never falls, and *every* subsequent command hangs. The 100 ms ceiling turns an infinite hang into a printable diagnostic.

### 1.2 The configuration sequence, command by command

Exercise 2 builds the full sequence; here is the annotated skeleton (datasheet §9.2.1 ordering):

```c
static void sx_config_radio(void) {
    sx_reset();                                   /* NRST pulse, wait BUSY  */

    uint8_t standby = SX126X_STANDBY_RC;
    sx_cmd(SX126X_CMD_SET_STANDBY, &standby, 1);  /* land in STANDBY_RC     */

    uint8_t reg = 0x01u;                          /* DC-DC + LDO regulator  */
    sx_cmd(SX126X_CMD_SET_REGULATOR_MODE, &reg, 1);

    uint8_t rfsw = 0x01u;                         /* DIO2 drives the RF SW  */
    sx_cmd(SX126X_CMD_SET_DIO2_AS_RF_SWITCH_CTRL, &rfsw, 1);

    uint8_t lora = SX126X_PACKET_TYPE_LORA;
    sx_cmd(SX126X_CMD_SET_PACKET_TYPE, &lora, 1); /* LoRa, not GFSK         */

    sx_set_rf_frequency(868100000u);              /* EU868 ch 0             */

    uint8_t pa[4] = { 0x04, 0x07, 0x00, 0x01 };   /* SX1262 +22 dBm PA cfg  */
    sx_cmd(SX126X_CMD_SET_PA_CONFIG, pa, 4);

    uint8_t txp[2] = { 22, 0x04 };                /* +22 dBm, 200 us ramp   */
    sx_cmd(SX126X_CMD_SET_TX_PARAMS, txp, 2);

    sx126x_lora_modulation_t mod = {              /* SF7 BW125 CR4/5        */
        .sf = SX126X_LORA_SF7, .bw = SX126X_LORA_BW_125,
        .cr = SX126X_LORA_CR_4_5, .low_data_rate_optimize = 0u,
    };
    sx_set_modulation(&mod);
}
```

Two subtleties worth their own paragraph:

**The frequency word.** `SetRfFrequency` does not take Hz; it takes a 32-bit word `frf = freq_hz / FREQ_STEP` where `FREQ_STEP = F_XTAL / 2^25`. With a 32 MHz crystal, `FREQ_STEP ≈ 0.9537 Hz`. So 868.1 MHz becomes `frf = round(868100000 / 0.9537) = 910163968 = 0x363851C2`. Get this arithmetic wrong by a rounding step and you transmit on the wrong frequency by a few hundred Hz — usually still demodulable, but it eats link margin and in a crowded band it can put you on top of a neighbour's channel.

```c
#define SX126X_XTAL_FREQ  32000000.0
#define SX126X_FREQ_STEP  (SX126X_XTAL_FREQ / (double)(1u << 25))

static void sx_set_rf_frequency(uint32_t hz) {
    uint32_t frf = (uint32_t)((double)hz / SX126X_FREQ_STEP);
    uint8_t p[4] = { (uint8_t)(frf >> 24), (uint8_t)(frf >> 16),
                     (uint8_t)(frf >> 8),  (uint8_t)(frf) };
    sx_cmd(SX126X_CMD_SET_RF_FREQUENCY, p, 4);
}
```

**Low-data-rate optimisation.** At SF11 and SF12 with 125 kHz bandwidth, the symbol duration exceeds 16 ms and clock drift over a long packet starts to matter; the datasheet (§6.1.4) requires you to set the low-data-rate-optimize bit in the modulation params. Forget it and your SF12 packets demodulate intermittently. Our SF7 default does not need it, but the mini-project's ADR path does, so the flag is in the struct.

### 1.3 Transmitting a packet

Once configured, a transmission is: set the buffer base addresses, set the per-packet params (preamble length, header type, payload length, CRC), write the payload into the chip's 256-byte data buffer, arm the IRQ mapping, and issue `SetTx`. Then wait for `TX_DONE` on DIO1.

```c
static bool sx_transmit(const uint8_t *payload, uint8_t len) {
    uint8_t base[2] = { 0x00, 0x00 };             /* TX base, RX base       */
    sx_cmd(SX126X_CMD_SET_BUFFER_BASE_ADDRESS, base, 2);

    sx_set_packet_params(8u, len);                /* 8-symbol preamble      */
    sx_write_buffer(0x00, payload, len);

    uint8_t irq[8] = {                            /* enable all; route TX   */
        0x03, 0xFF,                               /* IrqMask = ALL          */
        (uint8_t)((SX126X_IRQ_TX_DONE | SX126X_IRQ_TIMEOUT) >> 8),
        (uint8_t)( SX126X_IRQ_TX_DONE | SX126X_IRQ_TIMEOUT),  /* DIO1Mask  */
        0x00, 0x00, 0x00, 0x00,                   /* DIO2, DIO3 unused      */
    };
    sx_cmd(SX126X_CMD_SET_DIO_IRQ_PARAMS, irq, 8);

    uint8_t to[3] = { 0, 0, 0 };                  /* TX with no timeout     */
    sx_cmd(SX126X_CMD_SET_TX, to, 3);

    uint32_t guard = 0u;
    while (gpio_get(PIN_DIO1) == 0) {             /* poll for TX_DONE        */
        sleep_ms(1);
        if (++guard > 2000u) return false;        /* 2 s ceiling            */
    }
    uint8_t clr[2] = { 0x03, 0xFF };
    sx_cmd(SX126X_CMD_CLEAR_IRQ_STATUS, clr, 2);
    return true;
}
```

In a battery design you would not poll DIO1; you would sleep the MCU and let DIO1 wake it with an interrupt. We poll here for clarity. The mini-project shows the interrupt-driven version.

### 1.4 Airtime and the duty-cycle budget

Before you transmit, you must know how long the packet occupies the air, because EU868 caps you at 1% duty cycle per sub-band (ETSI EN 300 220). The airtime of a LoRa packet is a closed-form function of SF, BW, CR, preamble length, and payload length (Semtech application note AN1200.13). A 13-byte payload at SF7/BW125/CR4/5 is ~46 ms; the same payload at SF12/BW125 is ~1320 ms. At SF12, 1% duty cycle means you may transmit that packet once every ~132 seconds — about 27 times an hour. This is *the* constraint that shapes LoRaWAN application design: you do not get to chatter.

```c
/* Time-on-air in milliseconds (Semtech AN1200.13 formula). */
static double lora_airtime_ms(uint8_t sf, uint32_t bw_hz, uint8_t cr_denom_minus4,
                              uint8_t preamble, uint8_t payload, int explicit_hdr,
                              int low_dr_opt) {
    double rs = (double)bw_hz / (double)(1u << sf);   /* symbol rate        */
    double ts = 1000.0 / rs;                          /* symbol time (ms)   */
    double t_preamble = (preamble + 4.25) * ts;

    int de = low_dr_opt ? 1 : 0;
    int ih = explicit_hdr ? 0 : 1;
    double num = 8.0 * payload - 4.0 * sf + 28 + 16 - 20.0 * ih;
    double den = 4.0 * (sf - 2 * de);
    double n_payload = 8 + (num > 0 ? (double)((int)((num / den) + 0.999999)) : 0)
                           * (cr_denom_minus4 + 4);
    if (n_payload < 8) n_payload = 8;
    double t_payload = n_payload * ts;
    return t_preamble + t_payload;
}
```

Run this for your payload before you choose an uplink interval. If `airtime_ms / interval_ms > 0.01`, you are over the duty-cycle limit and the LoRaWAN stack (or your own discipline) must hold the transmission.

## 2. The LoRaWAN frame, byte by byte

Now the MAC. A LoRaWAN uplink, as it sits in the radio's TX buffer, is the **PHYPayload**:

```
+--------+-----------------------------------------+--------+
|  MHDR  |               MACPayload                |  MIC   |
| 1 byte |                                         | 4 byte |
+--------+-----------------------------------------+--------+

MACPayload = FHDR | FPort | FRMPayload
FHDR       = DevAddr(4) | FCtrl(1) | FCnt(2) | FOpts(0..15)
```

### 2.1 MHDR — the MAC header

One byte. Top 3 bits are **MType** (message type): `010` = unconfirmed data up, `100` = confirmed data up. Bottom 2 bits are **Major** (always `00` for LoRaWAN R1). The middle 3 bits are RFU. For our unconfirmed uplink:

```c
p[0] = (LORAWAN_MTYPE_UNCONFIRMED_UP << 5) | LORAWAN_MAJOR_R1;   /* = 0x40 */
```

"Unconfirmed" means the device does not ask the network for an ACK. This is the default and the right choice for telemetry: confirmed uplinks force a downlink, downlinks are duty-cycle-expensive for the *gateway*, and confirming every reading does not improve a metrics pipeline that tolerates the occasional lost sample. Reserve confirmed uplinks for the rare message you truly must not lose.

### 2.2 FHDR — the frame header

- **DevAddr (4 bytes, little-endian on the wire).** The network address assigned at join (or hard-coded for ABP). The little-endianness is a perennial trap: the TTN console shows the DevAddr big-endian (`0x26011F88`) but on the wire it is byte-reversed (`88 1F 01 26`). Mismatched endianness is the second-most-common "why is my MIC wrong" bug after frame-counter mistakes.
- **FCtrl (1 byte).** ADR bit, ADRACKReq, ACK, ClassB, and the low nibble is FOptsLen (the length of any MAC commands piggybacked in the header). For a bare uplink with no MAC commands, FCtrl = 0.
- **FCnt (2 bytes, little-endian).** The frame counter. Monotonic, per-direction. This is the replay defence and the most operationally fraught field — see §2.5.

### 2.3 FPort and FRMPayload

- **FPort (1 byte).** 0 means the FRMPayload is MAC commands (encrypted with NwkSKey). 1–223 are application ports (encrypted with AppSKey). 224 is the certification test port. You pick an FPort to route the payload on the application side — e.g. FPort 1 for sensor data, FPort 2 for config acks.
- **FRMPayload (0..N bytes).** The application data, **encrypted**. The encryption is an AES-128 stream cipher (spec §4.3.3): you build a sequence of 16-byte "A_i" blocks containing the direction, DevAddr, FCnt, and a block counter, encrypt each with AppSKey, and XOR the keystream into the plaintext. It is AES in a CTR-like mode. Exercise 3 sends an *empty* FRMPayload so we can focus on the MIC; the mini-project adds this encryption.

### 2.4 The MIC — AES-128-CMAC, the gate everything passes through

The 4-byte MIC is computed over a **B0 block** prepended to the message (MHDR | FHDR | FPort | FRMPayload), keyed with NwkSKey, using AES-128-CMAC (RFC 4493), truncated to the first 4 bytes. The B0 block (spec §4.4):

```
B0 = 0x49 | 0x00 0x00 0x00 0x00 | Dir | DevAddr(4 LE) | FCntUp(4 LE) | 0x00 | len(msg)
```

```c
uint8_t b0_and_msg[16 + LORAWAN_MAX_PHY_PAYLOAD];
memset(b0_and_msg, 0, 16);
b0_and_msg[0]  = 0x49u;
b0_and_msg[5]  = LORAWAN_DIR_UPLINK;
memcpy(&b0_and_msg[6], session->devaddr, 4);
cc_write_le32(&b0_and_msg[10], session->fcnt_up);
b0_and_msg[15] = (uint8_t)msg_len;
memcpy(&b0_and_msg[16], msg, msg_len);

uint8_t mac[16];
aes_cmac(session->nwk_skey, b0_and_msg, 16u + msg_len, mac);   /* RFC 4493 */
memcpy(mic, mac, 4);   /* first 4 bytes */
```

You implement `aes_cmac` in Exercise 3, and you check it against the RFC 4493 test vectors **before** you trust it for LoRaWAN. This is the right engineering order: verify the primitive against the standard's vectors, *then* build the protocol on top. If you skip the vector check and your LoRaWAN node fails, you cannot tell whether the bug is in the CMAC, the B0 block, the key bytes, or the byte order — four suspects instead of one.

### 2.5 The frame counter — the field that bricks ABP nodes

The network server tracks the highest FCnt it has accepted from each device and rejects any uplink whose FCnt is at or below it (replay defence, spec §4.3.1.5). Consequences:

- **You must persist FCnt across reboots** for an ABP device. If the device reboots, resets FCnt to 0, and re-transmits, the server sees a counter it has already seen and silently drops everything until the device's counter climbs back above the server's high-water mark. On a device that uplinks once an hour, "climb back above" can take *days*. This is why ABP is operationally painful and OTAA (which gets a fresh DevAddr and counter per join) is preferred.
- **TTN has a "frame counter checks" toggle** you can disable for development so a reset device is not locked out. Disabling it in production reopens the replay window. For the mini-project on TTN we disable the check (it is a teaching device) and we say so.
- **16-bit vs 32-bit counters.** The on-air FCnt is 16 bits, but the server and device track a 32-bit counter and only the low 16 bits go on the wire; the server infers the high 16 from continuity. After 65,536 uplinks the low 16 bits roll over and the server's high-half inference carries it — as long as you have not skipped so many uplinks that the inference becomes ambiguous.

## 3. Channel plans and the regional parameters

LoRaWAN's channel plan is *regional* — the LoRa Alliance's "Regional Parameters" (RP002) defines EU868, US915, AU915, AS923, and others. Two you should know:

- **EU868** — three mandatory default channels (868.1, 868.3, 868.5 MHz), 125 kHz, with a 1% duty-cycle limit per sub-band. The OTAA join uses these three; additional channels arrive via the JoinAccept's CFList. SF7–SF12, data rates DR0–DR5.
- **US915** — 64 uplink channels (125 kHz) plus 8 (500 kHz) across 902–928 MHz, organised in eight sub-bands of eight. No duty-cycle limit, but a **dwell-time** limit (400 ms max airtime per packet) and a frequency-hopping requirement instead. Networks usually pin a device to one sub-band ("sub-band 2," channels 8–15) so it does not hop across 64 channels the gateway is not listening on.

For the mini-project we transmit on a single EU868 channel (868.1 MHz) to keep the radio code simple; a production stack rotates across all enabled channels (spec requires pseudo-random channel selection per uplink) and obeys the per-sub-band duty cycle. We note the simplification and point at where a real stack (LoRaMAC-node, the Semtech reference, or ChirpStack's device-side equivalents) handles it.

## 4. Adaptive Data Rate (ADR) — the network turns your knobs

A LoRaWAN device near a gateway with a strong signal is wasting battery transmitting at SF12 when SF7 would reach fine. **ADR** lets the network tell the device "use a lower SF / lower power" by setting the ADR bit and sending `LinkADRReq` MAC commands in downlinks. The device measures its own link margin (from the gateway's reported SNR), and the network nudges it toward the lowest SF that still closes the link. ADR is why a fixed-location LoRaWAN node converges to optimal power over its first few hours of operation. Mobile nodes (asset trackers) disable ADR because their link margin changes faster than the control loop can track.

## 4.4 The SX1262 command reference you actually use

For quick reference while writing the driver, the commands the mini-project issues, in the order it issues them, with their opcodes and parameter counts (Semtech datasheet §13):

| Command                       | Opcode | Params | What it does                                        |
|-------------------------------|:------:|:------:|-----------------------------------------------------|
| SetStandby                    | 0x80   | 1      | Land in STANDBY_RC (0x00) or STANDBY_XOSC (0x01)    |
| SetRegulatorMode              | 0x96   | 1      | LDO (0x00) or DC-DC+LDO (0x01) for lower TX current |
| SetDio2AsRfSwitchCtrl         | 0x9D   | 1      | DIO2 drives the module's antenna RF switch          |
| SetDio3AsTcxoCtrl             | 0x97   | 4      | DIO3 powers + controls the TCXO (voltage + timeout) |
| SetPacketType                 | 0x8A   | 1      | LoRa (0x01) or GFSK (0x00)                           |
| SetRfFrequency                | 0x86   | 4      | The 32-bit frequency word (hz / FREQ_STEP)          |
| SetPaConfig                   | 0x95   | 4      | PA duty cycle, hpMax, deviceSel (1262=0x00), paLut  |
| SetTxParams                   | 0x8E   | 2      | Output power (dBm) and ramp time                    |
| SetModulationParams           | 0x8B   | 4      | SF, BW, CR, low-data-rate-optimize                  |
| SetBufferBaseAddress          | 0x8F   | 2      | TX base and RX base offsets in the 256-byte buffer  |
| SetPacketParams               | 0x8C   | 6      | Preamble len, header type, payload len, CRC, IQ     |
| WriteBuffer                   | 0x0E   | 1+N    | Offset byte then N payload bytes into the buffer     |
| SetDioIrqParams               | 0x08   | 8      | IRQ enable mask + per-DIO routing masks             |
| SetTx                         | 0x83   | 3      | Start transmit; 24-bit timeout (0 = no timeout)     |
| GetIrqStatus                  | 0x12   | 2 read | Read which IRQs fired (TX_DONE, TIMEOUT, …)         |
| ClearIrqStatus                | 0x02   | 2      | Clear the IRQ bits                                  |
| GetStatus                     | 0xC0   | 1 read | Chip mode (bits 6:4) + command status (bits 3:1)    |

A few notes from experience:

- **Every one of these is preceded by the BUSY wait.** No exceptions. The driver wraps that in `sx_cmd`, so you never forget — but if you hand-issue a command for debugging, wait on BUSY yourself first.
- **The TCXO command (SetDio3AsTcxoCtrl) is module-specific.** Some SX1262 modules have a TCXO on DIO3 (you must issue this with the right voltage code, e.g. 1.8 V = 0x02, and a startup-timeout); some have a plain crystal (you must *not* issue it). Getting this wrong is bug #2 from the SOLUTIONS list — BUSY stuck high forever. Check your module's schematic.
- **SetPaConfig's deviceSel byte selects the chip.** 0x00 for SX1262 (high power, up to +22 dBm), 0x01 for SX1261 (up to +15 dBm). Mismatching it is bug #5.
- **GetStatus is your heartbeat.** Read it after config; a sane value (chip mode 2 = STANDBY_RC, command status 5 or 1) means SPI is talking. 0x00 or 0xFF means the bus is dead — check wiring and NSS.

## 4.5 The link budget — why LoRa reaches so far

It is worth doing the link-budget arithmetic once so the "kilometres on milliwatts" claim stops being magic. The link budget is the total dB you have to spend between transmitter output and receiver sensitivity:

```
link budget (dB) = TX power (dBm) + TX antenna gain − cable loss
                   + RX antenna gain − RX sensitivity (dBm)
```

For an SX1262 at +22 dBm into a 2 dBi antenna, received by a gateway with a 3 dBi antenna at an SF12/BW125 sensitivity of −137 dBm:

```
budget = 22 + 2 + 3 − (−137) = 164 dB
```

That 164 dB is enormous. Compare it to free-space path loss, which at 868 MHz is `32.45 + 20·log10(f_MHz) + 20·log10(d_km)`. At 868 MHz the constant works out so that 164 dB of budget corresponds to roughly `d ≈ 15 km` in free space — and even with 30–40 dB of urban clutter loss you still close several kilometres. The single term that makes this possible is the **−137 dBm sensitivity**, which comes from LoRa's processing gain pulling the signal out below the noise floor. A 250 kbit/s 802.15.4 radio at the same power has a sensitivity around −100 dBm, ~37 dB worse — and 37 dB of budget is the difference between 100 m and 10 km. That sensitivity gap *is* the LoRa-vs-mesh range story, expressed in decibels.

The trade is right there in the same arithmetic: SF7 sensitivity is ~−123 dBm, 14 dB worse than SF12, so SF7 reaches much less far — but its airtime is ~25× shorter. Spreading factor is the dial that trades link budget for airtime, and ADR (§4) is the network spinning that dial for you toward the lowest SF that still closes the budget with margin.

## 4.6 CAD, GFSK, and the things we are not using

The SX1262 does more than the LoRaWAN-uplink path we drive. Two features worth knowing exist:

- **Channel Activity Detection (CAD).** `SetCad` puts the radio into a brief listen that reports whether a LoRa preamble is present, much cheaper than a full receive. A listen-before-talk protocol uses CAD to avoid transmitting over an in-progress packet. LoRaWAN Class A does not require CAD (the network handles collisions statistically), but mesh-style LoRa protocols (Meshtastic, for instance) lean on it heavily.
- **GFSK packet mode.** The same chip does plain (G)FSK (`SetPacketType(GFSK)`) — the modulation used by many legacy sub-GHz protocols and by the Bluetooth/proprietary world. You would use GFSK for a point-to-point link to a non-LoRa device, or for the FSK channel some regional plans define. We stay in LoRa packet type all week.

Knowing these exist matters because the datasheet's command set is shared across modes, and a stray `SetPacketType(GFSK)` left in your init will make every LoRa command behave strangely — a good thing to rule out when bring-up misbehaves.

## 5. From the MAC back to the chip — the full uplink path

Putting the two halves together, a Class-A uplink on the mini-project's device is:

1. Read the sensor (a value to send).
2. Encrypt the FRMPayload with AppSKey (AES-CTR-like, spec §4.3.3).
3. Build the FHDR with the current FCnt.
4. Compute the MIC with NwkSKey over the B0 block (Exercise 3's code).
5. Assemble the PHYPayload.
6. Pick a channel (EU868 868.1 for us), set SF/BW from the current ADR state.
7. Drive the SX1262 through the §1 sequence and `SetTx` the PHYPayload.
8. Open RX1 (and RX2) receive windows at the spec-defined delays (1 s and 2 s after end-of-uplink for EU868) to catch any downlink.
9. Increment and persist FCnt.
10. Sleep until the next uplink interval, respecting the duty-cycle budget from §1.4.

Steps 2–5 are pure software and you can verify them entirely on the host (that is what Exercise 3 does). Steps 6–8 are the SX1262 driver (Exercise 2). The mini-project stitches all ten together and you watch the packet appear in the TTN console's live-data view — the moment the MIC, the keys, the byte order, and the frequency word are all correct simultaneously, the device materialises in the console and you have closed a real LPWAN link.

Read Lecture 3 next for Thread, Matter, and the conceptual Matter-over-Thread build that completes the week's "when to pick which" arc.
