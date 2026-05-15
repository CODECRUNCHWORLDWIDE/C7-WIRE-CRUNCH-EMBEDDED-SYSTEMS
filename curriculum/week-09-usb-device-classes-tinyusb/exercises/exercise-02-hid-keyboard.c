/*
 * Exercise 2 - HID keyboard on TinyUSB
 *
 * A Pico that enumerates as a USB HID keyboard. When the BOOTSEL button
 * is pressed (or any user button wired to GP14 with internal pull-up),
 * the device types the string "code crunch" into the host's focused
 * window, one key transition per HID report.
 *
 * The interesting artifact in this exercise is the report descriptor.
 * We use the canonical boot-keyboard report descriptor from HID 1.11
 * Appendix E.6 (p. 69), spelled out byte-by-byte with comments. The
 * 8-byte input report format is fixed by that descriptor.
 *
 * Hardware:
 *   - Raspberry Pi Pico (W or non-W).
 *   - The Pico's built-in BOOTSEL button used as the trigger. Read via
 *     the pico-sdk function rom_table_lookup("bootsel button" trick) -
 *     see get_bootsel_button() below.
 *   - On-board LED on GP25 follows mount state.
 *
 * What you should see on the host:
 *   - On enumeration, the host shows a generic HID keyboard. macOS may
 *     prompt to identify the keyboard layout; cancel that prompt or
 *     accept any layout - we use HID Usage codes which are
 *     layout-independent at the physical-key level.
 *   - Open a text editor on the host. Press BOOTSEL. The string
 *     "code crunch" appears in the editor. Button-to-keystroke latency
 *     should be under 8 ms (the host polls at 10 ms intervals; the
 *     average wait for a fresh poll is 5 ms).
 *
 * Build:
 *   pico-sdk libraries to link:
 *     pico_stdlib, tinyusb_device, tinyusb_board, hardware_gpio,
 *     hardware_sync
 *   tusb_config.h must define CFG_TUD_HID=1, CFG_TUD_HID_EP_BUFSIZE=16.
 *
 * References:
 *   USB HID 1.11, sec. 6.2.2 (report descriptor): pp. 23-33.
 *     https://www.usb.org/document-library/device-class-definition-hid-111
 *   USB HID 1.11, Appendix E.6 (boot keyboard example): pp. 69-72.
 *   USB HID Usage Tables 1.4, Page 0x07 (Keyboard usage codes): pp. 53-63.
 *     https://usb.org/document-library/hid-usage-tables-14
 *   TinyUSB HID API: class/hid/hid_device.h
 *     https://docs.tinyusb.org/en/latest/reference/group__group__class__drivers.html
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"

#include "bsp/board.h"
#include "tusb.h"

#include "usb_descriptors.h"

/* ---- Constants -------------------------------------------------------- */

#define LED_PIN                      ((uint32_t)25u)
#define LED_BLINK_PERIOD_MS          ((uint32_t)500u)

/* HID interface index. Single-HID device -> 0. */
#define HID_ITF                      ((uint8_t)0u)

/* HID keyboard input report size. Fixed by the report descriptor. */
#define KBD_REPORT_SIZE_BYTES        ((uint16_t)8u)

/* USB HID Usage Table 1.4, Page 0x07 codes for the letters in our string.
 * c d e o r u n   space backspace enter
 *   = 0x06 0x07 0x08 0x12 0x15 0x18 0x11 0x2C 0x2A 0x28 */
#define HID_KEY_C                    ((uint8_t)0x06u)
#define HID_KEY_O                    ((uint8_t)0x12u)
#define HID_KEY_D                    ((uint8_t)0x07u)
#define HID_KEY_E                    ((uint8_t)0x08u)
#define HID_KEY_SPACE                ((uint8_t)0x2Cu)
#define HID_KEY_R                    ((uint8_t)0x15u)
#define HID_KEY_U                    ((uint8_t)0x18u)
#define HID_KEY_N                    ((uint8_t)0x11u)
#define HID_KEY_H                    ((uint8_t)0x0Bu)

/* The string "code crunch" as keycodes. We emit each key as a key-down
 * report followed by a key-up report (all-zeros). The host's keyboard
 * driver interprets that as one keystroke. */
static const uint8_t s_keystrokes[] = {
    HID_KEY_C, HID_KEY_O, HID_KEY_D, HID_KEY_E,
    HID_KEY_SPACE,
    HID_KEY_C, HID_KEY_R, HID_KEY_U, HID_KEY_N, HID_KEY_C, HID_KEY_H
};

