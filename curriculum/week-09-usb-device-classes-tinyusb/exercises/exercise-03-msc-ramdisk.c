/*
 * Exercise 3 - USB Mass Storage Class with a 64 KB RAM-disk backing store
 *
 * The Pico enumerates as a removable USB drive containing a 128 KB
 * FAT12-formatted volume with a single pre-existing file, README.TXT.
 * The host mounts the drive in its file manager; reads work; writes
 * persist within one power cycle.
 *
 * The interesting work in this exercise is the FAT12 layout. The boot
 * sector, the FAT itself, the root directory entry, and the README file
 * content all live as `const uint8_t` arrays that we copy into the RAM
 * disk image at init time. The reads-and-writes path then becomes a
 * simple memcpy in the SCSI READ_10 / WRITE_10 callbacks.
 *
 * Hardware:
 *   - Raspberry Pi Pico (W or non-W).
 *   - USB cable to your laptop.
 *
 * What you should see on the host:
 *   - macOS: a "CRUNCH" volume mounts at /Volumes/CRUNCH.
 *   - Linux: /media/<user>/CRUNCH appears in the file manager.
 *   - Windows: a new drive letter (typically E: or F:) labeled CRUNCH.
 *   In all three, README.TXT exists and contains the canonical
 *   "Week 9 - Code Crunch C7 Wire - USB device classes" string.
 *   You may drag small files onto the drive; they appear in the listing
 *   and persist until the Pico is power-cycled.
 *
 * Build:
 *   pico-sdk libraries to link:
 *     pico_stdlib, tinyusb_device, tinyusb_board
 *   tusb_config.h must define CFG_TUD_MSC=1, CFG_TUD_MSC_EP_BUFSIZE=512.
 *
 * References:
 *   USB MSC Bulk-Only Transport 1.0 (CBW/CSW format):
 *     https://www.usb.org/document-library/mass-storage-bulk-only-10
 *   SCSI SPC-3 (INQUIRY, TEST_UNIT_READY, MODE_SENSE_6):
 *     https://www.t10.org/cgi-bin/ac.pl?t=f&f=spc3r23.pdf
 *   SCSI SBC-3 (READ_CAPACITY_10, READ_10, WRITE_10):
 *     https://www.t10.org/cgi-bin/ac.pl?t=f&f=sbc3r36.pdf
 *   Microsoft FAT spec (boot sector, FAT layout, directory entries):
 *     https://academy.cba.mit.edu/classes/networking_communications/SD/FAT.pdf
 *   TinyUSB MSC API: class/msc/msc_device.h
 *     https://docs.tinyusb.org/en/latest/reference/group__group__class__drivers.html
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"

#include "bsp/board.h"
#include "tusb.h"

#include "usb_descriptors.h"

/* ---- Constants -------------------------------------------------------- */

#define LED_PIN                      ((uint32_t)25u)

/* The README.TXT body. Keep this short (under one cluster = 512 bytes). */
static const char readme_body[] =
    "Week 9 - Code Crunch C7 Wire - USB device classes\r\n"
    "\r\n"
    "This is a 64 KB RAM-disk backing store served by TinyUSB MSC.\r\n"
    "Content is volatile - power-cycle the Pico to reset.\r\n";

#define README_LEN  ((uint32_t)(sizeof readme_body - 1u))

/* ---- Module state ----------------------------------------------------- */

static volatile bool s_mounted = false;

/* The 128 KB RAM disk. 256 sectors of 512 bytes. SECTION + ALIGN keep it
 * from competing with the heap for cache lines. */
static uint8_t s_ramdisk[MSC_DISK_SIZE_BYTES] __attribute__((aligned(4)));

/* ---- FAT12 layout helpers --------------------------------------------
 *
 * Volume layout (sector indices):
 *   Sector 0:       Boot sector (BPB)
 *   Sector 1:       FAT copy 1
 *   Sector 2:       FAT copy 2
 *   Sector 3:       Root directory (16 32-byte entries = 1 sector)
 *   Sector 4..255:  Data area
 *     - Cluster 2 = sector 4 = README content
 *     - Cluster 3..253 = free
 */

#define SECTOR_BOOT       ((uint32_t)0u)
#define SECTOR_FAT1       ((uint32_t)1u)
#define SECTOR_FAT2       ((uint32_t)2u)
#define SECTOR_ROOTDIR    ((uint32_t)3u)
#define SECTOR_DATA_START ((uint32_t)4u)

#define README_CLUSTER    ((uint16_t)2u)

/* Boot sector: 512 bytes. The leading 62-byte BPB is fully specified;
 * the remainder is zeros with the 0x55AA signature at offset 510. */
