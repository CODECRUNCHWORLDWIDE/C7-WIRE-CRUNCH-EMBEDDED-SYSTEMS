# Week 9 — Quiz

Ten questions. Closed book to start; open USB 2.0 Chapter 9 and the TinyUSB docs only after you have committed an answer in writing. Answers at the bottom.

---

**1.** Decode the following SETUP packet byte-by-byte. What standard request is the host issuing, and to what?

```text
80 06 00 02 00 00 09 00
```

---

**2.** Your CDC ACM device's configuration descriptor has the following sizes for its components: 9 (config), 8 (IAD), 9 (IF.comm), 5 + 5 + 4 + 5 (CDC functional), 7 (notify EP), 9 (IF.data), 7 (bulk-OUT), 7 (bulk-IN). What value goes in `wTotalLength`? What value goes in `bNumInterfaces`?

---

**3.** The HID boot-keyboard report descriptor declares an 8-byte input report. Byte 0 is the modifier byte and byte 1 is a reserved byte. What is the meaning of bytes 2 through 7? If your firmware wants to type the letter `a` (HID Usage Page 0x07, Usage 0x04), what does it put in those six bytes?

---

**4.** A USB MSC device's SCSI READ_CAPACITY_10 response is 8 bytes long. For a 64 KB disk with 512-byte blocks, what 8 bytes does the device send? Spell them out in hexadecimal, in the order they appear on the wire.

---

**5.** Your composite-device configuration descriptor has 4 interfaces grouped under 3 IADs (one IAD covering interfaces 0 and 1 for CDC, one covering interface 2 for HID, one covering interface 3 for MSC). What `(bDeviceClass, bDeviceSubClass, bDeviceProtocol)` triple do you put in the *device* descriptor, and why?

---

**6.** What is the function of the CDC Union functional descriptor (CDC 1.2 §5.2.3.8)? What goes wrong, on which host OSes, if you omit it from a CDC ACM device?

---

**7.** Your firmware sets `bInterval=4` on a HID interrupt-IN endpoint. The host polls the device at full-speed (12 Mbit/s). How often does the host issue an IN token to that endpoint? Express your answer as a frequency in Hertz.

---

**8.** A USB packet capture shows the host issuing `GET_DESCRIPTOR(Configuration, 9 bytes)`, receiving 9 bytes from the device, and then issuing `GET_DESCRIPTOR(Configuration, 121 bytes)`. The second request returns only 75 bytes before the host gives up and the device falls off the bus. What is the most likely bug?

---

**9.** Your TinyUSB firmware calls `tud_cdc_write(buf, 200)`. The CDC TX FIFO (`CFG_TUD_CDC_TX_BUFSIZE`) is 128 bytes, currently empty. What does `tud_cdc_write` return? What happens to the bytes beyond the FIFO's capacity?

---

**10.** A composite device declares CDC, HID, and MSC. The host's USB driver loads `usbser.sys` (or equivalent) for the CDC interface but fails to load any HID driver. The device descriptor's triple is `(0x00, 0x00, 0x00)` and there are no IADs in the config tree. What is the bug, and what is the one-line fix?

---

## Answers

**1.** `bmRequestType=0x80` (device-to-host, standard, recipient=device). `bRequest=0x06` (GET_DESCRIPTOR). `wValue=0x0200` (descriptor type 0x02 = Configuration, index 0). `wIndex=0x0000`. `wLength=0x0009`. The host is requesting the first 9 bytes of the configuration descriptor — i.e., the 9-byte config header so it can learn `wTotalLength`.

**2.** `wTotalLength = 9 + 8 + 9 + 5+5+4+5 + 7 + 9 + 7 + 7 = 75 bytes`. `bNumInterfaces = 2` (the CDC comm interface and the CDC data interface; the IAD does not count as an interface).

**3.** Bytes 2 through 7 are six key-code slots, each holding an 8-bit HID Usage Tables Page 0x07 keycode (0 = empty slot, non-zero = pressed key). To type `a`, the firmware puts `0x04` in byte 2 and zeros in bytes 3 through 7 — then on the next report, all zeros (the key-up).

**4.** The disk has 128 blocks (64 KB / 512 bytes = 128); last LBA = 127 = 0x0000007F (big-endian, 4 bytes); block size = 512 = 0x00000200 (big-endian, 4 bytes). On the wire: `00 00 00 7F 00 00 02 00`.

**5.** `(0xEF, 0x02, 0x01)` — "Miscellaneous, Common Class, Interface Association Descriptor". This triple signals to the host (and especially Windows) that the device has multiple functions to be discovered via Interface Association Descriptors. Without it, Windows in particular falls back to single-class driver loading and the secondary interfaces' drivers may not bind.

**6.** The Union descriptor declares which interfaces (one master + zero or more slaves) belong to one logical CDC function. The CDC.comm interface is the master, the CDC.data interface is the slave. Without the Union descriptor, the host's CDC driver does not know which data interface to bind to the comm interface, and the byte pipe does not open. Symptom: enumeration completes; `/dev/ttyACM0` or `COMx` appears on the host; but attempting to open it fails or hangs. This breaks on Linux and Windows; macOS sometimes recovers via heuristic.

**7.** `bInterval=4` means "poll every 4 ms" for a full-speed interrupt endpoint (USB 2.0 §9.6.6, p. 270: "bInterval — Interval for polling endpoint for data transfers... in frame counts"; a full-speed frame is 1 ms). 4 ms = 250 Hz.

**8.** `wTotalLength` is reported as 121 but the actual descriptor array is 75 bytes. The host reads 75 bytes, expects 46 more, sees a timeout or junk and gives up. Fix: correct `wTotalLength` to 75, or add the missing 46 bytes of trailing descriptors.

**9.** `tud_cdc_write` copies as many bytes as fit (128) and returns 128. The remaining 72 bytes are *not* queued anywhere; they are silently dropped. The caller is expected to check the return value and re-call with the leftover later, or to call `tud_cdc_write_available()` before writing to confirm there is room.

**10.** Without IADs and with the device-descriptor triple `(0x00, 0x00, 0x00)`, the host treats each interface descriptor's class triple independently. Windows may load `usbser.sys` for the first CDC interface (the host's first-interface heuristic) but fail to load HIDClass.sys against interface 2 because the device descriptor does not flag this as an IAD device. One-line fix: change the device descriptor's triple to `(0xEF, 0x02, 0x01)` and add the three IADs to the config tree.
