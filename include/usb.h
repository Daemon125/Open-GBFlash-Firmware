/* Polled CH340-emulation USB device; src/usb.c, src/usb_desc.c.  bl_usb_poll()
 * must run every few ms: a bus reset has to be answered inside the host's ~10 ms
 * recovery window, and LK.c:1682's _delay_us(8500) is the tightest path here.
 * HAZARD: only this build may enable NVIC IRQ6 (bl_usb_irq_arm() alone).  Vector
 * 22 forwards to the app table at 0x4000, erased during an update: an armed IRQ6
 * fetches 0xFFFFFFFF there and stalls the update with CodeFlash unlocked. */

#ifndef BL_USB_H
#define BL_USB_H

#include <stdint.h>

#include "timebase.h"  /* keeps host/test_usb_desc.py building on include/ */

#ifdef __cplusplus
extern "C" {
#endif

#ifndef BL_USB_ECHO
#define BL_USB_ECHO             1       /* bring-up only: the pump eats rx   */
#endif

/* Service the SIE from IRQ6 instead of the poll loop.  FW_USB_IRQ=0 and
 * FW_USB_CDC=0 must each still build the byte-for-byte stock CH340 image. */
#ifndef FW_USB_IRQ
#define FW_USB_IRQ              0
#endif

/* CDC-ACM identity instead of a CH340.  A 64-byte bulk IN while still claiming
 * to be a CH340 stalls: CH341SER.SYS binds it and the device leaves the bus. */
#ifndef FW_USB_CDC
#define FW_USB_CDC              0
#endif

#ifndef FW_USB_CDC_BREAK
#define FW_USB_CDC_BREAK        0       /* fault injection; never ship != 0  */
#endif

/* The copy and the tx_head update must be atomic against tx_pump()'s rewind: a
 * half-done copy then a rewind strands the bytes and sends tx_buf[0]. */
#ifndef BL_USB_TX_LOCK_CHUNK
#define BL_USB_TX_LOCK_CHUNK    64u     /* one max-size bulk packet          */
#endif

#ifndef BL_USB_PLL_POWER_ON
#define BL_USB_PLL_POWER_ON     1       /* only the SIE needs the PLL        */
#endif

#ifndef BL_USB_PLL_SETTLE_MS
#define BL_USB_PLL_SETTLE_MS    5u      /* nominal: a calibrated busy loop   */
#endif

#ifndef BL_USB_RX_BUF_SIZE
#define BL_USB_RX_BUF_SIZE      512u    /* in-flight bytes before OUT NAKs   */
#endif

/* Staging ring for replies that do not nominate a direct region: with the
 * *_TX_DIRECT options on, acks and variable reads only. Raising it costs
 * FW_MAX_TRANSFER headroom. */
#ifndef BL_USB_TX_BUF_SIZE
#define BL_USB_TX_BUF_SIZE      1024u
#endif

#ifndef BL_USB_TX_TIMEOUT_MS
#define BL_USB_TX_TIMEOUT_MS    250u
#endif

/* Real ms between the two bl_time_ms() reads in bl_usb_tx(); src/timebase.c
 * asserts on it. */
#ifndef BL_USB_TX_MAX_GAP_MS
#define BL_USB_TX_MAX_GAP_MS    2u
#endif

/* Backstop for the tx wait when the clock is frozen. */
#ifndef BL_USB_TX_DEAD_CLOCK_POLLS
#if FW_USB_IRQ
#define BL_USB_TX_DEAD_CLOCK_POLLS  65536u
#else
#define BL_USB_TX_DEAD_CLOCK_POLLS  4096u   /* ~40 ms */
#endif
#endif

#ifndef BL_USB_TIME_MS
#  ifdef BL_HOST  /* ../bootloader/host/usb_model.c has no timebase to link */
#    include <stdint.h>
static inline uint32_t bl_usb_host_time_ms(void)
{
    static uint32_t t;      /* one counter per translation unit */
    return ++t;
}
#    define BL_USB_TIME_MS()    bl_usb_host_time_ms()
#  else
#    define BL_USB_TIME_MS()    bl_time_ms()
#  endif
#endif

#ifndef BL_USB_INIT_MAX_GAP_MS
#define BL_USB_INIT_MAX_GAP_MS  24u     /* PLL settle + detach, back to back */
#endif

#define BL_USB_EP0_PKT          8u        /* bMaxPacketSize0                   */
#define BL_USB_EP2_PKT          32u       /* CH340 bulk max; RX window is 64   */

extern uint8_t bl_usb_ep2_pkt_in;  /* runtime; the transmit path chunks to it */
extern uint8_t bl_usb_ep2_dbuf;
#define BL_USB_EP2_PKT_IN_MAX   64u     /* datasheet V2.1 17.2.2 cap         */

#define BL_USB_DESC_SET_CDC     0u      /* mirrored to the host through      */
#define BL_USB_DESC_SET_CH340   1u      /* VAR16_FW_USB_DESC                 */

#if FW_USB_CDC
/* Not WCH's 1A86:7523: CH341SER.SYS binds a CDC device on that pair too. */
#define BL_USB_CDC_VID          0x1209u   /* pid.codes                        */
#define BL_USB_CDC_PID          0x0008u   /* pid.codes test range             */


#define BL_USB_EP2_PKT_CDC      64u

#define BMREQ_CLASS_IN          0xA1u   /* device-to-host, class, interface   */
#define BMREQ_CLASS_OUT         0x21u   /* host-to-device, class, interface   */
#define CDC_SET_LINE_CODING     0x20u
#define CDC_GET_LINE_CODING     0x21u
#define CDC_SET_CONTROL_LINE_STATE 0x22u
#define CDC_LINE_CODING_LEN     7u
#define CDC_CTRL_DTR            0x0001u /* wValue bit 0 of 0x22              */

#endif /* FW_USB_CDC */

void bl_usb_init(void);   /* replays USB_DeviceInit (0x4B24); not re-entrant */
void bl_usb_poll(void);   /* one hardware event per call; never waits on it  */

/* Non-blocking.  Also re-opens bulk OUT once draining makes room: receive
 * credit follows receive-buffer space alone, never tx. */
uint32_t bl_usb_rx(uint8_t *buf, uint32_t max);

/* Returns the count queued; short only after a BL_USB_TX_TIMEOUT_MS stall. */
uint32_t bl_usb_tx(const uint8_t *buf, uint32_t n);



#if FW_USB_IRQ
#define BL_USB_IRQ_FAULT_NONE       0u  /* the codes below are sticky      */
#define BL_USB_IRQ_FAULT_VECTOR     1u  /* vectors.S built w/o FW_USB_IRQ  */
#define BL_USB_IRQ_FAULT_NOENTRY    2u  /* bootloader drops vector 22      */
#define BL_USB_IRQ_FAULT_SPURIOUS   3u  /* re-entry, nothing to clear      */
#define BL_USB_IRQ_FAULT_WEDGE      4u  /* bytes owed, no ISR progress     */
#define BL_USB_IRQ_FAULT_STALL      5u  /* gave up after FW_TX_STALL_MS    */
#define BL_USB_IRQ_FAULT_REINIT     6u  /* re-inited under the caller      */
#define BL_USB_IRQ_FAULT_STARVE     7u  /* USB_IRQ_BURST_LIMIT entries     */

void fw_usb_irq_handler(void);  /* vector 22, via the trampoline at 0x0000 */

/* Proves the vector path before enabling IRQ6; call only once the host has
 * configured the device.  A no-op when armed and once a fault has latched. */
int bl_usb_irq_arm(void);

void bl_usb_irq_disarm(uint8_t reason);  /* R8_USB_INT_FG latches; none lost */

int      bl_usb_irq_armed(void);
uint8_t  bl_usb_irq_fault(void);
uint32_t bl_usb_irq_entries(void);   /* counted before any branch in the ISR  */
uint32_t bl_usb_events(void);        /* both modes; survives a demotion       */

int bl_usb_tx_pending(void);  /* bytes the SIE has not taken: idle vs wedged */
#endif /* FW_USB_IRQ */

/* SET_CONFIGURATION seen, non-zero.  Do not gate bulk traffic on it: the CH340
 * port-open flag is dead state; its 0xA4 MODEM_CTRL compare misses drivers. */
int bl_usb_configured(void);

/* Stock CH340 descriptors; do not tidy. Provenance in src/usb_desc.c. */
extern const uint8_t bl_usb_desc_device[18];
extern const uint8_t bl_usb_desc_config[39];

#if FW_USB_CDC
extern const uint8_t bl_usb_desc_device_cdc[18];
#define BL_USB_CDC_CFG_LEN      67u
extern const uint8_t bl_usb_desc_config_cdc[BL_USB_CDC_CFG_LEN];
#endif

#if FW_USB_CDC
#define bl_usb_desc_dev_live    bl_usb_desc_device_cdc
#define bl_usb_desc_cfg_live    bl_usb_desc_config_cdc
#else
#define bl_usb_desc_dev_live    bl_usb_desc_device
#define bl_usb_desc_cfg_live    bl_usb_desc_config
#endif

extern const uint8_t bl_usb_ch340_vendor_tbl[26];

#define BL_USB_VENDOR_TBL_LAST  24u     /* cursor saturates at 0x482A, no wrap */

/* Defaulted so a TU that includes this builds without the Makefile's flags. */
#ifndef FW_FAST_CLOCK
#define FW_FAST_CLOCK 0
#endif
#ifndef FW_FAST_COPY
#define FW_FAST_COPY 0
#endif
#ifndef FW_TX_REWIND
#define FW_TX_REWIND 0
#endif
#ifndef FW_TX_DIRECT
#define FW_TX_DIRECT 0
#endif
#ifndef FW_RX_DBUF
#define FW_RX_DBUF 0
#endif

/* The pipeline stages both IN windows through tx_direct_ready() and the staging
 * ring, which src/usb.c defines only under FW_TX_DIRECT. */
#if FW_TX_PIPELINE && !FW_TX_DIRECT
#error "FW_TX_PIPELINE needs FW_TX_DIRECT: it stages the direct region."
#endif
#if FW_TX_DIRECT
/* Zero-copy byte stream; ordering and lifetime rules are above tx_pump() in
 * src/usb.c.  begin() needs the staging ring empty; end() runs on every path. */
void     bl_usb_tx_direct_begin(const uint8_t *base, uint16_t total);
void     bl_usb_tx_direct_publish(uint16_t avail);
uint16_t bl_usb_tx_direct_sent(void);
void     bl_usb_tx_direct_end(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* BL_USB_H */
