# Lecture 2 — I²C: multi-master, addressing, and the held-low SCL

> *Two wires, dozens of devices, one bus. Every slow chip on the line is allowed to make every fast chip wait.*

## Why I²C is harder than it looks

I²C looks simple on paper: SDA carries data, SCL carries the clock, both lines are pulled up by a single resistor each, and you address devices in-band with a 7-bit ID. A first-time bringup spec fits on a napkin.

In practice I²C is the bus that fails on every product. The failure modes are: a stuck SDA after an aborted transaction, a slave that holds SCL low forever because its firmware faulted mid-byte, a pull-up sized for one device that cannot meet rise-time when you add a fourth, a noise spike on SCL that the SCL filter rejects on one side of the bus and accepts on the other (so two listeners see different bit counts), an address collision the moment you wire a second BMP280 on the same bus. None of this happens with SPI.

The cost of I²C is in the corners. The benefit is two wires for arbitrarily many devices. For most embedded products it is worth it. For the ones where it isn't, the symptom shows up at scale, in production, on a Tuesday. So we treat I²C with the seriousness the corners deserve.

This lecture's references are RP2040 datasheet §4.3 (pp. 432–502) and UM10204 (the NXP I²C spec, rev 7.0). Page numbers below cite those revisions.

## The wire-level protocol

Two open-drain lines, SDA and SCL. "Open-drain" means each device has a transistor that can pull the line to GND but cannot drive it high; a shared pull-up resistor brings the line high when nothing is pulling. This is a *wired-AND*: the line is high iff every device is releasing it.

Every I²C transaction has the same skeleton (UM10204 §3.1, pp. 9–11):

```
SDA: ---\__ADDR[7..1]_R/W_ACK_DATA[7..0]_ACK_..._/---
SCL: ___/-\_/-\_/-\_/-\_/-\_/-\_/-\_/-\_/-\_/-\_____
        S  b7 b6 b5 b4 b3 b2 b1 b0 ACK ...        P
```

- **START condition (S):** SDA falls while SCL is high. This is one of only two times SDA may change while SCL is high (the other is STOP). Every other transition of SDA happens while SCL is low.
- **Address byte:** 7 bits of address, MSB first, followed by an R/W̄ bit. R/W̄ = 0 = write, R/W̄ = 1 = read.
- **ACK / NACK:** during the 9th SCL clock, the addressed slave pulls SDA low (ACK) or leaves it high (NACK / unaddressed). The master *releases* SDA for the 9th clock and reads it back to know.
- **Data byte(s):** 8 bits, MSB first, each followed by an ACK. On a master *write*, the slave ACKs. On a master *read*, the master ACKs each byte it wants to keep receiving and *NACKs* the last byte to tell the slave to stop.
- **STOP condition (P):** SDA rises while SCL is high.
- **RESTART (Sr):** another START without an intervening STOP. Equivalent to "STOP then START" except the master keeps the bus reserved across the transition (no other master can grab it). This is the primary use for restart: a read sequence where you first *write* the register address you want to read from, then RESTART, then *read* the data.

The 7-bit addressing field reserves four addresses (UM10204 §3.1.17, p. 21):

| Address range | Meaning                                          |
|---------------|--------------------------------------------------|
| `0b0000_000`  | General call                                     |
| `0b0000_001`  | CBUS                                             |
| `0b0000_010`  | Reserved (different I²C variants)                |
| `0b0000_011`  | Reserved                                         |
| `0b0000_1xx`  | Hs-mode master code                              |
| `0b1111_1xx`  | Reserved                                         |
| `0b1111_0xx`  | 10-bit addressing                                |

The remaining 112 addresses (`0x08`–`0x77` roughly, with some carve-outs) are available for slaves. The BMP280 is at `0x76` or `0x77`, the MPU-6050 at `0x68` or `0x69`, the SSD1306 at `0x3C` or `0x3D`. These do not collide; you can put all of them on one bus.

### Why SDA must change while SCL is low

