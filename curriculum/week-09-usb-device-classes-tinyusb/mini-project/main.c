/*
 * Week 9 Mini-Project - Composite USB Device: CDC + HID + MSC
 *
 * Three USB classes coexisting on one Pico. The configuration descriptor
 * is 146 bytes, broken down in mini-project/README.md. This file is the
 * main entry point and the place where the three class-service routines
 * live; usb_descriptors.c holds the descriptor tree and the descriptor
 * callbacks.
 *
 * Hardware:
 *   - Raspberry Pi Pico (W or non-W).
 *   - USB cable to the host.
 *   - GP25 on-board LED used as enumeration-state indicator.
 *   - BOOTSEL button used as the HID-keyboard trigger.
 *
 * Build: see mini-project/README.md for the CMakeLists.txt recipe.
 *
 * References:
 *   USB 2.0 spec, Chapter 9 (descriptors):
 *     https://www.usb.org/document-library/usb-20-specification
 *   USB CDC 1.2 spec (CDC functional descriptors):
 *     https://www.usb.org/document-library/class-definitions-communication-devices-12
 *   USB HID 1.11 spec (report descriptor):
 *     https://www.usb.org/document-library/device-class-definition-hid-111
 *   USB MSC BBB 1.0 (CBW/CSW transport):
 *     https://www.usb.org/document-library/mass-storage-bulk-only-10
 *   TinyUSB device-stack overview:
 *     https://docs.tinyusb.org/en/latest/reference/dev_concept.html
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"

#include "bsp/board.h"
#include "tusb.h"

#include "../exercises/usb_descriptors.h"

/* ---- Constants -------------------------------------------------------- */

#define LED_PIN                      ((uint32_t)25u)
#define LED_BLINK_PERIOD_MS          ((uint32_t)500u)

#define CDC_ITF                      ((uint8_t)0u)
#define ECHO_BUF_SIZE                ((uint32_t)64u)

#define KBD_REPORT_SIZE_BYTES        ((uint16_t)8u)
#define KEYSTROKE_DELAY_MS           ((uint32_t)30u)

#define HID_KEY_C                    ((uint8_t)0x06u)
#define HID_KEY_O                    ((uint8_t)0x12u)
#define HID_KEY_D                    ((uint8_t)0x07u)
#define HID_KEY_E                    ((uint8_t)0x08u)
#define HID_KEY_SPACE                ((uint8_t)0x2Cu)
#define HID_KEY_R                    ((uint8_t)0x15u)
#define HID_KEY_U                    ((uint8_t)0x18u)
#define HID_KEY_N                    ((uint8_t)0x11u)
#define HID_KEY_H                    ((uint8_t)0x0Bu)

static const uint8_t s_keystrokes[] = {
    HID_KEY_C, HID_KEY_O, HID_KEY_D, HID_KEY_E,
    HID_KEY_SPACE,
    HID_KEY_C, HID_KEY_R, HID_KEY_U, HID_KEY_N, HID_KEY_C, HID_KEY_H
};
#define KEYSTROKE_COUNT  ((uint32_t)(sizeof s_keystrokes / sizeof s_keystrokes[0]))

/* ---- Module state ----------------------------------------------------- */

static volatile bool s_mounted = false;
static uint32_t      s_last_led_ms = 0u;
static bool          s_led_on = false;

static uint32_t      s_cursor = 0u;
static bool          s_is_key_down = true;
static uint32_t      s_last_keystroke_ms = 0u;
static bool          s_typing = false;
static bool          s_button_prev = false;

/* The 128 KB RAM disk for MSC. */
static uint8_t s_ramdisk[MSC_DISK_SIZE_BYTES] __attribute__((aligned(4)));

/* README content. */
static const char s_readme_body[] =
    "Week 9 - Code Crunch C7 Wire - USB device classes\r\n"
    "\r\n"
    "Composite device: CDC ACM + HID Keyboard + Mass Storage Class.\r\n"
    "Open the CDC port (cu.usbmodem* / COM* / /dev/ttyACM*) for echo.\r\n"
    "Press BOOTSEL with a text editor focused for HID keyboard typing.\r\n"
    "This disk is 128 KB, FAT12, volatile - power-cycle to reset.\r\n";

#define S_README_LEN  ((uint32_t)(sizeof s_readme_body - 1u))

/* FAT12 layout constants (same as Exercise 3). */
#define SECTOR_BOOT       ((uint32_t)0u)
#define SECTOR_FAT1       ((uint32_t)1u)
#define SECTOR_FAT2       ((uint32_t)2u)
#define SECTOR_ROOTDIR    ((uint32_t)3u)
#define SECTOR_DATA_START ((uint32_t)4u)
#define README_CLUSTER    ((uint16_t)2u)

