# Week 9 — Resources

A curated reference list for USB device classes on the RP2040 with TinyUSB. Every link in this page resolves to a freely available document; no paywalls, no member-only USB-IF gated downloads (USB-IF gates *member-only* errata, but the core specifications are open).

---

## Primary specifications (USB-IF, free)

These are the documents that define every byte you will put on the wire this week. Download all five before Monday; they total about 1,400 pages but you only need the chapters cited in lectures.

- **USB 2.0 Specification** — <https://www.usb.org/document-library/usb-20-specification>
  The base specification. 650 pages. The chapters you must read this week:
  - **Chapter 8** ("Protocol Layer", pp. 195–238) — packet IDs, transaction phases, ACK/NAK/STALL semantics. Light reading; skim for context.
  - **Chapter 9** ("USB Device Framework", pp. 239–284) — *the most-referenced chapter of this week*. The six device states (§9.1.1), bus enumeration (§9.1.2), the eight standard requests (§9.4), the descriptor formats (§9.6). Read all 45 pages with a notepad.
  - **Section 5.5–5.8** ("Control / Bulk / Interrupt / Isochronous Transfers", pp. 35–66) — transfer-type semantics. Read 5.5 (control) thoroughly; the others by skim.
- **USB CDC 1.2 Specification** — <https://www.usb.org/document-library/class-definitions-communication-devices-12>
  120 pages. The relevant chapters:
  - **§3.6.2** ("Abstract Control Model") — the ACM subclass we use. Pages 19–22.
  - **§5.2.3** ("Functional Descriptors") — the Header, Call Management, ACM, and Union descriptors. Pages 36–48. Memorize the byte layouts; you will write them by hand in Exercise 1.
  - **§6.2** ("Management Element Requests") — class-specific control requests like `SET_LINE_CODING`, `GET_LINE_CODING`, `SET_CONTROL_LINE_STATE`. Pages 56–67.
- **USB HID 1.11 Specification** — <https://www.usb.org/document-library/device-class-definition-hid-111>
  97 pages. The relevant chapters:
  - **§6.2.2** ("Report Descriptor") — the bytecode that tells the host how to parse your input/output reports. Pages 23–33. *This is the chapter you will reread the most this week.*
  - **§7.2** ("Class-Specific Requests") — GET_REPORT, SET_REPORT, GET_IDLE, SET_IDLE, GET_PROTOCOL, SET_PROTOCOL. Pages 51–55.
  - **Appendix E** ("Example USB Descriptors for HID Class Devices") — boot keyboard and boot mouse report descriptors, copy-paste-ready. Pages 64–78.
- **USB HID Usage Tables 1.4** — <https://usb.org/document-library/hid-usage-tables-14>
  Free PDF. Defines the *semantic* meaning of usage codes referenced by report descriptors. The two pages you need most:
  - Usage Page 0x01 ("Generic Desktop") — pp. 30–42. Usages 0x06 (Keyboard), 0x02 (Mouse), 0x04 (Joystick), 0x05 (Game Pad), 0x30 (X), 0x31 (Y), 0x32 (Z), 0x33 (Rx), 0x34 (Ry), 0x35 (Rz), 0x38 (Wheel).
  - Usage Page 0x07 ("Keyboard/Keypad") — pp. 53–63. The 232 keyboard usage codes. 0x04 is `a`, 0x05 is `b`, ..., 0x28 is Enter, 0x2A is Backspace.
- **USB Mass Storage Bulk-Only Transport 1.0** — <https://www.usb.org/document-library/mass-storage-bulk-only-10>
  22 pages. The CBW/CSW formats and the bulk-only state machine. Read it once cover-to-cover; it is short.
- **Interface Association Descriptors ECN** — <https://www.usb.org/sites/default/files/iadclasscode_r10.pdf>
  6 pages. Defines the 8-byte IAD format and the `(0xEF, 0x02, 0x01)` device-class triple convention for composite devices. Read once.

---

## SCSI subset (for MSC)

USB-MSC carries SCSI commands inside its bulk-only transport. You implement six SCSI commands minimum for a "just works" device.

