# Exercises — Solutions and Walkthroughs

This file walks each exercise, what the expected behavior is, what the bench-side measurements should show, and the canonical bugs that show up the first time a student writes USB device-side code. Read it after attempting each exercise, not before.

---

## Exercise 1 — CDC ACM Echo

### What the firmware does

`exercise-01-cdc-echo.c` brings up a TinyUSB CDC ACM device. After enumeration (CDC ACM with one IAD, two interfaces, three endpoints — one notify interrupt-IN, two bulk for data), the firmware's main loop calls `tud_task()` to service USB and `cdc_echo_service()` to read available bytes and write them straight back.

### Expected enumeration

Plug the Pico into your host. Within 200–400 ms:

- **Linux**: `dmesg | tail` shows roughly:
  ```text
  usb 1-2: new full-speed USB device number 7 using xhci_hcd
  usb 1-2: New USB device found, idVendor=2e8a, idProduct=000a, bcdDevice= 1.00
  usb 1-2: New USB device strings: Mfr=1, Product=2, SerialNumber=3
  usb 1-2: Product: C7 Wire CDC Echo
  usb 1-2: Manufacturer: Code Crunch Worldwide
  cdc_acm 1-2:1.0: ttyACM0: USB ACM device
  ```
- **macOS**: `ls /dev/cu.*` shows a new `/dev/cu.usbmodemXXXX` entry.
- **Windows**: Device Manager shows a new "USB Serial Device (COMx)" under Ports.

### Test procedure

1. Open the CDC port with a terminal emulator.
   - Linux: `picocom /dev/ttyACM0` (any baud).
   - macOS: `screen /dev/cu.usbmodemXXXX 115200`.
   - Windows: PuTTY on the assigned COM port.
2. Type `hello`, press Enter.
3. The terminal should display `hello` followed by a newline. The echo is character-by-character; on `picocom` with line-buffering off you see the characters echo as you type.
4. Type a longer string (50 ASCII characters). Confirm the entire string echoes back without dropped bytes.

### Common bugs

1. **`wTotalLength` off by one.** Symptom: device enumerates partially; `lsusb -v` shows the device descriptor but the configuration descriptor truncates mid-tree. Fix: add a `_Static_assert(sizeof desc_config == DESC_LEN_CDC_ONLY_CONFIG, ...)` (we already have one in the exercise).
2. **Missing Union functional descriptor.** Symptom: the device enumerates, the bulk endpoints exist, but the terminal emulator's connect fails with "no such device" because the host's CDC driver did not bind the comm and data interfaces. Fix: include the 5-byte Union descriptor inside the comm interface.
3. **Endpoint address direction-bit wrong.** Symptom: `lsusb -v` shows the endpoints with reversed directions; the terminal emulator opens but writes hang. Fix: bulk-OUT is `0x02`, bulk-IN is `0x82` — bit 7 set means IN.
4. **`CFG_TUD_CDC_RX_BUFSIZE` too small.** Symptom: pasting a long string into the terminal drops bytes after the first 64. Fix: set `CFG_TUD_CDC_RX_BUFSIZE=256` in `tusb_config.h`.
5. **`tud_task()` not called often enough.** Symptom: echo works but with hundreds-of-milliseconds latency. Fix: call `tud_task()` every iteration of the main loop, not behind any blocking call.

### Expected throughput

A round-trip echo of 1024 bytes at the application layer takes ~3 ms on Linux, ~5 ms on Windows, ~3.5 ms on macOS. The variance is the host-side USB scheduler's grain, not the firmware.

---

## Exercise 2 — HID Keyboard

### What the firmware does

`exercise-02-hid-keyboard.c` brings up a TinyUSB HID device with one interface and two endpoints (interrupt-IN for input reports, interrupt-OUT for the LED state output reports). The report descriptor is the canonical 63-byte boot-keyboard descriptor from HID 1.11 Appendix E.6.

On BOOTSEL button rising edge, the firmware enters "typing mode" and emits a sequence of 8-byte input reports — alternating key-down (with one keycode in slot 2) and key-up (all zeros) — for the string `code crunch`. Each report takes one `KEYSTROKE_DELAY_MS=30` ms tick; the full string takes ~660 ms.

### Expected enumeration

