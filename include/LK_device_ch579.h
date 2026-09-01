/* Board layer for GBFlash (CH579M) against Lesserkuma's LK skeleton v15.
 * MISMATCH = upstream and this board disagree (lk-port-notes.md). */

#ifndef _LK_DEVICE_CH579_H_
#define _LK_DEVICE_CH579_H_

/* Each fails silently when missing: corrupted save, shifted dump, no write strobe. */
#if !defined(FW_LK_PATCH_0001)
#error "carried patch 0001 (AGB save-flash read-back poll) is not applied. See patches/"
#endif
#if !defined(FW_LK_PATCH_0002)
#error "carried patch 0002 (AGB address-latch settle) is not applied. See patches/"
#endif
#if !defined(FW_LK_PATCH_0003)
#error "carried patch 0003 (DMG flash we==0 -> /WR fallback) is not applied. See patches/"
#endif

#include "LK.h"

#include <stdint.h>

#include "cart.h"
#include "timebase.h"
#include "usb.h"

#define HARDWARE_GBFLASH

/* sizeof()'d by LK.c, so it has to stay a string literal. */
#define LK_DEVICE_NAME                      "Open-GBFlash"

extern uint8_t fw_lk_pcb_ver(void);
#define LK_PCB_VERSION                      fw_lk_pcb_ver()

/* 1/0 rather than true/false: LK.h defines those after including this header. */
#define LK_POWER_CONTROL_SUPPORT            1
#define LK_BOOTLOADER_RESET_SUPPORT         1
#define LK_CART_PRESENCE_SWITCH_SUPPORT     0
#define LK_CART_MODE_SWITCH_SUPPORT         0

/* No debug output: the one bulk pipe carries the protocol, text desyncs the host. */
#define dprint(s, params...)                {}

#ifndef LKDEV_REG32
#define LKDEV_REG32(a)  (*(volatile uint32_t *)(uintptr_t)(a))
#endif

#define LKDEV_PA_DIR    LKDEV_REG32(R32_PA_DIR)
#define LKDEV_PA_PIN    LKDEV_REG32(R32_PA_PIN)
#define LKDEV_PA_OUT    LKDEV_REG32(R32_PA_OUT)
#define LKDEV_PA_CLR    LKDEV_REG32(R32_PA_CLR)
#define LKDEV_PA_PU     LKDEV_REG32(R32_PA_PU)

#define LKDEV_PB_DIR    LKDEV_REG32(R32_PB_DIR)
#define LKDEV_PB_PIN    LKDEV_REG32(R32_PB_PIN)
#define LKDEV_PB_OUT    LKDEV_REG32(R32_PB_OUT)
#define LKDEV_PB_CLR    LKDEV_REG32(R32_PB_CLR)
#define LKDEV_PB_PU     LKDEV_REG32(R32_PB_PU)

#define LKDEV_CART_FACING   (PB_ADDR_HI | (0xC7u << 13))

/* 32 MHz, 1 nop/cycle: 5/7/10/13/16 nops -> ~156/219/313/406/500 ns of /RD low. */
#ifndef __NOP
#define __NOP()     __asm__ volatile ("nop" ::: "memory")
#endif

/* One __NOP() per instruction, never `.rept n`: GCC under-sizes the block by n and the
 * Thumb-1 branches around it (+/-254 B) go out of range. */
#define _delay_100ns()      { __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); }
#define _delay_200ns()      { __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); }
#define _delay_300ns()      { __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); }
#define _delay_400ns()      { __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); }
#define _delay_500ns()      { __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); }

/* 27 nops plus the loop's compare-and-branch is 32 cycles, 1 us at 32 MHz. */
#define _delay_us(us)                                                       \
    do {                                                                    \
        uint32_t _lk_n = (uint32_t)(us);                                    \
        while (_lk_n--) {                                                   \
            __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP();  \
            __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP();  \
            __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP();  \
            __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP();           \
        }                                                                   \
    } while (0)

/* MISMATCH: a real wait that pumps USB; usb.h has no path over ~10 ms without bl_usb_poll(). */
extern void fw_lk_delay_ms(uint32_t ms);
#define _delay_ms(ms)                       fw_lk_delay_ms((uint32_t)(ms))