- **SCSI Primary Commands - 3 (SPC-3)** — <https://www.t10.org/cgi-bin/ac.pl?t=f&f=spc3r23.pdf>
  Free PDF from the T10 committee. The commands you implement and their reference pages:
  - **INQUIRY** (opcode 0x12) — §6.4, pp. 142–164. Returns vendor, product, version strings (8/16/4 ASCII bytes). The host uses this to populate its "USB drive name" UI.
  - **TEST_UNIT_READY** (opcode 0x00) — §6.34, p. 252. One-byte command, zero-byte data. Reply with status only.
  - **MODE_SENSE_6** (opcode 0x1A) — §6.10, pp. 187–193. The host asks for "mode pages"; you can return a minimal "no special pages" response of 8 bytes.
  - **PREVENT_ALLOW_MEDIUM_REMOVAL** (opcode 0x1E) — §6.13, p. 197. Always reply OK; the RAM disk does not eject.
- **SCSI Block Commands - 3 (SBC-3)** — <https://www.t10.org/cgi-bin/ac.pl?t=f&f=sbc3r36.pdf>
  Free PDF. The block-storage-specific commands:
  - **READ_CAPACITY_10** (opcode 0x25) — §5.10, pp. 56–58. Returns last-LBA and block-size (8 bytes total).
  - **READ_10** (opcode 0x28) — §5.7, pp. 50–53. Reads N blocks starting at LBA.
  - **WRITE_10** (opcode 0x2A) — §5.27, pp. 90–93. Writes N blocks starting at LBA.

The TinyUSB MSC class implementation wraps the CBW/CSW state machine for you and calls your `tud_msc_*` callbacks for each SCSI opcode; you implement the command logic, not the transport.

---

## FAT12 filesystem (for the RAM disk's pre-formatted contents)

The mini-project's 64 KB RAM disk arrives pre-formatted as FAT12 with a `README.TXT` already present. You write the boot sector, FAT, root directory, and file data as a `const uint8_t disk[]` array.

- **Microsoft FAT Specification (fatgen103.doc, August 2005)** — <https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf>
  34 pages. Sections 3–5 cover boot sector, FAT layout, and directory entries.
  - **§3.1** ("Boot Sector and BPB") — pp. 7–18. The 62-byte BIOS Parameter Block at the start of sector 0, plus the boot code area.
  - **§4** ("File Allocation Table") — pp. 19–25. FAT12 entry packing (12 bits per entry, 3 bytes per 2 entries).
  - **§5** ("Directory Structure") — pp. 26–34. The 32-byte short-name directory entry format.
- **Wikipedia, "File Allocation Table"** — <https://en.wikipedia.org/wiki/File_Allocation_Table>
  Useful for a high-level reading of the cluster-chain mechanics before opening the Microsoft spec. The "FAT12" section is sufficient for the mini-project.

---

## TinyUSB documentation (project home, free)

The library reference. TinyUSB is open-source (MIT licensed) at <https://github.com/hathach/tinyusb>; the docs are at <https://docs.tinyusb.org/>.

- **Getting Started** — <https://docs.tinyusb.org/en/latest/reference/getting_started.html>
  15-minute read. Covers the directory layout, the `tusb_config.h` build switch model, and the `tud_task()` main loop convention.
- **Device Concepts** — <https://docs.tinyusb.org/en/latest/reference/dev_concept.html>
  25-minute read. The callback model, the descriptor-array conventions, and the class-stack overview.
- **Concurrency** — <https://docs.tinyusb.org/en/latest/reference/concurrency.html>
  The two cardinal rules: do not block in callbacks, do not call `tud_*_write` of more than `tud_*_write_available()`. Read once and tape it to your monitor.
- **Class API Reference** — <https://docs.tinyusb.org/en/latest/reference/group__group__class__drivers.html>
  Per-class API listings. The pages most relevant this week:
  - `class/cdc/cdc_device.h` — `tud_cdc_write`, `tud_cdc_read`, `tud_cdc_rx_cb`, `tud_cdc_line_coding_cb`.
  - `class/hid/hid_device.h` — `tud_hid_report`, `tud_hid_get_report_cb`, `tud_hid_set_report_cb`.
  - `class/msc/msc_device.h` — `tud_msc_inquiry_cb`, `tud_msc_capacity_cb`, `tud_msc_read10_cb`, `tud_msc_write10_cb`, `tud_msc_test_unit_ready_cb`, `tud_msc_scsi_cb`.
