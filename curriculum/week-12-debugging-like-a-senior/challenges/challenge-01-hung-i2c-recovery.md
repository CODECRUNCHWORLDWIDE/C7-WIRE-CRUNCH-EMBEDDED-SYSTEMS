# Challenge 1 — Hung I²C Bus: Reproduce, Prove, Recover

## Brief

Deliberately hang an I²C bus by resetting the master mid-transaction, *prove* on a logic analyzer that a slave is holding SDA low, recover the bus with the nine-clock-pulse trick from NXP UM10204 §3.1.16, and write a post-mortem with the analyzer captures. The goal is not the recovery code — that is twenty lines — but the discipline: never guess which device is holding a line; capture it, read the wire, then apply the spec-defined recovery.

You should spend ~2 hours on this challenge. The deliverable is the recovery firmware plus a markdown writeup `I2C-HANG-POSTMORTEM.md` and two logic-analyzer captures.

## Why this bug matters

A hung I²C bus is the single most common field failure of any I²C-connected embedded product, and the midterm reviewer stages it constantly. The cause is always the same: a master reset (watchdog, brownout, debugger, OTA) lands *mid-transaction*, while a slave was clocking out a read and driving SDA low for a bit, waiting for the next SCL clock. The master forgets the transaction; the slave waits forever; SDA stays low; the master cannot issue a START (which requires SDA to *fall* while SCL is high — impossible if SDA is already low). The bus is wedged and nothing your I²C peripheral does will recover it, because the peripheral cannot START into a stuck bus. The fix is to bit-bang SCL by hand until the slave finishes its byte and releases SDA. Knowing this — and being able to *prove* it on the wire rather than guess — is a senior skill.

## Hardware setup

- Your Pico (the I²C master).
- Any I²C sensor from Week 4 on the bus: a BME280 (`0x76`/`0x77`), an SSD1306 OLED (`0x3C`), or a DS3231 RTC (`0x68`). The BME280 is ideal because a register *read* is multi-byte, widening the window where a reset hangs it.
- Pull-ups on SDA and SCL (4.7 kΩ to 3V3; the Pico's internal pull-ups are weak — add externals for a clean capture).
- A logic analyzer with `D0→SDA`, `D1→SCL`, and a common ground. Sample at ≥2 MHz (the bus runs at 100–400 kHz; you want ≥4× the fastest edge).

## Procedure

### Phase 1 — Establish a healthy baseline capture

Before you break anything, capture a *working* transaction so you know what healthy looks like. Run a normal BME280 chip-id read (write register `0xD0`, read one byte; the BME280 answers `0x60`). Trigger the analyzer on the I²C START (SDA falling while SCL high) and capture one full transaction.

Apply the `i2c` protocol decoder in PulseView. A healthy read decodes as:

```text
START  0x76 W  ACK  0xD0 ACK  Sr  0x76 R  ACK  0x60 NACK  STOP
```

Save this as `i2c-healthy.sr` (or export the decoded transcript). This is your reference: address acked, register written, repeated start, data byte `0x60`, master NACKs, STOP. Annotate in your writeup which edges are the START and STOP.

### Phase 2 — Hang the bus

Force a reset mid-transaction. Two reliable ways:

1. **Watchdog mid-read.** Start a multi-byte BME280 read (read the calibration block, ~24 bytes), and arm a short watchdog so it fires partway through:

    ```c
    watchdog_enable(2, false);        /* 2 ms timeout */
    uint8_t cal[24];
    i2c_write_blocking(i2c0, 0x76, (uint8_t[]){0x88}, 1, true);  /* point at cal */
    i2c_read_blocking(i2c0, 0x76, cal, sizeof(cal), false);      /* watchdog fires
                                                                    somewhere here */
    ```

   The watchdog resets the Pico partway through the read. The BME280, mid-byte, is left driving SDA.

2. **Debugger-forced reset.** With GDB attached, `monitor reset halt` while a read is in flight. Cruder but deterministic if you can time it.

After the reset, your firmware re-runs `i2c_init` and tries a chip-id read. It times out — `i2c_read_blocking` returns `PICO_ERROR_GENERIC`. The bus is hung.

### Phase 3 — Prove the cause on the analyzer

This is the heart of the challenge. **Do not assume** the slave is holding SDA — *prove* it. Capture the bus in the hung state (trigger on "SDA low for longer than 1 ms" — a pattern/duration trigger, or just free-run and look). You should see:

- **SDA stuck low**, no edges.
- **SCL idle high**, no edges.
- No START, no STOP, no clocks.

Annotate this capture. The proof that it is the *slave* (not your Pico) holding SDA: with your Pico's I²C peripheral idle (or even with the Pico's SDA pad tristated as an input), SDA is *still* low — so something else on the bus is pulling it down. That something is the stuck slave. Save this capture as `i2c-hung.sr`.

In your writeup, answer:

1. Which device is holding SDA low, and how does the capture prove it (not just assert it)?
2. Why can the master not recover with a normal START? (Cite NXP UM10204 §3.1.10: a START is SDA falling while SCL high; SDA is already low, so there is no falling edge to make.)
3. How many SCL clocks, at most, does the slave need to finish its byte and release SDA? (Answer: up to 9 — 8 data bits plus the ACK/NACK clock; cite UM10204 §3.1.16.)

### Phase 4 — Recover with the nine-clock bus-clear

