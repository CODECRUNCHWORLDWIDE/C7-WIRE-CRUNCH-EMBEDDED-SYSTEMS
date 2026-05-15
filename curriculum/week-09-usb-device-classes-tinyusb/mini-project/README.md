# Mini-Project — Composite USB Device: CDC + HID + MSC

The Week 9 deliverable. One Raspberry Pi Pico enumerates as a single USB device with three logical functions: a CDC ACM virtual serial port for byte-stream I/O, a HID keyboard that types `code crunch` on the BOOTSEL button, and a Mass Storage Class device backing a 128 KB FAT12 RAM disk with a pre-populated `README.TXT`. All three coexist via Interface Association Descriptors. The mini-project ties together the three exercise pieces into one firmware target.

## Goal

Build a TinyUSB composite-device firmware that:

1. Enumerates cleanly on Linux (5.x or newer), macOS (12.x or newer), and Windows (10 21H2 or newer). No driver install required on any of the three.
2. Presents CDC, HID, and MSC interfaces simultaneously. All three drivers bind without manual user intervention.
3. Echoes bytes on the CDC interface (Exercise 1 behavior).
4. Types `code crunch` on the HID interface when the BOOTSEL button is pressed (Exercise 2 behavior).
5. Serves the 128 KB FAT12 RAM disk with a pre-populated `README.TXT` (Exercise 3 behavior).
6. Total firmware size under 64 KB compiled. CPU utilization below 20% with all three classes idle. CPU utilization below 50% under load (1 MB/s CDC traffic + simultaneous MSC read).

## Bench artifacts to commit

- `bench-captures/week09-enum-linux.pcapng` — full enumeration trace on Linux.
- `bench-captures/week09-enum-windows.png` — Device Manager screenshot showing all three interfaces.
- `bench-captures/week09-enum-macos.png` — System Information > USB screenshot.
- `bench-captures/week09-cdc-echo.png` — terminal showing 1 KB echo with no dropped bytes.
- `bench-captures/week09-hid-typing.png` — text editor screenshot showing `code crunchcode crunch...` from 5 BOOTSEL presses.
- `bench-captures/week09-msc-mount.png` — file manager screenshot showing the CRUNCH volume mounted with `README.TXT`.
- `USB-DESIGN-NOTE.md` — one-page write-up of the IAD decision and the `wTotalLength` arithmetic.

## Pass criteria

- Enumeration completes in under 300 ms on Linux, under 800 ms on Windows.
- CDC echo: 1024-byte burst, zero dropped bytes, round-trip latency under 5 ms.
- HID: button-to-keystroke latency under 8 ms (USB polling interval is 10 ms; statistical worst case is one polling slot).
- MSC: README.TXT readable; small (< 10 KB) file drag-and-drop succeeds; file persists across un-mount/re-mount within one power cycle.

## What the source files contain

| File                | Purpose                                                                          |
|---------------------|----------------------------------------------------------------------------------|
| `main.c`            | Entry point; TinyUSB init; the three class-service routines run from main loop.  |
| `usb_descriptors.c` | The composite-device descriptor tree (146 bytes), strings, HID report descriptor.|
| `tusb_config.h`     | Build-time switches: `CFG_TUD_CDC=1`, `CFG_TUD_HID=1`, `CFG_TUD_MSC=1`.          |
| `dma_adc_capture.h` | (Reused from Week 8 if you want optional ADC streaming alongside USB; not required for the pass criteria.) |

## Worked CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(week09_composite C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(week09_composite
    main.c
    usb_descriptors.c
)

