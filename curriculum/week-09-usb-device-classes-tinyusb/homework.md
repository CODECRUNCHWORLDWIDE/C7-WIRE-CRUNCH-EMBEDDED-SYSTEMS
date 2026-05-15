# Week 9 — Homework

Six problems. Most ask for a one-page write-up, a captured trace, or a small piece of code. Aim for ~6 hours total. Commit answers to `week-09-usb-device-classes-tinyusb/homework/`.

---

## Problem 1 — Walking an enumeration in Wireshark

Set up Wireshark with USBPcap (Windows) or `usbmon` (Linux) or use the Apple USB Prober (macOS) and capture the enumeration of your Week 9 Exercise 1 firmware. The capture will show roughly 30 USB transactions on endpoint zero in the first 200 ms after attach.

For your write-up, find and annotate the following five transactions in the capture (give the Wireshark frame number for each):

1. The bus reset (you may see this as "SE0" lasting ~10 ms, or as a Wireshark-synthesized "URB_RESET" entry depending on the capture tool).
2. The first `GET_DESCRIPTOR(Device, 8 bytes)` request and its response.
3. The `SET_ADDRESS(N)` request.
4. The `GET_DESCRIPTOR(Configuration, wTotalLength)` request — pick out the value of `wTotalLength` from the SETUP packet's `wLength` field.
5. The `SET_CONFIGURATION(1)` request.

For each transaction, copy the SETUP packet's 8 bytes verbatim from the capture and write out a decoded version (`bmRequestType=0xAA, bRequest=B, wValue=0xCCCC, wIndex=0xDDDD, wLength=0xEEEE`).

Commit the capture as `homework/problem-01-enumeration.pcapng` and the write-up as `homework/problem-01-walkthrough.md`. ~1 hour.

---

## Problem 2 — Compute the composite-device wTotalLength

You are building a composite device with the following interface roster:

- 2 × CDC ACM (each is 2 interfaces, 1 IAD wrapping both).
- 1 × HID (1 interface, 1 IAD wrapping it).
- 1 × MSC (1 interface, 1 IAD wrapping it).
- 1 × MIDI (1 interface, no IAD because MIDI is single-interface and the host's class-driver-binding heuristic doesn't need an IAD for it; we will model it as needing one anyway for consistency, so add 1 IAD).

Compute `bNumInterfaces` and `wTotalLength` step by step. Assume:

- Each CDC's interface tree contributes 9 + 5+5+4+5 + 7 + 9 + 7+7 = 58 bytes (i.e., the IF.comm + functionals + EP.notify + IF.data + 2 × EP.bulk).
- Each IAD is 8 bytes.
- HID's interface tree contributes 9 (IF) + 9 (HID class desc) + 7 (EP.in) + 7 (EP.out) = 32 bytes.
- MSC's interface tree contributes 9 (IF) + 7 (EP.in) + 7 (EP.out) = 23 bytes.
- MIDI's interface tree contributes (you decide; look up MIDIStreaming 1.0 §6 or use 45 bytes as a placeholder).
- The configuration descriptor header itself is 9 bytes.

Show the arithmetic. State your `bNumInterfaces` and `wTotalLength` values. Bonus: identify which two endpoint addresses you would assign to each function so that none collide. ~30 minutes.

---

## Problem 3 — A HID report-descriptor decode

Given the following 28-byte HID report descriptor, decode it into a human-readable description of the report format it declares.

```text
05 01 09 04 A1 01 09 30 09 31 15 81 25 7F 75 08
95 02 81 02 05 09 19 01 29 03 75 01 95 03 81 02
75 05 95 01 81 03 C0
```

Walk it item-by-item. For each Main item (`Input`, `Output`, `Feature`, `Collection`, `End Collection`), summarize the report content it appends. The final result is: an input report of N bytes, with the following field breakdown.

Hint: there are two `Input` items in this descriptor. The whole report is 3 bytes.

Commit as `homework/problem-03-hid-decode.md`. ~45 minutes.

---

## Problem 4 — Pre-format a 1 MB FAT12 RAM disk

