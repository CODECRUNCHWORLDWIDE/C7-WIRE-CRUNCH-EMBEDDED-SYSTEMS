# Lecture 1 — USB Enumeration and the Four Core Descriptors

> *Plug in the cable. For the next 200 milliseconds the host and the device have a structured conversation, eight bytes at a time, on a single bidirectional pipe called endpoint zero. The host asks five canonical questions; the device gives five canonical answers. At the end of that conversation either the device works — the host's class drivers load against your declared interfaces, your endpoints become available, your application code starts running — or the device fails to enumerate and the host's USB stack moves on. Everything that follows in Week 9 lives on the success branch.*

This lecture covers the lower layer of USB device work: the enumeration sequence and the descriptor formats it consumes. The next two lectures cover the upper layer: the class-specific protocols that run after enumeration succeeds. Read this one twice; the vocabulary is reused every day this week.

---

## 1. The six device states

USB 2.0 §9.1.1 (pp. 239–243) defines six states a USB device passes through, in this order, between cable insert and "ready to do useful work":

1. **Attached** — the device is connected to a port but VBUS is not necessarily present. On a real bus this state is transient (microseconds); on the RP2040 you observe it via the `VBUS_DETECT` pin if you wired it.
2. **Powered** — VBUS is present (the host has applied 5 V to the port) and the device has enough power to run its USB controller. The RP2040 detects this automatically; TinyUSB's `tud_init()` brings the controller up to this state.
3. **Default** — the host has issued a bus reset (held the differential lines in SE0 — both D+ and D- at logic low — for at least 10 ms; USB 2.0 §7.1.7.5, pp. 152–153). After reset the device responds to control transfers on endpoint zero at the default address 0. Only one device on the bus can be in Default state at a time; the host enumerates devices one at a time.
4. **Address** — the host has issued `SET_ADDRESS(N)` with N in the range 1..127. The device now responds to control transfers on its assigned address. The device is *not* yet usable for class traffic; only endpoint zero responds.
5. **Configured** — the host has issued `SET_CONFIGURATION(1)` (or whichever configuration number applies). The device's non-zero endpoints are now active and class drivers can use them.
6. **Suspended** — the bus has been idle for more than 3 ms; the host has signaled suspend; the device must reduce power consumption to ≤ 2.5 mA (bus-powered devices) within 10 ms (USB 2.0 §7.2.3, pp. 178–180). Resume is signaled by either end; on resume the device returns to its prior non-suspended state.

The state machine is fully deterministic on the device side. TinyUSB tracks it for you and calls `tud_mount_cb()` when you reach Configured, `tud_umount_cb()` on bus disconnect, `tud_suspend_cb(remote_wakeup_en)` on Suspended entry, `tud_resume_cb()` on Suspended exit.

### Where things go wrong

The vast majority of "device does not enumerate" symptoms resolve to the device never getting past Default state. The host issues `GET_DESCRIPTOR(Device)` against address 0, the device returns malformed bytes (wrong `bLength`, wrong `bDescriptorType`, `wTotalLength` shorter than the actual descriptor), the host retries 3–5 times per USB 2.0 §8.4.5 (p. 213), and then gives up. Symptom on the host: "USB device descriptor request failed" (Windows), `kernel: usb 1-2: device descriptor read/64, error -71` (Linux), the "USB Notification > Device cannot be used" badge (macOS).

We will reproduce this failure deliberately in Challenge 1 and walk the Wireshark capture.

---

## 2. Endpoint zero and the control transfer

Every USB device has endpoint zero. It is bidirectional (the only bidirectional endpoint on a device), it uses the *Control* transfer type, and it is the only endpoint guaranteed to exist before configuration. USB 2.0 §5.5 (pp. 35–43) defines control transfers; §8.5.3 (pp. 220–224) defines their three-stage protocol.

### The 8-byte SETUP packet

Every control transfer starts with an 8-byte SETUP packet from host to device:

```text
Offset  Size  Field            Meaning
------  ----  ---------------  ---------------------------------------------------
0       1     bmRequestType    Bit-field: direction, type, recipient (see below)
1       1     bRequest         The request code (0..11 for standard, class-defined otherwise)
2       2     wValue           Request-specific (little-endian)
4       2     wIndex           Request-specific (little-endian)
6       2     wLength          Number of bytes in the DATA stage (little-endian)
```