- **Examples directory** — <https://github.com/hathach/tinyusb/tree/master/examples/device>
  Forty-some example devices, each a complete buildable project. The relevant ones:
  - `cdc_msc` — combined CDC + MSC; the structural model for our mini-project.
  - `hid_composite` — keyboard + mouse + consumer-control + gamepad on a single HID interface.
  - `usbtmc` — Test & Measurement Class, ignore.

---

## pico-sdk USB references

The Pico SDK wraps TinyUSB for the RP2040 and adds a couple of small conveniences.

- **pico-sdk hardware_usb group** — <https://www.raspberrypi.com/documentation/pico-sdk/group__hardware__usb.html>
  Direct register access for the USB controller. You will rarely use these — TinyUSB owns the controller — but they are useful for the controller's GPIO routing and the boot-ROM interaction.
- **pico-sdk pico_stdio_usb** — <https://www.raspberrypi.com/documentation/pico-sdk/group__pico__stdio__usb.html>
  The convenience layer that gives you `printf()` over CDC with no descriptor work. We do *not* use this in Week 9 — once we add HID and MSC, this layer is no longer enough — but it is the layer that "just works" in C6's example code and is worth knowing about for low-effort CDC-only projects.
- **RP2040 Datasheet, §4.1 ("USB Controller")** — <https://datasheets.raspberrypi.com/rp2040/rp2040-datasheet.pdf>
  pp. 392–446. 54 pages. The dual-port SRAM (DPSRAM) layout, the endpoint control registers, the SOF interrupt. You will read this only if you fall down the rabbit hole; TinyUSB handles it all in production.

---

## USB packet analysis tools

USB packet captures are the highest-bandwidth way to debug enumeration bugs.

- **Wireshark with USBPcap (Windows)** — <https://desowin.org/usbpcap/>
  Free. Installs a kernel driver; Wireshark then shows USB as a capture interface. Decode is excellent; the "USB" protocol dissector is built into Wireshark itself.
- **usbmon (Linux)** — <https://www.kernel.org/doc/Documentation/usb/usbmon.txt>
  Built into the Linux kernel. `sudo modprobe usbmon; sudo wireshark -i usbmon1` captures everything on bus 1.
- **Apple USB Prober (macOS)** — Comes with Xcode's "Additional Tools for Xcode" download from <https://developer.apple.com/download/all/>. Less of a full packet capture, more of an enumeration-state inspector; combine with `ioreg -p IOUSB -l -w 0` from the command line.
- **Beagle USB 12 (hardware analyzer)** — <https://www.totalphase.com/products/beagle-usb12/>
  Commercial, $400. Captures full-speed USB at the line-protocol level with timing precision. Not required for this week but the cheapest hardware analyzer that is genuinely useful for production work.

---

## Background reading (free, optional)

These are not required for Week 9; they fill in the lower-layer detail when you find yourself curious why something works.

- **"USB in a NutShell" by Beyond Logic** — <https://www.beyondlogic.org/usbnutshell/usb1.shtml>
  Free web book, six chapters, 30 minutes of reading. The clearest introduction to USB protocol details outside the spec itself. Covers NRZI, bit-stuffing, packet IDs, the transaction phases.
- **"USB Complete" by Jan Axelson (5th ed., 2015)** — Not free; ~$60 in print. The reference textbook on USB device design. Worth borrowing from a library; chapter 5 ("Enumeration") and chapter 11 ("Mass Storage") are the most relevant.
- **"USB 2.0 vs USB 3.0 protocol differences" — Microchip AN1953** — <https://ww1.microchip.com/downloads/en/AppNotes/01953A.pdf>
  Free PDF, 12 pages. Useful context for why this week is full-speed-only.

---

## Reference firmware (open source, free)

When you get stuck, read someone else's working code. These are all MIT/BSD-licensed and run on RP2040 or close cousins.