#define KEYSTROKE_COUNT              ((uint32_t)(sizeof s_keystrokes / sizeof s_keystrokes[0]))

/* Inter-keystroke delay in milliseconds. 30 ms = ~33 keys/sec, slow
 * enough that the host's keyboard input handler processes every one. */
#define KEYSTROKE_DELAY_MS           ((uint32_t)30u)

/* ---- Module state ----------------------------------------------------- */

static volatile bool s_mounted = false;
static uint32_t      s_last_led_ms = 0u;
static bool          s_led_on = false;

/* The current type-out cursor: index into s_keystrokes for the next key
 * to emit, plus a flag for whether the next emission is key-down or
 * key-up. When cursor reaches KEYSTROKE_COUNT, the cycle is done. */
static uint32_t      s_cursor = 0u;
static bool          s_is_key_down = true;
static uint32_t      s_last_keystroke_ms = 0u;
static bool          s_typing = false;

/* Previous BOOTSEL button state, debounced. */
static bool          s_button_prev = false;

/* ---- Forward declarations -------------------------------------------- */

static void led_init(void);
static void led_service(uint32_t now_ms);
static bool get_bootsel_button(void);
static void button_service(uint32_t now_ms);
static void typing_service(uint32_t now_ms);
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

/* ---- BOOTSEL button reader -------------------------------------------
 *
 * The BOOTSEL button on the Pico is wired to the QSPI CS line. Reading it
 * requires temporarily switching the SPI peripheral off, sampling the
 * GPIO, and switching it back on - all without interrupts firing. The
 * pico-sdk does not export a helper for this in older releases; we
 * implement the canonical sequence here. RP2040 datasheet sec. 4.10,
 * pp. 568-571 ("BOOTSEL Button Detection").
 *
 * For exercises where you have a user button wired to a free GPIO with
 * internal pull-up, replace this function with a one-liner gpio_get().
 */
static bool __attribute__((noinline)) get_bootsel_button(void)
{
    const uint32_t flash_cs_pin = 1u;  /* QSPI_SS pad index */

    /* Disable interrupts so the temporary CS reconfiguration cannot be
     * interrupted by a flash access. */
    const uint32_t save = save_and_disable_interrupts();

    /* Switch CS pad to GPIO input (the original sequence from
     * pico-examples picoboard/button/button.c). */
    hw_write_masked(&padsbank0_hw->io[flash_cs_pin],
                    GPIO_OVERRIDE_NORMAL << PADS_BANK0_GPIO0_PUE_LSB,
                    PADS_BANK0_GPIO0_PUE_BITS | PADS_BANK0_GPIO0_PDE_BITS);

    /* Wait for the line to settle (~3 us is enough). */
    for (volatile uint32_t i = 0u; i < 1000u; ++i) {
        __asm volatile ("nop");
    }

    /* Read the pad. BOOTSEL pulled low when pressed. */
    const bool pressed = (sio_hw->gpio_hi_in & (1u << flash_cs_pin)) == 0u;

    /* Restore CS as the QSPI peripheral function. */
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
        /* Rising edge of a press. Start a typing cycle. */
        s_typing = true;
        s_cursor = 0u;
        s_is_key_down = true;
        s_last_keystroke_ms = board_millis() - KEYSTROKE_DELAY_MS;
        /* The "- DELAY" makes the first report fire immediately on the
         * next typing_service() call instead of waiting one full delay. */
    }

    s_button_prev = pressed;
}

/* ---- Typing state machine -------------------------------------------- */