`bmRequestType` is a bit-field (USB 2.0 §9.3, p. 248):

- bit 7: direction (0 = host-to-device, 1 = device-to-host)
- bits 6:5: type (00 = standard, 01 = class, 10 = vendor, 11 = reserved)
- bits 4:0: recipient (00000 = device, 00001 = interface, 00010 = endpoint, 00011 = other)

So `0x80` is "device-to-host, standard, recipient=device" — used by `GET_DESCRIPTOR`. `0x21` is "host-to-device, class, recipient=interface" — used by HID's `SET_REPORT` and CDC's `SET_LINE_CODING`.

### The three stages

1. **SETUP stage** — host sends the 8-byte packet above. Device hardware ACKs it; TinyUSB's `tud_control_xfer_cb()` mechanism fires (you only intercept this for vendor requests; standard and class are handled by TinyUSB's tables).
2. **DATA stage** (optional) — `wLength` bytes flow in the direction indicated by bit 7 of `bmRequestType`. The packet boundary is `bMaxPacketSize0` from the device descriptor (8 / 16 / 32 / 64 bytes for full-speed); a request with `wLength=18` and `bMaxPacketSize0=8` produces three data packets of 8, 8, and 2 bytes.
3. **STATUS stage** — a zero-length packet in the opposite direction of the DATA stage, acknowledging completion. If the DATA stage was device-to-host, STATUS is host-to-device (host sends ZLP); if the DATA stage was host-to-device or absent, STATUS is device-to-host (device sends ZLP). The device's ZLP in the device-to-host case is its commitment to "I have applied the change you requested" — `SET_ADDRESS` is the canonical example: the device must keep responding at address 0 *until* it has sent the STATUS ZLP, *then* switch to the new address.

The STATUS stage's direction-flip is the canonical thing students get wrong when they hand-roll a USB stack. TinyUSB handles it.

### The eight standard requests

USB 2.0 Table 9-4 (p. 251) defines eleven standard request codes (`bRequest`), of which the eight commonly used are:

| bRequest | Name              | bmRequestType | wValue                           | wIndex          | wLength          |
|---------:|-------------------|---------------|----------------------------------|-----------------|------------------|
| 0        | GET_STATUS        | 0x80/81/82    | 0                                | recipient ID    | 2                |
| 1        | CLEAR_FEATURE     | 0x00/01/02    | feature selector                 | recipient ID    | 0                |
| 3        | SET_FEATURE       | 0x00/01/02    | feature selector                 | recipient ID    | 0                |
| 5        | SET_ADDRESS       | 0x00          | new device address               | 0               | 0                |
| 6        | GET_DESCRIPTOR    | 0x80          | (descriptor type) << 8 \| index  | language ID     | descriptor size  |
| 7        | SET_DESCRIPTOR    | 0x00          | (descriptor type) << 8 \| index  | language ID     | descriptor size  |
| 8        | GET_CONFIGURATION | 0x80          | 0                                | 0               | 1                |
| 9        | SET_CONFIGURATION | 0x00          | configuration value              | 0               | 0                |

`GET_DESCRIPTOR` is the one you encounter most. `wValue` packs the descriptor type in the high byte and the index in the low byte; e.g., `wValue=0x0100` is "Device descriptor, index 0" (the only device descriptor), `wValue=0x0200` is "Configuration descriptor, index 0", `wValue=0x0303` is "String descriptor, index 3", and `wValue=0x2200` is "HID Report descriptor, index 0" (HID-class-specific, but reachable via the standard `GET_DESCRIPTOR`).

---

## 3. The enumeration sequence in detail

When you plug a USB device into a host, the host's enumeration code runs the following sequence. The timings are typical (Linux 5.x; Windows and macOS differ by tens of milliseconds in places).