UM10204 §3.1.4 (p. 10) defines the data validity rule: SDA may only change while SCL is low. The two exceptions — START and STOP — are *defined by* SDA changing while SCL is high. Everything else is data, and the receiver samples SDA on the rising edge of SCL.

This rule is what lets every slave decode the bus without a separate framing signal. If the rule is violated by a noisy bus or a stuck slave, decoders fall apart and the bus appears to "lose bytes."

## Clock stretching

A slow slave can hold SCL low after an ACK to defer the next bit (UM10204 §3.1.9, p. 14). The master *must* tolerate this: the master drives SCL high through a pull-up, the slave can drag it low, and the master only proceeds when it sees SCL actually go high. This is *clock stretching*, and the rule is universally enforced.

Two scenarios trigger clock stretching:
1. **The slave needs internal time.** An EEPROM doing a page write may stretch for tens of milliseconds. A BMP280 in *forced mode* stretches for a few milliseconds while it samples (BMP280 datasheet §3.6, p. 21).
2. **The master is slower than the slave expected.** Less common; mostly an FPGA or DMA-paced corner case.

The RP2040 I²C controller handles clock stretching automatically in hardware — your code does not call out to "wait for stretch to end." But on a scope you *see* it: a low pulse on SCL that is wider than the configured clock period. Mid-bringup, if your SCL pulses are 12 µs long instead of 5 µs, that is the slave stretching. It is not a bug.

Clock stretching can fail badly if the slave's firmware faults mid-transaction and holds SCL low *forever*. The bus is stuck. The recovery procedure (UM10204 §3.1.16, p. 20) is: send nine clock pulses on SCL with SDA released. Most slaves abort their state machine after nine clocks of inactivity. If nine clocks don't recover the bus, only a power cycle will.

## Multi-master arbitration

Two masters can drive the bus simultaneously. If both pull SDA low, the wire is low. If both release, the wire is high. If one pulls low and one releases, the wire is *low*, and the master that released *sees* SDA low — different from what it just drove. That master immediately backs off (UM10204 §3.1.11, p. 17). This is bit-wise non-destructive arbitration.

In practice, most embedded systems are single-master. The RP2040 I²C controller can operate as either master or slave; we use it as master throughout this course. Multi-master is touched here for completeness; you may encounter it on a system management bus (SMBus) or a CAN-to-I²C bridge.

The RP2040 I²C does support multi-master in hardware — if it loses arbitration mid-transaction, it sets the `TX_ABRT` interrupt with an `ARB_LOST` source bit (datasheet §4.3.16.x, `IC_TX_ABRT_SOURCE` register). Your transport layer should check this on every transaction.

## The RP2040 I²C controller — register walk

The RP2040 wraps a Synopsys DesignWare DW_apb_i2c. The register names are Synopsys's, not ARM's: every register is `IC_<something>`. Two instances, `I2C0` at `0x4004_4000` and `I2C1` at `0x4004_8000`. Documented in datasheet §4.3.16, pp. 463–502.

### `IC_CON` — Control (offset `0x00`)

| Bits  | Field                  | Meaning                                                |
|-------|------------------------|--------------------------------------------------------|
| 0     | `MASTER_MODE`          | 1 = master, 0 = slave                                   |
| 2:1   | `SPEED`                | `01`=Std (100 kHz), `10`=Fast (400 kHz), `11`=High      |
| 3     | `IC_10BITADDR_SLAVE`   | When acting as a slave, expect 10-bit                   |
| 4     | `IC_10BITADDR_MASTER`  | When acting as a master, send 10-bit                    |
| 5     | `IC_RESTART_EN`        | Allow RESTART within a transaction (set this)           |
| 6     | `IC_SLAVE_DISABLE`     | 1 = disable slave behavior (always set for master-only) |

For a master at standard 100 kHz with restart enabled: `IC_CON = 0x0063`.

### `IC_TAR` — Target Address (offset `0x04`)