static const uint8_t boot_sector_template[62] = {
    0xEBu, 0x3Cu, 0x90u,                /* BS_jmpBoot */
    'M','S','D','O','S','5','.','0',    /* BS_OEMName */
    0x00u, 0x02u,                       /* BPB_BytsPerSec = 512 */
    0x01u,                              /* BPB_SecPerClus = 1 */
    0x01u, 0x00u,                       /* BPB_RsvdSecCnt = 1 */
    0x02u,                              /* BPB_NumFATs = 2 */
    0x10u, 0x00u,                       /* BPB_RootEntCnt = 16 */
    0x00u, 0x01u,                       /* BPB_TotSec16 = 256 */
    0xF8u,                              /* BPB_Media = 0xF8 */
    0x01u, 0x00u,                       /* BPB_FATSz16 = 1 sector */
    0x20u, 0x00u,                       /* BPB_SecPerTrk = 32 */
    0x40u, 0x00u,                       /* BPB_NumHeads = 64 */
    0x00u, 0x00u, 0x00u, 0x00u,         /* BPB_HiddSec = 0 */
    0x00u, 0x00u, 0x00u, 0x00u,         /* BPB_TotSec32 = 0 */
    0x80u,                              /* BS_DrvNum */
    0x00u,                              /* BS_Reserved1 */
    0x29u,                              /* BS_BootSig */
    0x12u, 0x34u, 0x56u, 0x78u,         /* BS_VolID */
    'C','R','U','N','C','H',' ',' ',' ',' ',' ',  /* BS_VolLab (11 bytes) */
    'F','A','T','1','2',' ',' ',' '     /* BS_FilSysType (8 bytes) */
};

/* The FAT itself (1 sector = 512 bytes). FAT12 entry packing:
 *   entry 0: 0xFF8 (media descriptor)
 *   entry 1: 0xFFF (reserved)
 *   entry 2: 0xFFF (end-of-chain for our README file)
 *   entry 3..: 0 (free) */
static const uint8_t fat_template[6] = {
    0xF8u, 0xFFu, 0xFFu,        /* entries 0 and 1 packed */
    0xFFu, 0x0Fu, 0x00u         /* entry 2 (EOC) and entry 3 (free) */
};

/* The root directory: one 32-byte entry for README.TXT. */
static uint8_t make_rootdir_entry_size_lo(void) { return (uint8_t)(README_LEN & 0xFFu); }
static uint8_t make_rootdir_entry_size_hi1(void) { return (uint8_t)((README_LEN >> 8) & 0xFFu); }
static uint8_t make_rootdir_entry_size_hi2(void) { return (uint8_t)((README_LEN >> 16) & 0xFFu); }
static uint8_t make_rootdir_entry_size_hi3(void) { return (uint8_t)((README_LEN >> 24) & 0xFFu); }

/* ---- RAM-disk initialization ---------------------------------------- */

static void ramdisk_init(void)
{
    memset(s_ramdisk, 0, sizeof s_ramdisk);

    /* Sector 0: boot sector. */
    uint8_t * const boot = &s_ramdisk[SECTOR_BOOT * MSC_BLOCK_SIZE_BYTES];
    memcpy(boot, boot_sector_template, sizeof boot_sector_template);
    boot[510] = 0x55u;
    boot[511] = 0xAAu;

    /* Sectors 1 and 2: FAT copies (identical). */
    uint8_t * const fat1 = &s_ramdisk[SECTOR_FAT1 * MSC_BLOCK_SIZE_BYTES];
    uint8_t * const fat2 = &s_ramdisk[SECTOR_FAT2 * MSC_BLOCK_SIZE_BYTES];
    memcpy(fat1, fat_template, sizeof fat_template);
    memcpy(fat2, fat_template, sizeof fat_template);

    /* Sector 3: root directory, one entry for README.TXT. */
    uint8_t * const rootdir = &s_ramdisk[SECTOR_ROOTDIR * MSC_BLOCK_SIZE_BYTES];
    const uint8_t entry[32] = {
        'R','E','A','D','M','E',' ',' ',   /* DIR_Name (8 bytes) */
        'T','X','T',                        /* DIR_Name extension (3 bytes) */
        0x20u,                              /* DIR_Attr = ARCHIVE */
        0x00u,                              /* DIR_NTRes */
        0x00u,                              /* DIR_CrtTimeTenth */
        0x00u, 0x00u,                       /* DIR_CrtTime */
        0x21u, 0x59u,                       /* DIR_CrtDate */
        0x21u, 0x59u,                       /* DIR_LstAccDate */
        0x00u, 0x00u,                       /* DIR_FstClusHI (always 0 in FAT12/16) */
        0x00u, 0x00u,                       /* DIR_WrtTime */
        0x21u, 0x59u,                       /* DIR_WrtDate */
        CC_LE_U16_LO(README_CLUSTER),       /* DIR_FstClusLO (low byte) */
        CC_LE_U16_HI(README_CLUSTER),       /* DIR_FstClusLO (high byte) */
        make_rootdir_entry_size_lo(),
        make_rootdir_entry_size_hi1(),
        make_rootdir_entry_size_hi2(),
        make_rootdir_entry_size_hi3()
    };
    memcpy(rootdir, entry, sizeof entry);

    /* Sector 4: README content (cluster 2). */
    uint8_t * const data = &s_ramdisk[SECTOR_DATA_START * MSC_BLOCK_SIZE_BYTES];
    memcpy(data, readme_body, README_LEN);
}