target_include_directories(week09_composite PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(week09_composite
    pico_stdlib
    tinyusb_device
    tinyusb_board
    hardware_gpio
    hardware_sync
)

pico_add_extra_outputs(week09_composite)
```

Drop the four source files plus `pico_sdk_import.cmake` (from `<pico-sdk>/external/`) into a directory, run `cmake -B build && cmake --build build`, and you get `week09_composite.uf2` to flash.

## The composite descriptor at a glance

The configuration descriptor is 146 bytes, broken down as:

```text
Section                                Bytes  Cum.
-------                                -----  ----
Configuration descriptor                   9     9
IAD for CDC                                8    17
  Interface 0 (CDC.comm)                   9    26
    CDC Header                             5    31
    CDC Call Management                    5    36
    CDC ACM                                4    40
    CDC Union                              5    45
    Endpoint 0x81 (notify, interrupt-IN)   7    52
  Interface 1 (CDC.data)                   9    61
    Endpoint 0x02 (bulk-OUT)               7    68
    Endpoint 0x82 (bulk-IN)                7    75
IAD for HID                                8    83
  Interface 2 (HID)                        9    92
    HID class descriptor                   9   101
    Endpoint 0x83 (interrupt-IN)           7   108
    Endpoint 0x03 (interrupt-OUT)          7   115
IAD for MSC                                8   123
  Interface 3 (MSC)                        9   132
    Endpoint 0x84 (bulk-IN)                7   139
    Endpoint 0x04 (bulk-OUT)               7   146
```

`bNumInterfaces=4`, `wTotalLength=146`. The `_Static_assert` at the bottom of `usb_descriptors.c` validates this at compile time.

## Class-callback responsibilities at a glance

| Class | Callbacks you must implement                                              | Where they live    |
|------:|---------------------------------------------------------------------------|--------------------|
| CDC   | `tud_cdc_rx_cb` (optional), `tud_cdc_line_coding_cb` (optional)           | main.c             |
| HID   | `tud_hid_descriptor_report_cb`, `tud_hid_get_report_cb`, `tud_hid_set_report_cb` | main.c + usb_descriptors.c |
| MSC   | `tud_msc_inquiry_cb`, `tud_msc_test_unit_ready_cb`, `tud_msc_capacity_cb`, `tud_msc_read10_cb`, `tud_msc_write10_cb`, `tud_msc_scsi_cb`, `tud_msc_is_writable_cb` | main.c |
| Stack | `tud_descriptor_device_cb`, `tud_descriptor_configuration_cb`, `tud_descriptor_string_cb`, `tud_mount_cb`, `tud_umount_cb`, `tud_suspend_cb`, `tud_resume_cb` | usb_descriptors.c + main.c |

## The `USB-DESIGN-NOTE.md` template

Your `USB-DESIGN-NOTE.md` should be one page covering, in order:

1. **The IAD decision.** Why you used `(0xEF, 0x02, 0x01)` in the device-class triple. What fails on which host OSes if you do not. One paragraph.
2. **`wTotalLength` arithmetic.** The 146-byte computation, with the offset table above replicated. Confirm the `_Static_assert` passes at compile time.
3. **Endpoint allocation.** Which endpoint addresses each class claims, and why no two collide. (CDC uses EPs 1 (IN) and 2 (both); HID uses EP 3 (both); MSC uses EP 4 (both).)
4. **`tusb_config.h` settings.** The four key `CFG_TUD_*` constants you set, and why each one is necessary.
5. **Latency budget.** The button-to-keystroke and CDC-byte-to-byte latency expectations, with the polling-interval math.

Commit it alongside the firmware. This is the artifact your future self reads in Week 16 when you wire USB into the audio-streaming pipeline.

## Common pitfalls during integration

1. **The mini-project enumerates two classes but not the third.** Almost always a `CFG_TUD_*` count is wrong in `tusb_config.h`, or `wTotalLength` is off so the host stops parsing before reaching the third interface group. Fix: check both. The `_Static_assert` catches the second; only manual review catches the first.
2. **The CDC echo works but the HID keyboard "types nothing".** The host enumerated the HID interface fine, but TinyUSB's HID class driver has no `instance` mapping because `CFG_TUD_HID=0` was left at the default. Fix: set `CFG_TUD_HID=1` (and re-build, the build system caches header changes inconsistently sometimes).
3. **The MSC volume mounts but readonly.** `tud_msc_is_writable_cb` returns false (the default if not implemented). Fix: provide the callback returning true.
4. **The HID button press triggers but the first character is dropped.** The first call to `tud_hid_report` returns false because the previous report (or some other state) hasn't cleared yet. Fix: in `typing_service()`, do not advance the cursor if `tud_hid_ready()` is false; retry on the next main-loop tick.
5. **Power consumption higher than expected.** The main loop spins at full CPU instead of yielding to `__WFI` between TinyUSB ticks. Fix: add `__WFI()` in the main loop when nothing has work; the USB IRQ wakes the core when traffic arrives.

## Time budget

| Task                                       | Time |
|--------------------------------------------|-----:|
| Read the three exercises' SOLUTIONS.md     | 0.5h |
| Compose the composite descriptor           | 1.0h |
| Bring up CDC alone in the composite        | 0.5h |
| Add HID; verify keyboard works              | 0.5h |
| Add MSC; verify RAM disk mounts            | 0.5h |
| Bench-test on all three OSes               | 1.5h |
| Capture and label artifacts                | 0.5h |
| Write USB-DESIGN-NOTE.md                   | 0.5h |
| Slack                                      | 1.5h |
| **Total**                                  | **7h** |