Bits 9:0 hold the target slave address. For 7-bit, write `0x00` in bits 11:10 and the address in 6:0; bit 11 (`SPECIAL`) and bit 10 (`GC_OR_START`) are zero for normal addressing.

### `IC_DATA_CMD` — Data + Command (offset `0x10`)

This is the load-bearing FIFO of the whole controller.

| Bits | Field        | Meaning                                                                 |
|------|--------------|-------------------------------------------------------------------------|
| 7:0  | `DAT`        | Data byte (write side) / received byte (read side)                      |
| 8    | `CMD`        | 0 = master write, 1 = master read                                       |
| 9    | `STOP`       | 1 = generate STOP after this byte                                       |
| 10   | `RESTART`    | 1 = generate RESTART before this byte                                   |
| 11   | `FIRST_DATA_BYTE` | 1 = read-only flag, this is the first byte received                |

A full register-read transaction (read 6 bytes from MPU-6050 starting at `ACCEL_XOUT_H` = `0x3B`):

```c
i2c->IC_DATA_CMD = 0x3B;                            /* write the register address */
i2c->IC_DATA_CMD = (1u << 8) | (1u << 10);          /* CMD = read, RESTART before */
i2c->IC_DATA_CMD = (1u << 8);                       /* CMD = read */
i2c->IC_DATA_CMD = (1u << 8);
i2c->IC_DATA_CMD = (1u << 8);
i2c->IC_DATA_CMD = (1u << 8);
i2c->IC_DATA_CMD = (1u << 8) | (1u << 9);           /* CMD = read, STOP after */
/* Then drain RX side: read IC_DATA_CMD six times. */
for (int i = 0; i < 6; i++) {
    while (!(i2c->IC_STATUS & (1u << 3))) {}        /* RFNE: RX FIFO not empty */
    rx_buf[i] = i2c->IC_DATA_CMD & 0xFF;
}
```

Note how the entire START / address / write-the-pointer / RESTART / read-6-bytes-with-STOP sequence is encoded as 7 FIFO pushes and 6 FIFO pops. The DesignWare controller assembles the bus protocol from the upper bits of `IC_DATA_CMD`. This is unusual but elegant — once you grok the FIFO encoding, the controller becomes simple to drive.

### `IC_SS_SCL_HCNT` / `IC_SS_SCL_LCNT` — Standard-mode clock timing (offsets `0x14`, `0x18`)

These set the high and low counts (in `clk_peri` cycles) for SCL in standard mode. The formula (§4.3.5, p. 442):

```
Fscl = clk_peri / (HCNT + LCNT + spike_filter_cycles + sync_cycles)
```

With `clk_peri = 125 MHz`, target 100 kHz: `HCNT + LCNT ≈ 1250`. The SDK picks `HCNT = 650`, `LCNT = 730` (the asymmetry leaves headroom for setup/hold, and satisfies the UM10204 Table 10 minimums of 4.0 µs low, 4.7 µs high).

For fast mode (400 kHz), use `IC_FS_SCL_HCNT` / `IC_FS_SCL_LCNT` (offsets `0x1C`, `0x20`); `HCNT + LCNT ≈ 312`.

### `IC_ENABLE` — Enable (offset `0x6C`)

Bit 0: 1 = enable, 0 = disable. Most config registers (`IC_CON`, `IC_TAR`, the SCL timing) require the controller to be *disabled* before they can be written. Your init sequence is:

1. Disable (`IC_ENABLE = 0`).
2. Configure `IC_CON`, `IC_TAR`, `IC_SS_SCL_HCNT`, `IC_SS_SCL_LCNT`, etc.
3. Enable (`IC_ENABLE = 1`).
4. Push transaction bytes into `IC_DATA_CMD`.

### `IC_STATUS` — Status (offset `0x70`)

