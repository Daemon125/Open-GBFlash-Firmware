/* usb.c: CH579 USB device layer. FW_USB_CDC=1 is CDC-ACM, 64-byte bulk;
 * FW_USB_CDC=0 is the CH340 1A86:7523 set with 32 and the runtime fallback.
  * Vector 22 (IRQ6) is reachable only through the bootloader's trampoline at
 * 0x0000; bl_usb_irq_arm() probes that path. */

#include <stdint.h>

#include "bl_config.h"
#include "usb.h"

/* The two runs here that never read the clock: bl_usb_tx()'s wait and
 * bl_usb_init()'s PLL settle plus detach. Real durations, not nominal. */
__extension__ _Static_assert((uint32_t)BL_USB_TX_MAX_GAP_MS
                                 + (uint32_t)BL_USB_INIT_MAX_GAP_MS
                             < (uint32_t)BL_TIME_MAX_GAP_MS,
                             "usb.c's longest run between two bl_time_ms() "
                             "calls must fit inside one lap of the timebase");

__extension__ _Static_assert((uint32_t)BL_USB_TX_DEAD_CLOCK_POLLS >= 1024u,
                             "the dead-clock backstop could fire on a healthy "
                             "device before one millisecond has elapsed");

#define USB_BASE            0x40008000u

#define R8_USB_CTRL         BL_REG8(USB_BASE + 0x00u)
#define R8_UDEV_CTRL        BL_REG8(USB_BASE + 0x01u)
#define R8_USB_INT_EN       BL_REG8(USB_BASE + 0x02u)
#define R8_USB_DEV_AD       BL_REG8(USB_BASE + 0x03u)
#define R8_USB_MIS_ST       BL_REG8(USB_BASE + 0x05u)   /* read-only          */
#define R8_USB_INT_FG       BL_REG8(USB_BASE + 0x06u)
#define R8_USB_INT_ST       BL_REG8(USB_BASE + 0x07u)   /* read-only          */
#define R8_USB_RX_LEN       BL_REG8(USB_BASE + 0x08u)   /* read-only          */
#define R8_UEP4_1_MOD       BL_REG8(USB_BASE + 0x0Cu)
#define R8_UEP2_3_MOD       BL_REG8(USB_BASE + 0x0Du)
#define R16_UEP0_DMA        BL_REG16(USB_BASE + 0x10u)
#define R16_UEP1_DMA        BL_REG16(USB_BASE + 0x14u)
#define R16_UEP2_DMA        BL_REG16(USB_BASE + 0x18u)
#define R8_UEP0_T_LEN       BL_REG8(USB_BASE + 0x20u)
#define R8_UEP0_CTRL        BL_REG8(USB_BASE + 0x22u)
#define R8_UEP1_CTRL        BL_REG8(USB_BASE + 0x26u)
#define R8_UEP2_T_LEN       BL_REG8(USB_BASE + 0x28u)
#define R8_UEP2_CTRL        BL_REG8(USB_BASE + 0x2Au)

/* USB analog pad enable.  Plain RW: no safe-access window. */
#define R16_PIN_ANALOG_IE   BL_REG16(0x4000101Au)
#define RB_PIN_USB_IE       0x0080u

/* NVIC_ISER must never appear in the bootloader build; usb.h's header note. */
#define NVIC_ICER           BL_REG32(0xE000E180u)
#define NVIC_ICPR           BL_REG32(0xE000E280u)
#define USB_IRQ_BIT         (1u << 6)          /* IRQ6 = USB                  */

#if FW_USB_IRQ
#define NVIC_ISER           BL_REG32(0xE000E100u)
#define NVIC_ISPR           BL_REG32(0xE000E200u)

/* Index 22 of this image's table, src/vectors.S at 0x00004000. */
#define FW_USB_VECTOR_INDEX 22u
extern const uint32_t __vectors[];
#endif

/* Bit constants, verbatim from WCH CH579SFR.h. */
#define RB_UC_DEV_PU_EN     0x20u
#define RB_UC_INT_BUSY      0x08u
#define RB_UC_DMA_EN        0x01u

#define RB_UD_PD_DIS        0x80u
#define RB_UD_PORT_EN       0x01u

#define RB_UIE_SUSPEND      0x04u
#define RB_UIE_TRANSFER     0x02u
#define RB_UIE_BUS_RST      0x01u

#define RB_UIF_FIFO_OV      0x10u
#define RB_UIF_SUSPEND      0x04u
#define RB_UIF_TRANSFER     0x02u
#define RB_UIF_BUS_RST      0x01u

#define RB_UIS_TOG_OK       0x40u
#define MASK_UIS_TOKEN_EP   0x3Fu              /* MASK_UIS_TOKEN|MASK_UIS_ENDP */

#define RB_UEP1_TX_EN       0x40u
#define RB_UEP2_BUF_MOD     0x01u
#define RB_UEP2_RX_EN       0x08u
#define RB_UEP2_TX_EN       0x04u

#define RB_UEP_R_TOG        0x80u   /* SIE-managed on EP2 (AUTO_TOG); see
                                     * uep2_ctrl_rmw()                        */
#define RB_UEP_T_TOG        0x40u   /* likewise                               */
#define RB_UEP_AUTO_TOG     0x10u
#define MASK_UEP_R_RES      0x0Cu
#define   UEP_R_RES_ACK     0x00u
#define   UEP_R_RES_NAK     0x08u
#define MASK_UEP_T_RES      0x03u
#define   UEP_T_RES_ACK     0x00u
#define   UEP_T_RES_NAK     0x02u

#define UEP0_CTRL_IDLE      0x02u   /* R_RES=ACK, T_RES=NAK, toggles cleared  */
#define UEP0_CTRL_SETUP_ARM 0xC2u   /* R_TOG|T_TOG, ACK/NAK                   */
#define UEP0_CTRL_DATA      0xC0u   /* R_TOG|T_TOG, ACK/ACK; first IN = DATA1 */
#define UEP0_CTRL_STALL     0xCFu   /* R_TOG|T_TOG, STALL both directions     */
#define UEPn_CTRL_BULK      0x12u   /* AUTO_TOG, R_RES=ACK, T_RES=NAK         */

/* Transfer dispatch values, R8_USB_INT_ST & 0x3F. */
#define TOK_OUT_EP0         0x00u
#define TOK_OUT_EP2         0x02u
#define TOK_IN_EP0          0x20u
#define TOK_IN_EP2          0x22u
#define TOK_SETUP_EP0       0x30u
/* 0x21 (IN, EP1) is absent: EP1 sits at T_RES=NAK forever. */

/* Everything not listed STALLs, CLEAR_FEATURE(ENDPOINT_HALT) included. */
#define REQ_SET_ADDRESS     5u
#define REQ_GET_DESCRIPTOR  6u
#define REQ_GET_CONFIG      8u
#define REQ_SET_CONFIG      9u
#if FW_USB_CDC
#define REQ_GET_INTERFACE   10u
#define REQ_SET_INTERFACE   11u
#endif

#define DESC_TYPE_DEVICE    1u
#define DESC_TYPE_CONFIG    2u

/* Whole-byte match as at 0x4804: 0xC1/0xC2/0x41/0x42 fall through and STALL. */
#define BMREQ_VENDOR_IN     0xC0u
#define BMREQ_VENDOR_OUT    0x40u

/* The one vendor bRequest read here: the session-start marker.
 * See usb_session_flush(). */
#define CH341_REQ_SERIAL_INIT   0xA1u

/* R16_UEPn_DMA holds the low half of a 0x20000000-based address, so these must
 * be SRAM and 4-byte aligned. Sizes are the hardware window, not the payload. */
static volatile uint8_t ep0_buf[64]  __attribute__((aligned(4)));
static volatile uint8_t ep1_buf[64]  __attribute__((aligned(4)));
/* 256 is the dual-buffer layout, not slack: BUF_MOD uses all four 64-byte
 * windows, TX1 ending at +0x100. Undersize it and the SIE DMAs past the end. */
static volatile uint8_t ep2_buf[256] __attribute__((aligned(4)));

/* 32, not 64: Windows loads CH341SER.SYS for 1A86:7523 and a real CH340 has
 * 32-byte bulk endpoints. 64 hangs mid-transfer there; stock declares 32 too. */
#if FW_USB_CDC
/* 64 with the CDC set: this build does not claim to be a CH340. */
uint8_t bl_usb_ep2_pkt_in = BL_USB_EP2_PKT_CDC;
#else
uint8_t bl_usb_ep2_pkt_in = BL_USB_EP2_PKT;
#endif

/* On for correctness: short-packet receive corruption is a race between the SIE
 * writing the window and usb_copy() reading it. Two windows close it. */
uint8_t bl_usb_ep2_dbuf = 1u;

/* EP2 layout, datasheet V2.1 p.88 Table 17-4, RX_EN=TX_EN=BUF_MOD=1: RX at
 * +0/+64 by RB_UEP_R_TOG, TX at +128/+192 by RB_UEP_T_TOG. */