```text
t=0 ms     VBUS rises; device powers on; D+ pulled up via internal 1.5kΩ
           (full-speed signaling — RP2040 USB controller does this automatically)

t=20 ms    Host detects D+ pull-up; waits 100 ms for device to stabilize (USB 2.0 §7.1.7.3)

t=120 ms   Host issues bus reset (SE0 for ≥ 10 ms)

t=140 ms   Device is now in Default state, listening at address 0 on endpoint zero

t=145 ms   Host: SETUP { 0x80, 0x06, 0x0100, 0x0000, 0x0008 } — GET_DESCRIPTOR(Device, first 8 bytes)
           Host wants to know bMaxPacketSize0 before reading the full descriptor

t=146 ms   Device: DATA { 0x12, 0x01, 0x00, 0x02, 0xEF, 0x02, 0x01, 0x40 }
           (bLength=18, bDescriptorType=1, bcdUSB=0x0200, bDeviceClass=0xEF, ..., bMaxPacketSize0=64)
           Host: STATUS (ZLP host-to-device)

t=148 ms   Host issues second bus reset (some hosts; Linux does, Windows does not)

t=160 ms   Host: SETUP { 0x00, 0x05, 0x0001, 0x0000, 0x0000 } — SET_ADDRESS(1)
           Device: STATUS (ZLP device-to-host), then switches to address 1

t=162 ms   Host: SETUP { 0x80, 0x06, 0x0100, 0x0000, 0x0012 } — GET_DESCRIPTOR(Device, 18 bytes)
           (now at address 1)
           Device: DATA { ... full 18 bytes ... }
           Host: STATUS (ZLP)

t=165 ms   Host: SETUP { 0x80, 0x06, 0x0200, 0x0000, 0x0009 } — GET_DESCRIPTOR(Config, 9 bytes)
           Device returns just the 9-byte config header so host can read wTotalLength

t=167 ms   Host: SETUP { 0x80, 0x06, 0x0200, 0x0000, 0x00A1 } — GET_DESCRIPTOR(Config, wTotalLength=161)
           Device returns the full descriptor tree in three or four data packets

t=175 ms   Host: SETUP { 0x80, 0x06, 0x0300, 0x0000, 0x00FF } — GET_DESCRIPTOR(String, index 0)
           Device returns the language ID array (typically just { 0x04, 0x03, 0x09, 0x04 }
           = length=4, type=3, langID=0x0409 (US English))

t=177 ms   Host iterates: GET_DESCRIPTOR(String, index N) for each non-zero iManufacturer,
           iProduct, iSerialNumber referenced in the device and configuration descriptors

t=190 ms   Host: SETUP { 0x00, 0x09, 0x0001, 0x0000, 0x0000 } — SET_CONFIGURATION(1)
           Device: STATUS (ZLP), enters Configured state, calls tud_mount_cb()
           Non-zero endpoints become active

t=200 ms   Host's class drivers load: usbser.sys binds to the CDC interfaces,
           HIDClass.sys binds to the HID interface, USBSTOR.sys binds to the MSC interface
           User-visible devices appear in /dev or COM-port list or Device Manager
```

Total wall-clock time from plug-in to user-visible: typically 200–400 ms on Linux, 400–800 ms on Windows (the inbox-driver lookup takes longer), 300–500 ms on macOS.

### The eight transactions you must serve correctly

To pass enumeration you must serve correctly:

1. `GET_DESCRIPTOR(Device, 8 bytes)` — return the first 8 bytes of your 18-byte device descriptor.
2. `SET_ADDRESS` — accept and switch.
3. `GET_DESCRIPTOR(Device, 18 bytes)` — return the full device descriptor.
4. `GET_DESCRIPTOR(Configuration, 9 bytes)` — return the 9-byte config header.
5. `GET_DESCRIPTOR(Configuration, wTotalLength)` — return the entire concatenated descriptor tree.
6. `GET_DESCRIPTOR(String, 0)` — return the language ID array.
7. `GET_DESCRIPTOR(String, N)` for each referenced index — return a UTF-16LE string descriptor.
8. `SET_CONFIGURATION(1)` — accept and switch to Configured.

TinyUSB serves transactions 1, 2, 3, 4, 5, 6 from the byte arrays you provide in `tud_descriptor_device_cb()`, `tud_descriptor_configuration_cb()`, and `tud_descriptor_string_cb()`. Transactions 7 and 8 it serves from internal tables driven by your `tusb_config.h`. Your job is to make those byte arrays correct.

---

## 4. The device descriptor — 18 bytes

USB 2.0 §9.6.1, Table 9-8 (pp. 261–263). The 18-byte device descriptor is the host's first impression of you.