| Bit | Field          | Meaning                                          |
|-----|----------------|--------------------------------------------------|
| 0   | `ACTIVITY`     | Controller has activity                          |
| 1   | `TFNF`         | TX FIFO not full                                 |
| 2   | `TFE`          | TX FIFO empty                                    |
| 3   | `RFNE`         | RX FIFO not empty                                |
| 4   | `RFF`          | RX FIFO full                                     |
| 5   | `MST_ACTIVITY` | Master is active on the bus                      |

The polled inner loop: wait for `TFNF` before pushing into `IC_DATA_CMD`, wait for `RFNE` before reading from it.

### `IC_TX_ABRT_SOURCE` — TX Abort Source (offset `0x80`)

A 32-bit bitmap of *why* a transaction was aborted (datasheet §4.3.16.x). The bits you care about most:

| Bit | Source                      | Meaning                                                          |
|-----|-----------------------------|------------------------------------------------------------------|
| 0   | `ABRT_7B_ADDR_NOACK`        | The slave NACK'd the address byte — wrong address or no device   |
| 1   | `ABRT_10ADDR1_NOACK`        | 10-bit first byte NACK'd                                         |
| 2   | `ABRT_10ADDR2_NOACK`        | 10-bit second byte NACK'd                                        |
| 3   | `ABRT_TXDATA_NOACK`         | A data byte NACK'd mid-write                                     |
| 7   | `ABRT_SBYTE_ACKDET`         | Start byte ACK'd (shouldn't happen)                              |
| 12  | `ARB_LOST`                  | Lost arbitration to another master                               |

After a TX_ABRT, you must read `IC_CLR_TX_ABRT` (offset `0x54`) to clear the abort and unblock the controller. The transport layer should do this transparently and return the abort reason to the caller.

## A minimal I²C init for I²C0 on the Pi Pico W

```c
#define I2C0_BASE     0x40044000u
#define RESETS_BASE   0x4000C000u
#define IO_BANK0_BASE 0x40014000u

typedef struct {
    volatile uint32_t IC_CON;
    volatile uint32_t IC_TAR;
    volatile uint32_t IC_SAR;
    volatile uint32_t reserved_0c;
    volatile uint32_t IC_DATA_CMD;
    volatile uint32_t IC_SS_SCL_HCNT;
    volatile uint32_t IC_SS_SCL_LCNT;
    volatile uint32_t IC_FS_SCL_HCNT;
    volatile uint32_t IC_FS_SCL_LCNT;
    /* ... omitted ... */
} i2c_t;

#define I2C0  ((i2c_t *)I2C0_BASE)

void i2c0_init_100khz(void) {
    /* 1. Bring I2C0 out of reset. RP2040 datasheet §2.7, RESETS_RESET bit 3. */
    *(volatile uint32_t *)(RESETS_BASE + 0x3000) &= ~(1u << 3);  /* CLR alias */
    while ((*(volatile uint32_t *)(RESETS_BASE + 0x8) & (1u << 3)) == 0) {}

    /* 2. Configure SDA (GP4) and SCL (GP5) function-select to I2C (FUNCSEL=3). */
    /*    IO_BANK0 GPIO4_CTRL at 0x40014000 + 0x020, GPIO5_CTRL at 0x028. */
    *(volatile uint32_t *)(IO_BANK0_BASE + 0x020) = 3;  /* GP4 -> I2C0_SDA */
    *(volatile uint32_t *)(IO_BANK0_BASE + 0x028) = 3;  /* GP5 -> I2C0_SCL */

    /* 3. Disable the controller before configuring. */
    I2C0->IC_ENABLE = 0;
    while (I2C0->IC_ENABLE_STATUS & 1) {}

    /* 4. Master mode, 7-bit, 100 kHz, restart enabled. */
    I2C0->IC_CON = (1u << 0)    /* MASTER_MODE */
                 | (1u << 1)    /* SPEED = Std (100 kHz) */
                 | (1u << 5)    /* IC_RESTART_EN */
                 | (1u << 6);   /* IC_SLAVE_DISABLE */

    /* 5. Timing for 100 kHz at clk_peri = 125 MHz. */
    I2C0->IC_SS_SCL_HCNT = 650;
    I2C0->IC_SS_SCL_LCNT = 730;
    I2C0->IC_FS_SPKLEN   = 1;

    /* 6. Enable. */
    I2C0->IC_ENABLE = 1;
}
```

