# Challenge 1 — Shared bus, multiple devices

> *Three I²C devices on one wire pair. Bus-stuck recovery on demand. Inject a fault, recover from it, prove on a scope that you did.*

## Goal

Build a firmware that sequences three I²C devices on the same `I2C0` bus — BMP280 at `0x76`, MPU-6050 at `0x68`, SSD1306 at `0x3C` — and demonstrate the *nine-clock bus recovery* (UM10204 §3.1.16, p. 20) by deliberately stalling the bus and bringing it back without rebooting the MCU.

Production firmware will see a stuck bus once a month per fleet of 10,000 devices. The discipline this challenge teaches is: detect the stall, run the recovery, log the event, continue. Without it, your fleet has a known-failure mode whose recovery procedure is "user pulls the plug."

By the end you can:
- Drive three I²C peripherals on a single bus with non-overlapping transactions.
- Sequence the bus correctly so no two transactions interleave (no half-finished read when the next starts).
- Reinitialise the bus from a stuck state by reconfiguring SCL as a software GPIO, clocking nine pulses, and recovering.
- Inject a failure by deliberately dropping power to one slave mid-transaction, and recover.

## Setup

### Parts

- 1× Pi Pico W
- 1× BMP280 module
- 1× MPU-6050 module
- 1× SSD1306 OLED module — **the I²C variant this time** (4-pin: VCC, GND, SDA, SCL)
- 1× breadboard
- 8× jumper wires
- 2× 4.7 kΩ pull-up resistors
- 1× small momentary push-button or a jumper wire you can yank
- Logic analyzer

### Wiring

All three devices share GP4 (SDA) and GP5 (SCL). Each device gets its own VCC + GND wires. The pull-ups (4.7 kΩ each) live on the *common* SDA and SCL lines, not per-device.

| Device   | Addr  | SDA | SCL | VCC | GND |
|----------|-------|-----|-----|-----|-----|
| BMP280   | `0x76`| GP4 | GP5 | 3V3 | GND |
| MPU-6050 | `0x68`| GP4 | GP5 | 3V3 | GND |
| SSD1306  | `0x3C`| GP4 | GP5 | 3V3 | GND |

The push-button (or jumper) sits between the SSD1306's VCC and 3V3. Yanking it cuts power to *only* the SSD1306, leaving the BMP280 and MPU-6050 healthy. Depending on the failure timing, the SSD1306 may leave SDA or SCL stuck low at the moment power is cut.

### Prerequisites

- Exercises 1, 2, 3 of this week completed.
- Lecture 2 read, including the bus-recovery section.

## Reading

- **UM10204** §3.1.16 (Bus clear), p. 20.
- **RP2040 datasheet** §2.19 (IO_BANK0), pp. 240–290 — for re-routing GP4/5 between I²C and SIO at runtime.
- **Lecture 2** of this week.

## Steps

### 1. Bus-scan at boot

Add a function `i2c_bus_scan` that probes each 7-bit address from `0x08` to `0x77`:

```c
void i2c_bus_scan(void) {
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        uint8_t dummy = 0;
        int r = i2c0_write_read(addr, &dummy, 1, NULL, 0);
        if (r >= 0) uart_printf("  found 0x%02X\n", addr);
        else if ((-r) & 1) {} /* NOACK — no device */
        else            uart_printf("  abort  0x%02X (src=%d)\n", addr, -r);
    }
}
```

**Checkpoint:** UART prints `found 0x3C`, `found 0x68`, `found 0x76`. If any are missing, fix wiring first.

### 2. Round-robin reads

Build a 4 Hz loop that, on each iteration:

1. Reads BMP280 temperature + pressure.
2. Reads MPU-6050 accelerometer (6 bytes from `0x3B`).
3. Updates the SSD1306 with the three values.

```c
for (;;) {
    int32_t t_centi;
    uint32_t p_pa;
    int16_t ax, ay, az;
    bmp280_read(&bmp, &t_centi, &p_pa);
    mpu6050_read_accel(&mpu, &ax, &ay, &az);
    ssd1306_render(&oled, t_centi, p_pa, ax, ay, az);
    delay_ms(250);
}
```

**Checkpoint:** OLED refreshes 4 times a second with three live values. Saleae shows three back-to-back transactions per period.

### 3. Implement `i2c0_recover`

This is the load-bearing function. Pseudocode:

```c
bool i2c0_recover(void) {
    /* 1. Disable the I²C peripheral. */
    I2C0->IC_ENABLE = 0;

    /* 2. Re-route GP4 (SDA) and GP5 (SCL) as software GPIOs. */
    io_bank_funcsel(4, 5);  /* SIO */
    io_bank_funcsel(5, 5);
    sio_oe_set(1u << 5);    /* SCL: output */
    sio_oe_clr(1u << 4);    /* SDA: input (we observe it) */

    /* 3. If SDA is already high, nothing to recover. */
    if (sio_gpio_get(4)) goto restore;

    /* 4. Clock SCL 9 times with ~5 µs at each level. */
    for (int i = 0; i < 9; i++) {
        sio_out_set(1u << 5); delay_us(5);
        sio_out_clr(1u << 5); delay_us(5);
        if (sio_gpio_get(4)) break;   /* SDA released */
    }
    sio_out_set(1u << 5);   /* leave SCL high */
    delay_us(5);

    /* 5. Generate a STOP: SDA low, then SCL high, then SDA high. */
    sio_oe_set(1u << 4);
    sio_out_clr(1u << 4); delay_us(5);
    sio_out_set(1u << 5); delay_us(5);
    sio_out_set(1u << 4); delay_us(5);

    /* 6. Did it work? */
    bool ok = sio_gpio_get(4) && sio_gpio_get(5);

restore:
    /* 7. Restore I²C function on GP4/5 and re-enable. */
    io_bank_funcsel(4, 3);
    io_bank_funcsel(5, 3);
    I2C0->IC_ENABLE = 1;
    return ok;
}
```