#define EP2_SINGLE_RX_OFF   0x00u
#define EP2_SINGLE_TX_OFF   0x40u
#define EP2_DBUF_RX0_OFF    0x00u
#define EP2_DBUF_RX1_OFF    0x40u
#define EP2_DBUF_TX0_OFF    0x80u
#define EP2_DBUF_TX1_OFF    0xC0u

/* Which half the SIE used. AUTO_TOG means the toggle names the NEXT
 * transaction's buffer, so the packet just received is in the other one. */
#ifndef EP2_DBUF_TOG_LAG
#define EP2_DBUF_TOG_LAG    1u
#endif

static uint8_t ep2_rx_off(void)
{
    uint8_t tog;
    if (!bl_usb_ep2_dbuf) {
        return EP2_SINGLE_RX_OFF;
    }
    tog = (uint8_t)((R8_UEP2_CTRL & RB_UEP_R_TOG) != 0u);
    if (EP2_DBUF_TOG_LAG) {
        tog = (uint8_t)!tog;
    }
    return tog ? EP2_DBUF_RX1_OFF : EP2_DBUF_RX0_OFF;
}

static uint8_t ep2_tx_off(void)
{
    if (!bl_usb_ep2_dbuf) {
        return EP2_SINGLE_TX_OFF;
    }
    /* Staged before the transaction, so the toggle already names the window. */
    return ((R8_UEP2_CTRL & RB_UEP_T_TOG) != 0u) ? EP2_DBUF_TX1_OFF
                                                 : EP2_DBUF_TX0_OFF;
}

/* USB_SHARED is `volatile' with FW_USB_IRQ and nothing without, so the polled
 * image is identical. Without it credit is computed from a stale rx_head. */
#if FW_USB_IRQ
#define USB_SHARED volatile

__attribute__((always_inline))
static inline uint32_t usb_irq_lock(void)
{
    uint32_t primask;
    __asm volatile ("mrs %0, primask\n\tcpsid i"
                    : "=r" (primask) : : "memory");
    return primask;
}

__attribute__((always_inline))
static inline void usb_irq_unlock(uint32_t primask)
{
    if (primask == 0u) {
        __asm volatile ("cpsie i" : : : "memory");
    }
}

#define USB_LOCK_BEGIN  { uint32_t usb_lock_pm_ = usb_irq_lock();
#define USB_LOCK_END      usb_irq_unlock(usb_lock_pm_); }
#else
#define USB_SHARED
#define USB_LOCK_BEGIN  {
#define USB_LOCK_END    }
#endif

/* Reset on bus reset. Not USB_SHARED: only the transfer handler touches it. */
static const uint8_t *ep0_descr;    /* running source pointer, GET_DESCRIPTOR */
static uint16_t       ep0_req_len;  /* bytes left; also the parked address     */
static uint8_t        ep0_req_code;
static uint8_t        ep0_req_type;
static USB_SHARED uint8_t usb_config;
static uint8_t        ch340_cursor;

#if FW_USB_CDC
/* Stored only so GET_LINE_CODING hands back what the host set. 115200 8N1. */
static uint8_t cdc_line_coding[CDC_LINE_CODING_LEN] = {
    0x00, 0xC2, 0x01, 0x00,   /* dwDTERate 115200                            */
    0x00,                     /* bCharFormat 1 stop bit                      */
    0x00,                     /* bParityType none                            */
    0x08,                     /* bDataBits                                    */
};

static uint8_t cdc_dtr;

static USB_SHARED uint8_t tx_last_full;  /* last armed packet filled the endpoint */
#endif

/* rx_buf is a ring; tx_buf is linear and rewinds only when fully drained. */
static uint8_t  rx_buf[BL_USB_RX_BUF_SIZE] __attribute__((aligned(4)));
#if FW_USB_ISR_LEAN
/* Pipelined windows awaiting completion. Each must hold a full direct packet;
 * completion credits bl_usb_ep2_pkt_in bytes for it. */
static USB_SHARED uint8_t tx_pipe_outstanding;
#endif

#if FW_USB_ISR_LEAN
/* Credit depends only on ring space, so set this wherever a delivered packet
 * consumes some; bl_usb_rx() frees space and updates credit on its own path. */
static USB_SHARED uint8_t rx_credit_dirty;
#endif

/* Single producer/consumer: the handler writes rx_head, bl_usb_rx() rx_tail. */
static USB_SHARED uint16_t rx_head, rx_tail;
static uint8_t  tx_buf[BL_USB_TX_BUF_SIZE] __attribute__((aligned(4)));
/* Written from both contexts, so the transmit side needs the lock; see
 * tx_queue(). */
static USB_SHARED uint16_t tx_head, tx_tail;
static USB_SHARED uint8_t  tx_armed;

/* Bumped on every staging flush for a new host session (6b). */
static USB_SHARED uint32_t session_id;

#if FW_USB_IRQ
/* Non-zero while IRQ6 owns the SIE. */
static USB_SHARED uint8_t  usb_irq_on;

/* Sticky reason the layer is not in interrupt mode. */
static USB_SHARED uint8_t  usb_irq_fault;

static USB_SHARED uint32_t usb_irq_entry_count;

/* Consecutive handler entries that found nothing to clear. */
static USB_SHARED uint8_t  usb_irq_idle_entries;

/* Handler entries since thread mode last ran; zeroed by bl_usb_poll(). */
static USB_SHARED uint32_t usb_irq_burst;

/* Hardware events actually serviced, in EITHER mode. */
static USB_SHARED uint32_t usb_event_count;

/* An uncleanable source re-asserts on every return: thread mode never runs. */
#define USB_IRQ_IDLE_LIMIT  8u

/* Entries without thread mode running before the handler gives up: ~10 ms. */
#define USB_IRQ_BURST_LIMIT 20000u
#endif /* FW_USB_IRQ */

#define RX_CREDIT_BYTES     64u  /* a full RX window: no over-long packet overruns */
#define EP2_RX_WINDOW       64u  /* datasheet p.86, max on every endpoint          */

/* volatile also stops GCC's loop-distribution pass emitting a memcpy call. */
static void usb_copy(volatile uint8_t *dst, const volatile uint8_t *src,
                     uint32_t n)
{
#if FW_FAST_COPY
    
    if (n >= 16u && ((((uintptr_t)dst | (uintptr_t)src) & 3u) == 0u)) {
#if defined(__ARM_ARCH_6M__)
        uint32_t blocks = n >> 4;
        uint32_t d = (uint32_t)(uintptr_t)dst;
        uint32_t s = (uint32_t)(uintptr_t)src;

        __asm__ volatile (
            /* See usb_pll_power_on() for the `.syntax unified'. */
            ".syntax unified\n\t"
            "1:\n\t"
            "ldmia %[s]!, {r4, r5, r6, r7}\n\t"
            "stmia %[d]!, {r4, r5, r6, r7}\n\t"
            "subs  %[k], %[k], #1\n\t"
            "bne   1b\n\t"
            : [s] "+l" (s), [d] "+l" (d), [k] "+l" (blocks)
            :
            : "r4", "r5", "r6", "r7", "cc", "memory");

        dst = (volatile uint8_t *)(uintptr_t)d;
        src = (const volatile uint8_t *)(uintptr_t)s;
#else
        /* Portable equivalent for the host model; same 16-byte stride. */
        volatile uint32_t *d32 = (volatile uint32_t *)(void *)dst;
        const volatile uint32_t *s32 =
            (const volatile uint32_t *)(const void *)src;
        uint32_t blocks = n >> 4;

        while (blocks-- != 0u) {
            d32[0] = s32[0];
            d32[1] = s32[1];
            d32[2] = s32[2];
            d32[3] = s32[3];
            d32 += 4;
            s32 += 4;
        }
        dst = (volatile uint8_t *)(void *)d32;
        src = (const volatile uint8_t *)(const void *)s32;
#endif
        n &= 15u;
    }
#endif /* FW_FAST_COPY */

    if (n >= 4u &&
        ((((uintptr_t)dst | (uintptr_t)src) & 3u) == 0u)) {
        volatile uint32_t *d32 = (volatile uint32_t *)(void *)dst;
        const volatile uint32_t *s32 =
            (const volatile uint32_t *)(const void *)src;

        while (n >= 4u) {
            *d32++ = *s32++;
            n -= 4u;
        }
        dst = (volatile uint8_t *)(void *)d32;
        src = (const volatile uint8_t *)(const void *)s32;
    }

    while (n != 0u) {
        *dst = *src;
        dst++;
        src++;
        n--;
    }
}

/* Runs 1.75x long, and holds whether or not bl_time_init() has run. `left' is
 * volatile to force the loop-entry test, or a 0 argument runs for ~43 days. */
#if BL_USB_PLL_POWER_ON
static void usb_delay_ms(uint32_t ms)
{
    volatile uint32_t left = ms;

    while (left != 0u) {
        volatile uint32_t n = 4000u;
        while (n != 0u) {
            n--;
        }
        left--;
    }
}
#endif /* BL_USB_PLL_POWER_ON */

