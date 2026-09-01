/* The CH340 arrays are a byte-for-byte transcription of the GBFlash L15
 * application image; keep them identical to stock. bcdDevice 0x0304 (a real
 * CH340G reports 0x0263) and bMaxPacketSize0 8 are stock, not transcription
 * errors, and usb.c's EP0 path clamps to that 8, so the two change together. */

#include <stdint.h>

#include "usb.h"

/* Device descriptor, flash 0xB408. VID 0x1A86 / PID 0x7523. */
const uint8_t bl_usb_desc_device[18] = {
    0x12, 0x01, 0x10, 0x01, 0xFF, 0x00, 0x02, 0x08, 0x86,
    0x1A, 0x23, 0x75, 0x04, 0x03, 0x00, 0x00, 0x00, 0x01,
};

/* Configuration descriptor set, flash 0xB41A. 9 + 9 + 7 + 7 + 7 = 39.
 * Do not raise the bulk IN to 64 here: Windows binds CH341SER.SYS on this
 * VID/PID and it is written for 32-byte bulk endpoints, so enumeration succeeds
 * and the transfer then hangs. Raising the OUT can overrun the receive window. */
const uint8_t bl_usb_desc_config[39] = {
    0x09, 0x02, 0x27, 0x00, 0x01, 0x01, 0x00, 0x80, 0xF0, /* config, 1 iface, 480 mA  */
    0x09, 0x04, 0x00, 0x00, 0x03, 0xFF, 0x01, 0x02, 0x00, /* iface 0, 3 EP, FF/01/02  */
    0x07, 0x05, 0x82, 0x02, 0x20, 0x00, 0x00, /* EP 0x82 bulk IN, 32 = stock, offset 0x16 */
    0x07, 0x05, 0x02, 0x02, 0x20, 0x00, 0x00, /* EP 0x02 bulk OUT, 32                     */
    0x07, 0x05, 0x81, 0x03, 0x08, 0x00, 0x01, /* EP 0x81 intr IN, 8, never driven         */
};

/* CH340 canned vendor-IN replies, flash 0xB441. Thirteen two-byte replies handed
 * out in call order to bmRequestType==0xC0; bRequest/wValue/wIndex are ignored.
 * Listed as delivered into ep0buf[0..1]; the vendor SDK reverses each pair. */
const uint8_t bl_usb_ch340_vendor_tbl[26] = {
    0x30, 0x00,   /* chip version 0x30, what a real CH340G reports */
    0xC3, 0x00,
    0xFF, 0xEC,   /* modem status -> no lines asserted */
    0x9F, 0xEC,
    0xFF, 0xEC,
    0xDF, 0xEC,
    0xDF, 0xEC,
    0xDF, 0xEC,
    0x9F, 0xEC,
    0x9F, 0xEC,
    0x9F, 0xEC,
    0x9F, 0xEC,
    0xFF, 0xEC,   /* 13th, and every request after it */
};

#if FW_USB_CDC
/* A second identity with 64-byte bulk endpoints. The class is what makes the 64
 * stick: bDeviceClass 0x02 / bDeviceSubClass 0x02 binds usbser.sys or
 * AppleUSBACMData instead of CH341SER.SYS. bcdUSB stays 0x0110; at 0x0200 the
 * host asks for a DEVICE_QUALIFIER, which this firmware STALLs. */

const uint8_t bl_usb_desc_device_cdc[18] = {
    0x12, 0x01,               /* bLength 18, DEVICE                           */
    0x10, 0x01,               /* bcdUSB 1.10                                  */
    0x02,                     /* bDeviceClass    Communications               */
    0x02,                     /* bDeviceSubClass Abstract Control Model       */
    0x00,                     /* bDeviceProtocol none                         */
    0x08,                     /* bMaxPacketSize0 8                            */
    (uint8_t)(BL_USB_CDC_VID & 0xFFu), (uint8_t)(BL_USB_CDC_VID >> 8),
    (uint8_t)(BL_USB_CDC_PID & 0xFFu), (uint8_t)(BL_USB_CDC_PID >> 8),
    0x00, 0x01,               /* bcdDevice 1.00                               */
    0x00, 0x00, 0x00,         /* iManufacturer, iProduct, iSerialNumber: none */
    0x01,                     /* bNumConfigurations                           */
};

