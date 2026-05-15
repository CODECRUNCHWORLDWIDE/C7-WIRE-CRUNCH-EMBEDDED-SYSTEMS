# Challenge 2 — Custom HID Report Descriptor and a Vendor-Side Parser

The other side of HID. Exercise 2 copied the canonical boot-keyboard report descriptor from HID 1.11 Appendix E.6. This challenge has you author a fresh report descriptor from scratch for a custom input device: a 6-axis controller with three analog axes and three momentary buttons. You then write a Python host-side program that opens the HID device, parses the report descriptor it advertises (without hard-coded knowledge of your firmware), and prints decoded inputs.

This is the canonical "build it twice" exercise. The firmware side is C; the host side is Python with `hidapi`. The two halves must agree on the report format, but only via the report descriptor — neither side hard-codes byte offsets.

## Deliverables

1. Firmware (`firmware/challenge-02-custom-hid.c`) that enumerates as a HID device with one input report and one output report:
   - Input report (8 bytes total):
     - Byte 0: button state (3 bits used, 5 unused).
     - Bytes 1-2: X-axis position, signed 16-bit, little-endian, range -512..+511 (logical), -1.0..+1.0 (physical).
     - Bytes 3-4: Y-axis position, signed 16-bit, same range.
     - Bytes 5-6: Z-axis position, signed 16-bit, same range.
     - Byte 7: padding (reserved, 0).
   - Output report (1 byte):
     - Byte 0: LED mask (3 bits used for status LEDs; for this challenge, the GP25 LED follows bit 0).
2. Host-side Python program (`host/parse_custom_hid.py`) that:
   - Opens the device by VID/PID.
   - Calls `hid.device.get_indexed_string()` to read the product string.
   - Calls `hid.device.get_report_descriptor()` to fetch the raw bytes.
   - Decodes the report descriptor into a human-readable description (use the `hid_parser` Python package, or implement a minimal parser inline).
   - Reads input reports at 100 Hz and prints `Buttons: 0bABC  X: %f  Y: %f  Z: %f`.
3. A `challenge-02-postmortem.md` (one page) covering:
   - The exact 32 to ~50 bytes of your report descriptor, annotated.
   - The Logical Min/Max vs Physical Min/Max distinction for the analog axes.
   - One screenshot of the Python program's output as the firmware varies the axes (simulated; you do not need real analog inputs for this challenge — derive X/Y/Z from a free-running counter or a Lissajous pattern).

## Skeleton: the report descriptor (firmware side)

This is the byte sequence to start from. Annotate each item; verify the math.

```c
static const uint8_t hid_report_desc_6axis[] = {
    0x05, 0x01,        /* Usage Page (Generic Desktop)                  */
    0x09, 0x05,        /* Usage (Game Pad)                              */
    0xA1, 0x01,        /* Collection (Application)                      */
    /* --- Button byte ----------------------------------------------- */
    0x05, 0x09,        /*   Usage Page (Buttons)                        */
    0x19, 0x01,        /*   Usage Minimum (Button 1)                    */
    0x29, 0x03,        /*   Usage Maximum (Button 3)                    */
    0x15, 0x00,        /*   Logical Minimum (0)                         */
    0x25, 0x01,        /*   Logical Maximum (1)                         */
    0x75, 0x01,        /*   Report Size (1 bit)                         */
    0x95, 0x03,        /*   Report Count (3)                            */
    0x81, 0x02,        /*   Input (Data, Variable, Absolute)            */
    0x75, 0x05,        /*   Report Size (5 bits)                        */
    0x95, 0x01,        /*   Report Count (1)                            */
    0x81, 0x03,        /*   Input (Constant, Variable, Absolute) - pad  */
    /* --- X, Y, Z analog axes (signed 16-bit) ---------------------- */
    0x05, 0x01,        /*   Usage Page (Generic Desktop)                */
    0x09, 0x30,        /*   Usage (X)                                   */
    0x09, 0x31,        /*   Usage (Y)                                   */
    0x09, 0x32,        /*   Usage (Z)                                   */
    0x16, 0x00, 0xFE,  /*   Logical Minimum (-512), little-endian       */
    0x26, 0xFF, 0x01,  /*   Logical Maximum (+511)                      */
    0x36, 0x00, 0xFE,  /*   Physical Minimum (-512)                     */
    0x46, 0xFF, 0x01,  /*   Physical Maximum (+511)                     */
    0x75, 0x10,        /*   Report Size (16 bits)                       */
    0x95, 0x03,        /*   Report Count (3)                            */
    0x81, 0x02,        /*   Input (Data, Variable, Absolute)            */
    /* --- Padding byte --------------------------------------------- */
    0x75, 0x08,        /*   Report Size (8 bits)                        */
    0x95, 0x01,        /*   Report Count (1)                            */
    0x81, 0x03,        /*   Input (Constant, Variable, Absolute)        */
    /* --- LED output (3 bits + 5 pad bits) ------------------------- */
    0x05, 0x08,        /*   Usage Page (LEDs)                           */
    0x19, 0x01,        /*   Usage Minimum (1)                           */
    0x29, 0x03,        /*   Usage Maximum (3)                           */
    0x75, 0x01,        /*   Report Size (1 bit)                         */
    0x95, 0x03,        /*   Report Count (3)                            */
    0x91, 0x02,        /*   Output (Data, Variable, Absolute)           */
    0x75, 0x05,        /*   Report Size (5 bits)                        */
    0x95, 0x01,        /*   Report Count (1)                            */
    0x91, 0x03,        /*   Output (Constant)                           */
    0xC0               /* End Collection                                */
};
```