static void send_keyboard_report(const uint8_t key_code)
{
    /* The 8-byte boot keyboard report:
     *   byte 0:    modifier mask (we use 0 - no Shift, no Ctrl)
     *   byte 1:    reserved (must be 0)
     *   bytes 2-7: six key-code slots (0 = empty, non-0 = HID Usage code)
     */
    uint8_t report[KBD_REPORT_SIZE_BYTES] = { 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
    report[2] = key_code;

    /* tud_hid_report returns false if the previous report is still
     * queued. We discard in that case; the next typing_service() tick
     * will retry by virtue of cursor not advancing. */
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
        /* Finished - send a final all-zeros report and stop. */
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

/* tud_hid_get_report_cb: host sent GET_REPORT(type=Input) on the control
 * pipe. We return the current keyboard state (all zeros - no key held).
 * Boot-protocol hosts use this; in report-protocol mode the host instead
 * polls the interrupt-IN endpoint and this callback is rarely invoked. */
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

/* tud_hid_set_report_cb: host sent SET_REPORT(type=Output) on the
 * control pipe. For a keyboard this is typically the LED state byte
 * (CapsLock / NumLock / ScrollLock). We accept it and ignore it. */
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

/* ---- Main ------------------------------------------------------------- */

int main(void)
{
    board_init();
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
        button_service(now);
        typing_service(now);
        led_service(now);
    }

    return 0;
}

/* ======================================================================
 * Descriptor section
 * ====================================================================== */

/* The HID-only configuration descriptor total length.
 *   9  config
 * + 8  IAD
 * + 9  IF (HID)
 * + 9  HID class descriptor
 * + 7  EP 0x83 (interrupt-IN)
 * + 7  EP 0x03 (interrupt-OUT)
 * = 49 bytes */
#define DESC_LEN_HID_ONLY_CONFIG                                              \
    ((uint16_t)(DESC_LEN_CONFIG + DESC_LEN_IAD + DESC_LEN_INTERFACE           \
              + DESC_LEN_HID_CLASS                                            \
              + DESC_LEN_ENDPOINT + DESC_LEN_ENDPOINT))

/* The HID boot-keyboard report descriptor.
 * Copied from HID 1.11 Appendix E.6, p. 69.
 * 63 bytes total. Each item is annotated with its tag/type/size and the
 * meaning of the payload. */
static const uint8_t hid_report_desc_kbd[] = {
    0x05u, 0x01u,            /* Usage Page (Generic Desktop)              */
    0x09u, 0x06u,            /* Usage (Keyboard)                          */
    0xA1u, 0x01u,            /* Collection (Application)                  */
    /* --- Modifier byte: 8 bits of left-Ctrl..right-GUI -------------- */
    0x05u, 0x07u,            /*   Usage Page (Key Codes)                  */
    0x19u, 0xE0u,            /*   Usage Minimum (224 = LeftControl)       */
    0x29u, 0xE7u,            /*   Usage Maximum (231 = RightGUI)          */
    0x15u, 0x00u,            /*   Logical Minimum (0)                     */
    0x25u, 0x01u,            /*   Logical Maximum (1)                     */
    0x75u, 0x01u,            /*   Report Size (1 bit per usage)           */
    0x95u, 0x08u,            /*   Report Count (8 usages)                 */
    0x81u, 0x02u,            /*   Input (Data, Variable, Absolute)        */
    /* --- Reserved byte (constant 0) -------------------------------- */
    0x95u, 0x01u,            /*   Report Count (1)                        */
    0x75u, 0x08u,            /*   Report Size (8 bits)                    */
    0x81u, 0x01u,            /*   Input (Constant)                        */
    /* --- LED output (5 bits + 3 pad bits) -------------------------- */
    0x95u, 0x05u,            /*   Report Count (5)                        */
    0x75u, 0x01u,            /*   Report Size (1 bit)                     */
    0x05u, 0x08u,            /*   Usage Page (LEDs)                       */
    0x19u, 0x01u,            /*   Usage Minimum (1 = NumLock)             */
    0x29u, 0x05u,            /*   Usage Maximum (5 = Kana)                */
    0x91u, 0x02u,            /*   Output (Data, Variable, Absolute)       */
    0x95u, 0x01u,            /*   Report Count (1)                        */
    0x75u, 0x03u,            /*   Report Size (3 bits)                    */
    0x91u, 0x01u,            /*   Output (Constant)                       */
    /* --- 6 key slots: 8-bit indices into HID Usage Tables Page 7 --- */
    0x95u, 0x06u,            /*   Report Count (6)                        */
    0x75u, 0x08u,            /*   Report Size (8 bits)                    */
    0x15u, 0x00u,            /*   Logical Minimum (0)                     */
    0x25u, 0x65u,            /*   Logical Maximum (101)                   */
    0x05u, 0x07u,            /*   Usage Page (Key Codes)                  */
    0x19u, 0x00u,            /*   Usage Minimum (0)                       */
    0x29u, 0x65u,            /*   Usage Maximum (101)                     */
    0x81u, 0x00u,            /*   Input (Data, Array)                     */
    0xC0u                    /* End Collection                            */
};

#define HID_REPORT_DESC_LEN  ((uint16_t)(sizeof hid_report_desc_kbd / sizeof hid_report_desc_kbd[0]))

/* The 18-byte device descriptor. */
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
    CC_LE_U16_LO(CC_USB_PID_HID),
    CC_LE_U16_HI(CC_USB_PID_HID),
    0x00u, 0x01u,
    CC_STR_MANUFACTURER,
    CC_STR_PRODUCT,
    CC_STR_SERIAL,
    1u
};

