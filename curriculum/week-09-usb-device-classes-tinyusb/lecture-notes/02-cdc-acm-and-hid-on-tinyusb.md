# Lecture 2 — CDC ACM and HID on TinyUSB

> *Two classes, two very different ways of running. CDC sends bytes — your firmware writes to a FIFO and the host reads them as if they came from a serial port; the protocol does nothing interesting on top of the wire. HID sends structured reports — your firmware fills in a fixed-format struct and the host's HID parser decodes it against a report descriptor you authored at compile time; the protocol does a lot on top of the wire. Both feel about the same to write. They are not.*

This lecture covers two USB classes: Communications Device Class Abstract Control Model (CDC ACM, "the virtual serial port") and Human Interface Device (HID, "the keyboard / mouse / game controller / anything-with-buttons-and-knobs class"). Both run on TinyUSB. Both will be in this week's exercises and mini-project.

---

## 1. CDC ACM at a glance

CDC ACM is the class that USB-to-serial cables, USB-to-modem devices, and most "USB-enabled microcontroller dev boards" implement. The user-visible behavior is: a `/dev/cu.usbmodemXXXX` (macOS), `COM5` (Windows), `/dev/ttyACM0` (Linux) endpoint appears on the host when the device enumerates; opening that endpoint with a terminal emulator (`screen`, `picocom`, PuTTY, the Arduino IDE's serial monitor) opens a byte-pipe to the device.

CDC ACM is defined by:

- **USB CDC 1.2** — base CDC class definition. <https://www.usb.org/document-library/class-definitions-communication-devices-12>
- **USB PSTN 1.2** — Public Switched Telephone Network subclass spec that contains the ACM definition. <https://www.usb.org/document-library/class-definitions-pstn-devices-12>

The class has a lot of features (modem ringing notifications, V.250 AT-command auto-parsing, ISDN signaling) that nobody uses on microcontrollers. We use the byte-pipe subset. TinyUSB does the same.

### CDC's interface pair

A CDC ACM device declares *two* interfaces:

1. **Communication Class interface** — `bInterfaceClass=0x02, bInterfaceSubClass=0x02 (ACM), bInterfaceProtocol=0x00 (None)`. Contains the class-specific *functional descriptors* and one interrupt-IN endpoint used for "modem state change" notifications (line-coding changed, carrier detect, etc.). In practice the notification endpoint is rarely actually used; TinyUSB declares it because the spec requires it but never sends on it.
2. **CDC Data Class interface** — `bInterfaceClass=0x0A, bInterfaceSubClass=0x00, bInterfaceProtocol=0x00`. Contains a bulk-IN / bulk-OUT pair. *This* is where the actual byte stream flows.

The reason for the two-interface split is historical: the CDC spec was designed for telephony modems, where the "comm" interface carried out-of-band signaling (ring detect, off-hook) and the "data" interface carried the modem's payload. Modern microcontrollers don't use signaling, but the two-interface shape is mandatory.

### The four CDC functional descriptors

Inside the Communication Class interface, you must place four CDC functional descriptors before the endpoint descriptor. CDC 1.2 §5.2.3 (pp. 36–48). Each starts with `bFunctionLength` and `bDescriptorType=0x24 (CS_INTERFACE)` and is followed by a `bDescriptorSubtype` and class-specific bytes.

**Header functional descriptor (5 bytes, CDC 1.2 §5.2.3.1):**

```c
{ 0x05,        /* bFunctionLength */
  0x24,        /* bDescriptorType = CS_INTERFACE */
  0x00,        /* bDescriptorSubtype = Header */
  0x10, 0x01 } /* bcdCDC = 0x0110 (CDC 1.10) */
```

**Call Management functional descriptor (5 bytes, PSTN 1.2 §5.3.1):**

```c
{ 0x05,        /* bFunctionLength */
  0x24,        /* bDescriptorType = CS_INTERFACE */
  0x01,        /* bDescriptorSubtype = Call Management */
  0x00,        /* bmCapabilities = 0 (no call management; we are a byte pipe) */
  0x01 }       /* bDataInterface = 1 (the CDC Data interface's bInterfaceNumber) */
```

**Abstract Control Management functional descriptor (4 bytes, PSTN 1.2 §5.3.2):**

```c
{ 0x04,        /* bFunctionLength */
  0x24,        /* bDescriptorType = CS_INTERFACE */
  0x02,        /* bDescriptorSubtype = ACM */
  0x02 }       /* bmCapabilities: bit 1 = supports SET_LINE_CODING/SET_CONTROL_LINE_STATE/GET_LINE_CODING.
                  We claim this even though our firmware ignores the values; the host's serial-port
                  driver expects the device to handle these requests without stalling. */
```

**Union functional descriptor (5 bytes, CDC 1.2 §5.2.3.8):**

```c
{ 0x05,        /* bFunctionLength */
  0x24,        /* bDescriptorType = CS_INTERFACE */
  0x06,        /* bDescriptorSubtype = Union */
  0x00,        /* bMasterInterface = 0 (the CDC Comm interface's bInterfaceNumber) */
  0x01 }       /* bSlaveInterface0 = 1 (the CDC Data interface's bInterfaceNumber) */
```

The Union descriptor *binds* the Comm interface and the Data interface together as one logical function. Without it, hosts may treat the two interfaces as unrelated and the byte pipe will not open.

The total inside-the-Comm-interface is 5 + 5 + 4 + 5 = 19 bytes of functional descriptors plus one 7-byte endpoint descriptor for the notification endpoint, totaling 26 bytes. Plus the 9-byte interface descriptor itself, the Comm interface contributes 35 bytes to `wTotalLength`. Plus the 9-byte interface descriptor and the 2 × 7-byte endpoint descriptors for the Data interface, the Data interface contributes 23 bytes. Plus the 8-byte IAD wrapping them both: 35 + 23 + 8 = 66 bytes of CDC-section in the config descriptor.

### CDC's class-specific control requests

CDC ACM defines four class-specific control requests on endpoint zero (PSTN 1.2 §6.3, pp. 27–32):

| bRequest | Name                       | bmRequestType | wValue                       | wIndex          | wLength |
|---------:|----------------------------|---------------|------------------------------|-----------------|---------|
| 0x20     | SET_LINE_CODING            | 0x21          | 0                            | comm interface  | 7       |
| 0x21     | GET_LINE_CODING            | 0xA1          | 0                            | comm interface  | 7       |
| 0x22     | SET_CONTROL_LINE_STATE     | 0x21          | DTR\|RTS bits                | comm interface  | 0       |
| 0x23     | SEND_BREAK                 | 0x21          | break duration ms            | comm interface  | 0       |

The 7-byte line-coding structure is:

```c
typedef struct __attribute__((packed)) {
    uint32_t dwDTERate;     /* baud rate as uint32 little-endian (e.g. 115200) */
    uint8_t  bCharFormat;   /* 0=1 stop, 1=1.5 stop, 2=2 stop */
    uint8_t  bParityType;   /* 0=None, 1=Odd, 2=Even, 3=Mark, 4=Space */
    uint8_t  bDataBits;     /* 5, 6, 7, 8, 16 */
} cdc_line_coding_t;
```

These values are *not* enforced on the USB bus — USB does not run at "115200 baud", USB runs at 12 Mbit/s and the line-coding fields are a host-side legacy bridge for tools that ask. Your firmware can ignore the values entirely or record them as a hint. TinyUSB calls `tud_cdc_line_coding_cb(itf, line_coding)` when the host sends one; you can leave the callback empty.

`SET_CONTROL_LINE_STATE` sends DTR (bit 0) and RTS (bit 1) as a 2-bit field in `wValue`. Some host tools use DTR transitions to trigger a Pico reset (the canonical "1200-baud-DTR-touch" Arduino convention); TinyUSB's `tud_cdc_line_state_cb(itf, dtr, rts)` lets you observe it.

### CDC's runtime API (TinyUSB)

Once enumerated, you talk to the CDC pipe through six functions (TinyUSB `class/cdc/cdc_device.h`):

```c
uint32_t tud_cdc_available(void);
                          /* Bytes available to read from the host. */
uint32_t tud_cdc_read(void * buf, uint32_t bufsize);
                          /* Read up to bufsize bytes. Returns actual count. */
uint32_t tud_cdc_write(const void * buf, uint32_t bufsize);
                          /* Queue bytes for transmission to the host. Returns
                             actual count written to the internal FIFO. */
uint32_t tud_cdc_write_flush(void);
                          /* Force the internal FIFO to flush to the bus. */
uint32_t tud_cdc_write_available(void);
                          /* Free space in the internal FIFO. */
void     tud_cdc_rx_cb(uint8_t itf);
                          /* Callback invoked when bytes arrive from host;
                             implement this in your code, or leave empty
                             and poll tud_cdc_available() from a task. */
```

The internal FIFO sizes are set in `tusb_config.h` via `CFG_TUD_CDC_RX_BUFSIZE` and `CFG_TUD_CDC_TX_BUFSIZE` (typically 64 or 256 bytes). Writes that exceed `tud_cdc_write_available()` return less than the requested length; the unwritten bytes are silently dropped if you do not call again with the remainder.

The canonical CDC echo loop is six lines:

```c
if (tud_cdc_available()) {
    uint8_t buf[64];
    uint32_t n = tud_cdc_read(buf, sizeof buf);
    tud_cdc_write(buf, n);
    tud_cdc_write_flush();
}
```

Exercise 1 builds exactly this, then measures the round-trip latency from "byte sent by host" to "byte echoed back to host" — typically 1.5–3 ms on Linux, dominated by the host's USB scheduler, not by the firmware.

---

## 2. HID — the structured-report class

HID is the class for input devices. It is not literally restricted to keyboards and mice — HID's design treats input devices as "structured-report generators", and the host's HID stack parses arbitrary report formats. Game controllers, drawing tablets, temperature probes, even some industrial-instrument front panels — anything that produces a stream of structured small data — can use HID.

HID is defined by:

- **USB HID 1.11** — class spec. <https://www.usb.org/document-library/device-class-definition-hid-111>
- **USB HID Usage Tables 1.4** — the semantic dictionary for usage codes. <https://usb.org/document-library/hid-usage-tables-14>

The class is small (97 pages) but the report-descriptor bytecode is dense. The right way to learn HID report descriptors is to copy one from HID 1.11 Appendix E (which has worked examples for a boot keyboard and a boot mouse) and trace every byte.

### HID's interface

A HID device declares one interface: `bInterfaceClass=0x03, bInterfaceSubClass=0x00, bInterfaceProtocol=0x00` for a "non-boot" HID device (i.e., a device the OS HID stack parses fully, not one used by the BIOS pre-boot).

Inside the interface, before the endpoint descriptor, you place one HID class-specific descriptor (HID 1.11 §6.2.1, pp. 22–23):

```c
{ 0x09,        /* bLength */
  0x21,        /* bDescriptorType = HID */
  0x11, 0x01,  /* bcdHID = 0x0111 (HID 1.11) */
  0x00,        /* bCountryCode (0 = Not Localized) */
  0x01,        /* bNumDescriptors (one report descriptor follows) */
  0x22,        /* bDescriptorType of subordinate = REPORT (0x22) */
  REPORT_DESC_LEN_LO, REPORT_DESC_LEN_HI }
                /* wDescriptorLength: length of the report descriptor, little-endian.
                   The actual report descriptor is *not* in the configuration tree;
                   the host fetches it via GET_DESCRIPTOR(REPORT, ...) later. */
```

The HID interface then declares one or two endpoints:

- An interrupt-IN endpoint (mandatory) for the device to send input reports.
- An optional interrupt-OUT endpoint for the host to send output reports (keyboard LED state, for example) without using the control pipe.

Polling interval (`bInterval`) is typically 10 ms for a casual input device, 1 ms for a low-latency one.

### The report descriptor

A report descriptor is a sequence of *items*, each 1–5 bytes. The first byte of each item encodes the item's tag and type and the number of payload bytes (HID 1.11 §6.2.2.1, p. 24):

```text
First byte of an item:
  bits 7:4 = bTag       (e.g., 0x8 = Input, 0x9 = Output, 0xC = End Collection)
  bits 3:2 = bType      (00 = Main, 01 = Global, 10 = Local, 11 = Reserved)
  bits 1:0 = bSize      (00 = 0 bytes follow, 01 = 1, 10 = 2, 11 = 4)
```

So `0x05, 0x01` is a 2-byte item: tag=Usage Page (Global), payload "0x01". And `0x09, 0x06` is a 2-byte item: tag=Usage (Local), payload "0x06". Together, "Usage Page 0x01 = Generic Desktop, Usage 0x06 = Keyboard". The host's parser builds up state (the "current usage page", the "current logical min/max", etc.) and emits a "report format" when it sees an `Input`, `Output`, or `Feature` Main item.

### The boot-keyboard report descriptor

HID 1.11 Appendix E.6 (p. 69) lists the canonical 63-byte boot-keyboard report descriptor. Annotated:

```c
0x05, 0x01,        /* Usage Page (Generic Desktop)          */
0x09, 0x06,        /* Usage (Keyboard)                      */
0xA1, 0x01,        /* Collection (Application)              */
0x05, 0x07,        /*   Usage Page (Key Codes / Keyboard)   */
0x19, 0xE0,        /*   Usage Minimum (224 = Left Control)  */
0x29, 0xE7,        /*   Usage Maximum (231 = Right GUI)     */
0x15, 0x00,        /*   Logical Minimum (0)                 */
0x25, 0x01,        /*   Logical Maximum (1)                 */
0x75, 0x01,        /*   Report Size (1 bit)                 */
0x95, 0x08,        /*   Report Count (8)                    */
0x81, 0x02,        /*   Input (Data, Variable, Absolute) — modifier byte (8 bits) */
0x95, 0x01,        /*   Report Count (1)                    */
0x75, 0x08,        /*   Report Size (8 bits)                */
0x81, 0x01,        /*   Input (Constant) — reserved byte    */
0x95, 0x05,        /*   Report Count (5)                    */
0x75, 0x01,        /*   Report Size (1 bit)                 */
0x05, 0x08,        /*   Usage Page (LEDs)                   */
0x19, 0x01,        /*   Usage Minimum (1 = Num Lock)        */
0x29, 0x05,        /*   Usage Maximum (5 = Kana)            */
0x91, 0x02,        /*   Output (Data, Variable, Absolute) — 5 LED bits */
0x95, 0x01,        /*   Report Count (1)                    */
0x75, 0x03,        /*   Report Size (3 bits)                */
0x91, 0x01,        /*   Output (Constant) — 3 pad bits      */
0x95, 0x06,        /*   Report Count (6)                    */
0x75, 0x08,        /*   Report Size (8 bits)                */
0x15, 0x00,        /*   Logical Minimum (0)                 */
0x25, 0x65,        /*   Logical Maximum (101)               */
0x05, 0x07,        /*   Usage Page (Key Codes)              */
0x19, 0x00,        /*   Usage Minimum (0)                   */
0x29, 0x65,        /*   Usage Maximum (101)                 */
0x81, 0x00,        /*   Input (Data, Array) — 6 key slots   */
0xC0               /* End Collection                        */
```

The format this declares for an **input report** (device-to-host on the interrupt-IN endpoint) is:

```text
Byte 0:    modifier bits (8 bits, one per Ctrl/Shift/Alt/GUI, left+right = 8 keys)
Byte 1:    reserved (0)
Bytes 2-7: six key-code slots; 0 in a slot = "no key pressed in this slot"
                                non-0 = HID Usage Tables 1.4 Page 0x07 keycode
                                (0x04 = 'a', 0x05 = 'b', ..., 0x28 = Enter)
```

The format for an **output report** (host-to-device, e.g. Caps Lock LED) is:

```text
Byte 0: 5 LED bits (NumLock, CapsLock, ScrollLock, Compose, Kana) + 3 pad bits
```

Note that this report descriptor does NOT include a Report ID. When there is only one report per direction, the report bytes go straight on the interrupt endpoint without a leading ID. When there are multiple reports per direction (e.g., keyboard + mouse on the same HID interface), you add `0x85, ID` (Report ID Global item) at the top of each report's declaration, and the first byte of every report on the wire is the ID.

### The boot-mouse report descriptor

HID 1.11 Appendix E.10 (p. 75) lists the canonical 50-byte boot-mouse report descriptor. The format it declares is:

```text
Byte 0: buttons (8 bits: left, right, middle, ...)
Byte 1: dx (signed 8-bit relative X movement)
Byte 2: dy (signed 8-bit relative Y movement)
Byte 3: wheel (signed 8-bit relative scroll-wheel movement)
```

Mouse movements are *relative* — the device sends "I moved 3 pixels right and 1 pixel up since my last report"; the host's mouse driver accumulates the deltas into a screen-coordinate cursor position.

### HID's class-specific control requests

HID 1.11 §7.2 (pp. 51–55):

| bRequest | Name              | bmRequestType | wValue                                | wIndex          | wLength       |
|---------:|-------------------|---------------|---------------------------------------|-----------------|---------------|
| 0x01     | GET_REPORT        | 0xA1          | (report type) << 8 \| report ID       | HID interface   | report size   |
| 0x02     | GET_IDLE          | 0xA1          | 0 << 8 \| report ID                   | HID interface   | 1             |
| 0x03     | GET_PROTOCOL      | 0xA1          | 0                                     | HID interface   | 1             |
| 0x09     | SET_REPORT        | 0x21          | (report type) << 8 \| report ID       | HID interface   | report size   |
| 0x0A     | SET_IDLE          | 0x21          | (duration ms / 4) << 8 \| report ID   | HID interface   | 0             |
| 0x0B     | SET_PROTOCOL      | 0x21          | 0 (Boot) or 1 (Report)                | HID interface   | 0             |

Report types: 1 = Input, 2 = Output, 3 = Feature.

The host can use `GET_REPORT` to poll the device's input via the control pipe (slower than the interrupt endpoint but always available). It can use `SET_REPORT` to send an output report (e.g., LED state) via the control pipe instead of the interrupt-OUT endpoint.

`SET_IDLE` tells the device "don't send the same input report twice in a row faster than every N × 4 ms". A keyboard with `SET_IDLE(0)` sends a fresh report only when the key state changes; with `SET_IDLE(125)` it sends a report every 500 ms even with no key state change (so the host can detect a stuck key).

TinyUSB calls `tud_hid_get_report_cb(...)` for `GET_REPORT` and `tud_hid_set_report_cb(...)` for `SET_REPORT`. The other class requests are handled by TinyUSB internally.

### HID's runtime API (TinyUSB)

```c
bool tud_hid_ready(void);
                  /* Returns true when the device is mounted and the HID
                     endpoint is idle (ready to accept the next report). */
bool tud_hid_report(uint8_t report_id, const void * report, uint16_t len);
                  /* Send an input report. report_id = 0 when the descriptor
                     has no Report ID item; otherwise the ID byte is prepended
                     to the wire data. Returns true if queued, false if busy. */
uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t type,
                               uint8_t * buffer, uint16_t reqlen);
                  /* Called when host issues GET_REPORT via the control pipe.
                     Fill `buffer` with up to `reqlen` bytes; return actual size. */
void tud_hid_set_report_cb(uint8_t itf, uint8_t report_id, hid_report_type_t type,
                           const uint8_t * buffer, uint16_t bufsize);
                  /* Called when host issues SET_REPORT via the control pipe
                     (or sends an output report on the interrupt-OUT endpoint
                     if one is declared). For a keyboard, this is the LED state. */
```

Exercise 2 builds a keyboard that types `code crunch` on BOOTSEL press; the heart of the firmware is one call to `tud_hid_report(0, &keyboard_report, 8)` per key transition.

---

## 3. TinyUSB's callback discipline

TinyUSB calls your callbacks from its own task context (the thread that runs `tud_task()`). Two rules govern what you can do in a callback.

### Rule 1: Do not block

A callback that calls `vTaskDelay(100)`, `printf` (which blocks on the stdio FIFO), or any function that yields for more than a few microseconds, stalls the USB device entirely. The control-transfer state machine cannot progress; the host issues a timeout; the device falls off the bus.

Acceptable callback work:

- Read or write a small buffer (≤ FIFO size).
- Set a flag for the application task to act on.
- Increment a counter.
- Call `tud_*_write` of a few bytes when the FIFO has room.

Unacceptable callback work:

- Wait on a semaphore.
- Call `printf` to a UART or to stdio.
- Acquire a mutex held by another task.
- Do any computation that takes more than ~50 µs.

The mini-project's CDC echo callback satisfies Rule 1 by reading the host's bytes into a small local buffer, copying them to the TX FIFO, and returning. The HID keyboard callback satisfies Rule 1 by being empty (the keyboard is push-driven by the application task, not pull-driven by HID's control requests).

### Rule 2: Do not over-write

`tud_cdc_write(buf, len)` returns the actual count copied to the FIFO; the rest is dropped. `tud_hid_report(...)` returns false if the previous report is still queued; calling again drops the new one.

Acceptable patterns:

- `n = tud_cdc_write_available(); if (n >= len) tud_cdc_write(buf, len);`
- `while (! tud_hid_ready()) tud_task();` (in the application task, not in a callback)

Unacceptable patterns:

- Call `tud_cdc_write` in a tight loop without checking available space.
- Call `tud_hid_report` from a timer ISR without checking `tud_hid_ready()`.

Both rules are enforced by code review in this course, not by any compiler diagnostic. The TinyUSB docs page <https://docs.tinyusb.org/en/latest/reference/concurrency.html> states them clearly.

---

## 4. The `tusb_config.h` switches

TinyUSB reads a header file named `tusb_config.h` at compile time to know which classes are enabled, how many of each, what their endpoint numbers are, and how big the FIFOs are. Your project provides this file.

A minimal CDC-only `tusb_config.h`:

```c
#define CFG_TUSB_MCU            OPT_MCU_RP2040
#define CFG_TUSB_RHPORT0_MODE   (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)
#define CFG_TUSB_OS             OPT_OS_FREERTOS

#define CFG_TUD_ENDPOINT0_SIZE  64u

#define CFG_TUD_CDC             1u
#define CFG_TUD_CDC_RX_BUFSIZE  256u
#define CFG_TUD_CDC_TX_BUFSIZE  256u
#define CFG_TUD_CDC_EP_BUFSIZE  64u

#define CFG_TUD_HID             0u
#define CFG_TUD_MSC             0u
#define CFG_TUD_MIDI            0u
#define CFG_TUD_VENDOR          0u
```

For the composite mini-project, the switches are:

```c
#define CFG_TUD_CDC             1u
#define CFG_TUD_HID             1u
#define CFG_TUD_HID_EP_BUFSIZE  16u
#define CFG_TUD_MSC             1u
#define CFG_TUD_MSC_EP_BUFSIZE  512u
```

`CFG_TUD_HID` counts HID *interfaces*, not reports — a single HID interface that carries keyboard + mouse + consumer-control reports is still `CFG_TUD_HID=1`. `CFG_TUD_MSC_EP_BUFSIZE=512` matches the MSC block size (512 bytes is the SCSI block size for our RAM disk).

A wrong `tusb_config.h` is the second-most-common cause of "device enumerates but no class endpoints work" (the most common is a wrong `wTotalLength` in the configuration descriptor).

---

## 5. Worked example: CDC echo line count

The Exercise 1 CDC echo firmware:

```text
File                  Lines  Purpose
--------              -----  -------
main.c                ~120   tusb_init, FreeRTOS task, the 6-line echo loop
usb_descriptors.c     ~150   device + config + string descriptors
tusb_config.h         ~50    class enable + FIFO sizes
tud_descriptor_*_cb   inside usb_descriptors.c
--------              -----
Total                 ~320 lines of C plus ~80 bytes of descriptor data
```

That is the minimum-viable USB device. Two ~150-line files, plus a config header. Compare to a bare-metal USB stack (no TinyUSB), which is typically 5,000–15,000 lines depending on how many classes; TinyUSB earns its keep by handling the wire layer.

---

## 6. Where Lecture 3 goes

Lecture 3 covers MSC (the hardest of the three classes for a first-pass implementation — it has a protocol inside the protocol), the FAT12 RAM-disk pre-format, and the composite-device descriptor with IAD arithmetic. The HID and CDC pieces stand on their own; the composite descriptor in Lecture 3 ties all three together and shows you the byte-exact 161-byte configuration descriptor that the mini-project uses.

---

## Self-check

1. Why does CDC ACM declare two interfaces instead of one?
2. What does the Union functional descriptor do, and what fails if you omit it?
3. In the boot-keyboard report descriptor, what bytes of the input report are the modifier keys, and what bytes are the key-codes?
4. Your firmware calls `tud_cdc_write(buf, 100)` but `tud_cdc_write_available()` is currently 60. What happens to bytes 60..99?
5. What is the difference between `CFG_TUD_HID=1` with a single 5-report HID interface, and `CFG_TUD_HID=5` with one report per interface?