static const uint8_t s_boot_sector_template[62] = {
    0xEBu, 0x3Cu, 0x90u,
    'M','S','D','O','S','5','.','0',
    0x00u, 0x02u,
    0x01u,
    0x01u, 0x00u,
    0x02u,
    0x10u, 0x00u,
    0x00u, 0x01u,
    0xF8u,
    0x01u, 0x00u,
    0x20u, 0x00u,
    0x40u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u,
    0x80u,
    0x00u,
    0x29u,
    0x12u, 0x34u, 0x56u, 0x78u,
    'C','R','U','N','C','H',' ',' ',' ',' ',' ',
    'F','A','T','1','2',' ',' ',' '
};

static const uint8_t s_fat_template[6] = {
    0xF8u, 0xFFu, 0xFFu,
    0xFFu, 0x0Fu, 0x00u
};

/* ---- Forward declarations -------------------------------------------- */

static void led_init(void);
static void led_service(uint32_t now_ms);
static void cdc_echo_service(void);
static void typing_service(uint32_t now_ms);
static bool get_bootsel_button(void);
static void button_service(uint32_t now_ms);
static void ramdisk_init(void);
static void send_keyboard_report(uint8_t key_code);

/* ---- LED ------------------------------------------------------------- */

static void led_init(void)
{
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, false);
    s_led_on = false;
}

static void led_service(const uint32_t now_ms)
{
    if (! s_mounted) {
        if (s_led_on) {
            gpio_put(LED_PIN, false);
            s_led_on = false;
        }
        return;
    }

    if ((now_ms - s_last_led_ms) >= LED_BLINK_PERIOD_MS) {
        s_last_led_ms = now_ms;
        s_led_on = ! s_led_on;
        gpio_put(LED_PIN, s_led_on);
    }
}

/* ---- CDC echo --------------------------------------------------------- */

static void cdc_echo_service(void)
{
    if (! tud_cdc_n_available(CDC_ITF)) {
        return;
    }

    uint8_t buf[ECHO_BUF_SIZE];
    const uint32_t avail_in = tud_cdc_n_available(CDC_ITF);
    const uint32_t take = (avail_in > sizeof buf) ? (uint32_t)sizeof buf : avail_in;

    const uint32_t got = tud_cdc_n_read(CDC_ITF, buf, take);
    if (got == 0u) {
        return;
    }

    const uint32_t avail_out = tud_cdc_n_write_available(CDC_ITF);
    const uint32_t to_write  = (got > avail_out) ? avail_out : got;

    if (to_write > 0u) {
        (void)tud_cdc_n_write(CDC_ITF, buf, to_write);
        (void)tud_cdc_n_write_flush(CDC_ITF);
    }
}

/* ---- HID keyboard ---------------------------------------------------- */

static bool __attribute__((noinline)) get_bootsel_button(void)
{
    /* BOOTSEL reader - see exercise-02-hid-keyboard.c for the explanation
     * of why this sequence is needed. */
    const uint32_t flash_cs_pin = 1u;
    const uint32_t save = save_and_disable_interrupts();
    hw_write_masked(&padsbank0_hw->io[flash_cs_pin],
                    GPIO_OVERRIDE_NORMAL << PADS_BANK0_GPIO0_PUE_LSB,
                    PADS_BANK0_GPIO0_PUE_BITS | PADS_BANK0_GPIO0_PDE_BITS);
    for (volatile uint32_t i = 0u; i < 1000u; ++i) {
        __asm volatile ("nop");
    }
    const bool pressed = (sio_hw->gpio_hi_in & (1u << flash_cs_pin)) == 0u;
    hw_write_masked(&padsbank0_hw->io[flash_cs_pin],
                    GPIO_OVERRIDE_NORMAL << PADS_BANK0_GPIO0_PUE_LSB,
                    PADS_BANK0_GPIO0_PUE_BITS);
    restore_interrupts(save);
    return pressed;
}

static void button_service(const uint32_t now_ms)
{
    (void)now_ms;
    const bool pressed = get_bootsel_button();
    if (pressed && ! s_button_prev && ! s_typing) {
        s_typing = true;
        s_cursor = 0u;
        s_is_key_down = true;
        s_last_keystroke_ms = board_millis() - KEYSTROKE_DELAY_MS;
    }
    s_button_prev = pressed;
}