```c
typedef struct __attribute__((packed)) {
    uint8_t  bLength;            /* 18, always */
    uint8_t  bDescriptorType;    /* 0x01 = DEVICE */
    uint16_t bcdUSB;             /* 0x0200 for USB 2.0, 0x0110 for 1.1 */
    uint8_t  bDeviceClass;       /* 0x00 = "look at interface descriptors"
                                    0xEF = "look at IADs"
                                    0x02 = CDC (single-class device)
                                    0x09 = Hub
                                    others per USB-IF assignment */
    uint8_t  bDeviceSubClass;    /* depends on bDeviceClass */
    uint8_t  bDeviceProtocol;    /* depends on bDeviceClass */
    uint8_t  bMaxPacketSize0;    /* 8, 16, 32, or 64 (full-speed); 64 (high-speed) */
    uint16_t idVendor;           /* assigned by USB-IF; 0x2E8A for Raspberry Pi */
    uint16_t idProduct;          /* assigned by vendor */
    uint16_t bcdDevice;          /* device release in BCD, e.g. 0x0100 = v1.00 */
    uint8_t  iManufacturer;      /* index into string descriptor table; 0 = no string */
    uint8_t  iProduct;           /* index into string descriptor table */
    uint8_t  iSerialNumber;      /* index into string descriptor table */
    uint8_t  bNumConfigurations; /* almost always 1 */
} usb_device_descriptor_t;
```

The composite device's triple `(bDeviceClass=0xEF, bDeviceSubClass=0x02, bDeviceProtocol=0x01)` is the IAD signal — it tells the host "I am a multi-function device; look at my Interface Association Descriptors to find the per-interface classes". Without this triple, Windows in particular loads a generic USB driver against the device and the class-specific drivers never bind.

`idVendor=0x2E8A` is Raspberry Pi's USB-IF-assigned VID; we use it for non-commercial student work. For a commercial product you would get your own VID (USB-IF membership, $6000/year). For this course, `(0x2E8A, 0x000A)` is the convention; the second nibble of the PID lets you distinguish course projects from each other.

`bMaxPacketSize0=64` is the only sane choice for full-speed; the smaller sizes (8, 16, 32) exist for legacy low-speed devices.

---

## 5. The configuration descriptor — 9 bytes + trailing tree

USB 2.0 §9.6.3, Table 9-10 (pp. 264–266).

```c
typedef struct __attribute__((packed)) {
    uint8_t  bLength;             /* 9, always */
    uint8_t  bDescriptorType;     /* 0x02 = CONFIGURATION */
    uint16_t wTotalLength;        /* total length of this descriptor + all trailing
                                     interface, endpoint, and class-specific descriptors.
                                     CRITICAL: if wrong, host stops reading mid-tree. */
    uint8_t  bNumInterfaces;      /* number of interface descriptors in the trailing tree */
    uint8_t  bConfigurationValue; /* the value SET_CONFIGURATION uses to select this config;
                                     1, 2, 3, ... (0 is reserved for "unconfigured") */
    uint8_t  iConfiguration;      /* string index */
    uint8_t  bmAttributes;        /* bit 7: reserved, set to 1
                                     bit 6: self-powered (1) or bus-powered (0)
                                     bit 5: remote wakeup (1) or no (0)
                                     bits 4:0: reserved, set to 0
                                     Bus-powered, no remote wakeup = 0x80
                                     Self-powered, no remote wakeup = 0xC0
                                     Bus-powered, remote wakeup = 0xA0 */
    uint8_t  bMaxPower;           /* maximum power consumption in 2 mA units.
                                     500 mA / 2 = 250 = 0xFA (high-power bus device).
                                     100 mA / 2 = 50  = 0x32 (default, no negotiation).
                                     For self-powered devices set to a small value like 0x01. */
} usb_configuration_descriptor_t;
```

Following the 9-byte config descriptor in the same byte array (returned by `tud_descriptor_configuration_cb()`) come the interface descriptors, the class-specific functional descriptors, and the endpoint descriptors, in *exact* order:

- For each interface group (CDC, HID, MSC, ...):
  - Optionally an IAD (if multiple interfaces belong to one class group, which CDC does).
  - The Interface descriptor.
  - Any class-specific functional descriptors that go inside this interface (CDC's Header/Call-Mgmt/ACM/Union; HID's HID descriptor with report-descriptor length).
  - The Endpoint descriptors for this interface (1, 2, or 3 endpoints depending on the class).

`wTotalLength` is the sum of every descriptor's `bLength` in this chain. The compiler can compute this for you if every descriptor's length is a `#define` constant — see Lecture 3 for the worked composite-device math.