/* R8_UEP2_CTRL: software owns R_RES/T_RES/AUTO_TOG, the SIE owns R_TOG/T_TOG
 * and advances them on an accepted transaction. A read-modify-write straddling
 * one writes back a stale toggle: dropped packet or stale PID. Do not defer the
 * store into the RB_UIF_TRANSFER window; a NAKed OUT and a NAKed IN raise no
 * flag and the link deadlocks. */

/* clear_mask must name only software-owned bits; the toggles are preserved. */
static void uep2_ctrl_rmw(uint8_t clear_mask, uint8_t set_bits)
{
    uint8_t ctrl;
    uint8_t val;
    uint8_t st;
    uint8_t tok;
    uint8_t fix;
    uint32_t pass;

    for (pass = 0u; pass < 2u; pass++) {

        if ((R8_USB_INT_FG & RB_UIF_TRANSFER) != 0u) {
            /* Window open: RB_UC_INT_BUSY holds the SIE off until the clear. */
            ctrl = R8_UEP2_CTRL;
            R8_UEP2_CTRL = (uint8_t)((ctrl & (uint8_t)~clear_mask) | set_bits);
            return;
        }

        ctrl = R8_UEP2_CTRL;

        if ((R8_USB_INT_FG & RB_UIF_TRANSFER) != 0u) {
            /* Latched, so the next pass takes the window-open branch above. */
            continue;
        }

        val = (uint8_t)((ctrl & (uint8_t)~clear_mask) | set_bits);
        R8_UEP2_CTRL = val;

        if ((R8_USB_INT_FG & RB_UIF_TRANSFER) == 0u) {
            return;
        }

        /* A transaction completed across the store, so the toggle written is one
         * step behind. R8_USB_INT_ST is latched and still names the token. */
        st  = R8_USB_INT_ST;
        tok = (uint8_t)(st & MASK_UIS_TOKEN_EP);
        fix = 0u;

        if (tok == TOK_OUT_EP2) {
            /* Only an in-sequence packet advances the receive toggle. */
            if ((st & RB_UIS_TOG_OK) != 0u) {
                fix = RB_UEP_R_TOG;
            }
        } else if (tok == TOK_IN_EP2) {
            fix = RB_UEP_T_TOG;
        } else {
            /* An EP0 token; the SIE does not touch R8_UEP2_CTRL for those. */
        }

        if (fix != 0u) {
            R8_UEP2_CTRL = (uint8_t)(val ^ fix);
        }
        return;
    }

    /* Unreachable: pass 0 continues only with the flag set, nothing clears it. */
}

/* bsp_init (0x42B6): safe-access window, R8_HFCK_PWR_CTRL |= 0x10
 * (RB_CLK_PLL_PON), 3 ms. Read-modify-write, never a plain store:
 * RB_CLK_INT32M_PON in the same register clocks the core. The window is ~16
 * Tsys wide and an overrun drops the write, so the read is hoisted out of it. */
#if BL_USB_PLL_POWER_ON
#define RB_CLK_PLL_PON      0x10u

/* Spelled out in the asm template; these tie them to bl_config.h. */
__extension__ _Static_assert(BL_SAFE_ACCESS_SIG1 == 0x57, "SAM signature 1");
__extension__ _Static_assert(BL_SAFE_ACCESS_SIG2 == 0xA8, "SAM signature 2");

static void usb_pll_power_on(void)
{
    uint32_t sig = (uint32_t)BL_R8_SAFE_ACCESS_SIG;   /* 0x40001040 */
    uint32_t hfc = (uint32_t)BL_R8_HFCK_PWR_CTRL;     /* 0x4000100A */
    uint32_t t, u;

    __asm volatile (
        /* Required: GCC wraps inline asm in `.syntax divided' by default. */
        ".syntax unified          \n\t"
        "ldrb  %[t], [%[hf]]      \n\t"   /* read current, PON bits live      */
        "movs  %[u], #0x10        \n\t"   /* RB_CLK_PLL_PON                   */
        "orrs  %[t], %[t], %[u]   \n\t"
        "movs  %[u], #0x57        \n\t"
        "strb  %[u], [%[sg]]      \n\t"   /* unlock 1 of 2                    */
        "movs  %[u], #0xA8        \n\t"
        "strb  %[u], [%[sg]]      \n\t"   /* unlock 2 of 2: window opens      */
                "strb  %[t], [%[hf]]      \n\t"   /* the protected store              */
        "movs  %[u], #0           \n\t"
        "strb  %[u], [%[sg]]      \n\t"   /* re-lock                          */
        : [t] "=&l" (t), [u] "=&l" (u)
        : [hf] "l" (hfc), [sg] "l" (sig)
        : "memory", "cc");

    usb_delay_ms(BL_USB_PLL_SETTLE_MS);
}
#endif /* BL_USB_PLL_POWER_ON */


__extension__ _Static_assert((BL_USB_RX_BUF_SIZE & (BL_USB_RX_BUF_SIZE - 1u)) == 0u,
                             "the receive ring's size must be a power of two: "
                             "the wrap is a mask, not a modulo");
__extension__ _Static_assert((65536u % BL_USB_RX_BUF_SIZE) == 0u,
                             "the free-running index wrap must be a whole "
                             "number of buffers, or occupancy breaks at 65535");

#define RX_MASK             (BL_USB_RX_BUF_SIZE - 1u)

static uint16_t rx_used(void)
{
    return (uint16_t)(rx_head - rx_tail);
}

static uint16_t rx_free(void)
{
    return (uint16_t)(BL_USB_RX_BUF_SIZE - rx_used());
}

/* Open or close bulk OUT purely on receive-buffer space. Never from the transmit
 * path: re-ACKing OUT blind to receive state discards a whole packet. */
#if FW_RX_DBUF
static void rx_credit_update_n(uint16_t pending)
#else
static void rx_credit_update(void)
#endif
{
#if FW_RX_DBUF
    uint16_t space = rx_free();
    uint8_t  want;

    /* Unreachable; an underflow would wrap and grant credit into a full ring. */
    if (pending > space) {
        pending = space;
    }
    want = ((uint16_t)(space - pending) >= RX_CREDIT_BYTES) ? UEP_R_RES_ACK
                                                           : UEP_R_RES_NAK;
#else
    uint8_t want = (rx_free() >= RX_CREDIT_BYTES) ? UEP_R_RES_ACK
                                                  : UEP_R_RES_NAK;
#endif

    /* Write only on a transition: every store is an exposure to the 3b hazard. */
    if ((uint8_t)(R8_UEP2_CTRL & MASK_UEP_R_RES) != want) {
        uep2_ctrl_rmw(MASK_UEP_R_RES, want);
    }
}

#if FW_RX_DBUF
static void rx_credit_update(void)
{
    rx_credit_update_n(0u);
}
#endif

/* One accepted bulk OUT packet. The offset is an argument, not a re-read:
 * FW_RX_DBUF releases the bus, after which RB_UEP_R_TOG may have flipped. */
#if FW_RX_DBUF
static void rx_deliver_at(uint8_t off, uint32_t len)
#else
static void rx_deliver(uint32_t len)
#endif
{
#if FW_USB_ISR_LEAN
    rx_credit_dirty = 1u;
#endif
    uint16_t space = rx_free();

    /* Clamp to the window, not to sizeof ep2_buf: 256 overruns into TX. */
    if (len > EP2_RX_WINDOW) {
        len = EP2_RX_WINDOW;
    }
    if (len > space) {
        len = space;                           /* unreachable given credit    */
    }
    if (len != 0u) {
        /* Split at the wrap; only the destination can straddle the ring end. */
        uint16_t at = (uint16_t)(rx_head & RX_MASK);
        uint32_t first = BL_USB_RX_BUF_SIZE - at;
        if (first > len) {
            first = len;
        }
#if FW_RX_DBUF
        usb_copy(&rx_buf[at], &ep2_buf[off], first);
        if (len > first) {
            usb_copy(&rx_buf[0], &ep2_buf[off + first], len - first);
        }
#else
        usb_copy(&rx_buf[at], &ep2_buf[ep2_rx_off()], first);
        if (len > first) {
            usb_copy(&rx_buf[0], &ep2_buf[ep2_rx_off() + first], len - first);
        }
#endif
        rx_head = (uint16_t)(rx_head + len);
    }
}

/* Arm the next IN packet, or park the endpoint at NAK when drained. The CH340
 * identity must have no zero-length-packet logic; CDC needs one, below.
 * FW_TX_DIRECT reads the nominated region only while the staging ring is empty,
 * and only up to tx_dir_avail, a rising count. */
#if FW_TX_DIRECT
static const uint8_t * USB_SHARED tx_dir_base;  /* 0 when none nominated     */
static USB_SHARED uint16_t tx_dir_avail;  /* bytes the producer has published  */
static USB_SHARED uint16_t tx_dir_pos;    /* bytes handed to the SIE           */
static USB_SHARED uint16_t tx_dir_total;  /* region size, set by begin()       */