**Checkpoint:** On a healthy bus, `i2c0_recover()` returns immediately at step 3 (SDA high already). Add a UART log line at each step and confirm.

### 4. Detect a stuck bus

In your main loop, wrap each transaction in a timeout. If `i2c0_write_read` doesn't return within (say) 10 ms, call `i2c0_recover`, log the event, and retry the transaction.

Implementation idea: capture `time_us_before` and after; in `i2c0_write_read`'s polling loops, check `time_us() - time_us_before > 10000` and bail with a sentinel error. The caller catches the sentinel and runs recovery.

### 5. Inject the failure

With the firmware running and the OLED refreshing happily, yank the SSD1306's VCC wire for 100 ms, then restore it. Capture on Saleae what happens to SDA and SCL.

Expected outcomes:
- **Best case:** the SSD1306 was idle when you cut power; SDA stays high; you see nothing on the bus. After power restore, the SSD1306 is reset and the next transaction works.
- **Median case:** the SSD1306 was mid-ACK when you cut power; SDA is stuck low. Your firmware times out, runs `i2c0_recover`, the recovery clocks nine times, after about cycle 3 the SSD1306 has fully discharged and releases SDA, the bus recovers, and the next transaction works.
- **Worst case:** the SSD1306 was mid-byte read and its internal state machine holds SCL low when power returns. Recovery fails. The push-button trick gets you only this far; full recovery requires a hardware fix (a power-cycle line driven by the MCU).

Document each outcome with a Saleae capture.

### 6. Log the recovery

Every time `i2c0_recover` runs, append a line to a circular ring buffer (in SRAM) and print it on the next idle moment. Format:

```
[12345 ms] i2c recover: pre SDA=0 SCL=1, post SDA=1 SCL=1, cycles=4 OK
```

This is the fleet log line you would forward upstream over MQTT in Phase III. Format it accordingly: a fixed-width prefix, a key=value body, no free text.

### 7. Capture and commit

Capture three Saleae traces and commit them in `challenges/ch01/traces/`:

1. `healthy.sal` — round-robin reads over the three devices with no failures.
2. `stuck-bus.sal` — the moment you yank the SSD1306 power and the bus goes stuck.
3. `recovery.sal` — the nine-clock recovery and the following successful transaction.

## Artifact

Commit to `challenges/ch01/`:

1. Source: `i2c_recover.c`, plus updated `main.c` with timeout + recovery wired in.
2. Three Saleae captures (see above).
3. A `BUS-RECOVERY.md` (one page) that describes:
   - The detection rule (10 ms timeout).
   - The recovery procedure (nine clocks + STOP).
   - The three observed outcomes (best, median, worst case) with the trace files referenced.
   - The limitation: no software recovery exists for a slave that holds SCL low; document the hardware countermeasure (an MCU-controlled power switch on each slave).

## Common faults

| Symptom | Cause | First diagnostic |
|---|---|---|
| `i2c_bus_scan` finds 0 devices | Pull-ups missing or wrong GP pins | Scope idle SDA/SCL — both must be at 3.3 V |
| `i2c_bus_scan` finds only some devices | One module not powered, or its address differs (e.g. SSD1306 at `0x3D` if its address jumper is set) | Cross-check the module's silkscreen |
| Recovery procedure leaves SDA still low | The slave is faulted with SDA held; software cannot help. Or: GP4 was not configured as input properly during the clock-out phase. | Scope GP4 during recovery — confirm the MCU is not driving |
| Recovery succeeds but next transaction NACKs | The slave's state machine has been left in mid-transaction internally; soft-reset the slave with its register-level reset command | For BMP280: write `0xB6` to register `0xE0` |
| Random NACKs without the failure injection | Pull-up sizing too weak for the combined bus capacitance of 3 devices + breadboard | Drop pull-up to 2.2 kΩ or move to fast mode and watch the rise time |

## Stretch goals

- Add a fourth device — an AHT20 humidity sensor at `0x38`. Verify the bus scan finds all four. Note: adding devices increases bus capacitance and may force smaller pull-ups.
- Implement *priority arbitration* in software: when two transactions are queued, the BMP280 read goes first (it is the slowest, ~7 ms in forced mode, and dominates the period). MPU-6050 second (low-latency motion). SSD1306 last (display lag is tolerable). Document the scheduling logic.
- Bring up a *second* I²C bus on `I2C1` (GP6/GP7) and split the SSD1306 onto its own bus. Measure throughput improvement: when the SSD1306 framebuffer flush of 1024 bytes is no longer blocking the BMP280, the sensor refresh rate doubles.
- Reproduce the bus stall by setting up a *clock-stretching adversary* in PIO: a PIO program that holds SCL low for 50 ms when triggered. This gives you a deterministic stall for CI testing, instead of yanking a wire.

## Hand-in

Push to GitHub, tag `w04-ch01`, when:
- The four-device bus scan succeeds.
- The recovery procedure verifiably brings back a yanked bus.
- The three Saleae captures are committed.
- `BUS-RECOVERY.md` is reviewed.