- **Linux**: `dmesg | tail`:
  ```text
  usb 1-2: New USB device found, idVendor=2e8a, idProduct=000b
  input: Code Crunch Worldwide C7 Wire HID Keyboard as /devices/.../input/inputN
  hid-generic 0003:2E8A:000B.0001: input,hidraw0: USB HID v1.11 Keyboard
  ```
- **macOS**: "Keyboard Setup Assistant" may pop up asking to identify the keyboard layout. Cancel it — our keycodes are layout-independent at the HID Usage Tables level; the host's OS layer applies whatever layout the user has selected.
- **Windows**: Device Manager shows the device under "Keyboards" and "Human Interface Devices".

### Test procedure

1. After enumeration, open a text editor on the host (TextEdit, Notepad, gedit).
2. Focus the editor.
3. Press BOOTSEL. Within 30 ms the editor begins receiving keystrokes; over the next 660 ms the string `code crunch` appears.
4. Repeat 5 times. The editor should show `code crunchcode crunchcode crunchcode crunchcode crunch` with no missing characters.

### Common bugs

1. **Report descriptor byte count mismatched to `wDescriptorLength`.** Symptom: device enumerates, host fetches the report descriptor, parses the wrong number of bytes, and the resulting "report format" is junk. The keyboard then never receives input. Fix: ensure `HID_REPORT_DESC_LEN = sizeof hid_report_desc_kbd` and embed that into the HID class descriptor's `wDescriptorLength` field.
2. **Sending a report before `tud_hid_ready()` returns true.** Symptom: `tud_hid_report` returns false and the report is dropped. Fix: check `tud_hid_ready()` first; if false, defer the call to the next main loop tick.
3. **Sending only the key-down report, no key-up.** Symptom: the host sees "key `c` pressed and held forever"; the second `code crunch` press never registers because the host already has `c` down. Fix: always follow a key-down report with an all-zeros key-up report.
4. **Polling interval too long.** Symptom: button-to-keystroke latency exceeds 50 ms. Fix: `CC_HID_POLL_INTERVAL_MS = 10` gives 10 ms polling, average wait 5 ms, max 10 ms. Polling at 1 ms is reserved for game controllers and high-speed input devices.
5. **`bInterfaceSubClass=0x01, bInterfaceProtocol=0x01` (boot keyboard) but the host's HID stack defaults to report protocol.** Symptom: the host expects the boot-protocol report format (always 8 bytes, no report descriptor) but our descriptor declares the report-protocol format. Fix: use `bInterfaceSubClass=0x00, bInterfaceProtocol=0x00` (non-boot HID) — what we did.

### Expected latency