#if FW_TX_PIPELINE
#if FW_USB_TX_TOG_SHADOW
static USB_SHARED uint8_t tx_tog_pred;
static USB_SHARED uint8_t tx_tog_valid;
#endif

static USB_SHARED uint8_t tx_pipe_armed;  /* both windows hold valid bytes */

/* Which staged packets belonged to the region: a CDC terminator, a reply header
 * and an ACK all raise the same completion. Two can be outstanding. */
#define TX_STAGE_SLOTS 4u
static USB_SHARED uint8_t  tx_stage_dir[TX_STAGE_SLOTS];
static USB_SHARED uint16_t tx_stage_len[TX_STAGE_SLOTS];
static USB_SHARED uint8_t  tx_stage_head, tx_stage_tail;

static void tx_stage_push(uint16_t len, uint8_t is_direct)
{
    tx_stage_len[tx_stage_head] = len;
    tx_stage_dir[tx_stage_head] = is_direct;
    tx_stage_head = (uint8_t)((tx_stage_head + 1u) & (TX_STAGE_SLOTS - 1u));
}

/* Returns the direct-region length just delivered, or 0 for anything else. */
static uint16_t tx_stage_pop(void)
{
    uint16_t n;
    if (tx_stage_head == tx_stage_tail) {
        return 0u;
    }
    n = tx_stage_dir[tx_stage_tail] ? tx_stage_len[tx_stage_tail] : 0u;
    tx_stage_tail = (uint8_t)((tx_stage_tail + 1u) & (TX_STAGE_SLOTS - 1u));
    return n;
}

/* Bytes the SIE has SENT; tx_dir_pos counts bytes copied into a window. */
static USB_SHARED uint16_t tx_dir_sent_n;

/* Priming needs two full packets; sticky for the life of the region. */
static USB_SHARED uint8_t tx_pipe_engaged;
#endif

static void tx_direct_reset(void)
{
#if FW_TX_PIPELINE
    tx_pipe_armed = 0u;
    tx_pipe_engaged = 0u;
#if FW_USB_TX_TOG_SHADOW
    tx_tog_valid = 0u;
#endif
#if FW_USB_ISR_LEAN
    tx_pipe_outstanding = 0u;
#endif
    tx_dir_sent_n = 0u;
    tx_stage_head = 0u;
    tx_stage_tail = 0u;
#endif
    tx_dir_base  = (const uint8_t *)0;
    tx_dir_avail = 0u;
    tx_dir_pos   = 0u;
    tx_dir_total = 0u;
}

static uint16_t tx_direct_ready(void)
{
    if (tx_dir_base == (const uint8_t *)0) {
        return 0u;
    }
    return (uint16_t)(tx_dir_avail - tx_dir_pos);
}
#else
#define tx_direct_reset()   ((void)0)
#endif

/* FW_TX_PIPELINE clears RB_UIF_TRANSFER before the copy. The rule that makes
 * that legal: never clear the flag unless the window the SIE sends next already
 * holds valid bytes. Single-deep and must stay so, since one R8_UEP2_T_LEN
 * serves both windows: no short final packet, no CDC terminator, no staging
 * ring. Requires FW_USB_IRQ. */
#if FW_TX_PIPELINE
/* The window the SIE is not about to send from, i.e. the one to refill. */
static uint8_t ep2_tx_other(void)
{
    /* Unreachable while priming requires bl_usb_ep2_dbuf. */
    if (!bl_usb_ep2_dbuf) {
        return EP2_SINGLE_TX_OFF;
    }
    return (ep2_tx_off() == EP2_DBUF_TX1_OFF) ? EP2_DBUF_TX0_OFF
                                              : EP2_DBUF_TX1_OFF;
}

static uint16_t tx_pipe_ready(void)
{
    return tx_direct_ready();
}
#endif

static void tx_pump(void)
{
    uint16_t n = (uint16_t)(tx_head - tx_tail);

#if FW_TX_DIRECT
    if (n == 0u) {
        uint16_t m = tx_direct_ready();

        if (m != 0u) {
            if (m > (uint16_t)bl_usb_ep2_pkt_in) {
                m = (uint16_t)bl_usb_ep2_pkt_in;
            }
            /* A short packet ends the host's bulk transfer, so emit one only
             * when it completes the region. Does NOT fix the Stream read
             * desync of 2026-08-23; that is still unexplained. See
             * results/STATUS.md. */
            if ((m < (uint16_t)bl_usb_ep2_pkt_in) &&
                ((uint16_t)(tx_dir_pos + m) != tx_dir_total)) {
                return;
            }
#if FW_TX_PIPELINE
            /* Prime both windows; two FULL packets only, they share one T_LEN. */
            if ((tx_pipe_armed == 0u) && bl_usb_ep2_dbuf &&
                (m == (uint16_t)bl_usb_ep2_pkt_in) &&
                (tx_direct_ready() >= (uint16_t)(2u * (uint32_t)bl_usb_ep2_pkt_in))) {
                uint8_t first = ep2_tx_off();
#if FW_USB_TX_TOG_SHADOW
                tx_tog_pred = first; tx_tog_valid = 1u;
#endif
                uint8_t second = (first == EP2_DBUF_TX1_OFF) ? EP2_DBUF_TX0_OFF
                                                             : EP2_DBUF_TX1_OFF;
                usb_copy(&ep2_buf[first], &tx_dir_base[tx_dir_pos], m);
                usb_copy(&ep2_buf[second], &tx_dir_base[tx_dir_pos + m], m);
                R8_UEP2_T_LEN = (uint8_t)m;
                tx_dir_pos = (uint16_t)(tx_dir_pos + (uint16_t)(2u * m));
#if FW_USB_ISR_LEAN
                tx_pipe_outstanding = (uint8_t)(tx_pipe_outstanding + 2u);
#else
                tx_stage_push(m, 1u);
                tx_stage_push(m, 1u);
#endif
                uep2_ctrl_rmw(MASK_UEP_T_RES, UEP_T_RES_ACK);
                tx_armed = 1u;
                tx_pipe_armed = 1u;
                tx_pipe_engaged = 1u;
#if FW_USB_CDC
                                tx_last_full = 1u;
#endif
                return;
            }
#endif
            usb_copy(&ep2_buf[ep2_tx_off()], &tx_dir_base[tx_dir_pos], m);
            R8_UEP2_T_LEN = (uint8_t)m;
            tx_dir_pos = (uint16_t)(tx_dir_pos + m);
#if FW_TX_PIPELINE
            tx_stage_push(m, 1u);
#endif
            uep2_ctrl_rmw(MASK_UEP_T_RES, UEP_T_RES_ACK);
            tx_armed = 1u;
#if FW_USB_CDC
            tx_last_full = (uint8_t)(m == (uint16_t)bl_usb_ep2_pkt_in);
#endif
            return;
        }
    }
#endif

    /* Both arms touch only MASK_UEP_T_RES, through the guarded helper (3b). */
    if (n != 0u) {
        if (n > bl_usb_ep2_pkt_in) {
            n = bl_usb_ep2_pkt_in;
        }
        usb_copy(&ep2_buf[ep2_tx_off()], &tx_buf[tx_tail], n);
        R8_UEP2_T_LEN = (uint8_t)n;
        tx_tail = (uint16_t)(tx_tail + n);
#if FW_TX_PIPELINE
        tx_stage_push(n, 0u);           /* ring bytes are not the region's */
#endif
#if FW_TX_REWIND
        /* The drain branch never runs during a cartridge read; rewind here. */
        if (tx_head == tx_tail) {
            tx_head = 0u;
            tx_tail = 0u;
        }
#endif
        uep2_ctrl_rmw(MASK_UEP_T_RES, UEP_T_RES_ACK);
        tx_armed = 1u;
#if FW_USB_CDC
        tx_last_full = (uint8_t)(n == bl_usb_ep2_pkt_in);
#endif
    }
#if FW_USB_CDC
    else if (tx_last_full != 0u) {
        /* CDC terminator. The CH340 identity must never send one. */
        R8_UEP2_T_LEN = 0u;
        tx_last_full  = 0u;
        uep2_ctrl_rmw(MASK_UEP_T_RES, UEP_T_RES_ACK);
        tx_armed = 1u;
    }
#endif
    else {
        tx_head = 0u;                 /* rewind: linear buffer, empty         */
        tx_tail = 0u;
        R8_UEP2_T_LEN = 0u;
        uep2_ctrl_rmw(MASK_UEP_T_RES, UEP_T_RES_NAK);
        tx_armed = 0u;
    }
}

#if FW_TX_DIRECT
/* Nominate `base'. Nothing is published yet, so this transmits nothing. */
void bl_usb_tx_direct_begin(const uint8_t *base, uint16_t total)
{
    USB_LOCK_BEGIN
    tx_dir_base  = base;
    tx_dir_avail = 0u;
    tx_dir_pos   = 0u;
    tx_dir_total = total;
#if FW_TX_PIPELINE
    /* Symmetric with tx_direct_reset(); do not rely on end() always preceding. */
    tx_pipe_armed = 0u;
    tx_pipe_engaged = 0u;
#if FW_USB_TX_TOG_SHADOW
    tx_tog_valid = 0u;
#endif
#if FW_USB_ISR_LEAN
    tx_pipe_outstanding = 0u;
#endif
    tx_dir_sent_n = 0u;
    tx_stage_head = 0u;
    tx_stage_tail = 0u;
#endif
    USB_LOCK_END
}