/* ---- TinyUSB MSC callbacks ------------------------------------------ */

void tud_msc_inquiry_cb(const uint8_t lun,
                         uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
{
    (void)lun;
    const char v[] = "Crunch  ";
    const char p[] = "C7 RAMdisk      ";
    const char r[] = "1.00";
    memcpy(vendor_id,   v, 8);
    memcpy(product_id,  p, 16);
    memcpy(product_rev, r, 4);
}

bool tud_msc_test_unit_ready_cb(const uint8_t lun)
{
    (void)lun;
    return s_mounted;
}

void tud_msc_capacity_cb(const uint8_t lun, uint32_t * const block_count,
                          uint16_t * const block_size)
{
    (void)lun;
    *block_count = MSC_DISK_BLOCK_COUNT;
    *block_size  = (uint16_t)MSC_BLOCK_SIZE_BYTES;
}

bool tud_msc_start_stop_cb(const uint8_t lun, const uint8_t power_condition,
                            const bool start, const bool load_eject)
{
    (void)lun;
    (void)power_condition;
    (void)start;
    (void)load_eject;
    return true;
}

int32_t tud_msc_read10_cb(const uint8_t lun, const uint32_t lba, const uint32_t offset,
                           void * const buffer, const uint32_t bufsize)
{
    (void)lun;
    const uint32_t addr = (lba * MSC_BLOCK_SIZE_BYTES) + offset;
    if ((addr + bufsize) > sizeof s_ramdisk) {
        return -1;
    }
    memcpy(buffer, &s_ramdisk[addr], bufsize);
    return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(const uint8_t lun, const uint32_t lba, const uint32_t offset,
                            uint8_t * const buffer, const uint32_t bufsize)
{
    (void)lun;
    const uint32_t addr = (lba * MSC_BLOCK_SIZE_BYTES) + offset;
    if ((addr + bufsize) > sizeof s_ramdisk) {
        return -1;
    }
    memcpy(&s_ramdisk[addr], buffer, bufsize);
    return (int32_t)bufsize;
}

bool tud_msc_is_writable_cb(const uint8_t lun)
{
    (void)lun;
    return true;
}

/* tud_msc_scsi_cb is the fallback for any SCSI opcode TinyUSB does not
 * route to a specific callback. For our minimal six-command implementation
 * we return -1 (unsupported) for anything that gets here. */
int32_t tud_msc_scsi_cb(const uint8_t lun, const uint8_t scsi_cmd[16],
                         void * const buffer, const uint16_t bufsize)
{
    (void)lun;
    (void)scsi_cmd;
    (void)buffer;
    (void)bufsize;
    return -1;
}

/* ---- TinyUSB stack callbacks ----------------------------------------- */

void tud_mount_cb(void)
{
    s_mounted = true;
}

void tud_umount_cb(void)
{
    s_mounted = false;
}

void tud_suspend_cb(const bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    s_mounted = false;
}

void tud_resume_cb(void)
{
    s_mounted = true;
}

/* ---- Main ------------------------------------------------------------- */

int main(void)
{
    board_init();
    ramdisk_init();

    if (! tud_init(0u)) {
        for (;;) {
            __asm volatile ("nop");
        }
    }

    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, false);

    for (;;) {
        tud_task();
        gpio_put(LED_PIN, s_mounted);
    }

    return 0;
}

/* ======================================================================
 * Descriptor section
 * ====================================================================== */

/* MSC-only configuration descriptor total length.
 *   9  config
 * + 8  IAD
 * + 9  IF (MSC)
 * + 7  EP 0x84 (bulk-IN)
 * + 7  EP 0x04 (bulk-OUT)
 * = 40 bytes */
#define DESC_LEN_MSC_ONLY_CONFIG                                              \
    ((uint16_t)(DESC_LEN_CONFIG + DESC_LEN_IAD + DESC_LEN_INTERFACE           \
              + DESC_LEN_ENDPOINT + DESC_LEN_ENDPOINT))

static const uint8_t desc_device[DESC_LEN_DEVICE] = {
    DESC_LEN_DEVICE,
    USB_DESC_TYPE_DEVICE,
    0x00u, 0x02u,
    USB_CLASS_MISC,
    USB_SUBCLASS_MISC_COMMON,
    USB_PROTOCOL_MISC_IAD,
    CC_EP0_MAX_PACKET,
    CC_LE_U16_LO(CC_USB_VID),
    CC_LE_U16_HI(CC_USB_VID),
    CC_LE_U16_LO(CC_USB_PID_MSC),
    CC_LE_U16_HI(CC_USB_PID_MSC),
    0x00u, 0x01u,
    CC_STR_MANUFACTURER,
    CC_STR_PRODUCT,
    CC_STR_SERIAL,
    1u
};

static const uint8_t desc_config[DESC_LEN_MSC_ONLY_CONFIG] = {
    /* --- Configuration descriptor (9 bytes) --- */
    DESC_LEN_CONFIG,
    USB_DESC_TYPE_CONFIGURATION,
    CC_LE_U16_LO(DESC_LEN_MSC_ONLY_CONFIG),
    CC_LE_U16_HI(DESC_LEN_MSC_ONLY_CONFIG),
    1u,                          /* bNumInterfaces */
    1u,                          /* bConfigurationValue */
    0u,                          /* iConfiguration */
    0x80u,                       /* bmAttributes */
    50u,                         /* bMaxPower */

    /* --- IAD for MSC (8 bytes) --- */
    DESC_LEN_IAD,
    USB_DESC_TYPE_IAD,
    0u,                          /* bFirstInterface */
    1u,                          /* bInterfaceCount */
    USB_CLASS_MSC,
    USB_SUBCLASS_MSC_SCSI,
    USB_PROTOCOL_MSC_BBB,
    CC_STR_FUNCTION_MSC,

    /* --- MSC interface descriptor (9 bytes) --- */
    DESC_LEN_INTERFACE,
    USB_DESC_TYPE_INTERFACE,
    0u,                          /* bInterfaceNumber */
    0u,                          /* bAlternateSetting */
    2u,                          /* bNumEndpoints */
    USB_CLASS_MSC,
    USB_SUBCLASS_MSC_SCSI,
    USB_PROTOCOL_MSC_BBB,
    0u,                          /* iInterface */

    /* --- MSC bulk-IN endpoint (7 bytes) --- */
    DESC_LEN_ENDPOINT,
    USB_DESC_TYPE_ENDPOINT,
    CC_EP_MSC_IN,
    USB_EP_ATTR_BULK,
    CC_LE_U16_LO(CC_EP_BULK_MAX_PACKET),
    CC_LE_U16_HI(CC_EP_BULK_MAX_PACKET),
    0u,

    /* --- MSC bulk-OUT endpoint (7 bytes) --- */
    DESC_LEN_ENDPOINT,
    USB_DESC_TYPE_ENDPOINT,
    CC_EP_MSC_OUT,
    USB_EP_ATTR_BULK,
    CC_LE_U16_LO(CC_EP_BULK_MAX_PACKET),
    CC_LE_U16_HI(CC_EP_BULK_MAX_PACKET),
    0u
};

_Static_assert(sizeof desc_config == DESC_LEN_MSC_ONLY_CONFIG,
               "desc_config size must equal DESC_LEN_MSC_ONLY_CONFIG");

/* --- String table ----------------------------------------------------- */

static const char * const string_table[] = {
    NULL,
    "Code Crunch Worldwide",
    "C7 Wire MSC RAM Disk",
    "C7-WIRE-W09-E03-0001",
    NULL,
    NULL,
    "USB Mass Storage"
};

#define STRING_TABLE_SIZE  ((uint8_t)(sizeof string_table / sizeof string_table[0]))

static uint16_t s_string_desc_buf[32];

const uint8_t * tud_descriptor_device_cb(void)
{
    return desc_device;
}

const uint8_t * tud_descriptor_configuration_cb(const uint8_t index)
{
    (void)index;
    return desc_config;
}

const uint16_t * tud_descriptor_string_cb(const uint8_t index, const uint16_t langid)
{
    (void)langid;

    if (index == CC_STR_LANGID) {
        s_string_desc_buf[0] = ((uint16_t)USB_DESC_TYPE_STRING << 8) | 4u;
        s_string_desc_buf[1] = 0x0409u;
        return s_string_desc_buf;
    }

    if (index >= STRING_TABLE_SIZE) {
        return NULL;
    }

    const char * const str = string_table[index];
    if (str == NULL) {
        return NULL;
    }

    const size_t max_chars = (sizeof s_string_desc_buf / sizeof s_string_desc_buf[0]) - 1u;
    size_t len = 0u;
    while ((len < max_chars) && (str[len] != '\0')) {
        s_string_desc_buf[1u + len] = (uint16_t)str[len];
        len++;
    }
    const uint16_t bytes = (uint16_t)(2u + (2u * len));
    s_string_desc_buf[0] = ((uint16_t)USB_DESC_TYPE_STRING << 8) | (uint16_t)(bytes & 0xFFu);

    return s_string_desc_buf;
}
