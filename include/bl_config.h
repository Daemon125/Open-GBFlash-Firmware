/* Copied from the GBFlash bootloader project and not kept in sync. Outside this
 * header only BL_REG*, BL_SAFE_ACCESS_SIG*, BL_R8_SAFE_ACCESS_SIG,
 * BL_R8_HFCK_PWR_CTRL and BL_FSYS_HZ are read: src/usb.c and src/timebase.c. */

#ifndef BL_CONFIG_H
#define BL_CONFIG_H

#define BL_CODEFLASH_BASE       0x00000000
#define BL_CODEFLASH_SIZE       0x0003E800
#define BL_CODEFLASH_END        0x0003E800      /* exclusive                  */

#define BL_SELF_BASE            0x00000000
#define BL_SELF_SIZE            15872
#define BL_SELF_END             0x00003E00

#define BL_BOOTINFO_BASE        0x00003E00
#define BL_BOOTINFO_SECTOR_SIZE 0x00000200
#define BL_BOOTINFO_LEN         14              /* bytes actually used        */

#define BL_APP_BASE             0x00004000
#define BL_APP_MAX_SIZE         (BL_CODEFLASH_END - BL_APP_BASE)  /* 0x3A800 */

#define BL_WRITE_FLOOR          BL_BOOTINFO_BASE  /* 0x3E00; never erase below it, bootloader */

#define BL_SRAM_BASE            0x20000000
#define BL_SRAM_SIZE            0x00008000
#define BL_STACK_TOP            0x20008000      /* vector[0]                  */

/* No-init hole; reproduce it or the boot magic below is lost across SYSRESETREQ. */
#define BL_NOINIT_BASE          0x20000000
#define BL_NOINIT_SIZE          0x000000A0
#define BL_RW_BASE              0x200000A0      /* first byte of .data/.bss   */

/* Write VALUE here then SYSRESETREQ; SRAM survives it. Clear it once consumed,
 * on every path, or every later reset re-enters update mode. */
#define BL_BOOT_MAGIC_ADDR      0x20000090
#define BL_BOOT_MAGIC_VALUE     0xAA55BB01

#define BL_SCB_AIRCR            0xE000ED0C
#define BL_SYSRESETREQ_KEY      0x05FA0004

#define BL_VECTOR_COUNT         36              /* 16 core exceptions + 20 IRQs */
#define BL_VECTOR_TABLE_SIZE    0x90

/* Unread here. The bound must reject 0xFFFFFFFF: an EXC_RETURN-shaped branch
 * target taken in a fault handler escalates to LOCKUP on M0. */
#define BL_VEC_ACCEPT_LO        0x00004000      /* == BL_APP_BASE, 1 << 14    */
#define BL_VEC_ACCEPT_HI        0x00040000      /* 1 << 18                    */
#define BL_VEC_ACCEPT_LO_SHIFT  14
#define BL_VEC_ACCEPT_HI_SHIFT  18

/* Write SIG1 then SIG2 here immediately before a SAM register write, nothing between. */
#define BL_R8_SAFE_ACCESS_SIG   0x40001040
#define BL_SAFE_ACCESS_SIG1     0x57
#define BL_SAFE_ACCESS_SIG2     0xA8
#define BL_SAFE_ACCESS_LOCK     0x00

#define BL_R16_CLK_SYS_CFG      0x40001008      /* a SAM register */
#define BL_R8_HFCK_PWR_CTRL     0x4000100A      /* do not disturb: 0x1C live  */

/* Bit 9 selects the crystal: leave it clear, the boards ship without one. Bit 15
 * is RB_XO_DI, an X32MO pin sample not a ready bit; mask it out of a readback. */
#define BL_CLK_SYS_CFG_VALUE    0x0088

/* Overridden from the Makefile, which derives it from FW_SYS_CLK_CFG. The
 * timebase converts SysTick cycles to microseconds with this. */
#ifndef BL_FSYS_HZ
#define BL_FSYS_HZ              32000000
#endif

#define BL_R32_PB_DIR           0x400010C0
#define BL_R32_PB_PIN           0x400010C4
#define BL_R32_PB_OUT           0x400010C8
#define BL_R32_PB_CLR           0x400010CC
#define BL_R32_PB_PU            0x400010D0
#define BL_R32_PB_PD_DRV        0x400010D4