static void send_keyboard_report(const uint8_t key_code)
{
    uint8_t report[KBD_REPORT_SIZE_BYTES] = { 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
    report[2] = key_code;
    if (tud_hid_ready()) {
        (void)tud_hid_report(0u, report, KBD_REPORT_SIZE_BYTES);
    }
}

static void typing_service(const uint32_t now_ms)
{
    if (! s_typing || ! s_mounted) {
        return;
    }
    if ((now_ms - s_last_keystroke_ms) < KEYSTROKE_DELAY_MS) {
        return;
    }
    if (! tud_hid_ready()) {
        return;
    }

    if (s_cursor >= KEYSTROKE_COUNT) {
        send_keyboard_report(0u);
        s_typing = false;
        return;
    }

    if (s_is_key_down) {
        send_keyboard_report(s_keystrokes[s_cursor]);
        s_is_key_down = false;
    } else {
        send_keyboard_report(0u);
        s_is_key_down = true;
        s_cursor++;
    }
    s_last_keystroke_ms = now_ms;
}

/* ---- MSC RAM disk init ----------------------------------------------- */

static void ramdisk_init(void)
{
    memset(s_ramdisk, 0, sizeof s_ramdisk);

    uint8_t * const boot = &s_ramdisk[SECTOR_BOOT * MSC_BLOCK_SIZE_BYTES];
    memcpy(boot, s_boot_sector_template, sizeof s_boot_sector_template);
    boot[510] = 0x55u;
    boot[511] = 0xAAu;

    uint8_t * const fat1 = &s_ramdisk[SECTOR_FAT1 * MSC_BLOCK_SIZE_BYTES];
    uint8_t * const fat2 = &s_ramdisk[SECTOR_FAT2 * MSC_BLOCK_SIZE_BYTES];
    memcpy(fat1, s_fat_template, sizeof s_fat_template);
    memcpy(fat2, s_fat_template, sizeof s_fat_template);

    uint8_t * const rootdir = &s_ramdisk[SECTOR_ROOTDIR * MSC_BLOCK_SIZE_BYTES];
    const uint8_t entry[32] = {
        'R','E','A','D','M','E',' ',' ',
        'T','X','T',
        0x20u,
        0x00u,
        0x00u,
        0x00u, 0x00u,
        0x21u, 0x59u,
        0x21u, 0x59u,
        0x00u, 0x00u,
        0x00u, 0x00u,
        0x21u, 0x59u,
        CC_LE_U16_LO(README_CLUSTER),
        CC_LE_U16_HI(README_CLUSTER),
        (uint8_t)(S_README_LEN & 0xFFu),
        (uint8_t)((S_README_LEN >> 8) & 0xFFu),
        (uint8_t)((S_README_LEN >> 16) & 0xFFu),
        (uint8_t)((S_README_LEN >> 24) & 0xFFu)
    };
    memcpy(rootdir, entry, sizeof entry);

    uint8_t * const data = &s_ramdisk[SECTOR_DATA_START * MSC_BLOCK_SIZE_BYTES];
    memcpy(data, s_readme_body, S_README_LEN);
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

void tud_msc_capacity_cb(const uint8_t lun, uint32_t * const block_count, uint16_t * const block_size)
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

int32_t tud_msc_scsi_cb(const uint8_t lun, const uint8_t scsi_cmd[16],
                         void * const buffer, const uint16_t bufsize)
{
    (void)lun;
    (void)scsi_cmd;
    (void)buffer;
    (void)bufsize;
    return -1;
}

/* ---- TinyUSB HID callbacks ------------------------------------------ */

uint16_t tud_hid_get_report_cb(const uint8_t itf, const uint8_t report_id,
                                const hid_report_type_t report_type,
                                uint8_t * const buffer, const uint16_t reqlen)
{
    (void)itf;
    (void)report_id;
    (void)report_type;
    if (reqlen < KBD_REPORT_SIZE_BYTES) {
        return 0u;
    }
    memset(buffer, 0, KBD_REPORT_SIZE_BYTES);
    return KBD_REPORT_SIZE_BYTES;
}

void tud_hid_set_report_cb(const uint8_t itf, const uint8_t report_id,
                            const hid_report_type_t report_type,
                            const uint8_t * const buffer, const uint16_t bufsize)
{
    (void)itf;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}

/* ---- TinyUSB CDC callbacks ------------------------------------------- */

void tud_cdc_rx_cb(const uint8_t itf)
{
    (void)itf;
}

void tud_cdc_line_coding_cb(const uint8_t itf, const cdc_line_coding_t * const coding)
{
    (void)itf;
    (void)coding;
}

/* ---- TinyUSB stack callbacks ----------------------------------------- */

void tud_mount_cb(void)
{
    s_mounted = true;
    s_last_led_ms = board_millis();
}

void tud_umount_cb(void)
{
    s_mounted = false;
    s_typing = false;
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

    led_init();
    s_last_led_ms = board_millis();
    s_last_keystroke_ms = board_millis();

    for (;;) {
        tud_task();
        const uint32_t now = board_millis();
        cdc_echo_service();
        button_service(now);
        typing_service(now);
        led_service(now);
        /* WFI yields the core until the next IRQ. On the RP2040, the USB
         * IRQ, the SysTick (1 ms), and the GPIO IRQ on BOOTSEL all wake
         * us. The 1 ms tick keeps us responsive to button presses; the
         * USB IRQ keeps us responsive to host traffic. */
        __asm volatile ("wfi");
    }

    return 0;
}