/* Publish base[0 .. avail) and kick: nothing else starts a chain from NAK. */
void bl_usb_tx_direct_publish(uint16_t avail)
{
    USB_LOCK_BEGIN
    tx_dir_avail = avail;
    if ((tx_armed == 0u) && (tx_direct_ready() != 0u)) {
        tx_pump();
    }
    USB_LOCK_END
}

/* How many published bytes the SIE has been handed; the caller drains on this. */
uint16_t bl_usb_tx_direct_sent(void)
{
#if FW_TX_PIPELINE
        /* Completions, not copies. */
    return tx_dir_sent_n;
#else
    return tx_dir_pos;
#endif
}

/* Withdraw the region. Published-but-unstaged bytes are dropped. */
void bl_usb_tx_direct_end(void)
{
    USB_LOCK_BEGIN
    tx_direct_reset();
    USB_LOCK_END
}
#endif /* FW_TX_DIRECT */

#if FW_USB_IRQ
/* Non-blocking queue. Returns how many bytes were taken. tx_pump()'s drain
 * branch zeroes tx_head and tx_tail between any two instructions here, so an
 * unguarded store resurrects a retired tx_head at tx_tail 0 and retransmits. */
static uint32_t tx_queue(const uint8_t *buf, uint32_t n)
{
    uint32_t done = 0u;

    while (done < n) {
        uint32_t want = n - done;
        uint32_t room;

        if (want > (uint32_t)BL_USB_TX_LOCK_CHUNK) {
            want = (uint32_t)BL_USB_TX_LOCK_CHUNK;
        }

        USB_LOCK_BEGIN
        room = (uint32_t)(BL_USB_TX_BUF_SIZE - tx_head);
        if (want > room) {
            want = room;
        }
        if (want != 0u) {
            usb_copy(&tx_buf[tx_head], &buf[done], want);
            tx_head = (uint16_t)(tx_head + want);
        }
        USB_LOCK_END

        if (want == 0u) {
            break;                    /* staging full; report what was taken */
        }
        done += want;
    }

    if (done != 0u) {
        /* The kick, under the same lock as the test or two tx_pump()s stage the
         * same bytes. `tx_head != tx_tail' keeps it out of the drain branch. */
        USB_LOCK_BEGIN
        if ((tx_armed == 0u) && (tx_head != tx_tail)) {
            tx_pump();
        }
        USB_LOCK_END
    }
    return done;
}
#else
/* Non-blocking queue.  Returns how many bytes were taken. */
static uint32_t tx_queue(const uint8_t *buf, uint32_t n)
{
    uint32_t room = (uint32_t)(BL_USB_TX_BUF_SIZE - tx_head);

    if (n > room) {
        n = room;
    }
    if (n != 0u) {
        usb_copy(&tx_buf[tx_head], buf, n);
        tx_head = (uint16_t)(tx_head + n);
        if (tx_armed == 0u) {
            tx_pump();                /* the kick; the endpoint would sit at
                                       * NAK forever otherwise                */
        }
    }
    return n;
}
#endif /* FW_USB_IRQ */

/* 6b. Session boundaries. Closing a ch341 port kills the driver's URBs but
 * leaves the device addressed, configured and attached, so the next open finds
 * the old session's bytes staged and maybe a packet at T_RES=ACK. A close
 * cannot be observed; an open can, since every ch34x driver sends 0x40/0xA1
 * first. The toggles must survive this flush, unlike a bus reset. */
static void usb_session_flush(void)
{
    rx_head  = 0u;
    rx_tail  = 0u;
    tx_head  = 0u;
    tx_tail  = 0u;
    tx_direct_reset();   /* FW_TX_DIRECT: the bytes are no longer owed         */

    /* Cancel any staged packet: nothing is owed across a session boundary. */
    R8_UEP2_T_LEN = 0u;
    uep2_ctrl_rmw(MASK_UEP_T_RES, UEP_T_RES_NAK);
    tx_armed = 0u;

    /* Re-derived from an empty ring, so this can only open the endpoint. */
    rx_credit_update();

    session_id++;
}

/* bmRequestType 0xC0, the CH340 vendor-IN replay. bRequest, wValue and wIndex
 * are never examined, as at 0x4804, so the table can only be reproduced. */
static void ep0_vendor_in(void)
{
    uint8_t i = ch340_cursor;

    ep0_buf[0] = bl_usb_ch340_vendor_tbl[i];
    ep0_buf[1] = bl_usb_ch340_vendor_tbl[i + 1u];

    /* Saturates at the last pair; it does NOT wrap (cmp #0x18 / bge). */
    ch340_cursor = (uint8_t)((i >= BL_USB_VENDOR_TBL_LAST)
                             ? BL_USB_VENDOR_TBL_LAST : (i + 2u));
}

#if FW_USB_CDC
/* bmRequestType 0xA1: class, device-to-host, interface. Returns 1 to STALL.
 * BL_USB_EP0_PKT is 8 and the answer is 7, so there is no continuation. */
static int ep0_class_in(void)
{
    uint32_t n;

    if (ep0_req_code != CDC_GET_LINE_CODING) {
        return 1;
    }
    if (ep0_req_len > (uint16_t)CDC_LINE_CODING_LEN) {
        ep0_req_len = (uint16_t)CDC_LINE_CODING_LEN;
    }
    ep0_descr = cdc_line_coding;
    n = (ep0_req_len >= BL_USB_EP0_PKT) ? BL_USB_EP0_PKT : ep0_req_len;
    usb_copy(ep0_buf, ep0_descr, n);
    ep0_descr += n;
    return 0;
}

/* bmRequestType 0x21: class, host-to-device, interface. Returns 1 to STALL.
 * SET_CONTROL_LINE_STATE is the session marker (6b); flush on the DTR edge. */
static int ep0_class_out(uint16_t wvalue)
{
    uint8_t dtr;

    if (ep0_req_code == CDC_SET_LINE_CODING) {
        return 0;                       /* data stage handled in ep0_out()   */
    }
    if (ep0_req_code == CDC_SET_CONTROL_LINE_STATE) {
        dtr = (uint8_t)((wvalue & (uint16_t)CDC_CTRL_DTR) != 0u);
        if ((dtr != 0u) && (cdc_dtr == 0u)) {
            usb_session_flush();
        }
        cdc_dtr = dtr;
        return 0;
    }
    return 1;
}
#endif /* FW_USB_CDC */

/* Standard requests. Returns 1 to STALL. no-jump-tables: GCC otherwise builds a
 * Thumb-1 dispatch table calling __gnu_thumb1_case_sqi, absent under -nostdlib. */
__attribute__((optimize("no-jump-tables")))
static int ep0_standard(uint16_t wvalue)
{
    uint32_t type;
    uint32_t n;

    if (ep0_req_code == REQ_SET_ADDRESS) {
        /* Applied at the status stage; applying it here kills this transaction. */
        ep0_req_len = (uint16_t)(wvalue & 0xFFu);

    } else if (ep0_req_code == REQ_GET_DESCRIPTOR) {
        type = (uint32_t)(wvalue >> 8);
        if (type == DESC_TYPE_DEVICE) {
            /* bl_usb_desc_*_live is the CDC or the CH340 array, per FW_USB_CDC. */
            ep0_descr = bl_usb_desc_dev_live;
            n = bl_usb_desc_dev_live[0];            /* bLength                */
        } else if (type == DESC_TYPE_CONFIG) {
            ep0_descr = bl_usb_desc_cfg_live;
            n = bl_usb_desc_cfg_live[2];            /* low byte of wTotalLen  */
        } else {
            return 1;                               /* incl. type 3, STRING   */
        }
        /* The descriptor INDEX (low byte of wValue) is ignored, as at 0x48CC. */
        if (ep0_req_len > n) {
            ep0_req_len = (uint16_t)n;
        }
        n = (ep0_req_len >= BL_USB_EP0_PKT) ? BL_USB_EP0_PKT : ep0_req_len;
        usb_copy(ep0_buf, ep0_descr, n);
        ep0_descr += n;

    } else if (ep0_req_code == REQ_GET_CONFIG) {
        ep0_buf[0] = usb_config;
        if (ep0_req_len > 1u) {
            ep0_req_len = 1u;
        }

    } else if (ep0_req_code == REQ_SET_CONFIG) {
        usb_config = (uint8_t)(wvalue & 0xFFu);

#if FW_USB_CDC
    } else if (ep0_req_code == REQ_GET_INTERFACE) {
        /* One alternate setting per interface, so the answer is 0. */
        ep0_buf[0] = 0u;
        if (ep0_req_len > 1u) {
            ep0_req_len = 1u;
        }

    } else if (ep0_req_code == REQ_SET_INTERFACE) {
        if ((wvalue & 0xFFu) != 0u) {
            return 1;                   /* no such alternate setting: STALL  */
        }
#endif
    } else {
        /* GET_STATUS, CLEAR_FEATURE, SET_FEATURE, SET_DESCRIPTOR, SYNCH_FRAME. */
        return 1;
    }
    return 0;
}