---

## 6. The interface descriptor — 9 bytes

USB 2.0 §9.6.5, Table 9-12 (pp. 268–269).

```c
typedef struct __attribute__((packed)) {
    uint8_t  bLength;            /* 9, always */
    uint8_t  bDescriptorType;    /* 0x04 = INTERFACE */
    uint8_t  bInterfaceNumber;   /* 0-based index into this configuration's interfaces */
    uint8_t  bAlternateSetting;  /* alternate-setting number; 0 for the default */
    uint8_t  bNumEndpoints;      /* count of endpoint descriptors that follow
                                    (excluding endpoint zero, which is implicit) */
    uint8_t  bInterfaceClass;    /* class code; 0x02 CDC, 0x03 HID, 0x08 MSC, etc. */
    uint8_t  bInterfaceSubClass; /* subclass within the class */
    uint8_t  bInterfaceProtocol; /* protocol within the subclass */
    uint8_t  iInterface;         /* string index */
} usb_interface_descriptor_t;
```

The triple `(bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol)` is the host's hook to load a class driver. The relevant triples for this week:

| Class | bInterfaceClass | bInterfaceSubClass | bInterfaceProtocol | Meaning                                     |
|-------|----------------:|-------------------:|-------------------:|---------------------------------------------|
| CDC.comm | 0x02         | 0x02               | 0x00               | Communication, Abstract Control Model, no protocol |
| CDC.data | 0x0A         | 0x00               | 0x00               | CDC Data, no specific protocol               |
| HID   | 0x03            | 0x00               | 0x00               | HID, no boot subclass, no boot protocol      |
| HID-boot-kbd | 0x03     | 0x01               | 0x01               | HID, boot subclass, keyboard boot protocol   |
| HID-boot-mouse | 0x03   | 0x01               | 0x02               | HID, boot subclass, mouse boot protocol      |
| MSC   | 0x08            | 0x06               | 0x50               | MSC, SCSI transparent command set, BBB transport |

The boot-keyboard and boot-mouse subclasses exist so that a PC BIOS can use a USB keyboard or mouse before any OS driver has loaded; for an OS-resident HID device we use `bInterfaceSubClass=0x00, bInterfaceProtocol=0x00` and the OS HID driver parses our full report descriptor instead of relying on the boot-protocol shape.

---

## 7. The endpoint descriptor — 7 bytes

USB 2.0 §9.6.6, Table 9-13 (pp. 269–271).

```c
typedef struct __attribute__((packed)) {
    uint8_t  bLength;          /* 7, always */
    uint8_t  bDescriptorType;  /* 0x05 = ENDPOINT */
    uint8_t  bEndpointAddress; /* bit 7: direction (1 = IN, 0 = OUT)
                                  bits 6:4: reserved, 0
                                  bits 3:0: endpoint number (1..15) */
    uint8_t  bmAttributes;     /* bits 1:0: transfer type
                                              00 = Control
                                              01 = Isochronous
                                              10 = Bulk
                                              11 = Interrupt
                                  bits 3:2: (Iso only) synchronization type
                                  bits 5:4: (Iso only) usage type
                                  bits 7:6: reserved */
    uint16_t wMaxPacketSize;   /* max bytes per packet on this endpoint.
                                  Full-speed bulk: 8/16/32/64.
                                  Full-speed interrupt: 1..64.
                                  Full-speed isoc: 1..1023.
                                  Choose the largest your firmware can fill; 64 is typical for bulk. */
    uint8_t  bInterval;        /* interrupt: poll interval in ms (1..255)
                                  isoc: poll interval in (2^(bInterval-1)) ms units
                                  bulk: ignored (set to 0 or 1) */
} usb_endpoint_descriptor_t;
```

`bEndpointAddress` is a packed direction+number; conventional values:

- `0x81` = IN endpoint 1
- `0x01` = OUT endpoint 1
- `0x82` = IN endpoint 2
- ...

The RP2040's USB controller has 16 endpoint pairs (16 IN + 16 OUT, each addressable separately); endpoints 0 IN/OUT are reserved as the control pair. Endpoints 1–15 are available for class use. Our composite device uses endpoints 1 (CDC notification, interrupt IN), 2 (CDC data, bulk IN+OUT pair), 3 (HID, interrupt IN), 4 (HID, interrupt OUT for output reports like LED state), 5 (MSC, bulk IN+OUT pair).