About 30 lines. With pull-ups installed (4.7 kΩ from SDA to 3.3 V, 4.7 kΩ from SCL to 3.3 V) and an I²C device wired up, this is sufficient to clock the BMP280.

## A write-then-read transaction

```c
int i2c0_write_read(uint8_t addr,
                    const uint8_t *tx, size_t tx_len,
                    uint8_t *rx, size_t rx_len) {
    /* Set target address. The controller must be disabled to write IC_TAR. */
    I2C0->IC_ENABLE = 0;
    I2C0->IC_TAR = addr;
    I2C0->IC_ENABLE = 1;

    /* Push write bytes. The last write byte gets RESTART set if rx_len > 0;
     * the last byte overall gets STOP. */
    for (size_t i = 0; i < tx_len; i++) {
        uint32_t cmd = tx[i];
        if (i == tx_len - 1 && rx_len == 0) cmd |= (1u << 9);  /* STOP if no read follows */
        while (!(I2C0->IC_STATUS & (1u << 1))) {}  /* TFNF */
        I2C0->IC_DATA_CMD = cmd;
    }

    /* Push read commands. */
    for (size_t i = 0; i < rx_len; i++) {
        uint32_t cmd = (1u << 8);  /* CMD = read */
        if (i == 0)              cmd |= (1u << 10);  /* RESTART before first read */
        if (i == rx_len - 1)     cmd |= (1u << 9);   /* STOP after last read */
        while (!(I2C0->IC_STATUS & (1u << 1))) {}
        I2C0->IC_DATA_CMD = cmd;
    }

    /* Drain RX. */
    for (size_t i = 0; i < rx_len; i++) {
        /* Wait for RX byte or abort. */
        while (!(I2C0->IC_STATUS & (1u << 3))) {
            if (I2C0->IC_RAW_INTR_STAT & (1u << 6)) {   /* TX_ABRT */
                uint32_t src = I2C0->IC_TX_ABRT_SOURCE;
                (void)I2C0->IC_CLR_TX_ABRT;
                return -(int)src;                       /* negative = error */
            }
        }
        rx[i] = (uint8_t)(I2C0->IC_DATA_CMD & 0xFF);
    }
    return (int)rx_len;
}
```

About 40 lines, single-function. The function returns the byte count on success and a negative value encoding `IC_TX_ABRT_SOURCE` on failure. This is the kernel of the transport layer we build in Lecture 3.

## Pull-up sizing in practice

Recall the UM10204 §7.2 budget (resources.md cheat-sheet):

- Standard 100 kHz, 400 pF max bus capacitance, 1 µs max rise time → 4.7 kΩ to 10 kΩ.
- Fast 400 kHz, 400 pF max, 300 ns max rise → 2.2 kΩ to 4.7 kΩ.

A small Pi Pico breadboard with 3–4 devices is ~60–100 pF. Use **4.7 kΩ** for standard mode and **2.2 kΩ** for fast mode. Two resistors, one per line, between the line and 3.3 V.

If you can scope the rising edge of SDA: it should reach 70% of VDD within the budget. A slow rise (rounded waveform that takes 3 µs to cross threshold) means the pull-up is too weak or the bus is too capacitive. Drop the resistor value or shorten the wires.

## Bus-stuck recovery

A slave that faults mid-transaction may release neither SDA nor SCL. The bus is *stuck low*. The RP2040 cannot generate a START because START requires SDA to be high before it falls.

The recovery procedure (UM10204 §3.1.16):

1. Reconfigure SCL as a software-driven GPIO (FUNCSEL=5 = SIO).
2. With SDA tri-stated, drive SCL high and low for nine cycles, leaving ~5 µs at each level.
3. After cycle 9, if SDA goes high (the slave released), re-route SCL back to the I²C peripheral and re-init. The bus is recovered.
4. If SDA is still low after 9 clocks, the slave is unrecoverable; power-cycle the slave (or the whole board if VDD is shared).