Measure on a Saleae 8 with two probes: one on BOOTSEL (via the Pico's QSPI CS test point), one on the host's USB sniffer. The button-edge-to-first-report-on-the-bus latency should be < 10 ms (one polling interval). The first-report-to-character-in-editor latency is < 5 ms on most hosts.

---

## Exercise 3 — MSC RAM Disk

### What the firmware does

`exercise-03-msc-ramdisk.c` brings up a TinyUSB MSC device with one interface and a bulk-IN / bulk-OUT pair. The backing store is a 128 KB `uint8_t s_ramdisk[]` array initialized at boot to a FAT12 filesystem layout with one file (`README.TXT`) present in the root directory.

The TinyUSB MSC class driver handles the CBW/CSW state machine and the SCSI command dispatch. Our callbacks implement six SCSI commands:

- INQUIRY → return vendor/product/revision strings.
- TEST_UNIT_READY → return ready when enumeration is complete.
- READ_CAPACITY → return 256 blocks of 512 bytes.
- READ_10 → memcpy from `s_ramdisk` into the host's buffer.
- WRITE_10 → memcpy from the host's buffer into `s_ramdisk`.
- All other SCSI opcodes fall through `tud_msc_scsi_cb` and return -1.

### Expected enumeration

- **macOS**: a "CRUNCH" volume mounts at `/Volumes/CRUNCH`. Finder displays a folder containing one file.
- **Linux**: `udisks` auto-mounts the volume at `/media/<user>/CRUNCH` if your distro auto-mounts removable USB; otherwise `lsblk` shows a new `/dev/sdX1` device. `mount /dev/sdb1 /mnt/crunch` mounts it manually.
- **Windows**: A new drive letter (typically E: or higher) appears in File Explorer labeled "CRUNCH (E:)".

### Test procedure

1. After enumeration, open the CRUNCH volume in the host's file manager.
2. Verify `README.TXT` is present and contains the canonical text.
3. Create a small text file (< 1 KB) and drag it onto the CRUNCH volume.
4. Verify the file appears in the volume's listing.
5. Eject the volume (right-click → Eject; macOS: drag to trash; Linux: `umount`), wait 5 seconds, replug the Pico.
6. Verify CRUNCH re-mounts and that the file you added in step 3 is *not* present — the RAM disk is volatile, content reverts to the pre-formatted layout on power-cycle.
7. Repeat step 3 (re-add a file), then re-mount *without* power-cycling. Verify the file persists.

### Common bugs

1. **SCSI big-endian mistake on READ_CAPACITY response.** Symptom: host shows a 64 GB drive that is empty; trying to mount fails because the FAT12 layout is at byte 0..512 of a 64 GB image. Fix: SCSI multi-byte fields are big-endian; encode the last-LBA and block-size most-significant-byte-first.
2. **FAT12 entry packing wrong for entry 2.** Symptom: volume mounts but the README.TXT file shows zero bytes. The host follows the FAT chain from cluster 2 and immediately hits "free cluster" (the packing was wrong; entry 2's value was zero instead of EOC). Fix: bytes 3..5 of the FAT must be `{ 0xFF, 0x0F, 0x00 }` for entry 2 = EOC + entry 3 = free.
3. **README size in the directory entry is the block size (512) instead of the actual content length.** Symptom: opening README.TXT shows the correct text followed by 400 bytes of garbage (or zeros, since the rest of the sector is zero). Fix: `DIR_FileSize` is the byte-accurate file length, not the rounded-up cluster size.
4. **Disk not initialized in `tud_msc_test_unit_ready_cb`.** Symptom: the host's MSC driver issues TEST_UNIT_READY repeatedly and never gets a success response; the volume is in the host's "not yet ready" state and never mounts. Fix: `tud_msc_test_unit_ready_cb` must return true once enumeration is complete (we use the `s_mounted` flag).
5. **`CFG_TUD_MSC_EP_BUFSIZE=64` instead of `512`.** Symptom: enumeration completes but every read/write transfer is split into 8 sub-transfers; transfer throughput drops by 8x. Fix: set the EP buffer size to match the block size.

### Expected throughput

- macOS: sequential read of a 64 KB file from the RAM disk: ~1.0 MB/s.
- Linux: same: ~1.1 MB/s.
- Windows: same: ~0.9 MB/s.
The throughput is host-limited; full-speed USB caps at 1.2 MB/s in practice. Random access (single 512-byte block reads) is ~600 KB/s.

---

## Diagnosing failed enumeration

When a device fails to enumerate, run through this checklist in order:

1. **Capture the USB packets.** USBPcap on Windows, `usbmon` on Linux, Apple USB Prober on macOS. Find the failed transaction — it will show a STALL response or a timeout.
2. **Look at the device descriptor request.** If the SETUP packet for `GET_DESCRIPTOR(Device, 8 bytes)` arrives but the device's response is shorter than 8 bytes, your `desc_device` array is malformed.
3. **Look at the configuration descriptor request.** If the first `GET_DESCRIPTOR(Configuration, 9 bytes)` succeeds but the second `GET_DESCRIPTOR(Configuration, wTotalLength)` returns truncated data, `wTotalLength` is larger than your actual descriptor array.
4. **Look at the SET_CONFIGURATION request.** If `SET_CONFIGURATION(1)` is the last successful transaction and the host doesn't issue any further class-specific requests, `tud_mount_cb()` was likely never called — check that `s_mounted` is set true.
5. **Check `tusb_config.h`.** If `CFG_TUD_CDC=0` but your config descriptor declares a CDC interface, TinyUSB's link-time setup may be inconsistent. The build should fail; if it doesn't, the runtime fails silently.

Five steps. The capture is the load-bearing artifact; once you have it, the bug is almost always obvious in 30 seconds.

---

## Bench-test cheat sheet

| Exercise | Bench artifact                                                              |
|---------:|-----------------------------------------------------------------------------|
| 1        | `picocom`/PuTTY/screen log showing 1 KB echo with zero dropped bytes        |
| 2        | TextEdit screenshot showing `code crunch` × 5 with all 60 chars present     |
| 3        | Finder/Explorer screenshot showing CRUNCH volume mounted with README.TXT    |

Commit one screenshot per exercise to `bench-captures/` in your repo. If the screenshot looks right, the exercise passes.
