/*
 * Composite-device USB descriptors for the Week 9 mini-project.
 *
 * Contains:
 *   - The 18-byte device descriptor with IAD class triple (0xEF/0x02/0x01).
 *   - The 146-byte configuration descriptor tree with 3 IADs and 4 interfaces.
 *   - The HID boot-keyboard report descriptor (63 bytes).
 *   - The string table (7 strings).
 *   - All four TinyUSB descriptor callbacks.
 *
 * The math: wTotalLength = 146, broken down as
 *    9   configuration descriptor header
 *   +8   IAD for CDC
 *   +9   interface 0 (CDC.comm)
 *   +5+5+4+5 four CDC functional descriptors
 *   +7   endpoint 0x81 (CDC notify, interrupt-IN)
 *   +9   interface 1 (CDC.data)
 *   +7+7 endpoints 0x02 (bulk-OUT) and 0x82 (bulk-IN)
 *   +8   IAD for HID
 *   +9   interface 2 (HID)
 *   +9   HID class descriptor
 *   +7+7 endpoints 0x83 (interrupt-IN) and 0x03 (interrupt-OUT)
 *   +8   IAD for MSC
 *   +9   interface 3 (MSC)
 *   +7+7 endpoints 0x84 (bulk-IN) and 0x04 (bulk-OUT)
 * = 146 bytes.
 *
 * Verified by the _Static_assert at the bottom of this file.
 *
 * References:
 *   USB 2.0 spec, Chapter 9 (descriptors):
 *     https://www.usb.org/document-library/usb-20-specification
 *   USB Interface Association Descriptor ECN (2003-07-23):
 *     https://www.usb.org/sites/default/files/iadclasscode_r10.pdf
 *   USB CDC 1.2, Chapter 5 (functional descriptors):
 *     https://www.usb.org/document-library/class-definitions-communication-devices-12
 *   USB HID 1.11, Chapter 6 (HID class descriptor):
 *     https://www.usb.org/document-library/device-class-definition-hid-111
 */

#include <stdint.h>
#include <stddef.h>

#include "tusb.h"

#include "../exercises/usb_descriptors.h"

/* ---- Computed lengths ------------------------------------------------- */

#define DESC_LEN_CDC_GROUP                                                   \
    ((uint16_t)(DESC_LEN_IAD + DESC_LEN_INTERFACE                            \
              + DESC_LEN_CDC_HEADER + DESC_LEN_CDC_CALLMGMT                  \
              + DESC_LEN_CDC_ACM + DESC_LEN_CDC_UNION                        \
              + DESC_LEN_ENDPOINT + DESC_LEN_INTERFACE                       \
              + DESC_LEN_ENDPOINT + DESC_LEN_ENDPOINT))
/* = 66 */

#define DESC_LEN_HID_GROUP                                                   \
    ((uint16_t)(DESC_LEN_IAD + DESC_LEN_INTERFACE + DESC_LEN_HID_CLASS       \
              + DESC_LEN_ENDPOINT + DESC_LEN_ENDPOINT))
/* = 40 */

#define DESC_LEN_MSC_GROUP                                                   \
    ((uint16_t)(DESC_LEN_IAD + DESC_LEN_INTERFACE                            \
              + DESC_LEN_ENDPOINT + DESC_LEN_ENDPOINT))
/* = 31 */

#define DESC_LEN_TOTAL                                                       \
    ((uint16_t)(DESC_LEN_CONFIG + DESC_LEN_CDC_GROUP                         \
              + DESC_LEN_HID_GROUP + DESC_LEN_MSC_GROUP))
/* = 9 + 66 + 40 + 31 = 146 */

/* ---- HID report descriptor (boot keyboard) ---------------------------- */

