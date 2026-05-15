/*
 * Challenge 1 - The Broken Descriptor
 *
 * The forensics exercise. We deliberately break the configuration
 * descriptor in three different ways and watch the host's reaction in a
 * USB packet capture. The post-mortem teaches you the failure-mode
 * vocabulary: what symptoms each kind of descriptor bug produces on each
 * host OS, and how to diagnose them from a Wireshark / usbmon trace.
 *
 * This is a code-and-write challenge. The .c file contains the firmware
 * that exposes the three broken variants under a build-time switch.
 * The bench work is to flash each variant, capture the USB conversation,
 * and write a one-page post-mortem committed as
 *   challenge-01-postmortem.md
 * to your repo.
 *
 * The three breaks:
 *
 * Break A: wTotalLength too small by 12 bytes.
 *   Symptom: device enumerates partially; the host reads only the first
 *   wTotalLength bytes of the config tree; the trailing interface (in
 *   our CDC + extra bytes layout, the trailing IF and one endpoint) is
 *   never seen by the host; the host's CDC driver does not bind.
 *
 * Break B: wTotalLength too large by 32 bytes.
 *   Symptom: the host reads 32 extra bytes past the actual array end
 *   into whatever .rodata follows. Depending on what is there, the host
 *   either sees garbage descriptors and rejects them, or (worse) sees
 *   plausible-looking descriptors that crash the host's driver. Linux
 *   is the most diagnostic here ("descriptor read/64, error -71" in
 *   dmesg).
 *
 * Break C: Endpoint 0x82 declared twice.
 *   Symptom: enumeration appears to succeed but only one interface's
 *   bulk-IN works; the second's NAKs forever. Wireshark shows ENDPOINT_
 *   STALL responses on the duplicate.
 *
 * The student's job: build with -DBREAK=A (then B, then C), flash,
 * capture, write up.
 *
 * Hardware:
 *   - Raspberry Pi Pico.
 *   - USB packet analyzer (Wireshark + USBPcap, or usbmon on Linux).
 *
 * Build:
 *   pico-sdk libraries to link:
 *     pico_stdlib, tinyusb_device, tinyusb_board
 *   tusb_config.h must define CFG_TUD_CDC=1, CFG_TUD_VENDOR=1.
 *   Compile with one of -DBREAK_A, -DBREAK_B, -DBREAK_C selected.
 *
 * References:
 *   USB 2.0 spec, Chapter 9: pp. 239-284.
 *     https://www.usb.org/document-library/usb-20-specification
 *   Wireshark USB protocol dissector:
 *     https://wiki.wireshark.org/CaptureSetup/USB
 *   Linux usbmon documentation:
 *     https://www.kernel.org/doc/Documentation/usb/usbmon.txt
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "pico/stdlib.h"
#include "bsp/board.h"
#include "tusb.h"

#include "../exercises/usb_descriptors.h"

/* Default to break A if no break is selected. The Makefile sets one. */
#if !defined(BREAK_A) && !defined(BREAK_B) && !defined(BREAK_C)
    #define BREAK_A 1
#endif

/* ---- Descriptors ----------------------------------------------------- */

#define DESC_LEN_CDC_PLUS_PADDING                                            \
    ((uint16_t)(DESC_LEN_CONFIG + DESC_LEN_IAD + DESC_LEN_INTERFACE          \
              + DESC_LEN_CDC_HEADER + DESC_LEN_CDC_CALLMGMT                  \
              + DESC_LEN_CDC_ACM + DESC_LEN_CDC_UNION                        \
              + DESC_LEN_ENDPOINT + DESC_LEN_INTERFACE                       \
              + DESC_LEN_ENDPOINT + DESC_LEN_ENDPOINT))
/* = 75 */

/* Break A: report wTotalLength as 63 instead of 75. */
#if defined(BREAK_A)
    #define WTOTALLENGTH_REPORTED  ((uint16_t)(DESC_LEN_CDC_PLUS_PADDING - 12u))
#elif defined(BREAK_B)
    /* Break B: report wTotalLength as 107 instead of 75. */
    #define WTOTALLENGTH_REPORTED  ((uint16_t)(DESC_LEN_CDC_PLUS_PADDING + 32u))