/* Patch 0002. /CS-latch to first /RD. Too short and the cart's counter comes up a halfword
 * ahead: a stable, shifted dump. Do not fold it into RAW_AGB_DATA_DIR_IN(). */
#define _delay_agb_latch()                                                  \
    {                                                                       \
        __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); \
        __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); \
        __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); \
        __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); __NOP(); \
    }

#define _delay_dmg_slow_access()            _delay_us(2)

/* Read bl_time_ms() every iteration: a gap over BL_TIME_MAX_GAP_MS = 524 ms loses a lap of
 * the 24-bit SysTick accumulator. MISMATCH: `time_start > 0` holes millisecond zero. */
#define TIMESTAMP_NOW()                     bl_time_ms()
#define _timeout_init()                     time_start = TIMESTAMP_NOW();
#define _timeout_reset()                    time_start = 0;
#define _timeout_check()                    ((time_start > 0) && (TIMESTAMP_NOW() - time_start > 500))

#define PIN_WR                              PB_WR
#define PIN_RD                              PB_RD
#define PIN_CS                              PB_CS
#define PIN_CS2                             PB_CS2
#define PIN_AUDIO                           PB_AUDIO
#define PIN_CLK                             PB_CLK
#define VOLTAGE_SELECT                      PB_VSEL

/* Active low, idling high; CLK alone idles low. PB_CLR is write-1-to-clear, PB_OUT a latch. */

#define PIN_WR_H()      { LKDEV_PB_OUT |= PB_WR;    }
#define PIN_WR_L()      { LKDEV_PB_CLR  = PB_WR;    }
#define PIN_RD_H()      { LKDEV_PB_OUT |= PB_RD;    }
#define PIN_RD_L()      { LKDEV_PB_CLR  = PB_RD;    }
#define PIN_CS_H()      { LKDEV_PB_OUT |= PB_CS;    }
#define PIN_CS_L()      { LKDEV_PB_CLR  = PB_CS;    }
#define PIN_CS2_H()     { LKDEV_PB_OUT |= PB_CS2;   }
#define PIN_CS2_L()     { LKDEV_PB_CLR  = PB_CS2;   }
#define PIN_AUDIO_H()   { LKDEV_PB_OUT |= PB_AUDIO; }
#define PIN_AUDIO_L()   { LKDEV_PB_CLR  = PB_AUDIO; }
#define PIN_CLK_H()     { LKDEV_PB_OUT |= PB_CLK;   }
#define PIN_CLK_L()     { LKDEV_PB_CLR  = PB_CLK;   }

#define PIN_AUDIO_DIR_OUT() { LKDEV_PB_DIR |= PB_AUDIO;  }
#define PIN_AUDIO_DIR_IN()  { LKDEV_PB_DIR &= ~PB_AUDIO; }

/* A0..A15 on PA0..PA15, A16..A23 on PB0..PB7; LK.c indexes both constant and runtime 0..23. */
static inline void lkdev_addr_bit(uint32_t pin, uint32_t high)
{
    if (pin < 16u) {
        if (high) { LKDEV_PA_OUT |= (1u << pin); }
        else      { LKDEV_PA_CLR  = (1u << pin); }
    } else {
        if (high) { LKDEV_PB_OUT |= (1u << (pin - 16u)); }
        else      { LKDEV_PB_CLR  = (1u << (pin - 16u)); }
    }
}
#define PIN_ADDR_H(pin)     { lkdev_addr_bit((uint32_t)(pin), 1u); }
#define PIN_ADDR_L(pin)     { lkdev_addr_bit((uint32_t)(pin), 0u); }

#define PIN_A0_H()      { LKDEV_PA_OUT |= 0x0001u;  }
#define PIN_A0_L()      { LKDEV_PA_CLR  = 0x0001u;  }
#define PIN_A0_OUT()    { LKDEV_PA_DIR |= 0x0001u;  }
#define PIN_A0_IN()     { LKDEV_PA_DIR &= ~0x0001u; }