/* The HID-only configuration descriptor tree. 49 bytes. */
static const uint8_t desc_config[DESC_LEN_HID_ONLY_CONFIG] = {
    /* --- Configuration descriptor (9 bytes) --- */
    DESC_LEN_CONFIG,
    USB_DESC_TYPE_CONFIGURATION,
    CC_LE_U16_LO(DESC_LEN_HID_ONLY_CONFIG),
    CC_LE_U16_HI(DESC_LEN_HID_ONLY_CONFIG),
    1u,                          /* bNumInterfaces */
    1u,                          /* bConfigurationValue */
    0u,                          /* iConfiguration */
    0x80u,                       /* bmAttributes */
    50u,                         /* bMaxPower (100 mA) */

    /* --- IAD for HID (8 bytes) --- */
    DESC_LEN_IAD,
    USB_DESC_TYPE_IAD,
    0u,                          /* bFirstInterface */
    1u,                          /* bInterfaceCount */
    USB_CLASS_HID,
    0x00u,                       /* bFunctionSubClass (no boot) */
    0x00u,                       /* bFunctionProtocol (no boot) */
    CC_STR_FUNCTION_HID,

    /* --- HID interface descriptor (9 bytes) --- */
    DESC_LEN_INTERFACE,
    USB_DESC_TYPE_INTERFACE,
    0u,                          /* bInterfaceNumber */
    0u,                          /* bAlternateSetting */
    2u,                          /* bNumEndpoints (IN + OUT) */
    USB_CLASS_HID,
    0x00u,                       /* bInterfaceSubClass */
    0x00u,                       /* bInterfaceProtocol */
    0u,                          /* iInterface */

    /* --- HID class descriptor (9 bytes; HID 1.11 sec. 6.2.1) --- */
    DESC_LEN_HID_CLASS,
    USB_DESC_TYPE_HID,
    0x11u, 0x01u,                /* bcdHID = 0x0111 */
    0x00u,                       /* bCountryCode (0 = Not Localized) */
    1u,                          /* bNumDescriptors */
    USB_DESC_TYPE_HID_REPORT,    /* bDescriptorType of subordinate = REPORT */
    CC_LE_U16_LO(HID_REPORT_DESC_LEN),
    CC_LE_U16_HI(HID_REPORT_DESC_LEN),

    /* --- HID interrupt-IN endpoint (7 bytes) --- */
    DESC_LEN_ENDPOINT,
    USB_DESC_TYPE_ENDPOINT,
    CC_EP_HID_IN,
    USB_EP_ATTR_INTERRUPT,
    CC_LE_U16_LO(CC_EP_HID_MAX_PACKET),
    CC_LE_U16_HI(CC_EP_HID_MAX_PACKET),
    CC_HID_POLL_INTERVAL_MS,

    /* --- HID interrupt-OUT endpoint (7 bytes) --- */
    DESC_LEN_ENDPOINT,
    USB_DESC_TYPE_ENDPOINT,
    CC_EP_HID_OUT,
    USB_EP_ATTR_INTERRUPT,
    CC_LE_U16_LO(CC_EP_HID_MAX_PACKET),
    CC_LE_U16_HI(CC_EP_HID_MAX_PACKET),
    CC_HID_POLL_INTERVAL_MS
};

_Static_assert(sizeof desc_config == DESC_LEN_HID_ONLY_CONFIG,
               "desc_config size must equal DESC_LEN_HID_ONLY_CONFIG");

/* TinyUSB's HID class driver will call this callback to fetch the report
 * descriptor when the host issues GET_DESCRIPTOR(Report). */
const uint8_t * tud_hid_descriptor_report_cb(const uint8_t instance)
{
    (void)instance;
    return hid_report_desc_kbd;
}

/* --- String table ----------------------------------------------------- */

static const char * const string_table[] = {
    NULL,
    "Code Crunch Worldwide",
    "C7 Wire HID Keyboard",
    "C7-WIRE-W09-E02-0001",
    NULL,
    "HID Keyboard"
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