/* 9 + 9 + 5 + 5 + 4 + 5 + 7 + 9 + 7 + 7 = 67. No IAD: one function, so the
 * device-level class is the grouping. EP 0x81 sits at T_RES=NAK forever. */
const uint8_t bl_usb_desc_config_cdc[BL_USB_CDC_CFG_LEN] = {
    0x09, 0x02, 0x43, 0x00,   /* configuration, wTotalLength 67                */
    0x02,                     /* bNumInterfaces 2                             */
#if FW_USB_CDC_BREAK == 1
    0x00,                     /* bConfigurationValue 0: broken on purpose     */
#else
    0x01,                     /* bConfigurationValue                          */
#endif
    0x00,                     /* iConfiguration                               */
    0x80, 0xF0,               /* bus powered, 480 mA                          */
    0x09, 0x04, 0x00, 0x00, 0x01, 0x02, 0x02, 0x01, 0x00, /* iface 0: Comm/ACM/V.250 */
    0x05, 0x24, 0x00, 0x10, 0x01,      /* CDC Header, bcdCDC 1.10            */
    0x05, 0x24, 0x01, 0x00, 0x01,      /* CDC Call Management, data iface 1  */
    0x04, 0x24, 0x02, 0x02,            /* CDC ACM, caps line coding + state  */
    0x05, 0x24, 0x06, 0x00, 0x01,      /* CDC Union, control 0, data 1       */
    0x07, 0x05, 0x81, 0x03, 0x08, 0x00, 0x10, /* EP 0x81 intr IN, 8, bInt 16 */
    /* 16, not 1. EP1 IN is never serviced (src/usb.c, TOK_IN_EP1), so the
     * host's 1 ms poll of it only ever took bus bandwidth from EP2's bulk
     * IN. Measured on a 4 MiB AGB read: Stream 874 -> 960 KiB/s, MemCpy
     * 861 -> 943, transport ceiling 929 -> 992 KB/s. Saturates here; 32
     * and 255 measure the same. Single is unchanged, it is bus-bound. */
    0x09, 0x04, 0x01, 0x00, 0x02, 0x0A, 0x00, 0x00, 0x00, /* iface 1: CDC Data    */
    0x07, 0x05, 0x02, 0x02, 0x40, 0x00, 0x00, /* EP 0x02 bulk OUT, 64             */
#if FW_USB_CDC_BREAK == 2
    0x07, 0x05, 0x83, 0x02, 0x40, 0x00, 0x00, /* 0x83: broken on purpose          */
#else
    0x07, 0x05, 0x82, 0x02, 0x40, 0x00, 0x00, /* EP 0x82 bulk IN, 64              */
#endif
};

#if FW_USB_CDC_BREAK
#warning "FW_USB_CDC_BREAK is set: this image's CDC descriptors are broken and must never be shipped"
#endif

#if defined(__GNUC__) && !defined(__cplusplus)
__extension__ _Static_assert(sizeof bl_usb_desc_config_cdc == 67,
                             "CDC configuration set must be 67 bytes");
__extension__ _Static_assert(sizeof bl_usb_desc_device_cdc == 18,
                             "device descriptor must be 18 bytes");
#endif
#endif /* FW_USB_CDC */

#if defined(__GNUC__) && !defined(__cplusplus)
__extension__ _Static_assert(sizeof bl_usb_desc_device == 18,
                             "device descriptor must be 18 bytes");
__extension__ _Static_assert(sizeof bl_usb_desc_config == 39,
                             "config descriptor set must be 39 bytes");
__extension__ _Static_assert(sizeof bl_usb_ch340_vendor_tbl == 26,
                             "vendor table must be 26 bytes = 13 replies");
__extension__ _Static_assert(BL_USB_VENDOR_TBL_LAST + 2u
                             == sizeof bl_usb_ch340_vendor_tbl,
                             "cursor must saturate on the last pair");
#endif