static const uint8_t hid_report_desc_kbd[] = {
    0x05u, 0x01u,            /* Usage Page (Generic Desktop)              */
    0x09u, 0x06u,            /* Usage (Keyboard)                          */
    0xA1u, 0x01u,            /* Collection (Application)                  */
    0x05u, 0x07u,            /*   Usage Page (Key Codes)                  */
    0x19u, 0xE0u,            /*   Usage Minimum (224 = LeftControl)       */
    0x29u, 0xE7u,            /*   Usage Maximum (231 = RightGUI)          */
    0x15u, 0x00u,            /*   Logical Minimum (0)                     */
    0x25u, 0x01u,            /*   Logical Maximum (1)                     */
    0x75u, 0x01u,            /*   Report Size (1 bit)                     */
    0x95u, 0x08u,            /*   Report Count (8)                        */
    0x81u, 0x02u,            /*   Input (Data, Variable, Absolute)        */
    0x95u, 0x01u,            /*   Report Count (1)                        */
    0x75u, 0x08u,            /*   Report Size (8 bits)                    */
    0x81u, 0x01u,            /*   Input (Constant) - reserved             */
    0x95u, 0x05u,            /*   Report Count (5)                        */
    0x75u, 0x01u,            /*   Report Size (1 bit)                     */
    0x05u, 0x08u,            /*   Usage Page (LEDs)                       */
    0x19u, 0x01u,            /*   Usage Minimum (1)                       */
    0x29u, 0x05u,            /*   Usage Maximum (5)                       */
    0x91u, 0x02u,            /*   Output (Data, Variable, Absolute)       */
    0x95u, 0x01u,            /*   Report Count (1)                        */
    0x75u, 0x03u,            /*   Report Size (3 bits)                    */
    0x91u, 0x01u,            /*   Output (Constant)                       */
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

/* ---- Device descriptor (18 bytes) ------------------------------------- */

static const uint8_t desc_device[DESC_LEN_DEVICE] = {
    DESC_LEN_DEVICE,
    USB_DESC_TYPE_DEVICE,
    0x00u, 0x02u,                          /* bcdUSB = 2.0 */
    USB_CLASS_MISC,                        /* 0xEF: Miscellaneous */
    USB_SUBCLASS_MISC_COMMON,              /* 0x02: Common Class */
    USB_PROTOCOL_MISC_IAD,                 /* 0x01: IAD */
    CC_EP0_MAX_PACKET,
    CC_LE_U16_LO(CC_USB_VID),
    CC_LE_U16_HI(CC_USB_VID),
    CC_LE_U16_LO(CC_USB_PID_COMPOSITE),
    CC_LE_U16_HI(CC_USB_PID_COMPOSITE),
    0x00u, 0x01u,                          /* bcdDevice = 1.00 */
    CC_STR_MANUFACTURER,
    CC_STR_PRODUCT,
    CC_STR_SERIAL,
    1u                                     /* bNumConfigurations */
};

/* ---- Configuration descriptor tree (146 bytes) ------------------------ */

static const uint8_t desc_config[DESC_LEN_TOTAL] = {
    /* === Configuration descriptor (9 bytes) === */
    DESC_LEN_CONFIG, USB_DESC_TYPE_CONFIGURATION,
    CC_LE_U16_LO(DESC_LEN_TOTAL), CC_LE_U16_HI(DESC_LEN_TOTAL),
    4u,                                    /* bNumInterfaces */
    1u,                                    /* bConfigurationValue */
    0u,                                    /* iConfiguration */
    0x80u,                                 /* bmAttributes: bus-powered */
    50u,                                   /* bMaxPower: 100 mA */

    /* === CDC section (66 bytes) === */
    /* IAD for CDC */
    DESC_LEN_IAD, USB_DESC_TYPE_IAD,
    0u,                                    /* bFirstInterface */
    2u,                                    /* bInterfaceCount */
    USB_CLASS_CDC, USB_SUBCLASS_CDC_ACM, 0x00u,
    CC_STR_FUNCTION_CDC,

    /* Interface 0: CDC.comm */
    DESC_LEN_INTERFACE, USB_DESC_TYPE_INTERFACE,
    0u, 0u, 1u,
    USB_CLASS_CDC, USB_SUBCLASS_CDC_ACM, 0x00u, 0u,

    /* CDC Header functional descriptor */
    DESC_LEN_CDC_HEADER, USB_DESC_TYPE_CS_INTERFACE,
    0x00u, 0x10u, 0x01u,

    /* CDC Call Management functional descriptor */
    DESC_LEN_CDC_CALLMGMT, USB_DESC_TYPE_CS_INTERFACE,
    0x01u, 0x00u, 1u,

    /* CDC ACM functional descriptor */
    DESC_LEN_CDC_ACM, USB_DESC_TYPE_CS_INTERFACE,
    0x02u, 0x02u,

    /* CDC Union functional descriptor */
    DESC_LEN_CDC_UNION, USB_DESC_TYPE_CS_INTERFACE,
    0x06u, 0u, 1u,

    /* CDC notification endpoint 0x81 */
    DESC_LEN_ENDPOINT, USB_DESC_TYPE_ENDPOINT,
    CC_EP_CDC_NOTIFY, USB_EP_ATTR_INTERRUPT,
    CC_LE_U16_LO(CC_EP_CDC_NOTIFY_MAX_PACKET),
    CC_LE_U16_HI(CC_EP_CDC_NOTIFY_MAX_PACKET),
    CC_CDC_NOTIFY_POLL_INTERVAL_MS,

    /* Interface 1: CDC.data */
    DESC_LEN_INTERFACE, USB_DESC_TYPE_INTERFACE,
    1u, 0u, 2u,
    USB_CLASS_CDC_DATA, 0x00u, 0x00u, 0u,

    /* CDC bulk-OUT endpoint 0x02 */
    DESC_LEN_ENDPOINT, USB_DESC_TYPE_ENDPOINT,
    CC_EP_CDC_DATA_OUT, USB_EP_ATTR_BULK,
    CC_LE_U16_LO(CC_EP_BULK_MAX_PACKET),
    CC_LE_U16_HI(CC_EP_BULK_MAX_PACKET),
    0u,

    /* CDC bulk-IN endpoint 0x82 */
    DESC_LEN_ENDPOINT, USB_DESC_TYPE_ENDPOINT,
    CC_EP_CDC_DATA_IN, USB_EP_ATTR_BULK,
    CC_LE_U16_LO(CC_EP_BULK_MAX_PACKET),
    CC_LE_U16_HI(CC_EP_BULK_MAX_PACKET),
    0u,

    /* === HID section (40 bytes) === */
    /* IAD for HID */
    DESC_LEN_IAD, USB_DESC_TYPE_IAD,
    2u, 1u,
    USB_CLASS_HID, 0x00u, 0x00u,
    CC_STR_FUNCTION_HID,

    /* Interface 2: HID */
    DESC_LEN_INTERFACE, USB_DESC_TYPE_INTERFACE,
    2u, 0u, 2u,
    USB_CLASS_HID, 0x00u, 0x00u, 0u,

    /* HID class descriptor */
    DESC_LEN_HID_CLASS, USB_DESC_TYPE_HID,
    0x11u, 0x01u,                          /* bcdHID = 1.11 */
    0x00u,                                 /* bCountryCode */
    1u,                                    /* bNumDescriptors */
    USB_DESC_TYPE_HID_REPORT,
    CC_LE_U16_LO(HID_REPORT_DESC_LEN),
    CC_LE_U16_HI(HID_REPORT_DESC_LEN),

    /* HID interrupt-IN endpoint 0x83 */
    DESC_LEN_ENDPOINT, USB_DESC_TYPE_ENDPOINT,
    CC_EP_HID_IN, USB_EP_ATTR_INTERRUPT,
    CC_LE_U16_LO(CC_EP_HID_MAX_PACKET),
    CC_LE_U16_HI(CC_EP_HID_MAX_PACKET),
    CC_HID_POLL_INTERVAL_MS,

    /* HID interrupt-OUT endpoint 0x03 */
    DESC_LEN_ENDPOINT, USB_DESC_TYPE_ENDPOINT,
    CC_EP_HID_OUT, USB_EP_ATTR_INTERRUPT,
    CC_LE_U16_LO(CC_EP_HID_MAX_PACKET),
    CC_LE_U16_HI(CC_EP_HID_MAX_PACKET),
    CC_HID_POLL_INTERVAL_MS,

    /* === MSC section (31 bytes) === */
    /* IAD for MSC */
    DESC_LEN_IAD, USB_DESC_TYPE_IAD,
    3u, 1u,
    USB_CLASS_MSC, USB_SUBCLASS_MSC_SCSI, USB_PROTOCOL_MSC_BBB,
    CC_STR_FUNCTION_MSC,

    /* Interface 3: MSC */
    DESC_LEN_INTERFACE, USB_DESC_TYPE_INTERFACE,
    3u, 0u, 2u,
    USB_CLASS_MSC, USB_SUBCLASS_MSC_SCSI, USB_PROTOCOL_MSC_BBB, 0u,

    /* MSC bulk-IN endpoint 0x84 */
    DESC_LEN_ENDPOINT, USB_DESC_TYPE_ENDPOINT,
    CC_EP_MSC_IN, USB_EP_ATTR_BULK,
    CC_LE_U16_LO(CC_EP_BULK_MAX_PACKET),
    CC_LE_U16_HI(CC_EP_BULK_MAX_PACKET),
    0u,

    /* MSC bulk-OUT endpoint 0x04 */
    DESC_LEN_ENDPOINT, USB_DESC_TYPE_ENDPOINT,
    CC_EP_MSC_OUT, USB_EP_ATTR_BULK,
    CC_LE_U16_LO(CC_EP_BULK_MAX_PACKET),
    CC_LE_U16_HI(CC_EP_BULK_MAX_PACKET),
    0u
};

/* Compile-time check that the descriptor array is exactly DESC_LEN_TOTAL
 * bytes. If this fails, you have miscounted the descriptor lengths, and
 * the host will fail to enumerate at the configuration-tree read step. */
_Static_assert(sizeof desc_config == DESC_LEN_TOTAL,
               "desc_config size must equal DESC_LEN_TOTAL (146)");

/* ---- String table ----------------------------------------------------- */

static const char * const string_table[] = {
    NULL,                                  /* 0 handled separately (LANGID) */
    "Code Crunch Worldwide",               /* 1: iManufacturer */
    "C7 Wire Composite Device",            /* 2: iProduct */
    "C7-WIRE-W09-COMP-0001",               /* 3: iSerialNumber */
    "CDC ACM Serial Port",                 /* 4: iFunction (CDC) */
    "HID Keyboard",                        /* 5: iFunction (HID) */
    "USB Mass Storage"                     /* 6: iFunction (MSC) */
};

#define STRING_TABLE_SIZE  ((uint8_t)(sizeof string_table / sizeof string_table[0]))

static uint16_t s_string_desc_buf[32];

/* ---- TinyUSB descriptor callbacks ------------------------------------ */

const uint8_t * tud_descriptor_device_cb(void)
{
    return desc_device;
}

const uint8_t * tud_descriptor_configuration_cb(const uint8_t index)
{
    (void)index;
    return desc_config;
}

const uint8_t * tud_hid_descriptor_report_cb(const uint8_t instance)
{
    (void)instance;
    return hid_report_desc_kbd;
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
