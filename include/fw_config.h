#ifndef FW_CONFIG_H
#define FW_CONFIG_H

#define FW_CFW_ID           'L'           /* FlashGBX binds on this (hw_GBFlash.py) */

/* 15 is what the L15 host protocol expects and what this implements: the PING
 * challenge and the SET_VAR_STATE ack in proto.c, and the audio write-enable
 * pin that FlashGBX refuses to use below 14 (LK_Device.py:4054). Below 12 the
 * host drops the ACKed SET_VARIABLE path.
 *
 * hw_GBFlash.DEVICE_MAX_FW is 12 and looks like a ceiling. It is declared in
 * all four device classes and read by none of them; detection tests only
 * fw_ver >= DEVICE_MIN_FW. */
#define FW_VERSION          15

#ifndef FW_TIMESTAMP
/* Build date reported to the host, seconds since the epoch. Set at release, not
 * derived from the clock: the released image has to stay byte-identical to the
 * one tested on hardware, and a value that moves with the build does not.
 *
 * The host file this project ships compares it against res/fw_Open-GBFlash.zip.
 * A stock FlashGBX compares it against its own table of stock build dates and
 * offers an update whenever it differs. */
#define FW_TIMESTAMP        1788313704u
#endif

#if FW_TIMESTAMP != 0u && FW_TIMESTAMP < 1730592000u
#error "FW_TIMESTAMP below 1730592000 makes FlashGBX flag this as unofficial \
firmware and offer to overwrite it on every connect"
#endif

#define FW_PCB_NAME         "Open-GBFlash"

/* Mirrors ../bootloader/include/boot.h. Nothing here reads them; ld/firmware.ld
 * and tools/mkimage.py hold the copies that enforce the layout. */
#define FW_BOOTINFO_BASE    0x00003E00u
#define FW_APP_BASE         0x00004000u
#define FW_CODEFLASH_END    0x0003E800u

#define FW_BL_MAGIC_ADDR    0x20000090u   /* write VALUE here, then SYSRESETREQ */
#define FW_BL_MAGIC_VALUE   0xAA55BB01u

/* Sizes g_reply[]; 0x8000 does not fit in SRAM. A host that hard-codes a larger
 * value instead of reading it back gets half a block and desynchronises. */
#ifndef FW_MAX_TRANSFER
#define FW_MAX_TRANSFER     0x5000u
#endif


#define FW_CMD_BUF_LEN      0x40u

#define FW_CART_STEP_DEFAULT  32u         /* pre-init default only; fw_main() sets the live value */

/* /CS re-assert interval. AGB Stream takes its read step from this
 * (main.c agb_step_bytes); CMD_FW_SET_LATCH overrides it. */
#define FW_CART_LATCH         128u

#endif /* FW_CONFIG_H */