/* MISMATCH: the VCC bit only; LK does the settle and line parking itself (LK.c:862-901) and
 * fw_cart_power(1) would fight it. MISMATCH: LK.c:887 drives /CS2 high into an unpowered
 * cart, back-powering it (cart.h:104). POWER_OFF drops DIR before the rail, un-latches 5 V. */
#define CART_POWER_ON()                                                     \
    { LKDEV_PB_OUT |= PB_VCC_EN; }

#define CART_POWER_OFF()                                                    \
    {                                                                       \
        LKDEV_PB_DIR &= ~LKDEV_CART_FACING;                                 \
        LKDEV_PA_DIR &= ~PA_AD_MASK;                                        \
        LKDEV_PB_CLR  = PB_VCC_EN;                                          \
        LKDEV_PB_CLR  = PB_VSEL;                                            \
    }

/* 5 V into a 3.3 V AGB cartridge is permanent damage. The guard in fw_lk_voltage_5v() reads
 * our own st->mode: LK.c:190 sets its mode to DMG one line before calling this. */
extern void fw_lk_voltage_5v(void);
#define SET_VOLTAGE_5V()    { fw_lk_voltage_5v(); }

#define SET_VOLTAGE_3_3V()  { LKDEV_PB_CLR = PB_VSEL; fw_cart_settle(); }

/* Runs before the rail exists (LK.c:862) and PB_OUT still latches the control lines high from
 * the last power-off: park low first, or five highs land on an unpowered cartridge. */
#define RAW_PINS_DIR_OUT()                                                  \
    {                                                                       \
        LKDEV_PB_CLR  = LKDEV_CART_FACING;                                  \
        LKDEV_PA_OUT  = 0;                                                  \
        LKDEV_PB_DIR |= (0xC7u << 13);      /* /WR /RD /CS /CS2 AUDIO   */  \
        LKDEV_PB_DIR |= PB_CLK;                                             \
        LKDEV_PA_DIR |= PA_AD_MASK;                                         \
        LKDEV_PB_DIR |= PB_ADDR_HI;                                         \
    }

#define RAW_PINS_DIR_IN()                                                   \
    {                                                                       \
        LKDEV_PB_DIR &= ~LKDEV_CART_FACING;                                 \
        LKDEV_PA_DIR &= ~PA_AD_MASK;                                        \
    }

/* Presence probe: a present cartridge holds lines an empty slot lets drift. */
#define PULLUPS_ON()        { LKDEV_PA_PU |= PA_AD_MASK;  }
#define PULLUPS_OFF()       { LKDEV_PA_PU &= ~PA_AD_MASK; }

/* PB12, active low. MISMATCH: LK leaves an idle device dark; fw_lk_led_idle() re-parks it lit. */
#define ACTIVITY_LED_ON()   { LKDEV_PB_CLR  = PB_LED; }
#define ACTIVITY_LED_OFF()  { LKDEV_PB_OUT |= PB_LED; }

/* Not stubbed: an empty SUSPEND lets power drop mid-program. */
#define AUTO_POWEROFF_SUSPEND()     { auto_off_timer_suspended = 1; }
#define AUTO_POWEROFF_RESUME()      { auto_off_timer_suspended = 0; }

/* Not no-ops: IRQ6 is armed, so nothing services USB across the bracket. LK.c:1406's
 * _delay_ms(4) takes most of usb.h's ~10 ms budget; anything added here must fit the rest. */
#define DISABLE_INTERRUPTS()    __asm__ volatile ("cpsid i" ::: "memory")
#define ENABLE_INTERRUPTS()     __asm__ volatile ("cpsie i" ::: "memory")

/* MISMATCH: lk_conn_recv()/lk_conn_send() are void, no short-read or dead-host path. The tail
 * is filled with FW_LK_RECV_FILL and the command answers one ACK_ERROR; a send cannot abort. */
extern void fw_lk_conn_recv(uint8_t *data, uint16_t count);
extern void fw_lk_conn_send(const uint8_t *data, uint16_t count);
extern void fw_lk_conn_send_byte(uint8_t data);

#define CONN_RECV(data, count)      { fw_lk_conn_recv((data), (count)); }
#define CONN_SEND(data, count)      { fw_lk_conn_send((data), (count)); }
#define CONN_SEND_BYTE(data)        { fw_lk_conn_send_byte((uint8_t)(data)); }