`bInterval` for an interrupt endpoint is the host's polling interval. HID at `bInterval=10` (10 ms) gives 100 Hz polling; for low-latency input (game controllers) `bInterval=1` (1 ms, 1000 Hz polling) is appropriate.

---

## 8. The string descriptor — variable length

USB 2.0 §9.6.7 (pp. 271–273). String descriptors are UTF-16LE-encoded; the first byte is `bLength` (in bytes, including the header), the second is `bDescriptorType=0x03`, and the remaining bytes are the UTF-16LE text.

String index 0 is special: instead of a string, it is an array of `LANGID`s (16-bit language identifiers from <https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-lcid/>). The typical content is just `{ 0x04, 0x03, 0x09, 0x04 }` — length 4 bytes, type 3, LANGID 0x0409 (US English).

For string indices > 0, the host repeats the request with `wIndex` set to the LANGID it wants; the device returns the localized string. Most devices declare only one LANGID and return the same string regardless of `wIndex`.

TinyUSB's `tud_descriptor_string_cb(uint8_t index, uint16_t langid)` callback returns a `uint16_t const *` pointing to the UTF-16LE string buffer (without the header — TinyUSB prepends it). The convention is to keep a `char const * string_table[]` of ASCII strings indexed by descriptor index, and convert ASCII to UTF-16LE in the callback for indices 1..N.

---

## 9. Worked example: a CDC-only device's full descriptor tree

For Exercise 1 (`exercise-01-cdc-echo.c`) we build a CDC-ACM-only device. The descriptor tree is:

```text
Configuration descriptor (9 bytes)
  Interface Association Descriptor for CDC (8 bytes)
  Interface 0: Communication Class (9 bytes)
    Header functional descriptor       (5 bytes)
    Call Management functional desc    (5 bytes)
    Abstract Control Mgmt functional   (4 bytes)
    Union functional descriptor        (5 bytes)
    Endpoint 0x81 (IN, interrupt, 8B)  (7 bytes)
  Interface 1: CDC Data Class (9 bytes)
    Endpoint 0x02 (OUT, bulk, 64B)     (7 bytes)
    Endpoint 0x82 (IN,  bulk, 64B)     (7 bytes)
-----------------------------------------------
wTotalLength = 9 + 8 + 9 + 5+5+4+5 + 7 + 9 + 7 + 7 = 75 bytes
bNumInterfaces = 2
```

We will write this byte array in Exercise 1 and capture its delivery in Wireshark. The 9 + 8 + 9 + ... arithmetic is the load-bearing step; one off-by-one in `wTotalLength` and the host stops reading at the wrong byte, and no class driver loads.

---

## 10. Where the next two lectures go

This lecture established the *what* of USB device descriptors: every byte of the four core descriptors, every state of the device state machine, every transaction of the enumeration sequence. Lecture 2 covers the *how* of CDC and HID — the class-specific protocols that run on the non-zero endpoints after enumeration succeeds. Lecture 3 covers MSC's bulk-only transport, the FAT12 RAM-disk pre-format, and the composite-device descriptor with its IAD arithmetic.

The TinyUSB code you will write in the exercises is short — under 200 lines per class, plus a 200-byte descriptor array per class. The work is not in the line count; it is in getting the descriptor bytes right and in obeying the two TinyUSB callback rules (do not block, do not over-write). Both are well worth the half-evening it takes to internalize them.

---

## Self-check

Answer these before moving to Lecture 2; the answers are in `quiz.md`.

1. What is the exact byte sequence for the SETUP packet of `GET_DESCRIPTOR(Device, first 8 bytes)`?
2. Why does the host issue `GET_DESCRIPTOR(Device, 8)` before `GET_DESCRIPTOR(Device, 18)`? What is the host learning from the first request that it needs for the second?
3. Your device's `wTotalLength` is 75 but you accidentally concatenated a 76th byte to the descriptor array. What does the host do? Does enumeration succeed?
4. Your device's `bMaxPacketSize0` is 8, but you set `wMaxPacketSize` on a bulk endpoint to 64. Does the host care? Why or why not?
5. You are building a composite device with CDC + HID + MSC. What `(bDeviceClass, bDeviceSubClass, bDeviceProtocol)` triple do you set in the device descriptor, and why?