#elif defined(BREAK_C)
    /* Break C: keep wTotalLength correct but make the second EP descriptor's
     * address duplicate the first. */
    #define WTOTALLENGTH_REPORTED  ((uint16_t)DESC_LEN_CDC_PLUS_PADDING)
#endif

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
    CC_LE_U16_LO(CC_USB_PID_CDC),
    CC_LE_U16_HI(CC_USB_PID_CDC),
    0x00u, 0x01u,
    CC_STR_MANUFACTURER,
    CC_STR_PRODUCT,
    CC_STR_SERIAL,
    1u
};

#if defined(BREAK_C)
    #define BULK_IN_ADDR_FOR_DATA   CC_EP_CDC_NOTIFY  /* duplicate 0x81 here */
#else
    #define BULK_IN_ADDR_FOR_DATA   CC_EP_CDC_DATA_IN
#endif

static const uint8_t desc_config[DESC_LEN_CDC_PLUS_PADDING] = {
    /* Configuration descriptor */
    DESC_LEN_CONFIG, USB_DESC_TYPE_CONFIGURATION,
    CC_LE_U16_LO(WTOTALLENGTH_REPORTED), CC_LE_U16_HI(WTOTALLENGTH_REPORTED),
    2u, 1u, 0u, 0x80u, 50u,
    /* IAD */
    DESC_LEN_IAD, USB_DESC_TYPE_IAD, 0u, 2u,
    USB_CLASS_CDC, USB_SUBCLASS_CDC_ACM, 0x00u, CC_STR_FUNCTION_CDC,
    /* CDC.comm interface */
    DESC_LEN_INTERFACE, USB_DESC_TYPE_INTERFACE, 0u, 0u, 1u,
    USB_CLASS_CDC, USB_SUBCLASS_CDC_ACM, 0x00u, 0u,
    /* CDC Header */
    DESC_LEN_CDC_HEADER, USB_DESC_TYPE_CS_INTERFACE, 0x00u, 0x10u, 0x01u,
    /* CDC Call Mgmt */
    DESC_LEN_CDC_CALLMGMT, USB_DESC_TYPE_CS_INTERFACE, 0x01u, 0x00u, 1u,
    /* CDC ACM */
    DESC_LEN_CDC_ACM, USB_DESC_TYPE_CS_INTERFACE, 0x02u, 0x02u,
    /* CDC Union */
    DESC_LEN_CDC_UNION, USB_DESC_TYPE_CS_INTERFACE, 0x06u, 0u, 1u,
    /* EP 0x81 (notify) */
    DESC_LEN_ENDPOINT, USB_DESC_TYPE_ENDPOINT, CC_EP_CDC_NOTIFY,
    USB_EP_ATTR_INTERRUPT,
    CC_LE_U16_LO(CC_EP_CDC_NOTIFY_MAX_PACKET),
    CC_LE_U16_HI(CC_EP_CDC_NOTIFY_MAX_PACKET),
    CC_CDC_NOTIFY_POLL_INTERVAL_MS,
    /* CDC.data interface */
    DESC_LEN_INTERFACE, USB_DESC_TYPE_INTERFACE, 1u, 0u, 2u,
    USB_CLASS_CDC_DATA, 0x00u, 0x00u, 0u,
    /* EP 0x02 (bulk OUT) */
    DESC_LEN_ENDPOINT, USB_DESC_TYPE_ENDPOINT, CC_EP_CDC_DATA_OUT,
    USB_EP_ATTR_BULK,
    CC_LE_U16_LO(CC_EP_BULK_MAX_PACKET),
    CC_LE_U16_HI(CC_EP_BULK_MAX_PACKET),
    0u,
    /* EP bulk-IN; for BREAK_C this address duplicates 0x81. */
    DESC_LEN_ENDPOINT, USB_DESC_TYPE_ENDPOINT, BULK_IN_ADDR_FOR_DATA,
    USB_EP_ATTR_BULK,
    CC_LE_U16_LO(CC_EP_BULK_MAX_PACKET),
    CC_LE_U16_HI(CC_EP_BULK_MAX_PACKET),
    0u
};