static void ep0_setup(void)
{
    uint16_t wvalue;
    uint32_t n;
    int      stall = 0;

    R8_UEP0_CTRL = UEP0_CTRL_SETUP_ARM;

    if (R8_USB_RX_LEN != 8u) {
        R8_UEP0_CTRL = UEP0_CTRL_STALL;
        return;
    }

    ep0_req_len  = (uint16_t)(ep0_buf[6] | ((uint16_t)ep0_buf[7] << 8));
    ep0_req_code = ep0_buf[1];
    ep0_req_type = ep0_buf[0];
    wvalue       = (uint16_t)(ep0_buf[2] | ((uint16_t)ep0_buf[3] << 8));

    if (ep0_req_type == BMREQ_VENDOR_IN) {
        ep0_vendor_in();
    } else if (ep0_req_type == BMREQ_VENDOR_OUT) {
        /* Accept anything, answer with a ZLP status. There is no UART, so the
         * baud divisor must not be validated. 0xA1 is the session marker (6b). */
        if (ep0_req_code == CH341_REQ_SERIAL_INIT) {
            usb_session_flush();
        }
#if FW_USB_CDC
    } else if (ep0_req_type == BMREQ_CLASS_IN) {
        stall = ep0_class_in();
    } else if (ep0_req_type == BMREQ_CLASS_OUT) {
        stall = ep0_class_out(wvalue);
#endif
    } else {
        stall = ep0_standard(wvalue);
    }

    if (stall != 0) {
        R8_UEP0_CTRL = UEP0_CTRL_STALL;
        return;
    }

    if ((ep0_req_type & 0x80u) != 0u) {          /* device-to-host            */
        n = (ep0_req_len > BL_USB_EP0_PKT) ? BL_USB_EP0_PKT : ep0_req_len;
        ep0_req_len = (uint16_t)(ep0_req_len - n);
    } else {                                     /* host-to-device: no data   */
        n = 0u;
    }
    R8_UEP0_T_LEN = (uint8_t)n;
        R8_UEP0_CTRL  = UEP0_CTRL_DATA;
}

static void ep0_in(void)
{
    uint32_t n;

    /* Dispatch on the type as well as the code. On bRequest alone, C0 05 ...
     * loads R8_USB_DEV_AD from a raw wLength and drops the device off the bus,
     * and C0 06 ... streams SRAM from a stale ep0_descr with no clamp. */
    uint32_t standard = ((ep0_req_type & 0x60u) == 0u);

    switch (standard ? ep0_req_code : 0xFFu) {

    case REQ_GET_DESCRIPTOR:
        n = (ep0_req_len >= BL_USB_EP0_PKT) ? BL_USB_EP0_PKT : ep0_req_len;
        if (ep0_descr == 0) {                    /* stale request after reset */
            n = 0u;
        } else {
            usb_copy(ep0_buf, ep0_descr, n);
            ep0_descr += n;
        }
        ep0_req_len = (uint16_t)(ep0_req_len - n);
        R8_UEP0_T_LEN = (uint8_t)n;
        /* No AUTO_TOG and no hardware-managed bits on EP0: flip by hand, no 3b. */
        R8_UEP0_CTRL = (uint8_t)(R8_UEP0_CTRL ^ RB_UEP_T_TOG);
        break;

    case REQ_SET_ADDRESS:
        /* Preserve RB_UDA_GP_BIT (0x80), as the application does at 0x4A18. */
        R8_USB_DEV_AD = (uint8_t)((R8_USB_DEV_AD & 0x80u)
                                  | (uint8_t)(ep0_req_len & 0x7Fu));
        R8_UEP0_CTRL  = UEP0_CTRL_IDLE;
        break;

    default:
        R8_UEP0_T_LEN = 0u;
        R8_UEP0_CTRL  = UEP0_CTRL_IDLE;
        break;
    }
}

static void ep0_out(void)
{
#if FW_USB_CDC
    /* SET_LINE_CODING is the one transfer with an OUT data stage, and its status
     * IN is a ZLP that is always DATA1; UEP0_CTRL_IDLE would NAK it forever. */
    if ((ep0_req_type == BMREQ_CLASS_OUT)
            && (ep0_req_code == CDC_SET_LINE_CODING)) {
        uint32_t n = R8_USB_RX_LEN;
        uint32_t i;
        uint8_t  changed = 0u;

        if (n > (uint32_t)CDC_LINE_CODING_LEN) {
            n = (uint32_t)CDC_LINE_CODING_LEN;   /* never trust a host length */
        }
        for (i = 0u; i < n; i++) {
            uint8_t b = ep0_buf[i];
            if (cdc_line_coding[i] != b) {
                changed = 1u;
                cdc_line_coding[i] = b;
            }
        }
        /* A line reconfiguration is a session boundary (6b), on a change only. */
        if (changed != 0u) {
            usb_session_flush();
        }
        R8_UEP0_T_LEN = 0u;
        R8_UEP0_CTRL  = (uint8_t)(RB_UEP_T_TOG | UEP_T_RES_ACK | UEP_R_RES_ACK);
        return;
    }
#endif
    R8_UEP0_T_LEN = 0u;
    R8_UEP0_CTRL  = UEP0_CTRL_IDLE;
}

/* Do not re-run the init sequence here; a bus reset does not reset the SIE's
 * config. Do not touch FW_BL_MAGIC_ADDR either: a bus reset must not arm a
 * reboot. ch340_cursor must be reset, or enumeration works exactly once. */
static void usb_bus_reset(void)
{
    R8_USB_DEV_AD = 0u;
    R8_UEP0_CTRL  = UEP0_CTRL_IDLE;   /* clears both EP0 toggles              */
    R8_UEP1_CTRL  = UEPn_CTRL_BULK;   /* EP1 stays NAKed forever              */
    R8_UEP2_CTRL  = UEPn_CTRL_BULK;   /* OUT re-armed, IN cancelled, DATA0    */

    R8_UEP2_T_LEN = 0u;

    ch340_cursor = 0u;                /* not reset by the application         */
    ep0_descr    = 0;
    ep0_req_len  = 0u;
    ep0_req_code = 0u;
    ep0_req_type = 0u;
    usb_config   = 0u;

    rx_head  = 0u;
    rx_tail  = 0u;
    tx_head  = 0u;
    tx_tail  = 0u;
    tx_armed = 0u;
    tx_direct_reset();                                    /* FW_TX_DIRECT     */

    /* A bus reset is a session boundary too; the 0x12 above cleared the toggles. */
    session_id++;
}

/* USB_DeviceInit (0x4B24) reproduced write for write and in order. The endpoint
 * map (UEP4_1_MOD 0x40, UEP2_3_MOD 0x0C) is what the descriptors advertise; do
 * not substitute WCH's 0xCC/0xCC. */

/* How long D+ is held low. USB 2.0 7.1.7.3 gives TDDIS = 2.5 us minimum SE0;
 * without a real interval the host misses the disconnect. NOMINAL, and
 * usb_delay_ms runs 1.75x, so ~10.5 ms. */
#ifndef BL_USB_DETACH_MS
#define BL_USB_DETACH_MS  6u
#endif

static void usb_device_init(void)
{
    uint8_t was_attached = (uint8_t)((R8_USB_CTRL & RB_UC_DEV_PU_EN) != 0u);

    R8_USB_CTRL   = 0x00u;                        /* off while configuring    */

    /* Re-attach only; at boot this would be 10 ms of SE0 nothing waits for. */
    if (was_attached) {
        usb_delay_ms(BL_USB_DETACH_MS);
    }
    R8_UEP4_1_MOD = RB_UEP1_TX_EN;                /* 0x40                     */
    R8_UEP2_3_MOD = (uint8_t)(RB_UEP2_RX_EN | RB_UEP2_TX_EN
                              | (bl_usb_ep2_dbuf ? RB_UEP2_BUF_MOD : 0u));

    R16_UEP0_DMA = (uint16_t)(uint32_t)(uintptr_t)ep0_buf;
    R16_UEP1_DMA = (uint16_t)(uint32_t)(uintptr_t)ep1_buf;
    R16_UEP2_DMA = (uint16_t)(uint32_t)(uintptr_t)ep2_buf;

    R8_UEP0_CTRL = UEP0_CTRL_IDLE;                /* 0x02                     */
    R8_UEP1_CTRL = UEPn_CTRL_BULK;                /* 0x12                     */
    R8_UEP2_CTRL = UEPn_CTRL_BULK;                /* 0x12                     */

    R8_USB_DEV_AD = 0x00u;

    /* RB_UC_INT_BUSY is what makes polling safe. RB_UC_DEV_PU_EN attaches the
     * D+ pull-up, so the host sees the device from this store onward. */
    R8_USB_CTRL = RB_UC_DEV_PU_EN | RB_UC_INT_BUSY | RB_UC_DMA_EN;   /* 0x29 */

    R8_USB_INT_FG = 0xFFu;                        /* clear every W1C flag     */

    R16_PIN_ANALOG_IE = (uint16_t)(R16_PIN_ANALOG_IE | RB_PIN_USB_IE);

    R8_UDEV_CTRL = RB_UD_PD_DIS | RB_UD_PORT_EN;  /* 0x81                     */

    /* SIE flag sources only; exception delivery is the NVIC's. */
    R8_USB_INT_EN = RB_UIE_SUSPEND | RB_UIE_TRANSFER | RB_UIE_BUS_RST; /* 0x07 */
}

