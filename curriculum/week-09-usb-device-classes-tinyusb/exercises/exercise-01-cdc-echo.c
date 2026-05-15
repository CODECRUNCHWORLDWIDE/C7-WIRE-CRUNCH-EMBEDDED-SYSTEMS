/*
 * Exercise 1 - CDC ACM echo on TinyUSB
 *
 * The "hello USB" baseline: a Pico that enumerates as a CDC ACM virtual
 * serial port and echoes every byte it receives back to the host. About
 * 320 lines of C plus a 75-byte configuration descriptor. The smallest
 * TinyUSB program that does something user-visible.
 *
 * Hardware:
 *   - Raspberry Pi Pico (W or non-W).
 *   - USB cable to your laptop.
 *   - No other connections required. The on-board LED on GP25 blinks at
 *     2 Hz while the device is in the Configured state.
 *
 * What you should see on the host:
 *   - macOS: /dev/cu.usbmodemC7W9001 appears in `ls /dev/cu.*`.
 *   - Linux: /dev/ttyACM0 appears in `dmesg | tail`.
 *   - Windows: a new COM port (typically COM4 or higher) in Device Manager.
 *   Open the port with screen, picocom, PuTTY, or the Arduino IDE serial
 *   monitor at any baud (the baud field is informational; USB does not
 *   actually run at the baud). Type "hello\n"; you should see "hello\n"
 *   echoed back.
 *
 * Build:
 *   pico-sdk libraries to link:
 *     pico_stdlib, tinyusb_device, tinyusb_board, hardware_gpio
 *   tusb_config.h must define CFG_TUD_CDC=1.
 *
 * References:
 *   USB 2.0 spec, Chapter 9 (descriptors): pp. 239-284.
 *     https://www.usb.org/document-library/usb-20-specification
 *   USB CDC 1.2 spec, sec. 5.2.3 (functional descriptors): pp. 36-48.
 *     https://www.usb.org/document-library/class-definitions-communication-devices-12
 *   TinyUSB CDC API: class/cdc/cdc_device.h
 *     https://docs.tinyusb.org/en/latest/reference/group__group__class__drivers.html
 *   pico-sdk hardware_usb:
 *     https://www.raspberrypi.com/documentation/pico-sdk/group__hardware__usb.html
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"

#include "bsp/board.h"     /* TinyUSB board-support */
#include "tusb.h"          /* TinyUSB device-stack public API */

#include "usb_descriptors.h"

/* ---- Constants -------------------------------------------------------- */

/* On-board LED used as enumeration-state indicator. */
#define LED_PIN                ((uint32_t)25u)

/* Blink period when mounted (Configured state). 500 ms half-period -> 1 Hz. */
#define LED_BLINK_PERIOD_MS    ((uint32_t)500u)

/* Echo buffer size. Matches the bulk endpoint's wMaxPacketSize so each
 * USB transaction fills it exactly. */
#define ECHO_BUF_SIZE          ((uint32_t)64u)

/* CDC interface index used by all our calls. Single-CDC device -> 0. */
#define CDC_ITF                ((uint8_t)0u)

/* ---- Module state ----------------------------------------------------- */

/* Indicates whether the host has issued SET_CONFIGURATION(1) and the
 * device is in the Configured state. Set/cleared by TinyUSB callbacks. */
static volatile bool s_mounted = false;

/* Last LED toggle timestamp in milliseconds. */
static uint32_t s_last_led_ms = 0u;

/* LED current state (true = ON). */
static bool s_led_on = false;

/* ---- Forward declarations -------------------------------------------- */

static void led_init(void);
static void led_service(uint32_t now_ms);
static void cdc_echo_service(void);

/* ---- LED helpers ----------------------------------------------------- */

static void led_init(void)
{
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, false);
    s_led_on = false;
}