- **TinyUSB CDC-MSC example** — <https://github.com/hathach/tinyusb/tree/master/examples/device/cdc_msc>
  Combined CDC + MSC. The closest existing example to our mini-project (we add HID on top).
- **TinyUSB HID Composite example** — <https://github.com/hathach/tinyusb/tree/master/examples/device/hid_composite>
  Multi-interface HID (keyboard + mouse + consumer-control + gamepad on one interface, with report-ID demultiplexing). The keyboard report descriptor is the canonical one we copy in Exercise 2.
- **pico-examples USB** — <https://github.com/raspberrypi/pico-examples/tree/master/usb>
  The Raspberry Pi Foundation's official Pico USB examples. Mostly CDC-stdio convenience.
- **CircuitPython's USB stack** — <https://github.com/adafruit/circuitpython/tree/main/supervisor/shared/usb>
  CircuitPython composites CDC + MSC + MIDI + HID on the Pico W and friends. Heavy reading; pull it out only if you want a worked example of a four-class composite descriptor.

---

## Errata and "but it works on my machine" notes

A handful of host-OS quirks that bite RP2040-based USB devices in production. None are TinyUSB bugs; all are host-driver behaviors you must accommodate.

- **Windows 10 21H1 and earlier require a manual `usbser.inf` install for CDC ACM on devices with `bDeviceClass=0x00, bDeviceSubClass=0x00`.** Workaround: use `bDeviceClass=0x02 (CDC), bDeviceSubClass=0x02, bDeviceProtocol=0x01` for CDC-only devices, or `bDeviceClass=0xEF, bDeviceSubClass=0x02, bDeviceProtocol=0x01` (IAD) for composites — both inbox-driver-trigger on Windows 10 21H2 and later. Cite Microsoft docs at <https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/usb-driver-installation-based-on-compatible-ids>.
- **macOS occasionally caches a stale "this device failed to enumerate" entry in `ioreg`.** Workaround: unplug, wait 10 seconds, plug back in. If it persists, `sudo killall -HUP usbd` cycles the user-space USB daemon. Cite Apple Developer Forums thread "USB device fails to re-enumerate after firmware change", 2022.
- **Linux's `udev` rules treat the first 100 ms after enumeration as a settling window.** A device that issues a `SET_CONFIGURATION` reply too quickly (< 5 ms) can race with `udev`; symptom is a `/dev/ttyACM0` that exists but is owned by `root:root` instead of `root:dialout`. Workaround: TinyUSB's default `SET_CONFIGURATION` handling is conservative enough to avoid this in practice.
- **Apple's `IOUSBHostInterface` rejects HID report descriptors larger than 4096 bytes.** Our descriptors are < 100 bytes; not an issue for the mini-project. Note for the record.

---

## What to skim, what to read, what to memorize

| Document                                | Strategy                                                                    |
|-----------------------------------------|-----------------------------------------------------------------------------|
| USB 2.0 Chapter 9                       | Read with notepad. Memorize §9.6 descriptor tables.                         |
| USB CDC 1.2 §3.6.2 + §5.2.3             | Read carefully. Memorize the four CDC functional descriptors' byte layouts. |
| HID 1.11 §6.2.2                         | Read carefully. Re-read after Lecture 2.                                    |
| HID Usage Tables 1.4 (Page 0x01, 0x07)  | Bookmark; reference when writing report descriptors.                        |
| MSC BBB 1.0                             | Read once cover to cover; it is 22 pages.                                   |
| SCSI SPC-3 + SBC-3                      | Skim; reference the six opcodes you implement.                              |
| FAT spec                                | Skim §3 and §5; reference §4 only if you debug the FAT.                     |
| TinyUSB docs (getting-started, concept) | Read both. Bookmark the API reference.                                      |
| RP2040 datasheet §4.1                   | Skim. Reference if you fall down the controller rabbit hole.                |
| USB in a NutShell                       | Read on a coffee break. Background context only.                            |

When in doubt, USB 2.0 Chapter 9 is the source of truth. Every USB device-side bug you will hit this week resolves to either "a descriptor byte is wrong" or "a class callback returned the wrong status"; the first kind is Chapter 9, the second kind is the relevant class spec.