void bl_usb_init(void)
{
    /* Mask IRQ6 and drop anything pending before the pull-up goes up. ISER is
     * zero out of reset, but a warm reset from a running application is not. */
    NVIC_ICER = USB_IRQ_BIT;
    NVIC_ICPR = USB_IRQ_BIT;

#if FW_USB_IRQ
    /* usb_irq_fault survives re-init: do not walk back into a known wedge. */
    usb_irq_on = 0u;
    usb_irq_idle_entries = 0u;
    usb_irq_burst = 0u;
#endif

#if BL_USB_PLL_POWER_ON
    usb_pll_power_on();
#endif

    ep0_descr    = 0;
    ep0_req_len  = 0u;
    ep0_req_code = 0u;
    ep0_req_type = 0u;
    usb_config   = 0u;
    ch340_cursor = 0u;
    rx_head      = 0u;
    rx_tail      = 0u;
    tx_head      = 0u;
    tx_tail      = 0u;
    tx_armed     = 0u;
    tx_direct_reset();                                    /* FW_TX_DIRECT     */
    session_id   = 0u;

    usb_device_init();
}

#if BL_USB_ECHO
/* Loopback for bring-up only. With echo on, the pump drains the receive staging
 * before the framer sees a byte, so recovery is U22 held at power-on. */
static void usb_echo_pump(void)
{
    uint8_t  tmp[BL_USB_EP2_PKT];
    uint32_t room = (uint32_t)(BL_USB_TX_BUF_SIZE - tx_head);
    uint32_t n;

    if (room == 0u) {
        return;                      /* transmit full: leave it in rx_buf     */
    }
    if (room > sizeof tmp) {
        room = sizeof tmp;
    }
    n = bl_usb_rx(tmp, room);
    if (n != 0u) {
        (void)tx_queue(tmp, n);
    }
}
#endif

/* The tail of every poll, factored out to run INSIDE the RB_UIF_TRANSFER
 * window where there is one (3b). */
static void usb_pumps(void)
{
#if BL_USB_ECHO
    usb_echo_pump();
#endif

    if ((tx_armed == 0u) &&
        ((tx_head != tx_tail)
#if FW_TX_DIRECT
         /* A nominated region is a queue too, or a stall never restarts. */
         || (tx_direct_ready() != 0u)
#endif
        )) {
        tx_pump();
    }

#if FW_USB_ISR_LEAN
    if (rx_credit_dirty != 0u) {
        rx_credit_dirty = 0u;
        rx_credit_update();
    }
#else
    rx_credit_update();
#endif
}

#if FW_USB_IRQ
static void usb_service(void);

/* While the interrupt is armed this does nothing: two contexts dispatching on
 * R8_USB_INT_ST is not a race a lock can paper over. */
void bl_usb_poll(void)
{
    if (usb_irq_on != 0u) {
        /* Proves thread mode still runs, so the handler can tell busy from
         * starved (USB_IRQ_BURST_LIMIT). */
        usb_irq_burst = 0u;
        return;
    }
    usb_service();
}

static void usb_service(void)
#else
void bl_usb_poll(void)
#endif
{
    /* Read once: the register's read-only status bits change underneath. */
    uint8_t fg = R8_USB_INT_FG;

    if ((fg & RB_UIF_TRANSFER) != 0u) {
        /* R8_USB_INT_ST must be read BEFORE the flag is cleared. */
        uint8_t st = R8_USB_INT_ST;
#if FW_TX_PIPELINE || FW_RX_DBUF
        /* Set by a path that already released the bus: clearing the flag twice
         * swallows a latched event. */
        uint8_t pipe_cleared = 0u;
#endif

        switch ((uint8_t)(st & MASK_UIS_TOKEN_EP)) {

        case TOK_SETUP_EP0:
            ep0_setup();
            break;

        case TOK_IN_EP0:
            ep0_in();
            break;

        case TOK_OUT_EP0:
            ep0_out();
            break;

        case TOK_OUT_EP2:
#if FW_RX_DBUF
            /* Receive mirror of FW_TX_PIPELINE. Both reads must precede the
             * release: RB_UEP_R_TOG flips as soon as the next packet is
             * accepted, and R8_USB_RX_LEN is shared by every endpoint. */
            if (bl_usb_ep2_dbuf != 0u) {
                uint8_t  rx_off = ep2_rx_off();
                uint32_t rx_len = 0u;

                if ((st & RB_UIS_TOG_OK) != 0u) {
                    rx_len = R8_USB_RX_LEN;
                    if (rx_len > EP2_RX_WINDOW) {
                        rx_len = EP2_RX_WINDOW;
                    }
                }

                rx_credit_update_n((uint16_t)rx_len);

                R8_USB_INT_FG = RB_UIF_TRANSFER;   /* release the bus */
                pipe_cleared = 1u;

                if (rx_len != 0u) {
                    rx_deliver_at(rx_off, rx_len);
                }
                break;
            }
            /* Single-buffered fallback: fall through to close-then-copy. */
#endif
            /* Close the window before copying out of it: with BUF_MOD clear the
             * SIE would accept the next packet into ep2_buf mid-copy. */
            uep2_ctrl_rmw(MASK_UEP_R_RES, UEP_R_RES_NAK);

            /* TOG_OK clear means a retransmission; taking it duplicates data. */
            if ((st & RB_UIS_TOG_OK) != 0u) {
#if FW_RX_DBUF
                rx_deliver_at(ep2_rx_off(), R8_USB_RX_LEN);
#else
                rx_deliver(R8_USB_RX_LEN);
#endif
            }
            break;

        case TOK_IN_EP2:
#if FW_TX_PIPELINE
            /* Credit the packet only if it was the region's. Before any branch,
             * so the wind-down path credits it too. */
#if FW_USB_ISR_LEAN
            if (tx_pipe_outstanding != 0u) {
                tx_pipe_outstanding--;
                tx_dir_sent_n = (uint16_t)(tx_dir_sent_n
                                           + (uint16_t)bl_usb_ep2_pkt_in);
            } else {
                tx_dir_sent_n = (uint16_t)(tx_dir_sent_n + tx_stage_pop());
            }
#else
            tx_dir_sent_n = (uint16_t)(tx_dir_sent_n + tx_stage_pop());
#endif
#endif
#if FW_TX_PIPELINE
            /* The window the SIE sends next was staged last round: release first. */
            if (tx_pipe_armed != 0u) {
#if FW_USB_TX_TOG_SHADOW
                /* Tracks the freed window, the complement of ep2_tx_off().
                 * Computed before the release: RB_UEP_T_TOG may flip after it,
                 * and the refill then has one 50 us transaction to finish in. */
                uint8_t freed;
                uint16_t left;

                if (tx_tog_valid != 0u) {
                    freed = tx_tog_pred;
                    tx_tog_pred = (tx_tog_pred == EP2_DBUF_TX1_OFF)
                                  ? EP2_DBUF_TX0_OFF : EP2_DBUF_TX1_OFF;
                } else {
                    freed = ep2_tx_other();
                }
#else
                uint8_t freed = ep2_tx_other();
                uint16_t left;
#endif
                R8_USB_INT_FG = RB_UIF_TRANSFER;   /* release the bus */
                pipe_cleared = 1u;

                left = tx_pipe_ready();
                if (left >= (uint16_t)bl_usb_ep2_pkt_in) {
                    /* Refill the window just consumed. T_RES is still ACK and
                     * T_LEN still 64, so no R8_UEP2_CTRL write. */
                    usb_copy(&ep2_buf[freed], &tx_dir_base[tx_dir_pos],
                             (uint16_t)bl_usb_ep2_pkt_in);
                    tx_dir_pos = (uint16_t)(tx_dir_pos
                                            + (uint16_t)bl_usb_ep2_pkt_in);
#if FW_USB_ISR_LEAN
                    tx_pipe_outstanding++;
#else
                    tx_stage_push((uint16_t)bl_usb_ep2_pkt_in, 1u);
#endif
                } else {
                    /* Tail short of a packet, plus the CDC ZLP: back to tx_pump(). */
                    tx_pipe_armed = 0u;
                }
                break;
            }
#endif
            tx_pump();
            break;

        default:
            
            break;
        }

        /* Run the pumps while the SIE is still held off, then clear: the only
         * interval where R8_UEP2_CTRL needs no inference about SIE timing (3b). */
        usb_pumps();
#if FW_TX_PIPELINE || FW_RX_DBUF
        
        if (pipe_cleared == 0u) {
            R8_USB_INT_FG = RB_UIF_TRANSFER;
        }
#else
        R8_USB_INT_FG = RB_UIF_TRANSFER;
#endif
#if FW_USB_IRQ
        usb_event_count++;
#endif
        return;
    }

    if ((fg & RB_UIF_BUS_RST) != 0u) {
        usb_bus_reset();
        R8_USB_INT_FG = RB_UIF_BUS_RST;
#if FW_USB_IRQ
        usb_event_count++;
#endif
    } else if ((fg & RB_UIF_SUSPEND) != 0u) {
        /* Suspend and resume are indistinguishable here; nothing is torn down. */
        (void)R8_USB_MIS_ST;
        R8_USB_INT_FG = RB_UIF_SUSPEND;
    } else if ((fg & RB_UIF_FIFO_OV) != 0u) {
        R8_USB_INT_FG = RB_UIF_FIFO_OV;
    } else {
        /* Only read-only status bits set; skip the bus access. */
    }

    /* No transfer window here, so uep2_ctrl_rmw()'s detect-and-repair keeps the
     * stores safe. Credit granted only inside a window would deadlock. */
    usb_pumps();
}

