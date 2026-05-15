/*
 * usb_descriptors.h - Shared USB descriptor declarations for Week 9 exercises.
 *
 * Each exercise compiles one of three role-specific .c files alongside this
 * header. The descriptor *values* differ per exercise (CDC-only vs HID-only
 * vs MSC-only), but the byte-pattern conventions, length constants, and
 * endpoint-numbering scheme are shared.
 *
 * References:
 *   USB 2.0 spec, Chapter 9 (descriptors):
 *     https://www.usb.org/document-library/usb-20-specification
 *   TinyUSB device-stack overview:
 *     https://docs.tinyusb.org/en/latest/reference/dev_concept.html
 *
 * Endpoint numbering (consistent across exercises and the mini-project):
 *   EP 0  : Control (implicit)
 *   EP 1  : CDC notification, interrupt-IN  (0x81)
 *   EP 2  : CDC data, bulk-OUT (0x02) + bulk-IN (0x82)
 *   EP 3  : HID, interrupt-IN  (0x83) + interrupt-OUT (0x03)
 *   EP 4  : MSC, bulk-OUT (0x04) + bulk-IN (0x84)
 *
 * Class triples (USB-IF assigned):
 *   CDC.comm   : 0x02 / 0x02 / 0x00   (Communication / ACM / no protocol)
 *   CDC.data   : 0x0A / 0x00 / 0x00   (CDC Data / no subclass / no protocol)
 *   HID        : 0x03 / 0x00 / 0x00   (HID / no boot / no boot protocol)
 *   MSC        : 0x08 / 0x06 / 0x50   (MSC / SCSI transparent / BBB)
 *   Composite  : 0xEF / 0x02 / 0x01   (Miscellaneous / Common / IAD)
 */

#ifndef CC_C7_W09_USB_DESCRIPTORS_H
#define CC_C7_W09_USB_DESCRIPTORS_H

#include <stdint.h>
#include <stddef.h>

/* --- USB-IF descriptor type codes (USB 2.0 Table 9-5, p. 251) ---------- */

#define USB_DESC_TYPE_DEVICE              ((uint8_t)0x01u)
#define USB_DESC_TYPE_CONFIGURATION       ((uint8_t)0x02u)
#define USB_DESC_TYPE_STRING              ((uint8_t)0x03u)
#define USB_DESC_TYPE_INTERFACE           ((uint8_t)0x04u)
#define USB_DESC_TYPE_ENDPOINT            ((uint8_t)0x05u)
#define USB_DESC_TYPE_IAD                 ((uint8_t)0x0Bu)
#define USB_DESC_TYPE_CS_INTERFACE        ((uint8_t)0x24u)
#define USB_DESC_TYPE_HID                 ((uint8_t)0x21u)
#define USB_DESC_TYPE_HID_REPORT          ((uint8_t)0x22u)

/* --- USB endpoint attributes (USB 2.0 Table 9-13, p. 270) -------------- */

#define USB_EP_ATTR_CONTROL               ((uint8_t)0x00u)
#define USB_EP_ATTR_ISOCHRONOUS           ((uint8_t)0x01u)
#define USB_EP_ATTR_BULK                  ((uint8_t)0x02u)
#define USB_EP_ATTR_INTERRUPT             ((uint8_t)0x03u)

/* --- USB device-class triples ------------------------------------------ */

#define USB_CLASS_CDC                     ((uint8_t)0x02u)
#define USB_CLASS_CDC_DATA                ((uint8_t)0x0Au)
#define USB_CLASS_HID                     ((uint8_t)0x03u)
#define USB_CLASS_MSC                     ((uint8_t)0x08u)
#define USB_CLASS_MISC                    ((uint8_t)0xEFu)

#define USB_SUBCLASS_CDC_ACM              ((uint8_t)0x02u)
#define USB_SUBCLASS_MSC_SCSI             ((uint8_t)0x06u)
#define USB_PROTOCOL_MSC_BBB              ((uint8_t)0x50u)
#define USB_SUBCLASS_MISC_COMMON          ((uint8_t)0x02u)
#define USB_PROTOCOL_MISC_IAD             ((uint8_t)0x01u)

/* --- Descriptor section lengths (in bytes) ----------------------------- */

