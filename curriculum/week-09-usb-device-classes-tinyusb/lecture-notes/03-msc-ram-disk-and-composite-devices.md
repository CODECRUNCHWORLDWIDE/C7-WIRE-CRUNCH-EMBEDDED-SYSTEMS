# Lecture 3 — MSC, the RAM Disk, and the Composite Descriptor

> *Mass Storage Class is the only class this week that runs a protocol inside the protocol. USB-MSC's Bulk-Only Transport carries SCSI command blocks; you implement six SCSI commands against a backing store you author yourself; the backing store has to look like a FAT12 filesystem because that is what the host's MSC driver mounts. There are three layers of decoding before you get to "the host wants to read sector 1". Once you understand the three layers, MSC is straightforward. Before you understand them, it feels like four separate bugs at once.*

This lecture covers Mass Storage Class with a 64 KB RAM-disk backing store, and the composite-device descriptor that wraps CDC + HID + MSC into one logical USB device with Interface Association Descriptors.

---

## 1. MSC's three layers

A USB MSC transaction has three nested protocols, outer to inner:

1. **USB control + bulk transport** — the standard USB transaction layer; one bulk-OUT endpoint carrying commands, one bulk-IN endpoint carrying responses, and endpoint zero for class-specific requests.
2. **Bulk-Only Transport (BBB)** — `USB Mass Storage Bulk-Only Transport 1.0` (<https://www.usb.org/document-library/mass-storage-bulk-only-10>), a 22-page spec that defines a 31-byte Command Block Wrapper (CBW) on bulk-OUT, an optional data phase on bulk-IN or bulk-OUT, and a 13-byte Command Status Wrapper (CSW) on bulk-IN.
3. **SCSI** — the command set inside the CBW. We implement the six commands required for a read/write block device (SPC-3 INQUIRY, TEST_UNIT_READY, MODE_SENSE_6, PREVENT_ALLOW_MEDIUM_REMOVAL; SBC-3 READ_CAPACITY_10, READ_10, WRITE_10).

TinyUSB handles layers 1 and 2 for you. Your job is layer 3: implement six SCSI command callbacks.

### The CBW (31 bytes)

USB MSC BBB 1.0 §5.1, p. 13:

```c
typedef struct __attribute__((packed)) {
    uint32_t dCBWSignature;          /* 0x43425355 (ASCII "USBC" little-endian) */
    uint32_t dCBWTag;                /* host-chosen tag; echoed in the CSW */
    uint32_t dCBWDataTransferLength; /* expected data phase byte count */
    uint8_t  bmCBWFlags;             /* bit 7: direction (1 = device-to-host) */
    uint8_t  bCBWLUN;                /* logical unit number (we have one LUN; 0) */
    uint8_t  bCBWCBLength;           /* length of CBWCB in bytes (1..16) */
    uint8_t  CBWCB[16];              /* the SCSI command, padded with zeros */
} msc_cbw_t;
```

The 31-byte CBW arrives in one bulk-OUT packet (full-speed bulk endpoint with `wMaxPacketSize=64` fits 31 bytes easily). The device hardware ACKs it; TinyUSB inspects `CBWCB[0]` (the SCSI opcode) and dispatches to your callback.

### The CSW (13 bytes)

USB MSC BBB 1.0 §5.2, p. 14:

```c
typedef struct __attribute__((packed)) {
    uint32_t dCSWSignature;     /* 0x53425355 (ASCII "USBS") */
    uint32_t dCSWTag;           /* echo of the CBW's tag */
    uint32_t dCSWDataResidue;   /* dCBWDataTransferLength - bytes actually transferred */
    uint8_t  bCSWStatus;        /* 0 = success, 1 = failure, 2 = phase error */
} msc_csw_t;
```

TinyUSB constructs the CSW for you based on the return value of your callback. You return `xfer_in_bytes` for success, `-1` for failure; TinyUSB packs the right bytes.

### The SCSI command set we implement

```text
Opcode  Mnemonic                 Spec section          Action
------  -------                  ------------          ------
0x00    TEST_UNIT_READY          SPC-3 §6.34, p. 252   Return OK if disk ready
0x12    INQUIRY                  SPC-3 §6.4, p. 142    Return vendor/product strings
0x1A    MODE_SENSE_6             SPC-3 §6.10, p. 187   Return "no special pages"
0x1E    PREVENT_ALLOW_MEDIUM     SPC-3 §6.13, p. 197   Return OK (RAM disk doesn't eject)
0x25    READ_CAPACITY_10         SBC-3 §5.10, p. 56    Return last LBA + block size
0x28    READ_10                  SBC-3 §5.7, p. 50     Read N blocks at LBA
0x2A    WRITE_10                 SBC-3 §5.27, p. 90    Write N blocks at LBA
```

`READ_10` and `WRITE_10` are the load-bearing ones. The other five are "answer the host's questions about who you are and whether you are ready".

### INQUIRY response (36 bytes)

The INQUIRY response is a 36-byte block (SPC-3 §6.4.2, pp. 144–149):

```c
static const uint8_t inquiry_response[36] = {
    0x00,        /* peripheral qualifier (0 = connected) + device type (0 = Direct Access Block) */
    0x80,        /* removable media bit (bit 7) */
    0x02,        /* version (SPC-2) */
    0x02,        /* response data format */
    36 - 4,      /* additional length (n - 4) */
    0x00, 0x00, 0x00, /* reserved + various flags */
    'C','r','u','n','c','h',' ',' ',  /* T10 vendor identification (8 ASCII bytes) */
    'C','7',' ','R','A','M','d','i','s','k',' ',' ',' ',' ',' ',' ',  /* product (16) */
    '1','.','0','0'  /* product revision (4 ASCII bytes) */
};
```

This is what the host displays in its "Drive: Crunch C7 RAMdisk" label.

### READ_CAPACITY_10 response (8 bytes)

The READ_CAPACITY_10 response is 8 bytes (SBC-3 §5.10.2, p. 57):

```text
Bytes 0-3: returned last-LBA, big-endian uint32 (n_blocks - 1)
Bytes 4-7: block size in bytes, big-endian uint32 (typically 512)
```

For a 64 KB RAM disk with 512-byte blocks: 128 blocks, last LBA = 127 = 0x0000007F, block size = 512 = 0x00000200. The bytes are:

```c
static const uint8_t read_capacity_response[8] = {
    0x00, 0x00, 0x00, 0x7F,   /* last LBA = 127 */
    0x00, 0x00, 0x02, 0x00    /* block size = 512 */
};
```

Note SCSI's big-endian convention is the opposite of USB's little-endian descriptors. Mix them up and the host sees a disk with `0x7F000000 + 1` blocks of `0x00020000` bytes each.

---

## 2. The TinyUSB MSC callbacks

TinyUSB's `class/msc/msc_device.h` defines the callbacks:

```c
void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor[8], uint8_t product[16], uint8_t rev[4]);
                    /* Called to fill the INQUIRY response strings.
                       The opcode-byte handling is TinyUSB's; you provide just the strings. */

bool tud_msc_test_unit_ready_cb(uint8_t lun);
                    /* Return true if the medium is ready. We always return true. */

void tud_msc_capacity_cb(uint8_t lun, uint32_t * block_count, uint16_t * block_size);
                    /* Return the disk's capacity. */

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject);
                    /* Called for START_STOP_UNIT; just return true. */

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void * buffer, uint32_t bufsize);
                    /* Read `bufsize` bytes starting at LBA `lba`, offset `offset` within
                       the block. Return actual byte count read, or -1 on error. */

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           uint8_t * buffer, uint32_t bufsize);
                    /* Write `bufsize` bytes starting at LBA `lba`, offset `offset`.
                       Return actual byte count written, or -1 on error. */

int32_t tud_msc_scsi_cb(uint8_t lun, const uint8_t scsi_cmd[16], void * buffer, uint16_t bufsize);
                    /* Fallback for any opcode TinyUSB doesn't handle directly.
                       Return data byte count, 0 for status-only success, -1 for failure. */
```

The implementation of `tud_msc_read10_cb` for a RAM-disk backing store is one line of `memcpy`:

```c
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void * buffer, uint32_t bufsize)
{
    (void)lun;
    const uint32_t addr = (lba * MSC_BLOCK_SIZE_BYTES) + offset;
    if (addr + bufsize > sizeof ramdisk) {
        return -1;
    }
    memcpy(buffer, &ramdisk[addr], bufsize);
    return (int32_t)bufsize;
}
```

`tud_msc_write10_cb` is the same with the `memcpy` direction reversed.

---

## 3. The FAT12 RAM-disk layout

The RAM disk is 64 KB = 131,072 bytes = 256 blocks of 512 bytes. We pre-format it as FAT12 with one file (`README.TXT`) present, so the host immediately sees a readable filesystem and we do not have to handle the host trying to format the disk on first mount.

FAT12 spec: Microsoft fatgen103.doc <https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf>.

A FAT12 volume has four regions in order:

1. **Reserved area** — sectors 0 to (BPB_RsvdSecCnt - 1). Contains the boot sector at sector 0. Typically just sector 0; `BPB_RsvdSecCnt=1`.
2. **FAT area** — `BPB_NumFATs` copies of the FAT, each `BPB_FATSz16` sectors. Typically 2 copies of a 1-sector FAT.
3. **Root directory area** — `(BPB_RootEntCnt * 32) / BPB_BytsPerSec` sectors. For 16 root entries × 32 bytes/entry / 512 bytes/sector = 1 sector.
4. **Data area** — the rest. Cluster N maps to sector `(BPB_RsvdSecCnt + BPB_NumFATs * BPB_FATSz16 + RootDirSectors) + (N - 2) * BPB_SecPerClus`.

For our 256-block (128 KB) disk with 1-sector cluster size:

```text
Sector 0:        Boot sector (BPB)
Sector 1:        FAT copy 1
Sector 2:        FAT copy 2 (optional but conventional)
Sectors 3-3:     Root directory (1 sector = 16 entries)
Sectors 4-255:   Data area (252 clusters × 512 bytes = 129,024 bytes)
```

### The boot sector (sector 0)

The first 62 bytes are the BPB (BIOS Parameter Block). We write them as a `const uint8_t` block:

```c
static const uint8_t boot_sector[512] = {
    /* BPB */
    0xEB, 0x3C, 0x90,                    /* BS_jmpBoot: jump over BPB to boot code */
    'M','S','D','O','S','5','.','0',     /* BS_OEMName (8 bytes) */
    0x00, 0x02,                          /* BPB_BytsPerSec = 512 */
    0x01,                                /* BPB_SecPerClus = 1 */
    0x01, 0x00,                          /* BPB_RsvdSecCnt = 1 */
    0x02,                                /* BPB_NumFATs = 2 */
    0x10, 0x00,                          /* BPB_RootEntCnt = 16 */
    0x00, 0x01,                          /* BPB_TotSec16 = 256 (total sectors) */
    0xF8,                                /* BPB_Media = 0xF8 (fixed disk) */
    0x01, 0x00,                          /* BPB_FATSz16 = 1 sector per FAT */
    0x20, 0x00,                          /* BPB_SecPerTrk = 32 (unused for non-INT13) */
    0x40, 0x00,                          /* BPB_NumHeads = 64 (unused) */
    0x00, 0x00, 0x00, 0x00,              /* BPB_HiddSec = 0 */
    0x00, 0x00, 0x00, 0x00,              /* BPB_TotSec32 = 0 (using TotSec16) */
    /* Extended BPB (FAT12/16) */
    0x80,                                /* BS_DrvNum */
    0x00,                                /* BS_Reserved1 */
    0x29,                                /* BS_BootSig = 0x29 */
    0x12, 0x34, 0x56, 0x78,              /* BS_VolID (any value) */
    'C','R','U','N','C','H',' ',' ',' ',' ',' ',  /* BS_VolLab (11 bytes) */
    'F','A','T','1','2',' ',' ',' ',     /* BS_FilSysType (8 bytes) */
    /* Rest of sector 0 = zero, with boot signature 0x55 0xAA at offset 510 */
    [510] = 0x55, [511] = 0xAA
};
```

### The FAT (sector 1)

FAT12 packs entries 12 bits wide. Entries 0 and 1 are reserved with specific values (FAT[0] = media byte | 0xF00, FAT[1] = 0xFFF). Entry 2 is the first data cluster (cluster 2 maps to sector 4 of the disk in our layout).

For a single-cluster file at cluster 2 with end-of-chain marker:

```c
static const uint8_t fat[512] = {
    /* FAT[0] = 0xFF8 (media), FAT[1] = 0xFFF (reserved), FAT[2] = 0xFFF (EOC for our file) */
    0xF8, 0xFF, 0xFF,    /* entries 0 and 1 packed: 0xFF8 + 0xFFF */
    0xFF, 0x0F, 0x00,    /* entry 2: 0xFFF (EOC), entry 3: 0x000 (free) */
    /* the rest of the FAT = all zeros (free clusters) */
};
```

The FAT12 entry packing rule: every pair of consecutive 12-bit entries fits in 3 bytes. Bytes 0-2 hold entries 0 and 1; bytes 3-5 hold entries 2 and 3; etc. Within each 3-byte group:

```text
Byte 0 = low 8 bits of entry N (even)
Byte 1 (low 4 bits) = high 4 bits of entry N (even)
Byte 1 (high 4 bits) = low 4 bits of entry N+1 (odd)
Byte 2 = high 8 bits of entry N+1 (odd)
```

So entry 0 = 0xFF8 means byte 0 = 0xF8 and byte 1's low nibble = 0xF. Entry 1 = 0xFFF means byte 1's high nibble = 0xF and byte 2 = 0xFF. Combined: bytes 0..2 = `{0xF8, 0xFF, 0xFF}`. Entry 2 = 0xFFF means byte 3 = 0xFF and byte 4's low nibble = 0xF. Entry 3 = 0x000 means byte 4's high nibble = 0x0 and byte 5 = 0x00. Combined: bytes 3..5 = `{0xFF, 0x0F, 0x00}`.

### The root directory (sector 3)

A 32-byte directory entry (fatgen103.doc §5.1, pp. 26–28):

```c
static const uint8_t root_dir[512] = {
    /* Entry 0: README  TXT */
    'R','E','A','D','M','E',' ',' ',  /* DIR_Name[0..7] (filename, padded with spaces) */
    'T','X','T',                       /* DIR_Name[8..10] (extension) */
    0x20,                              /* DIR_Attr (0x20 = archive bit) */
    0x00,                              /* DIR_NTRes (reserved) */
    0x00,                              /* DIR_CrtTimeTenth */
    0x00, 0x00,                        /* DIR_CrtTime */
    0x21, 0x59,                        /* DIR_CrtDate (1 Jan 2025 encoded) */
    0x21, 0x59,                        /* DIR_LstAccDate */
    0x00, 0x00,                        /* DIR_FstClusHI (always 0 in FAT12/16) */
    0x00, 0x00,                        /* DIR_WrtTime */
    0x21, 0x59,                        /* DIR_WrtDate */
    0x02, 0x00,                        /* DIR_FstClusLO = cluster 2 */
    README_LEN_BYTE_0, README_LEN_BYTE_1,
    README_LEN_BYTE_2, README_LEN_BYTE_3,    /* DIR_FileSize, little-endian */
    /* Remaining 15 entries are zeros (free) */
};
```

The README content lives at sector 4 (cluster 2). The whole thing is a 256-block `uint8_t ramdisk[131072]` array with sectors 0, 1, 2 (FAT copy), 3, and 4 populated, the rest zero. The host's MSC driver mounts this and the file appears.

### Why pre-format

Without pre-formatting, the host's MSC driver mounts an "uninitialized" disk and the host's OS may prompt to format it (Windows is loud about this; macOS shows a "this disk is not readable" dialog). Pre-formatting lets the device appear with a working filesystem immediately. The 1.5 KB of pre-format data (`boot_sector + fat + root_dir + README content`) is `const` in flash, not in RAM — only the writable parts of the RAM disk (the data area sectors beyond the README) live in RAM.

---

## 4. Interface Association Descriptors

The composite device problem is: the device descriptor has one `(bDeviceClass, bDeviceSubClass, bDeviceProtocol)` triple, but our device implements three classes. How does the host know which interface belongs to which class?

The USB 2.0 spec's original answer was: set `bDeviceClass=0` (the "look at interface descriptors" trigger) and the host walks the configuration tree, reading the `bInterfaceClass` of each interface descriptor. This works for single-class devices. For *composite* devices where the classes need to be grouped — CDC has two interfaces that must be treated as one logical function — the original spec offered no way to express the grouping. Windows in particular treated each CDC interface as an independent device, which broke driver binding.

The Interface Association Descriptors ECN (2003-07-23, <https://www.usb.org/sites/default/files/iadclasscode_r10.pdf>) added an 8-byte descriptor that groups interfaces:

```c
typedef struct __attribute__((packed)) {
    uint8_t bLength;             /* 8, always */
    uint8_t bDescriptorType;     /* 0x0B = INTERFACE_ASSOCIATION */
    uint8_t bFirstInterface;     /* first interface in the group */
    uint8_t bInterfaceCount;     /* number of interfaces in the group */
    uint8_t bFunctionClass;      /* same triple as bInterfaceClass would be */
    uint8_t bFunctionSubClass;
    uint8_t bFunctionProtocol;
    uint8_t iFunction;           /* string index for function name */
} usb_iad_t;
```

Place an IAD before each group of interfaces that belong to one class. The device descriptor's triple becomes `(0xEF, 0x02, 0x01)` ("Multi-Interface Function" / "Common Class" / "Interface Association Descriptor") to signal "look at my IADs".

Our composite has three IADs:

```c
/* IAD for CDC (interfaces 0 and 1) */
{ 0x08, 0x0B, 0x00, 0x02, 0x02, 0x02, 0x00, 0x04 }
/* IAD for HID (interface 2) */
{ 0x08, 0x0B, 0x02, 0x01, 0x03, 0x00, 0x00, 0x05 }
/* IAD for MSC (interface 3) */
{ 0x08, 0x0B, 0x03, 0x01, 0x08, 0x06, 0x50, 0x06 }
```

The `iFunction` indices (4, 5, 6) point to string descriptors that name each function ("CDC ACM Serial Port", "HID Keyboard", "USB Mass Storage"); the host's UI shows these strings.

---

## 5. The composite configuration descriptor — byte-by-byte

The mini-project's configuration descriptor. We compute lengths with `#define` arithmetic so the compiler verifies `wTotalLength`.

```c
/* --- Descriptor length constants ---------------------------------------- */
#define DESC_LEN_CONFIG          ((uint16_t)9u)
#define DESC_LEN_IAD             ((uint16_t)8u)
#define DESC_LEN_INTERFACE       ((uint16_t)9u)
#define DESC_LEN_ENDPOINT        ((uint16_t)7u)
#define DESC_LEN_CDC_HEADER      ((uint16_t)5u)
#define DESC_LEN_CDC_CALLMGMT    ((uint16_t)5u)
#define DESC_LEN_CDC_ACM         ((uint16_t)4u)
#define DESC_LEN_CDC_UNION       ((uint16_t)5u)
#define DESC_LEN_HID             ((uint16_t)9u)

/* Per-class section lengths */
#define DESC_LEN_CDC_SECTION  \
    (DESC_LEN_IAD + DESC_LEN_INTERFACE                                   \
     + DESC_LEN_CDC_HEADER + DESC_LEN_CDC_CALLMGMT                       \
     + DESC_LEN_CDC_ACM + DESC_LEN_CDC_UNION                             \
     + DESC_LEN_ENDPOINT                                                 \
     + DESC_LEN_INTERFACE                                                \
     + DESC_LEN_ENDPOINT + DESC_LEN_ENDPOINT)
                            /* = 8 + 9 + 5+5+4+5 + 7 + 9 + 7+7 = 66 */

#define DESC_LEN_HID_SECTION  \
    (DESC_LEN_IAD + DESC_LEN_INTERFACE + DESC_LEN_HID                    \
     + DESC_LEN_ENDPOINT + DESC_LEN_ENDPOINT)
                            /* = 8 + 9 + 9 + 7 + 7 = 40 */

#define DESC_LEN_MSC_SECTION  \
    (DESC_LEN_IAD + DESC_LEN_INTERFACE                                   \
     + DESC_LEN_ENDPOINT + DESC_LEN_ENDPOINT)
                            /* = 8 + 9 + 7 + 7 = 31 */

/* Total configuration descriptor length */
#define DESC_LEN_CONFIG_TOTAL  \
    (DESC_LEN_CONFIG + DESC_LEN_CDC_SECTION                              \
     + DESC_LEN_HID_SECTION + DESC_LEN_MSC_SECTION)
                            /* = 9 + 66 + 40 + 31 = 146 */
```

So `wTotalLength = 146` bytes. `bNumInterfaces = 2 (CDC) + 1 (HID) + 1 (MSC) = 4`.

The full descriptor array layout, with the byte offsets that go into `wTotalLength`:

```text
Offset  Length  Content
------  ------  -------
0       9       Configuration descriptor (wTotalLength=146, bNumInterfaces=4)
9       8       IAD for CDC (interfaces 0-1, class 0x02/0x02/0x00)
17      9       Interface 0 (CDC.comm, class 0x02/0x02/0x00, 1 endpoint)
26      5       CDC Header functional descriptor (bcdCDC = 0x0110)
31      5       CDC Call Management functional descriptor (data IF = 1)
36      4       CDC ACM functional descriptor (bmCapabilities = 0x02)
40      5       CDC Union functional descriptor (master=0, slave=1)
45      7       Endpoint 0x81 (CDC notification, interrupt-IN, 8 B, bInterval=16)
52      9       Interface 1 (CDC.data, class 0x0A/0x00/0x00, 2 endpoints)
61      7       Endpoint 0x02 (CDC data, bulk-OUT, 64 B)
68      7       Endpoint 0x82 (CDC data, bulk-IN, 64 B)
75      8       IAD for HID (interface 2, class 0x03/0x00/0x00)
83      9       Interface 2 (HID, class 0x03/0x00/0x00, 2 endpoints)
92      9       HID class descriptor (bcdHID=0x0111, wReportLen=...)
101     7       Endpoint 0x83 (HID interrupt-IN, 16 B, bInterval=10)
108     7       Endpoint 0x03 (HID interrupt-OUT, 16 B, bInterval=10)
115     8       IAD for MSC (interface 3, class 0x08/0x06/0x50)
123     9       Interface 3 (MSC, class 0x08/0x06/0x50, 2 endpoints)
132     7       Endpoint 0x84 (MSC bulk-IN, 64 B)
139     7       Endpoint 0x04 (MSC bulk-OUT, 64 B)
------  ------
Total:  146 bytes
```

The MSC has 5 + 1 = 5 endpoint pairs? No — count: CDC uses endpoint 1 (notify, IN-only), endpoint 2 (data, both directions). HID uses endpoint 3 (both directions). MSC uses endpoint 4 (both directions). Plus the implicit endpoint 0 (control). That is 5 endpoint addresses claimed, well under the RP2040's 16-endpoint limit.

---

## 6. The composite-device class triple

The device descriptor for the composite is:

```c
static const uint8_t desc_device[18] = {
    18,         /* bLength */
    0x01,       /* bDescriptorType = DEVICE */
    0x00, 0x02, /* bcdUSB = 0x0200 (USB 2.0) */
    0xEF,       /* bDeviceClass = Miscellaneous */
    0x02,       /* bDeviceSubClass = Common Class */
    0x01,       /* bDeviceProtocol = IAD */
    0x40,       /* bMaxPacketSize0 = 64 */
    0x8A, 0x2E, /* idVendor = 0x2E8A (Raspberry Pi) */
    0x0C, 0x00, /* idProduct = 0x000C (our composite course project) */
    0x00, 0x01, /* bcdDevice = 0x0100 (v1.00) */
    1, 2, 3,    /* iManufacturer, iProduct, iSerialNumber */
    1           /* bNumConfigurations */
};
```

The triple `(0xEF, 0x02, 0x01)` is the signal to Windows that this is an IAD-based composite. Without it, Windows 10 sometimes still loads class drivers (it has gotten smarter over the years), but the inbox-driver lookup is more conservative and you may see "USB Composite Device" instead of the per-function names.

---

## 7. The composite-device string table

The string descriptors backing the composite are:

```c
static const char * string_desc_arr[] = {
    (const char[]){ 0x09, 0x04 },     /* 0: LANGID = 0x0409 (US English) */
    "Code Crunch Worldwide",          /* 1: iManufacturer */
    "C7 Wire Composite Device",       /* 2: iProduct */
    "C7-WIRE-W09-0001",               /* 3: iSerialNumber */
    "CDC ACM Serial Port",            /* 4: IAD CDC iFunction */
    "HID Keyboard",                   /* 5: IAD HID iFunction */
    "USB Mass Storage"                /* 6: IAD MSC iFunction */
};
```

`tud_descriptor_string_cb(index, langid)` converts strings 1..6 to UTF-16LE on the fly and prepends the 2-byte header. String 0 is returned verbatim with its header.

---

## 8. Putting it together: what the host sees

After enumeration, the host sees one USB device with three logical functions:

- **CDC ACM Serial Port** ("/dev/cu.usbmodemC7W9001" / "COM5" / "/dev/ttyACM0") — opens with `screen`, `picocom`, PuTTY; supports bidirectional byte stream at any baud (USB doesn't actually run at the baud).
- **HID Keyboard** — generates keystrokes when the firmware calls `tud_hid_report()`. Visible in the host's keyboard list as "C7 Wire Composite Device".
- **USB Mass Storage** ("CRUNCH" volume label, 128 KB capacity) — appears as a removable drive in Finder / Explorer / file manager, contains `README.TXT`.

The mini-project's bench test consists of:

1. Plug in the Pico. Verify all three functions enumerate (`lsusb -v`, Device Manager, System Information).
2. Open a terminal emulator on the CDC port. Type `hello`. Verify `hello` echoes back.
3. Press the BOOTSEL button. Verify `code crunch` appears in the focused window.
4. Open the CRUNCH drive in a file manager. Verify `README.TXT` is present and readable.
5. Drag a small text file (< 10 KB) onto the drive. Verify the file appears in the file listing and survives unmount/remount.

Pass criteria: all five steps succeed; the USBPcap shows enumeration completing in < 300 ms with no STALLs; the HID button-to-keystroke latency is < 8 ms.

---

## 9. Common composite-device bugs

The five canonical bugs we will hit in Exercise 3, the challenges, and the mini-project:

1. **Wrong `wTotalLength`** — the sum of all trailing descriptor lengths. Off by even one byte and the host stops parsing mid-tree. Symptom: device shows up in enumeration but the second or third interface's class driver fails to bind.
2. **Missing IAD before a multi-interface class group** — CDC's two interfaces show up as unrelated. Symptom on Windows: "USB Composite Device" parent with one of the CDC interfaces missing a driver.
3. **Wrong device-class triple `(0xEF, 0x02, 0x01)`** — older Windows versions fall back to single-class driver loading. Symptom: only the first declared interface's class driver loads.
4. **Endpoint number collision** — two interfaces declare the same endpoint number in the same direction. Symptom: enumeration looks fine but only one of the two classes works.
5. **`tusb_config.h` `CFG_TUD_*` count too low** — if you declare two CDC interfaces in your descriptor but `CFG_TUD_CDC=1`, TinyUSB only services the first; the second's bulk endpoints NAK forever. Symptom: descriptor enumerates but data never flows on the second instance.

All five surface within the first 10 minutes of bring-up if you have a USB packet capture running. Without a capture they look identical from the OS-side error messages.

---

## 10. Close

We have now defined three USB classes (CDC, HID, MSC), the wrap that makes them coexist (IADs + the class triple), and the descriptor tree that ties everything to byte-exact lengths. The exercises and the mini-project will exercise each class individually and then together. The single most-referenced artifact from this lecture is the offset table in §5; copy it to a notebook before starting the mini-project, because every "the descriptor is wrong somehow" debugging session uses it as the ground truth.

---

## Self-check

1. What is the SCSI opcode for READ_10? What four pieces of information does the host pass in the command?
2. Why are SCSI multi-byte fields big-endian when USB multi-byte fields are little-endian?
3. The FAT12 root directory has 16 entries. How many bytes is that? At what sector offset does it live in the boot sector's BPB layout?
4. Your composite device's `wTotalLength=146`. You add a Microsoft OS Descriptor (24 bytes) to the configuration tree. What is the new `wTotalLength`?
5. Two interface descriptors in your composite declare `bEndpointAddress=0x81`. What is the symptom, and at which layer of the stack does it show up?