/* Tracks the bulk IN packet size so one chunk fills one USB packet; do not pin it to a
 * literal, slower. LK.c only compares it against a u16 length. */
#define CHUNK_MAX_LEN               bl_usb_ep2_pkt_in

extern void fw_lk_bootloader_reset(void);
#define BOOTLOADER_RESET()          { fw_lk_bootloader_reset(); }

/* A0..A15 on PA0..PA15, D0..D7 on PB0..PB7. Byte addresses, not halfword indices. */

#define RAW_DMG_ADDR_SET(addr)      { LKDEV_PA_OUT = (uint32_t)(addr) & PA_AD_MASK; }
#define RAW_DMG_ADDR_DIR_OUT()      { LKDEV_PA_DIR |= PA_AD_MASK;  }
#define RAW_DMG_ADDR_DIR_IN()       { LKDEV_PA_DIR &= ~PA_AD_MASK; }

/* MISMATCH: this re-arms the address bus too. lk_agb_cart_write_sram_byte() (LK.c:1634) does
 * not, and then the address never reaches the cart, the write lands nowhere, and it ACKs. */
#define RAW_DMG_DATA_DIR_OUT()      { LKDEV_PA_DIR |= PA_AD_MASK;            \
                                      LKDEV_PB_DIR |= PB_ADDR_HI;  }
#define RAW_DMG_DATA_DIR_IN()       { LKDEV_PB_DIR &= ~PB_ADDR_HI; }
#define RAW_DMG_DATA_SET(data)      { LKDEV_PB_CLR  = PB_ADDR_HI;            \
                                      LKDEV_PB_OUT |= (uint32_t)(data) & PB_ADDR_HI; }
#define RAW_DMG_DATA_GET()          ((uint32_t)(LKDEV_PB_PIN & PB_ADDR_HI))

/* AD0..AD15 carry A0..A15 while /CS latches, then the data word; A16..A23 stay on PB0..PB7. */

#define RAW_AGB_ADDR_SET(addr)                                              \
    {                                                                       \
        LKDEV_PA_OUT  = (uint32_t)(addr) & PA_AD_MASK;                      \
        LKDEV_PB_CLR  = PB_ADDR_HI;                                         \
        LKDEV_PB_OUT |= ((uint32_t)(addr) >> 16) & PB_ADDR_HI;              \
    }

#define RAW_AGB_ADDR_DIR_OUT()      { LKDEV_PA_DIR |= PA_AD_MASK;           \
                                      LKDEV_PB_DIR |= PB_ADDR_HI;  }
#define RAW_AGB_ADDR_DIR_IN()       { LKDEV_PA_DIR &= ~PA_AD_MASK;          \
                                      LKDEV_PB_DIR &= ~PB_ADDR_HI; }

#define RAW_AGB_DATA_SET(data)      { LKDEV_PA_OUT = (uint32_t)(data) & PA_AD_MASK; }
#define RAW_AGB_DATA_DIR_OUT()      { LKDEV_PA_DIR |= PA_AD_MASK; }
#define RAW_AGB_DATA_GET()          ((uint32_t)(LKDEV_PA_PIN & PA_AD_MASK))

/* No settle here; patch 0002 calls _delay_agb_latch() at the five sites. */
#define RAW_AGB_DATA_DIR_IN()       { LKDEV_PA_DIR &= ~PA_AD_MASK; }

/* Patch 0001. LK.c:1710's fixed _delay_us(20) is short for an MX29L010 written back to back:
 * the save erases to zeroes while every command ACKs. Do not loosen the 400-poll bound. */
#define AGB_SAVE_FLASH_WAIT(addr, want)                                     \
    {                                                                       \
        u32 _lk_poll;                                                       \
        u8  _lk_got = (u8)~(u8)(want);                                      \
        for (_lk_poll = 0; _lk_poll < 400u; _lk_poll++) {                   \
            _lk_got = lk_agb_cart_read_sram_byte((u16)(addr));              \
            if (_lk_got == (u8)(want)) { break; }                           \
        }                                                                   \
        (void)_lk_got;                                                      \
    }

#endif /* _LK_DEVICE_CH579_H_ */