#define DESC_LEN_DEVICE                   ((uint16_t)18u)
#define DESC_LEN_CONFIG                   ((uint16_t)9u)
#define DESC_LEN_IAD                      ((uint16_t)8u)
#define DESC_LEN_INTERFACE                ((uint16_t)9u)
#define DESC_LEN_ENDPOINT                 ((uint16_t)7u)
#define DESC_LEN_CDC_HEADER               ((uint16_t)5u)
#define DESC_LEN_CDC_CALLMGMT             ((uint16_t)5u)
#define DESC_LEN_CDC_ACM                  ((uint16_t)4u)
#define DESC_LEN_CDC_UNION                ((uint16_t)5u)
#define DESC_LEN_HID_CLASS                ((uint16_t)9u)

/* --- Pico USB-IF identifiers (course convention) ----------------------- */

#define CC_USB_VID                        ((uint16_t)0x2E8Au)  /* Raspberry Pi */
#define CC_USB_PID_CDC                    ((uint16_t)0x000Au)  /* Ex.1 CDC */
#define CC_USB_PID_HID                    ((uint16_t)0x000Bu)  /* Ex.2 HID */
#define CC_USB_PID_MSC                    ((uint16_t)0x000Du)  /* Ex.3 MSC */
#define CC_USB_PID_COMPOSITE              ((uint16_t)0x000Cu)  /* mini-project */

/* --- Endpoint addresses (direction bit packed in MSB) ------------------ */

#define CC_EP_CDC_NOTIFY                  ((uint8_t)0x81u)
#define CC_EP_CDC_DATA_OUT                ((uint8_t)0x02u)
#define CC_EP_CDC_DATA_IN                 ((uint8_t)0x82u)
#define CC_EP_HID_IN                      ((uint8_t)0x83u)
#define CC_EP_HID_OUT                     ((uint8_t)0x03u)
#define CC_EP_MSC_OUT                     ((uint8_t)0x04u)
#define CC_EP_MSC_IN                      ((uint8_t)0x84u)

/* --- Max packet sizes -------------------------------------------------- */

#define CC_EP0_MAX_PACKET                 ((uint8_t)64u)
#define CC_EP_BULK_MAX_PACKET             ((uint16_t)64u)
#define CC_EP_HID_MAX_PACKET              ((uint16_t)16u)
#define CC_EP_CDC_NOTIFY_MAX_PACKET       ((uint16_t)8u)

/* --- HID polling intervals (ms) ---------------------------------------- */

#define CC_HID_POLL_INTERVAL_MS           ((uint8_t)10u)
#define CC_CDC_NOTIFY_POLL_INTERVAL_MS    ((uint8_t)16u)

/* --- MSC parameters ---------------------------------------------------- */

#define MSC_BLOCK_SIZE_BYTES              ((uint32_t)512u)
#define MSC_DISK_SIZE_BYTES               ((uint32_t)(128u * 1024u))  /* 128 KB */
#define MSC_DISK_BLOCK_COUNT              ((uint32_t)(MSC_DISK_SIZE_BYTES / MSC_BLOCK_SIZE_BYTES))

/* --- String descriptor indices (project convention) ------------------- */

#define CC_STR_LANGID                     ((uint8_t)0u)
#define CC_STR_MANUFACTURER               ((uint8_t)1u)
#define CC_STR_PRODUCT                    ((uint8_t)2u)
#define CC_STR_SERIAL                     ((uint8_t)3u)
#define CC_STR_FUNCTION_CDC               ((uint8_t)4u)
#define CC_STR_FUNCTION_HID               ((uint8_t)5u)
#define CC_STR_FUNCTION_MSC               ((uint8_t)6u)

/* --- Helpers: byte-order writers as constant expressions --------------- */

/* USB descriptors are little-endian; SCSI fields are big-endian. The byte
 * arrays use plain hex bytes, but these helpers make the source intent
 * explicit at the call site. They generate at most a few bytes; there is
 * no runtime cost beyond what a literal would have. */
#define CC_LE_U16_LO(v)                   ((uint8_t)((uint16_t)(v) & 0xFFu))
#define CC_LE_U16_HI(v)                   ((uint8_t)(((uint16_t)(v) >> 8) & 0xFFu))
#define CC_BE_U32_B0(v)                   ((uint8_t)(((uint32_t)(v) >> 24) & 0xFFu))
#define CC_BE_U32_B1(v)                   ((uint8_t)(((uint32_t)(v) >> 16) & 0xFFu))
#define CC_BE_U32_B2(v)                   ((uint8_t)(((uint32_t)(v) >> 8) & 0xFFu))
#define CC_BE_U32_B3(v)                   ((uint8_t)((uint32_t)(v) & 0xFFu))

#endif /* CC_C7_W09_USB_DESCRIPTORS_H */