Total: 76 bytes. Verify: input report is `(3 bits buttons + 5 bits pad) + 3 × 16 bits axes + 8 bits pad = 8 + 48 + 8 = 64 bits = 8 bytes`, matching the spec above. Output report is `3 + 5 = 8 bits = 1 byte`.

## Skeleton: the host-side parser (Python)

```python
import hid

VID = 0x2E8A
PID = 0x000E  # course convention for Challenge 2

h = hid.device()
h.open(VID, PID)
h.set_nonblocking(False)

print("Product:", h.get_indexed_string(1))
print("Manufacturer:", h.get_indexed_string(2))

# hidapi does not have a built-in report-descriptor fetcher on every
# platform; on Linux you can read /sys/class/hidraw/hidrawN/device/report_descriptor
# directly, or use the hid_parser package's HidReportDescriptorParser.

from hid_parser import ReportDescriptor

with open('/sys/class/hidraw/hidraw0/device/report_descriptor', 'rb') as f:
    desc = f.read()

rd = ReportDescriptor(desc)
print(rd)  # pretty-print the parsed descriptor

# Read input reports at 100 Hz.
import time, struct
while True:
    data = h.read(8, timeout_ms=100)
    if not data:
        continue
    buttons = data[0] & 0x07
    x = struct.unpack('<h', bytes(data[1:3]))[0]
    y = struct.unpack('<h', bytes(data[3:5]))[0]
    z = struct.unpack('<h', bytes(data[5:7]))[0]
    print(f"Buttons: {buttons:03b}  X: {x/512:+.3f}  Y: {y/512:+.3f}  Z: {z/512:+.3f}")
    time.sleep(0.01)
```

Install `hidapi` and `hid_parser`:

```bash
pip install hidapi hid_parser
```

On Linux you may need to add a udev rule so `hidraw` is accessible to your user:

```text
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="2e8a", ATTRS{idProduct}=="000e", MODE="0666"
```

## Bench test

1. Flash the firmware. Plug in the Pico.
2. On the host, run `python parse_custom_hid.py`.
3. Verify the parser pretty-prints your report descriptor with 76 bytes total, ending in `End Collection`.
4. Verify the input-report read loop produces sane numbers: with the firmware emitting a Lissajous (X = sin(2πt/T), Y = cos(2πt/T), Z = sin(2π·3t/T)), the printed values should trace a closed curve every T seconds.
5. Send an output report from Python (`h.write([0x00, 0b101])`); verify the GP25 LED follows bit 0.

## Pass criterion

The Python parser successfully decodes the descriptor without hand-edited byte offsets, and the live read loop produces the expected Lissajous figures (verifiable by piping the output to `matplotlib` for a quick scatter plot).

## Notes

- This challenge is intentionally open-ended on the firmware side. You can drive the axes from real analog inputs (a thumbstick wired to GP26/GP27 ADC pins, plus a third axis from a slide pot on GP28); from the on-board accelerometer of a Pico-based dev board if you have one; or from a math function of `board_millis()`. Any of the three earns full credit. The grading is on whether the host-side parser correctly decodes your descriptor; that test is independent of the data source.
- The Logical Min/Max vs Physical Min/Max distinction matters for HID-aware tools (e.g., the Linux input subsystem). For our use here Logical and Physical can be the same; if you set them differently, the host's HID stack will linearly scale the logical value to the physical range when reporting to user-space.
- The HID Usage Tables 1.4 (Page 0x09 = Buttons) does not have semantic names for buttons; "Button 1", "Button 2", etc. are the canonical labels. If you want named buttons (like "Trigger", "Thumb"), use the Generic Desktop usages 0x90 through 0xB0.

## Suggested time budget

| Task                                            | Time |
|-------------------------------------------------|-----:|
| Author the report descriptor; build firmware    | 1.5h |
| Bring up the firmware; verify enumeration       | 0.5h |
| Install hidapi/hid_parser; first script run     | 0.5h |
| Wire the input-report read loop                 | 0.5h |
| Write the post-mortem                           | 0.5h |
| **Total**                                       | 3.5h |

This is the longest of the two challenges. Plan an evening.