#define BL_LED_PIN              12              /* activity LED, push-pull, low = lit */
#define BL_LED_MASK             (1u << 12)

#define BL_BTN_PIN              23              /* U22 button, pull-up, active low */
#define BL_BTN_MASK             (1u << 23)
#define BL_BTN_SETTLE_MS        1               /* settle after enabling the pull-up */

/* PB22 is the PCB-revision strap and likely the H1 / ISP boot pad, a different
 * pin from U22. Never configure or drive it; recovery may rest on it. */
#define BL_H1_PIN               22

#define BL_R32_FLASH_DATA       0x40001800
#define BL_R32_FLASH_ADDR       0x40001804
#define BL_R8_FLASH_COMMAND     0x40001808
#define BL_R8_FLASH_PROTECT     0x40001809
#define BL_R16_FLASH_STATUS     0x4000180A

#define BL_ROM_CMD_PROG         0x9A            /* program one 32-bit word    */
#define BL_ROM_CMD_ERASE        0xA6            /* erase one 512-byte sector  */

/* Reads back 0x08 after a 0x88 write; bit 7 is write-only. Never write 0x8C: it
 * unlocks InfoFlash, which holds CFG_BOOT_EN, the H1 / ROM-ISP recovery bit. */
#define BL_FLASH_LOCK           0x80
#define BL_FLASH_UNLOCK_CODE    0x88

/* No busy bit: the MCU is paused for the operation. Success reads back 0x40. */
#define BL_FLASH_STAT_CMD_TOUT  0x0001
#define BL_FLASH_STAT_CMD_ERR   0x0002
#define BL_FLASH_STAT_ADDR_OK   0x0040
#define BL_FLASH_STAT_OK_MASK   (BL_FLASH_STAT_ADDR_OK | BL_FLASH_STAT_CMD_TOUT | BL_FLASH_STAT_CMD_ERR)
#define BL_FLASH_STAT_OK_VALUE  BL_FLASH_STAT_ADDR_OK

#define BL_FLASH_SECTOR_SIZE    512             /* 0x3E00 and 0x4000 are multiples: no shared sector */
#define BL_FLASH_SECTOR_MASK    (BL_FLASH_SECTOR_SIZE - 1)

/* Record at 0x3E00, LE: marker u16, "LFBG", app CRC16, app length u32, CRC16 over
 * bytes 0x00..0x0B. Leave the marker 0xFFFF: 0xFFFF -> 0x5555 sets bits. */
#define BL_BI_OFF_MARKER        0x00
#define BL_BI_OFF_NAME          0x02
#define BL_BI_OFF_APP_CRC       0x06
#define BL_BI_OFF_APP_LEN       0x08
#define BL_BI_OFF_REC_CRC       0x0C

#define BL_BI_MARKER_ERASED     0xFFFF
#define BL_BI_MARKER_STAMPED    0x5555          /* accepted, never written    */
#define BL_BI_NAME_0            'L'
#define BL_BI_NAME_1            'F'
#define BL_BI_NAME_2            'B'
#define BL_BI_NAME_3            'G'

/* Stale: the official bootloader caps applen at 0x38800 (ld/firmware.ld,
 * tools/mkimage.py, Makefile). Over that cap the updater erases boot-info on
 * packet 1 then NAKs the rest: stuck in update mode. */
#define BL_BI_MAX_APP_LEN       BL_APP_MAX_SIZE         /* 0x3A800 */

#define BL_APP_SP_MASK          0x2FFE0000      /* sane if (word at 0x4000 & MASK) == VALUE */
#define BL_APP_SP_VALUE         0x20000000

#define BL_CRC16_INIT           0xFFFF
#define BL_CRC16_POLY_REFL      0xA001          /* CRC16/MODBUS, no final xor */

#ifndef __ASSEMBLER__   /* macros above reach assembly: integer literals only */

#include <stdint.h>

#define BL_REG8(a)   (*(volatile uint8_t  *)(uintptr_t)(a))
#define BL_REG16(a)  (*(volatile uint16_t *)(uintptr_t)(a))
#define BL_REG32(a)  (*(volatile uint32_t *)(uintptr_t)(a))

extern uint32_t __stack_top;
extern uint32_t __data_load, __data_start, __data_end;
extern uint32_t __bss_start,  __bss_end;

void Reset_Handler(void);

#endif /* __ASSEMBLER__ */

#endif /* BL_CONFIG_H */