_Static_assert(sizeof desc_config == DESC_LEN_CDC_PLUS_PADDING,
               "desc_config size must equal DESC_LEN_CDC_PLUS_PADDING");

/* ---- String table (minimal) ----------------------------------------- */

static const char * const string_table[] = {
    NULL,
    "Code Crunch Worldwide",
    "C7 Wire Broken-Descriptor",
    "C7-WIRE-W09-C01-0001",
    "CDC Broken"
};
#define STRING_TABLE_SIZE  ((uint8_t)(sizeof string_table / sizeof string_table[0]))

static uint16_t s_string_desc_buf[32];

/* ---- TinyUSB descriptor callbacks ------------------------------------ */

const uint8_t * tud_descriptor_device_cb(void) { return desc_device; }
const uint8_t * tud_descriptor_configuration_cb(const uint8_t i) { (void)i; return desc_config; }
const uint16_t * tud_descriptor_string_cb(const uint8_t index, const uint16_t langid)
{
    (void)langid;
    if (index == CC_STR_LANGID) {
        s_string_desc_buf[0] = ((uint16_t)USB_DESC_TYPE_STRING << 8) | 4u;
        s_string_desc_buf[1] = 0x0409u;
        return s_string_desc_buf;
    }
    if (index >= STRING_TABLE_SIZE) { return NULL; }
    const char * const str = string_table[index];
    if (str == NULL) { return NULL; }
    const size_t cap = (sizeof s_string_desc_buf / sizeof s_string_desc_buf[0]) - 1u;
    size_t len = 0u;
    while ((len < cap) && (str[len] != '\0')) {
        s_string_desc_buf[1u + len] = (uint16_t)str[len];
        len++;
    }
    const uint16_t bytes = (uint16_t)(2u + (2u * len));
    s_string_desc_buf[0] = ((uint16_t)USB_DESC_TYPE_STRING << 8) | (uint16_t)(bytes & 0xFFu);
    return s_string_desc_buf;
}

/* ---- Empty CDC callbacks (we just want the enumeration to happen) ---- */

void tud_mount_cb(void)   { }
void tud_umount_cb(void)  { }
void tud_suspend_cb(const bool r) { (void)r; }
void tud_resume_cb(void)  { }
void tud_cdc_rx_cb(const uint8_t i) { (void)i; }
void tud_cdc_line_coding_cb(const uint8_t i, const cdc_line_coding_t * const c)
{
    (void)i;
    (void)c;
}

/* ---- Main ------------------------------------------------------------- */

int main(void)
{
    board_init();
    if (! tud_init(0u)) {
        for (;;) { __asm volatile ("nop"); }
    }

    for (;;) {
        tud_task();
    }

    return 0;
}

/* ======================================================================
 * Student instructions
 *
 * 1. Build three .uf2 images:
 *      cmake -B build-A -DBREAK_A=1 && cmake --build build-A
 *      cmake -B build-B -DBREAK_B=1 && cmake --build build-B
 *      cmake -B build-C -DBREAK_C=1 && cmake --build build-C
 *
 * 2. For each .uf2:
 *      a. Start the USB capture tool (Wireshark + USBPcap on Windows;
 *         sudo modprobe usbmon && sudo wireshark on Linux; Apple USB
 *         Prober + ioreg on macOS).
 *      b. Plug in the Pico in BOOTSEL mode and copy the .uf2 to it.
 *      c. After the Pico reboots, the capture should show the
 *         enumeration sequence (and its failure mode).
 *      d. Save the capture as bench-captures/challenge01-break-X.pcapng.
 *      e. Note in your post-mortem:
 *           - The exact host-side error message (dmesg / Device Manager /
 *             ioreg output).
 *           - The exact transaction in the capture where the failure
 *             becomes visible.
 *           - The fix you would apply to make this descriptor variant
 *             enumerate correctly.
 *
 * 3. Commit:
 *      - challenge-01-postmortem.md     (your one-page write-up)
 *      - bench-captures/challenge01-break-A.pcapng
 *      - bench-captures/challenge01-break-B.pcapng
 *      - bench-captures/challenge01-break-C.pcapng
 *
 * Pass criterion: post-mortem identifies all three failure modes with
 * the correct symptom-to-cause mapping and a one-line fix for each.
 * ====================================================================== */