uint32_t bl_usb_rx(uint8_t *buf, uint32_t max)
{
    uint32_t n = rx_used();

    if (n > max) {
        n = max;
    }
    if (n != 0u) {
        uint16_t at = (uint16_t)(rx_tail & RX_MASK);
        uint32_t first = BL_USB_RX_BUF_SIZE - at;
        if (first > n) {
            first = n;
        }
        usb_copy(buf, &rx_buf[at], first);
        if (n > first) {
            usb_copy(buf + first, &rx_buf[0], n - first);
        }
        rx_tail = (uint16_t)(rx_tail + n);
    }
    /* The lock is for the register, not the ring: uep2_ctrl_rmw() defends
     * against the SIE, not against a second software writer. */
    USB_LOCK_BEGIN
    rx_credit_update();
    USB_LOCK_END
    return n;
}

/* The wait is clocked, not counted: usb_delay_ms() would push the stall past
 * BL_TIME_MAX_GAP_MS and fire every window spanning it late. tx_buf is linear,
 * so a poll frees the whole buffer or nothing and the `continue' cannot repeat. */
uint32_t bl_usb_tx(const uint8_t *buf, uint32_t n)
{
    uint32_t sent  = 0u;
#if FW_FAST_CLOCK
    /* Sampled at the first stall, not at entry: bl_time_ms() is not free. */
    uint32_t start = 0u;
    uint8_t  timing = 0u;
#else
    uint32_t start = BL_USB_TIME_MS();
#endif
    uint32_t frozen_polls = 0u;

    while (sent < n) {
        uint32_t now;

        sent += tx_queue(&buf[sent], n - sent);
        if (sent >= n) {
            break;
        }

        /* Staging full. Each poll services at most one transfer. */
        bl_usb_poll();
        if (tx_head < (uint16_t)BL_USB_TX_BUF_SIZE) {
            /* Room appeared. Skipping the clock read happens at most once. */
            continue;
        }

        /* Report short rather than hang. Differences only: bl_time_ms() wraps. */
        now = BL_USB_TIME_MS();
#if FW_FAST_CLOCK
        if (timing == 0u) {
            timing = 1u;
            start  = now;
        }
#endif
        if ((uint32_t)(now - start) >= (uint32_t)BL_USB_TX_TIMEOUT_MS) {
            break;
        }
        if (now == start) {
                        /* now == start for the whole cap: the time base is not running. */
            frozen_polls++;
            if (frozen_polls >= (uint32_t)BL_USB_TX_DEAD_CLOCK_POLLS) {
                break;
            }
        }
    }
    return sent;
}

#if FW_USB_IRQ

/* Vector 22 of this image's table, reached through the bootloader's IPSR
 * trampoline at 0x0000, which calls it as an ordinary C function. */
void fw_usb_irq_handler(void)
{
    uint8_t fg;

    /* Before any branch: a counter a decision can skip freezes. */
    usb_irq_entry_count++;

    if (++usb_irq_burst >= (uint32_t)USB_IRQ_BURST_LIMIT) {
        NVIC_ICER = USB_IRQ_BIT;
        usb_irq_on = 0u;
        if (usb_irq_fault == (uint8_t)BL_USB_IRQ_FAULT_NONE) {
            usb_irq_fault = (uint8_t)BL_USB_IRQ_FAULT_STARVE;
        }
        return;
    }

    fg = R8_USB_INT_FG;
    if ((fg & (uint8_t)(RB_UIF_TRANSFER | RB_UIF_BUS_RST | RB_UIF_SUSPEND
                        | RB_UIF_FIFO_OV)) == 0u) {
        usb_irq_idle_entries++;
        if (usb_irq_idle_entries >= (uint8_t)USB_IRQ_IDLE_LIMIT) {
            /* The ICER store takes effect before the exception return. */
            NVIC_ICER = USB_IRQ_BIT;
            usb_irq_on = 0u;
            if (usb_irq_fault == (uint8_t)BL_USB_IRQ_FAULT_NONE) {
                usb_irq_fault = (uint8_t)BL_USB_IRQ_FAULT_SPURIOUS;
            }
        }
        return;
    }
    usb_irq_idle_entries = 0u;

    usb_service();
}

/* Arm IRQ6 only after proving the path. __vectors[22] holds fw_usb_irq_handler
 * only if vectors.S was assembled with FW_USB_IRQ; otherwise the first USB
 * interrupt lands in fw_fault_handler's `b .', recoverable only with U22 held at
 * power-on. The bootloader at 0x0000 is a different image, so IRQ6 is pended in
 * software and the entry observed. */
int bl_usb_irq_arm(void)
{
    uint32_t before;
    uint32_t spin;

    if (usb_irq_on != 0u) {
        return 1;
    }
    if (usb_irq_fault != (uint8_t)BL_USB_IRQ_FAULT_NONE) {
        return 0;                     /* demoted once; never again this life */
    }

    if (__vectors[FW_USB_VECTOR_INDEX]
            != (uint32_t)(uintptr_t)&fw_usb_irq_handler) {
        usb_irq_fault = (uint8_t)BL_USB_IRQ_FAULT_VECTOR;
        return 0;
    }

    before = usb_irq_entry_count;
    NVIC_ICPR = USB_IRQ_BIT;
    NVIC_ISER = USB_IRQ_BIT;
    NVIC_ISPR = USB_IRQ_BIT;
    __asm volatile ("dsb" : : : "memory");
    __asm volatile ("isb" : : : "memory");

    /* The exception is taken at the ISB, but a bounded wait costs two us. */
    for (spin = 0u; spin < 256u; spin++) {
        if (usb_irq_entry_count != before) {
            break;
        }
        __asm volatile ("nop");
    }

    if (usb_irq_entry_count == before) {
        NVIC_ICER = USB_IRQ_BIT;
        NVIC_ICPR = USB_IRQ_BIT;
        usb_irq_fault = (uint8_t)BL_USB_IRQ_FAULT_NOENTRY;
        return 0;
    }

    usb_irq_idle_entries = 0u;
    usb_irq_burst = 0u;

    
    __asm volatile ("cpsie i" : : : "memory");
    usb_irq_on = 1u;
    return 1;
}

void bl_usb_irq_disarm(uint8_t reason)
{
    uint8_t was_armed;

    USB_LOCK_BEGIN
    was_armed = usb_irq_on;
    NVIC_ICER = USB_IRQ_BIT;
    NVIC_ICPR = USB_IRQ_BIT;
    usb_irq_on = 0u;
    if (was_armed && (usb_irq_fault == (uint8_t)BL_USB_IRQ_FAULT_NONE)) {
        usb_irq_fault = reason;
    }
    USB_LOCK_END

    /* Nothing is lost by disarming mid-session: every event latches in
     * R8_USB_INT_FG and RB_UC_INT_BUSY holds the SIE off while one is pending. */
}

int bl_usb_irq_armed(void)
{
    return (usb_irq_on != 0u) ? 1 : 0;
}

uint8_t bl_usb_irq_fault(void)
{
    return usb_irq_fault;
}

uint32_t bl_usb_irq_entries(void)
{
    return usb_irq_entry_count;
}

uint32_t bl_usb_events(void)
{
    return usb_event_count;
}
#endif /* FW_USB_IRQ */

#if FW_USB_IRQ
int bl_usb_tx_pending(void)
{
    /* tx_armed must stay in the test: tx_pump() advances tx_tail when it STAGES,
     * so a ring-only test returns 0 for every single-packet reply. */
    return ((tx_head != tx_tail) || (tx_armed != 0u)) ? 1 : 0;
}
#endif /* FW_USB_IRQ */

int bl_usb_configured(void)
{
    return (usb_config != 0u) ? 1 : 0;
}

