/* DMG/AGB cart edge connector on CH579 GPIO. Pins: re/symbols-cartio.md §1. */

#ifndef FW_CART_H
#define FW_CART_H

#include <stdint.h>

#define R32_PA_DIR      0x400010A0u
#define R32_PA_PIN      0x400010A4u
#define R32_PA_OUT      0x400010A8u
#define R32_PA_CLR      0x400010ACu
#define R32_PA_PU       0x400010B0u

#define R32_PB_DIR      0x400010C0u
#define R32_PB_PIN      0x400010C4u
#define R32_PB_OUT      0x400010C8u
#define R32_PB_CLR      0x400010CCu
#define R32_PB_PU       0x400010D0u
#define R32_PB_PD_DRV   0x400010D4u

#define PA_AD_MASK      0x0000FFFFu /* cart 6..21: DMG A0..A15, AGB AD0..15 */
#define PB_ADDR_HI      0x000000FFu /* cart 22..29: DMG D0..D7, AGB A16..A23 */

#define PB_LED          (1u << 12)  /* status LED, no cart pin           */
#define PB_WR           (1u << 13)  /* cart 3                            */
#define PB_RD           (1u << 14)  /* cart 4                            */
#define PB_CS           (1u << 15)  /* cart 5, AGB address latch         */
#define PB_CLK          (1u << 18)  /* cart 2, PHI, idles LOW            */
#define PB_CS2          (1u << 19)  /* cart 30, DMG /RESET               */
#define PB_AUDIO        (1u << 20)  /* cart 31                           */
/* Low 3.3 V, high 5 V (re/symbols-cartio.md §1 has PB21 wrong). 5 V at a 3.3 V
 * AGB cartridge is irreversible; fw_cart_voltage() refuses it outside DMG. */
#define PB_VSEL         (1u << 21)
#define PB_VCC_EN       (1u << 22)  /* cart VCC enable                   */
#define PB_BUTTON       (1u << 23)  /* U22 button, input, pull-up        */

#define PB_CTRL_IDLE    (0xC7u << 13)   /* /WR /RD /CS /CS2 AUDIO, all high */

void fw_cart_init(void);            /* first; leaves cart power off */

#define FW_CART_SETTLE_MS   10u     /* rail settle, either direction */

/* Cold start for the cartridge's own controller, not the rail. Too short reads
 * shifted forward one halfword, stable and plausible: raise this first. */
#define FW_CART_POWERON_MS  300u

/* Settle spent inside CART_PWR_ON before its ACK, the rest charged to the first
 * bus access. Neither may blow the host's 1 s ACK (LK_Device.py:132). */
#define FW_CART_POWERON_ACK_MS  100u

/* Cart-facing lines go to input before the rail drops and stay there until it
 * is back and settled: a high into an unpowered cartridge back-powers it. */
void fw_cart_power(int on);

void fw_cart_pullups(int on);       /* 0xAB / 0xAC, cart-present probe */

int fw_cart_voltage(int five_volt, int dmg_mode);

void fw_cart_tristate(void);        /* SET_ADDR_AS_INPUTS (0xA8) */

void fw_cart_settle(void);

/* The cart auto-increments per /RD: one latch covers the whole read. */
void fw_cart_agb_open(uint32_t hwaddr);

uint32_t fw_cart_agb_burst(uint8_t *out, uint32_t count);   /* count even */

void fw_cart_agb_close(void);

/* FW_AGB_LEAF only. Both counts must be non-zero: zero walks dst past the
 * group-end pointer and on into MMIO. dst must be halfword-aligned. */
void fw_cart_agb_read_leaf(uint32_t hwaddr, uint16_t *dst,
                           uint32_t halfwords, uint32_t grp_halfwords);

void fw_cart_dmg_setup(void);       /* after a mode change, before cycle 1 */

#define FW_DMG_WE_WR        1u      /* /WR   (PB13), and the default         */
#define FW_DMG_WE_AUDIO     2u      /* AUDIO (PB20), what some carts wire WE */
#define FW_DMG_WE_WR_RESET  3u      /* /CS2 (PB19) held around a /WR pulse   */

void fw_cart_set_we_pin(uint8_t we);    /* FLASH_WE_PIN, LK.h:75-77; DMG only */
void fw_cart_audio_drive(uint8_t on);
void fw_cart_audio_dir(uint8_t out);

#if FW_DMG_WRITE_BURST
int g_dmg_we_is_wr(void);
void fw_cart_dmg_amd_program_byte(const uint32_t *cmd_addr,
                                  const uint16_t *cmd_val,
                                  uint32_t pa, uint8_t pd);
void fw_cart_dmg_write_burst_release(void);
#endif
#if FW_DMG_POLL_TIGHT
void fw_cart_dmg_status_poll_open(void);
uint8_t fw_cart_dmg_status_poll_read(uint32_t addr);
void fw_cart_dmg_status_poll_close(void);
#endif

uint32_t fw_cart_dmg_read(uint32_t addr, uint8_t *out, uint32_t count,
                          uint8_t method, uint8_t cs_pulse);

/* Always /WR; fw_cart_dmg_flash_write() strobes the FLASH_WE_PIN selection. */
void fw_cart_dmg_write(uint32_t addr, uint8_t value, uint8_t cs_pulse);

void fw_cart_dmg_flash_write(uint32_t addr, uint8_t value, uint8_t cs_pulse);

void fw_cart_dmg_pulse_reset(void); /* FLASH_PULSE_RESET, no settle after */

void fw_cart_dmg_mbc_reset(void);

void fw_cart_clk_pulses(uint32_t n);    /* CLK_TOGGLE (0xA9), MBC3+RTC */

void fw_cart_agb_write(uint32_t hwaddr, uint16_t value);

/* Buffered-program data phase: LE halfwords from `hwaddr' up. */
void fw_cart_agb_write_burst(uint32_t hwaddr, const uint8_t *data,
                             uint32_t words);

/* Saves sit behind /CS2 on the A16..A23 pins; bracket every run of accesses. */
void fw_cart_agb_sram_open(void);
void fw_cart_agb_sram_close(void);
uint32_t fw_cart_agb_sram_read(uint32_t addr, uint8_t *out, uint32_t count);
void fw_cart_agb_sram_write(uint32_t addr, uint8_t value);

void fw_cart_agb_sram_program(uint32_t addr, const uint8_t *data, uint32_t len,
                              uint8_t method);

uint16_t fw_cart_agb_peek(uint32_t hwaddr);     /* safe between writes */

/* Approximate. For a strobe width use BUS_NOPS() in cart.c: it is exact. */
void fw_cart_delay_nops(uint32_t n);

void fw_cart_agb_bootup(void);

void fw_cart_agb_rtc_read(uint8_t *out);    /* fills 8: status + 7 clock */

void fw_cart_agb_3d_open(uint32_t hwaddr, uint16_t buffer_size);
uint32_t fw_cart_agb_3d_read(uint8_t *out, uint32_t count);  /* [GATED] */
void fw_cart_agb_3d_close(void);

/* Call once before a run of EEPROM words; without it writes land wrong. */
void fw_cart_agb_eeprom_bus(void);

/* selector is the host's size byte: 2 = 64K part (14 address bits), else 4K. */
void fw_cart_agb_eeprom_read(uint32_t addr, uint8_t *out, uint8_t selector);

void fw_cart_agb_eeprom_write(uint32_t addr, const uint8_t *data,
                              uint8_t selector);  /* blocks ~10 ms */

#endif /* FW_CART_H */