Implement the recovery from Lecture 3. The algorithm: detach the pins from the I²C peripheral, drive them as open-drain GPIO, release SDA, toggle SCL up to 9 times (stopping early if SDA goes high), issue a manual STOP, then re-attach the pins to the peripheral and re-init.

```c
void cc_i2c_bus_clear(i2c_inst_t *i2c, uint sda_pin, uint scl_pin) {
    /* Take manual control of the pins (the I2C peripheral cannot START into
       a stuck bus). */
    gpio_set_function(scl_pin, GPIO_FUNC_SIO);
    gpio_set_function(sda_pin, GPIO_FUNC_SIO);
    gpio_set_dir(scl_pin, GPIO_OUT);
    gpio_put(scl_pin, 1);
    gpio_set_dir(sda_pin, GPIO_IN);          /* release SDA; pull-up takes it high */

    /* Up to 9 SCL pulses to free a stuck slave (NXP UM10204 §3.1.16). */
    int pulses = 0;
    for (; pulses < 9; pulses++) {
        gpio_put(scl_pin, 0); sleep_us(5);   /* ~100 kHz half-period */
        gpio_put(scl_pin, 1); sleep_us(5);
        if (gpio_get(sda_pin)) {             /* SDA released -> slave is free */
            pulses++;
            break;
        }
    }

    /* Manual STOP: SDA low->high while SCL high. */
    gpio_set_dir(sda_pin, GPIO_OUT);
    gpio_put(sda_pin, 0); sleep_us(5);
    gpio_put(scl_pin, 1); sleep_us(5);
    gpio_put(sda_pin, 1); sleep_us(5);       /* the STOP edge */

    /* Hand the pins back to the I2C peripheral and re-init the bus. */
    gpio_set_function(sda_pin, GPIO_FUNC_I2C);
    gpio_set_function(scl_pin, GPIO_FUNC_I2C);
    i2c_init(i2c, 100 * 1000);

    printf("[i2c] bus clear: %d SCL pulses to free SDA\n", pulses);
}
```

Capture the recovery on the analyzer — you should see your 9 (or fewer) SCL pulses, SDA rising partway through, and the manual STOP. Then confirm a chip-id read succeeds again. The number of pulses it actually took is a useful datum: report it.

### Phase 5 — Make the recovery automatic

A senior product does not need a human to clear the bus. Wire the recovery into your I²C init path: on boot, before the first transaction, check whether SDA is stuck low (read the pad as a GPIO input); if it is, run `cc_i2c_bus_clear` first. Now a mid-transaction reset self-heals on the next boot. Demonstrate this: hang the bus, let the Pico reboot, and confirm it recovers with no human intervention. Capture it.

## Deliverable

1. `i2c_recovery.c` — the firmware: the bus-hang reproducer, `cc_i2c_bus_clear`, and the automatic on-boot recovery.
2. `i2c-healthy.sr`, `i2c-hung.sr`, `i2c-recovered.sr` — three logic-analyzer captures (sigrok session files or exported decoded transcripts).
3. `I2C-HANG-POSTMORTEM.md` (1500–2500 words) with required sections:
   - **Summary** — what hangs the bus, how the recovery works, in two paragraphs.
   - **Healthy baseline** — the decoded healthy transaction, annotated.
   - **The hang** — the hung-state capture, with the *proof* that the slave (not the master) holds SDA.
   - **Why a normal START fails** — the UM10204 §3.1.10 reasoning.
   - **The recovery** — the nine-clock capture, the pulse count, the spec citation.
   - **Automation** — the on-boot self-heal and its capture.
   - **References** — UM10204 sections, RP2040 §4.3, cited inline.

Commit with a message like `week-12/challenge-01: hung-I2C reproduce, prove, recover`.

## Pass criteria

- All three captures are committed and the decoded transcripts are readable.
- The post-mortem *proves* (does not merely assert) that the slave holds SDA, using the capture as evidence.
- The recovery code compiles, runs, and the `i2c-recovered.sr` capture shows the SCL pulses freeing SDA followed by a successful chip-id read.
- The automatic on-boot recovery is demonstrated end-to-end (hang → reboot → self-heal).
- The UM10204 §3.1.16 nine-clock reasoning is correctly stated, including *why* nine (8 data bits + 1 ACK clock).

## Why this challenge matters

The instinct, when an I²C bus is dead, is to re-read your driver code. But the driver is almost never the problem — the bus is in a state no driver can recover from, and the only instrument that tells you *which device* is holding *which line* is the analyzer on the wire. This challenge trains the reflex: dead bus → capture it → read the wire → apply the spec-defined recovery. That reflex, and the ability to prove the cause from a capture rather than guess, is exactly what separates a senior from a junior at the bench. The midterm reviewer will hand you a wedged bus; this challenge is your rehearsal.

## References

- NXP UM10204 "I²C-bus specification" rev. 7.0: §3.1.10 (START/STOP definitions), §3.1.16 (Bus clear — the nine-clock recovery). <https://www.nxp.com/docs/en/user-guide/UM10204.pdf>
- RP2040 datasheet §4.3 "I²C", pp. 446–490 (the DesignWare IP, pin functions). <https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf>
- sigrok `i2c` protocol decoder. <https://sigrok.org/wiki/Protocol_decoders>
- Lecture 3, §4 (the hung-I²C case study).