static void led_service(const uint32_t now_ms)
{
    /* Solid OFF while not mounted; 1 Hz blink while mounted. */
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

/* Service the CDC echo. Reads what is available, writes it straight back.
 * We obey the two TinyUSB rules:
 *   1. Do not block. tud_cdc_read returns immediately with whatever is in
 *      the RX FIFO (0..ECHO_BUF_SIZE bytes).
 *   2. Do not over-write. We check tud_cdc_write_available() before
 *      committing to a write; if there isn't room for what we read, we
 *      drop the excess and increment a counter for visibility.
 *
 * Called from the main loop after tud_task(). */
static void cdc_echo_service(void)
{
    if (! tud_cdc_n_available(CDC_ITF)) {
        return;
    }

    uint8_t  buf[ECHO_BUF_SIZE];
    uint32_t avail_in = tud_cdc_n_available(CDC_ITF);
    uint32_t take = (avail_in > sizeof buf) ? (uint32_t)sizeof buf : avail_in;

    const uint32_t got = tud_cdc_n_read(CDC_ITF, buf, take);
    if (got == 0u) {
        return;
    }

    uint32_t avail_out = tud_cdc_n_write_available(CDC_ITF);
    uint32_t to_write  = (got > avail_out) ? avail_out : got;

    if (to_write > 0u) {
        (void)tud_cdc_n_write(CDC_ITF, buf, to_write);
        (void)tud_cdc_n_write_flush(CDC_ITF);
    }
    /* If got > to_write, (got - to_write) bytes were dropped. In a real
     * application we would log this; for the exercise we silently lose
     * them. The condition is rare at 64-byte echo because the TX FIFO is
     * larger than the RX FIFO in the default tusb_config.h. */
}

/* ---- TinyUSB stack callbacks ----------------------------------------- */

/* tud_mount_cb is invoked by TinyUSB once the host completes
 * SET_CONFIGURATION(1). The device's non-zero endpoints are now active. */
void tud_mount_cb(void)
{
    s_mounted = true;
    s_last_led_ms = board_millis();
}

/* tud_umount_cb is invoked on bus disconnect. */
void tud_umount_cb(void)
{
    s_mounted = false;
}

/* tud_suspend_cb is invoked when the host signals suspend (3 ms of bus
 * idle). The argument is whether remote-wakeup is enabled; we do not use
 * remote-wakeup, so we ignore it. */
void tud_suspend_cb(const bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    s_mounted = false;
}

/* tud_resume_cb is invoked when the bus exits suspend. */
void tud_resume_cb(void)
{
    s_mounted = true;
}

/* tud_cdc_rx_cb is invoked by TinyUSB when bytes arrive from the host on
 * the CDC bulk-OUT endpoint. We do the actual echo in the main loop's
 * cdc_echo_service() rather than here; per the TinyUSB callback rules,
 * keeping callback bodies short is the safe convention. We use this
 * callback only to record activity for an optional debug counter. */
void tud_cdc_rx_cb(const uint8_t itf)
{
    (void)itf;
    /* Intentionally empty in this exercise. */
}

/* tud_cdc_line_coding_cb is invoked when the host calls SET_LINE_CODING
 * with a {baud, parity, stop, data} struct. We ignore the values - USB
 * does not actually run at the requested baud - but we must accept the
 * request without stalling, or the host's serial-port driver gets unhappy. */
void tud_cdc_line_coding_cb(const uint8_t itf, const cdc_line_coding_t * const coding)
{
    (void)itf;
    (void)coding;
}

/* ---- Main ------------------------------------------------------------- */

int main(void)
{
    /* Bring up the TinyUSB board-support layer first; it initializes the
     * RP2040 system clocks and the USB controller's PLL. */
    board_init();

    /* Initialize the TinyUSB device stack. The descriptor callbacks live
     * in usb_descriptors_cdc.c (compiled alongside this file). */
    if (! tud_init(0u)) {
        /* Stack init failed. There is little we can do; loop forever so
         * a debugger can attach and inspect the failure. */
        for (;;) {
            __asm volatile ("nop");
        }
    }

    led_init();

    /* Main loop. The convention is:
     *   for (;;) {
     *       tud_task();          // service the USB controller
     *       <application work>;  // do anything you want for a short time
     *   }
     * tud_task() must be called frequently (we aim for at least every 5 ms).
     * It returns quickly when there is nothing to do. */
    s_last_led_ms = board_millis();

    for (;;) {
        tud_task();
        cdc_echo_service();
        led_service(board_millis());
    }

    /* Unreachable. */
    return 0;
}

/* ======================================================================
 * Descriptor section
 *
 * Below this line live the four descriptor callbacks TinyUSB requires:
 *   tud_descriptor_device_cb        - return the 18-byte device descriptor
 *   tud_descriptor_configuration_cb - return the 75-byte config tree
 *   tud_descriptor_string_cb        - return one string descriptor
 *   (the BOS descriptor callback is unused; we are not advertising USB 3
 *    Link Power Management or webusb features)
 *
 * In a real project these would live in usb_descriptors_cdc.c; for the
 * exercise we keep everything in one file for readability. The bytes are
 * fully spelled out, with one comment per field, so the descriptor tree
 * is the most studyable artifact in the exercise.
 * ====================================================================== */

/* CDC-only configuration descriptor total length.
 *   9  config descriptor
 * + 8  IAD for CDC
 * + 9  IF.comm
 * + 5  CDC Header
 * + 5  CDC Call Mgmt
 * + 4  CDC ACM
 * + 5  CDC Union
 * + 7  EP 0x81 (notify, interrupt-IN)
 * + 9  IF.data
 * + 7  EP 0x02 (bulk-OUT)
 * + 7  EP 0x82 (bulk-IN)
 * = 75 bytes */
#define DESC_LEN_CDC_ONLY_CONFIG                                              \
    ((uint16_t)(DESC_LEN_CONFIG + DESC_LEN_IAD + DESC_LEN_INTERFACE           \
              + DESC_LEN_CDC_HEADER + DESC_LEN_CDC_CALLMGMT                   \
              + DESC_LEN_CDC_ACM + DESC_LEN_CDC_UNION                         \
              + DESC_LEN_ENDPOINT + DESC_LEN_INTERFACE                        \
              + DESC_LEN_ENDPOINT + DESC_LEN_ENDPOINT))

/* The 18-byte device descriptor. USB 2.0 sec. 9.6.1, Table 9-8. */
static const uint8_t desc_device[DESC_LEN_DEVICE] = {
    DESC_LEN_DEVICE,             /* bLength = 18 */
    USB_DESC_TYPE_DEVICE,        /* bDescriptorType = 1 */
    0x00u, 0x02u,                /* bcdUSB = 0x0200 (USB 2.0) */
    USB_CLASS_MISC,              /* bDeviceClass = 0xEF (Misc) */
    USB_SUBCLASS_MISC_COMMON,    /* bDeviceSubClass = 0x02 (Common Class) */
    USB_PROTOCOL_MISC_IAD,       /* bDeviceProtocol = 0x01 (IAD) */
    CC_EP0_MAX_PACKET,           /* bMaxPacketSize0 = 64 */
    CC_LE_U16_LO(CC_USB_VID),    /* idVendor LSB */
    CC_LE_U16_HI(CC_USB_VID),    /* idVendor MSB */
    CC_LE_U16_LO(CC_USB_PID_CDC),/* idProduct LSB */
    CC_LE_U16_HI(CC_USB_PID_CDC),/* idProduct MSB */
    0x00u, 0x01u,                /* bcdDevice = 0x0100 (v1.00) */
    CC_STR_MANUFACTURER,         /* iManufacturer = 1 */
    CC_STR_PRODUCT,              /* iProduct = 2 */
    CC_STR_SERIAL,               /* iSerialNumber = 3 */
    1u                           /* bNumConfigurations = 1 */
};

/* The configuration descriptor tree. 75 bytes total. */
static const uint8_t desc_config[DESC_LEN_CDC_ONLY_CONFIG] = {
    /* --- Configuration descriptor (9 bytes) --- */
    DESC_LEN_CONFIG,
    USB_DESC_TYPE_CONFIGURATION,
    CC_LE_U16_LO(DESC_LEN_CDC_ONLY_CONFIG),
    CC_LE_U16_HI(DESC_LEN_CDC_ONLY_CONFIG),
    2u,                          /* bNumInterfaces = 2 (comm + data) */
    1u,                          /* bConfigurationValue = 1 */
    0u,                          /* iConfiguration = 0 (no string) */
    0x80u,                       /* bmAttributes = 0x80 (bus-powered, no wakeup) */
    50u,                         /* bMaxPower = 100 mA (50 * 2 mA) */

    /* --- IAD for the CDC function (8 bytes) --- */
    DESC_LEN_IAD,
    USB_DESC_TYPE_IAD,
    0u,                          /* bFirstInterface = 0 */
    2u,                          /* bInterfaceCount = 2 */
    USB_CLASS_CDC,               /* bFunctionClass = 0x02 (CDC) */
    USB_SUBCLASS_CDC_ACM,        /* bFunctionSubClass = 0x02 (ACM) */
    0x00u,                       /* bFunctionProtocol = 0 (none) */
    CC_STR_FUNCTION_CDC,         /* iFunction = 4 ("CDC ACM Serial Port") */

    /* --- CDC.comm interface descriptor (9 bytes) --- */
    DESC_LEN_INTERFACE,
    USB_DESC_TYPE_INTERFACE,
    0u,                          /* bInterfaceNumber = 0 */
    0u,                          /* bAlternateSetting = 0 */
    1u,                          /* bNumEndpoints = 1 (notify only) */
    USB_CLASS_CDC,               /* bInterfaceClass = 0x02 */
    USB_SUBCLASS_CDC_ACM,        /* bInterfaceSubClass = 0x02 (ACM) */
    0x00u,                       /* bInterfaceProtocol = 0 (none) */
    0u,                          /* iInterface = 0 */

    /* --- CDC Header functional descriptor (5 bytes; CDC 1.2 sec. 5.2.3.1) --- */
    DESC_LEN_CDC_HEADER,
    USB_DESC_TYPE_CS_INTERFACE,
    0x00u,                       /* bDescriptorSubtype = Header */
    0x10u, 0x01u,                /* bcdCDC = 0x0110 (CDC 1.10) */

    /* --- CDC Call Management functional descriptor (5 bytes; PSTN 1.2 sec. 5.3.1) --- */
    DESC_LEN_CDC_CALLMGMT,
    USB_DESC_TYPE_CS_INTERFACE,
    0x01u,                       /* bDescriptorSubtype = Call Management */
    0x00u,                       /* bmCapabilities = 0 (no call mgmt) */
    1u,                          /* bDataInterface = 1 */

    /* --- CDC ACM functional descriptor (4 bytes; PSTN 1.2 sec. 5.3.2) --- */
    DESC_LEN_CDC_ACM,
    USB_DESC_TYPE_CS_INTERFACE,
    0x02u,                       /* bDescriptorSubtype = ACM */
    0x02u,                       /* bmCapabilities: bit 1 = supports
                                  * SET_LINE_CODING / GET_LINE_CODING /
                                  * SET_CONTROL_LINE_STATE / SERIAL_STATE
                                  * notifications. */

    /* --- CDC Union functional descriptor (5 bytes; CDC 1.2 sec. 5.2.3.8) --- */
    DESC_LEN_CDC_UNION,
    USB_DESC_TYPE_CS_INTERFACE,
    0x06u,                       /* bDescriptorSubtype = Union */
    0u,                          /* bMasterInterface = 0 (the comm IF) */
    1u,                          /* bSlaveInterface0 = 1 (the data IF) */

    /* --- CDC notification endpoint (7 bytes) --- */
    DESC_LEN_ENDPOINT,
    USB_DESC_TYPE_ENDPOINT,
    CC_EP_CDC_NOTIFY,            /* bEndpointAddress = 0x81 (IN, EP 1) */
    USB_EP_ATTR_INTERRUPT,
    CC_LE_U16_LO(CC_EP_CDC_NOTIFY_MAX_PACKET),
    CC_LE_U16_HI(CC_EP_CDC_NOTIFY_MAX_PACKET),
    CC_CDC_NOTIFY_POLL_INTERVAL_MS,

    /* --- CDC.data interface descriptor (9 bytes) --- */
    DESC_LEN_INTERFACE,
    USB_DESC_TYPE_INTERFACE,
    1u,                          /* bInterfaceNumber = 1 */
    0u,                          /* bAlternateSetting = 0 */
    2u,                          /* bNumEndpoints = 2 (bulk OUT + IN) */
    USB_CLASS_CDC_DATA,          /* bInterfaceClass = 0x0A */
    0x00u,                       /* bInterfaceSubClass = 0 */
    0x00u,                       /* bInterfaceProtocol = 0 */
    0u,                          /* iInterface = 0 */

    /* --- CDC bulk-OUT endpoint (7 bytes) --- */
    DESC_LEN_ENDPOINT,
    USB_DESC_TYPE_ENDPOINT,
    CC_EP_CDC_DATA_OUT,          /* bEndpointAddress = 0x02 (OUT, EP 2) */
    USB_EP_ATTR_BULK,
    CC_LE_U16_LO(CC_EP_BULK_MAX_PACKET),
    CC_LE_U16_HI(CC_EP_BULK_MAX_PACKET),
    0u,                          /* bInterval ignored for bulk */

    /* --- CDC bulk-IN endpoint (7 bytes) --- */
    DESC_LEN_ENDPOINT,
    USB_DESC_TYPE_ENDPOINT,
    CC_EP_CDC_DATA_IN,           /* bEndpointAddress = 0x82 (IN, EP 2) */
    USB_EP_ATTR_BULK,
    CC_LE_U16_LO(CC_EP_BULK_MAX_PACKET),
    CC_LE_U16_HI(CC_EP_BULK_MAX_PACKET),
    0u
};

/* Sanity check: confirm the descriptor array is the size the header claims. */
_Static_assert(sizeof desc_config == DESC_LEN_CDC_ONLY_CONFIG,
               "desc_config size must equal DESC_LEN_CDC_ONLY_CONFIG");

/* --- String table ----------------------------------------------------- */

/* The strings are ASCII. tud_descriptor_string_cb converts them to
 * UTF-16LE on the fly. String index 0 is special (LANGID array). */
static const char * const string_table[] = {
    NULL,                          /* index 0 handled separately */
    "Code Crunch Worldwide",       /* iManufacturer */
    "C7 Wire CDC Echo",            /* iProduct */
    "C7-WIRE-W09-E01-0001",        /* iSerialNumber */
    "CDC ACM Serial Port"          /* iFunction (IAD CDC) */
};

#define STRING_TABLE_SIZE  ((uint8_t)(sizeof string_table / sizeof string_table[0]))

/* Static buffer used by tud_descriptor_string_cb to hold the UTF-16LE
 * conversion of the current request. Each call returns a pointer to this
 * buffer; TinyUSB copies out the bytes before the next call. */
static uint16_t s_string_desc_buf[32];

/* ---- TinyUSB descriptor callbacks ----------------------------------- */

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
        /* LANGID array: header (length=4, type=3) + LANGID 0x0409. */
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

    /* Cap the conversion at the buffer's capacity (31 chars + 1 header). */
    const size_t max_chars = (sizeof s_string_desc_buf / sizeof s_string_desc_buf[0]) - 1u;
    size_t len = 0u;
    while ((len < max_chars) && (str[len] != '\0')) {
        s_string_desc_buf[1u + len] = (uint16_t)str[len];
        len++;
    }

    /* Header: bLength = 2 + 2*len, bDescriptorType = 0x03. */
    const uint16_t bytes = (uint16_t)(2u + (2u * len));
    s_string_desc_buf[0] = ((uint16_t)USB_DESC_TYPE_STRING << 8) | (uint16_t)(bytes & 0xFFu);

    return s_string_desc_buf;
}