This is the procedure your transport-layer init should run *before* the first transaction, every boot. It costs ~50 µs at standard rate and saves you from a bus that was stuck across a soft reset. Reference code is in `pico-examples/i2c/lcd_1602_i2c/` (clear example) and `pico-sdk/src/rp2_common/pico_i2c_slave/`.

## Common faults — the I²C edition

1. **No ACK.** `IC_TX_ABRT_SOURCE.ABRT_7B_ADDR_NOACK` is set. Either the slave is at a different address than you think (BMP280's SDO floating? AD0 floating on MPU-6050?), the slave isn't powered, or the bus is stuck. Diagnostic: scope the 9th SCL clock of the address byte; SDA should be low. If SDA is high, no slave answered.
2. **Stuck-low SDA.** Both lines should be high at idle. If SDA is stuck low at idle, run the recovery procedure. If SCL is also stuck low, a slave is holding it — power-cycle.
3. **NACK mid-data.** `ABRT_TXDATA_NOACK`. The slave ACK'd the address but NACK'd a data byte. Many EEPROMs do this when you exceed their page-write boundary; some sensors do it on a malformed command. Diagnostic: scope which byte NACK'd, cross-reference the device's register spec.
4. **Clock stretching beyond the master's timeout.** The RP2040 has no software-visible timeout — it waits forever. If a slave holds SCL for 100 ms while doing a flash erase, your code blocks for 100 ms. On a single-threaded firmware this is bad; on a FreeRTOS task it is fine as long as priority inheritance is sane.
5. **Pull-up too weak.** Slow rising edges. The bus *might* work at 100 kHz but corrupts at 400 kHz. Diagnostic: scope SDA's rise time. If > 1 µs, drop the pull-up.
6. **Bit reversed.** Some breakout boards label "A0/A1/A2" but the chip's datasheet calls them "SDO/SDA/AD0." Wiring SDO into the address pin instead of the data line means *every* transaction NACKs. Diagnostic: read the breakout board's silkscreen *and* the chip's datasheet, agree they match.

## What you take away

- I²C is two open-drain lines with addressing in-band. 7-bit address + R/W̄ + ACK per byte. RESTART for in-transaction direction changes.
- Pull-ups are *required*. Size them for the rise-time budget: 4.7 kΩ standard, 2.2 kΩ fast.
- The RP2040 I²C controller is a Synopsys DW_apb_i2c. The trick is `IC_DATA_CMD`: every START/STOP/RESTART/CMD is encoded in the upper bits of FIFO pushes.
- Always check `IC_TX_ABRT_SOURCE` after a transaction. Always clear it via `IC_CLR_TX_ABRT`.
- The nine-clock bus recovery is the most-skipped, most-needed init step. Add it to every transport layer.
- A working I²C bus is *quiet at idle*. SDA high, SCL high. If either is low when nobody is talking, you have a problem.

Tomorrow we lift these two transports (SPI, I²C) and the device drivers above them into the protocol/transport split that makes the code testable, swappable, and shippable.

## References

- UM10204 — I²C-Bus Specification and User Manual (NXP rev 7.0, Oct-2021), §3.1, pp. 8–22; §3.1.16 (Bus clear), p. 20; §7.2 (Rise time and bus capacitance), p. 41.
- RP2040 datasheet (Sep-2024), §4.3 I²C, pp. 432–502; §4.3.16 (Register list), p. 463; §4.3.5 (Timing parameters), p. 442.
- Synopsys DW_apb_i2c databook (Synopsys proprietary; the RP2040 datasheet reproduces all relevant register tables).
- BMP280 datasheet (Bosch rev 1.20), §5 (Digital interface), pp. 30–36.
- MPU-6050 register map (InvenSense RM-MPU-6000A-00 rev 4.2), §3.1 (Register Map).