The Exercise 3 RAM disk is 128 KB and uses 1-sector clusters. For homework, work out the FAT12 pre-format for a *1 MB* disk:

1. Compute the number of clusters needed.
2. Choose a cluster size (the FAT12 spec limits the cluster count to under 4085 to remain FAT12; if you need more clusters than that, you must move to FAT16). What cluster size keeps the cluster count under 4085 for a 1 MB disk?
3. Compute the FAT size in sectors.
4. Compute the root directory size for 16 entries.
5. Compute the boot sector's BPB fields: `BPB_BytsPerSec`, `BPB_SecPerClus`, `BPB_RsvdSecCnt`, `BPB_NumFATs`, `BPB_RootEntCnt`, `BPB_TotSec16` (or `BPB_TotSec32`), `BPB_FATSz16`.

Show the arithmetic. State your final BPB values as a `static const uint8_t bpb_1mb[]` array. Commit as `homework/problem-04-fat12-1mb.md`. ~1 hour.

---

## Problem 5 — Build and trace a USB throughput benchmark

Write a small variation of the Exercise 1 CDC echo firmware that:

1. Maintains a 1 MB pseudo-random buffer (use the linear-congruential generator from Week 8 Exercise 1).
2. On receiving any byte from the host, streams the entire 1 MB buffer out the bulk-IN endpoint as fast as it can.
3. After streaming, sends a marker line `--END (transferred X bytes in Y ms)\n` followed by `Y` (the firmware's `board_millis()` delta).

On the host, write a Python script that opens the CDC port, sends one byte, and reads bytes until it sees `--END`; print the actual measured throughput. Run it on three OSes if you have access; record the throughput on each.

Expected: 0.9 to 1.1 MB/s on a clean USB 2.0 host. Commit your firmware variant as `homework/problem-05-cdc-bench.c` and the Python script + results as `homework/problem-05-results.md`. ~1.5 hours.

---

## Problem 6 — TinyUSB callback discipline audit

Read the TinyUSB concurrency page at <https://docs.tinyusb.org/en/latest/reference/concurrency.html>. Then audit the following four (hypothetical) callback bodies and identify, for each, which TinyUSB callback rule it violates and what fix you would apply.

```c
/* A */
void tud_cdc_rx_cb(uint8_t itf) {
    uint8_t buf[256];
    uint32_t n = tud_cdc_n_read(itf, buf, sizeof buf);
    printf("[cdc] received %u bytes: ", (unsigned)n);
    for (uint32_t i = 0; i < n; ++i) {
        printf("%02X ", buf[i]);
    }
    printf("\n");
}

/* B */
uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t id,
                                hid_report_type_t type,
                                uint8_t * buffer, uint16_t reqlen) {
    xSemaphoreTake(report_mutex, portMAX_DELAY);
    memcpy(buffer, &current_report, sizeof current_report);
    xSemaphoreGive(report_mutex);
    return sizeof current_report;
}

/* C */
void tud_mount_cb(void) {
    for (int i = 0; i < 100000; ++i) {
        gpio_put(LED_PIN, true);
        sleep_us(50);
        gpio_put(LED_PIN, false);
        sleep_us(50);
    }
}

/* D */
void tud_cdc_rx_cb(uint8_t itf) {
    char log_buf[64];
    uint32_t n = tud_cdc_n_read(itf, (uint8_t *)log_buf, sizeof log_buf);
    /* echo straight back */
    tud_cdc_n_write(itf, log_buf, n);
    tud_cdc_n_write_flush(itf);
}
```

For each, write 1-3 sentences identifying the rule violated (block-in-callback, over-write, neither) and a one-line fix. Commit as `homework/problem-06-callback-audit.md`. ~30 minutes.

---

## Total time budget

| Problem | Time |
|--------:|-----:|
| 1       | 1.0h |
| 2       | 0.5h |
| 3       | 0.75h|
| 4       | 1.0h |
| 5       | 1.5h |
| 6       | 0.5h |
| Slack   | 0.75h|
| **Sum** | **6h** |
