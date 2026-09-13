/* FlashGBX wire protocol, device side, from LK_Device.DEVICE_CMD/DEVICE_VAR.
 * No framing or length prefix: a reply one byte short desyncs the stream. */

#ifndef FW_PROTO_H
#define FW_PROTO_H

/* Changes sizeof(fw_state_t); the fallback must match usb.h's. */
#ifndef FW_USB_CDC
#define FW_USB_CDC 0
#endif

#include <stdint.h>

#define CMD_NULL                    0x30u
/* 0x00 straight after 0xC8 is ReadROM_3DMemory's page terminator and must not
 * be ACKed (LK_Device.py:1705-1716); every other 0x00 is _try_write()'s resync
 * and must be. See page_terminator_due. */
#define CMD_RESYNC                  0x00u
#define CMD_QUERY_FW_INFO           0xA1u
#define CMD_SET_MODE_AGB            0xA2u
#define CMD_SET_MODE_DMG            0xA3u
#define CMD_SET_VOLTAGE_3_3V        0xA4u
#define CMD_SET_VOLTAGE_5V          0xA5u
#define CMD_SET_VARIABLE            0xA6u
#define CMD_GET_VARIABLE            0xADu
#define CMD_ENABLE_PULLUPS          0xABu
#define CMD_DISABLE_PULLUPS         0xACu
#define CMD_AGB_BOOTUP_SEQUENCE     0xC9u
#define CMD_AGB_READ_GPIO_RTC       0xCAu
#define CMD_SET_ADDR_AS_INPUTS      0xA8u
#define CMD_SET_PIN                 0xF5u
#define CMD_DMG_CART_READ           0xB1u
#define CMD_DMG_CART_WRITE          0xB2u
#define CMD_DMG_MBC_RESET           0xB4u
#define CMD_CLK_TOGGLE              0xA9u
#define CMD_SET_FLASH_CMD           0xA7u
#define CMD_DMG_FLASH_WRITE_BYTE    0xD1u
#define CMD_AGB_FLASH_WRITE_SHORT   0xD2u
#define CMD_FLASH_PROGRAM           0xD3u
#if FW_DMG_PROFILE
#define CMD_PROFILE_READ            0xDFu
#endif
#define CMD_CART_WRITE_FLASH_CMD    0xD4u
#define CMD_AGB_CART_READ           0xC1u
#define CMD_BOOTLOADER_RESET        0xF1u
#define CMD_CART_PWR_ON             0xF2u
#define CMD_CART_PWR_OFF            0xF3u
#define CMD_QUERY_CART_PWR          0xF4u
#define CMD_GET_SWITCH_STATE        0xF6u
#define CMD_PING                    0xFEu

/* Named only so arg_len() and payload_len() can swallow them whole. Without an
 * entry the argument bytes are dispatched as opcodes. */
#define CMD_DEBUG                   0xA0u
#define CMD_GET_VAR_STATE           0xAEu
#define CMD_SET_VAR_STATE           0xAFu
#define CMD_DMG_CART_WRITE_SRAM     0xB3u
#define CMD_DMG_MBC7_READ_EEPROM    0xB5u
#define CMD_DMG_MBC7_WRITE_EEPROM   0xB6u
#define CMD_DMG_MBC6_MMSA_WRITE_FLASH 0xB7u
#define CMD_DMG_SET_BANK_CHANGE_CMD 0xB8u
#define CMD_DMG_EEPROM_WRITE        0xB9u
#define CMD_AGB_CART_WRITE          0xC2u
#define CMD_AGB_CART_READ_SRAM      0xC3u
#define CMD_AGB_CART_WRITE_SRAM     0xC4u
#define CMD_AGB_CART_READ_EEPROM    0xC5u
#define CMD_AGB_CART_WRITE_EEPROM   0xC6u
#define CMD_AGB_CART_WRITE_FLASH_DATA 0xC7u
#define CMD_AGB_CART_READ_3D_MEMORY 0xC8u
#define CMD_CALC_CRC32              0xD5u

/* No length prefix either way (LK_Device.py:920-934); the device fixes it. */
#define FW_VAR_STATE_LEN            64u

/* Diagnostic: TRANSFER_SIZE bytes in, a BE u32 checksum of them out. */
#define CMD_FW_ECHO_PAYLOAD         0xE7u
/* Diagnostic: last payload's byte count as a BE u16, then its buffer verbatim. */
#define CMD_FW_DUMP_PAYLOAD         0xE6u

/* Private. TRANSFER_SIZE bytes from RAM, no bus cycles: transport ceiling. */
#define CMD_FW_BENCH_TX             0xE0u

/* Private. Arg: cart fill step in 16-byte units; 0 restores main.c's choice. */
#define CMD_FW_SET_STEP             0xE1u

/* Private. Arg: /CS re-latch interval in 32-byte units, 0 for the default.
 * Separate knob from the poll step; do not fold the two together. */
#define CMD_FW_SET_LATCH            0xE2u

/* wait_for_ack() takes 0x01 or 0x03, reads 0x02 as an error, else times out. */

#define ACK_OK                      0x01u
#define ACK_ERROR                   0x02u

/* SET_VARIABLE is 0xA6 <size> <u32 key BE> <u32 value BE>. The size byte picks
 * the table: ADDRESS is (32, key 0) and TRANSFER_SIZE is (16, key 0). */

#define VAR_SIZE_8                  1u
#define VAR_SIZE_16                 2u
#define VAR_SIZE_32                 4u

#define VAR32_ADDRESS               0x00u
#define VAR32_AUTO_POWEROFF_TIME    0x01u

#define VAR16_TRANSFER_SIZE         0x00u
#define VAR16_BUFFER_SIZE           0x01u
#define VAR16_DMG_ROM_BANK          0x02u
#define VAR16_STATUS_REGISTER       0x03u
#define VAR16_LAST_BANK_ACCESSED    0x04u
#define VAR16_STATUS_REGISTER_MASK  0x05u
#define VAR16_STATUS_REGISTER_VALUE 0x06u
/* Private. 32-byte buffers agb_stream_pump() programmed during the last block. */
#define VAR16_FW_STREAMED_CHUNKS    0x7Fu
/* Private. Low byte = IRQ6 owns the SIE; high byte = BL_USB_IRQ_FAULT_*. */
#define VAR16_FW_USB_IRQ            0x7Eu
/* Private. BL_USB_DESC_SET_* (0 CDC, 1 CH340); a non-CDC build also answers 0. */
#define VAR16_FW_USB_DESC           0x7Cu
/* Private. pump() wait-loop passes with no TX ring space, last read. Saturates. */
#define VAR16_FW_PUMP_STALLS        0x7Du

#define VAR8_CART_MODE              0x00u
#define VAR8_DMG_ACCESS_MODE        0x01u
#define VAR8_FLASH_COMMAND_SET      0x02u
#define VAR8_FLASH_METHOD           0x03u
#define VAR8_FLASH_WE_PIN           0x04u
#define VAR8_FLASH_PULSE_RESET      0x05u
#define VAR8_FLASH_COMMANDS_BANK_1  0x06u
#define VAR8_FLASH_SHARP_VERIFY_SR  0x07u
#define VAR8_FLASH_DOUBLE_DIE       0x0Au
#define VAR8_AGB_IRQ_ENABLED        0x10u
#define VAR8_DMG_AUDIO_ENABLED      0x11u
#define VAR8_DMG_READ_CS_PULSE      0x08u
#define VAR8_DMG_WRITE_CS_PULSE     0x09u
#define VAR8_DMG_READ_METHOD        0x0Bu
#define VAR8_AGB_READ_METHOD        0x0Cu
#define VAR8_CART_POWERED           0x0Du
#define VAR8_PULLUPS_ENABLED        0x0Eu
#define VAR8_AUTO_POWEROFF_EN       0x0Fu

/* Capability bits in QUERY_FW_INFO (hw_GBFlash.py LoadFirmwareVersion). Clear
 * FW_CAP2_BOOTLOADER_RESET and every reflash needs U22 held at power-on. */

#define FW_CAP1_CART_POWER_CTRL     0x01u
#define FW_CAP1_PRESENCE_SWITCH     0x02u
#define FW_CAP1_MODE_SWITCH         0x04u
#define FW_CAP2_BOOTLOADER_RESET    0x01u
#define FW_CAP2_UNREGISTERED        0x80u
/* Clear FW_CAP1_CART_POWER_CTRL and the host never energises the slot
 * (hw_GBFlash.py:118, LK_Device.py:846): every read is open bus. */
#define FW_CAP1                     FW_CAP1_CART_POWER_CTRL
#define FW_CAP2                     FW_CAP2_BOOTLOADER_RESET

/* Pairs one CART_WRITE_FLASH_CMD may carry; a longer sequence is refused. */
#define FW_BATCH_MAX    32u

#define FW_BANK_CMD_MAX 8u

typedef enum {
    FW_MODE_NONE = 0,
    FW_MODE_DMG,
    FW_MODE_AGB
} fw_mode_t;

typedef struct {
    uint32_t address;           /* auto-advances after a read */
    uint16_t transfer_size;
    uint16_t buffer_size;
    uint16_t dmg_rom_bank;
    uint8_t  cart_mode;
    uint8_t  pcb_ver;           /* board revision, not cart_mode; read-only */
    uint8_t  dmg_access_mode;
    uint8_t  dmg_read_method;
    uint8_t  dmg_read_cs_pulse;
    uint8_t  agb_read_method;
    uint8_t  cart_powered;
    uint8_t  pullups_enabled;
    fw_mode_t mode;

    uint8_t  read_requested;    /* main.c does the read and clears this */
    uint8_t  read_is_save;      /* AGB_CART_READ_SRAM; separate chip select */
    uint8_t  read_is_eeprom;
    uint8_t  read_is_3d;
    uint8_t  page_terminator_due;   /* cleared in execute(); see CMD_RESYNC */
    uint16_t page_bytes_done;
    uint8_t  page_end_requested;
    uint8_t  rtc_read_requested;    /* main.c fills 8 bytes from the S3511 */
    uint8_t  bootup_requested;
    uint8_t  eeprom_sel;        /* host size byte: 2 = 64K part, else 4K */

    uint8_t  crc_requested;     /* CALC_CRC32 from ADDRESS, current mode's bus */
    uint32_t crc_len;

    uint8_t  save_write_requested;  /* transfer_size payload bytes follow */
    uint8_t  save_write_is_dmg;
    uint8_t  save_write_flash;      /* method selector: 1 standard, 2 Atmel */
    uint8_t  save_write_eeprom;     /* AGB_CART_WRITE_EEPROM; holds the size sel */

    /* Stock 8-bit table 0x2000196E (FLASH_METHOD +3), 16-bit table 0x20001960
     * (mask +10, value +12). */
    uint16_t status_register;
    uint16_t last_bank_accessed;
    uint16_t status_reg_mask;
    uint16_t status_reg_value;
    uint8_t  flash_we_pin_var;
    uint8_t  we_pin_requested;
    uint8_t  flash_pulse_reset;
    uint8_t  flash_commands_bank_1;
    uint8_t  flash_sharp_verify_sr;
    uint8_t  flash_double_die;
    /* Pin direction, not a flag: lk_dmg_flash_enable_audio() (LK.c:235-248). */
    uint8_t  audio_requested;
    uint8_t  agb_irq_enabled;
    uint8_t  dmg_audio_enabled;

    uint8_t  dmg_bank_cmd_count;    /* DMG_SET_BANK_CHANGE_CMD, held after setup */
    uint32_t dmg_bank_cmd_val[FW_BANK_CMD_MAX];
    uint8_t  dmg_bank_cmd_type[FW_BANK_CMD_MAX];
    uint16_t save_write_len;

    uint8_t  bench_requested;

    /* cart_powered is the rail after settling, not the request. */
    uint8_t  power_requested;
    uint8_t  power_target;

    uint16_t cart_step;         /* bytes read between USB service calls */
    uint16_t cart_latch;        /* /CS re-assert interval; FW_CART_LATCH default */

    /* Host step override, 0 for none. Never written to cart_step: main.c keeps
     * that equal to the bulk IN packet size and holds the only copy. */
    uint16_t cart_step_pin;


    uint8_t  pullup_requested;

    uint8_t  setpin_high;
    uint32_t setpin_mask;       /* SET_PIN: only bit 0, the cart rail, acts */

    uint8_t  tristate_requested;    /* SET_ADDR_AS_INPUTS */

    uint8_t  voltage_requested;     /* main.c drives PB21, guards 5 V to DMG */
    uint8_t  voltage_five;
    uint8_t  voltage_is_five;   /* set when the last voltage request was honoured */

    uint8_t  write_requested;   /* DMG_CART_WRITE; MBC bank switches come here */
    uint32_t write_addr;
    uint8_t  write_value;

    uint8_t  mbc_reset_requested;

    uint8_t  debug_requested;   /* CMD_DEBUG: pulse CLK for a scope */
    uint8_t  clk_requested;
    uint32_t clk_pulses;

    uint8_t  dmg_write_cs_pulse;

    /* SET_FLASH_CMD: unlock sequence replayed before each programmed word. */
    uint8_t  flash_command_set;
    uint8_t  flash_method;
    uint8_t  flash_we_pin;
    uint32_t flash_cmd_addr[6];
    uint16_t flash_cmd_val[6];

    uint8_t  batch_requested;   /* CART_WRITE_FLASH_CMD: raw (addr, val) writes */
    uint8_t  batch_flashcart;
    uint8_t  batch_count;
    uint32_t batch_addr[FW_BATCH_MAX];
    uint16_t batch_val[FW_BATCH_MAX];

    uint8_t  program_requested; /* FLASH_PROGRAM: transfer_size bytes follow */
    uint16_t program_len;

    /* do_flash_program() resumes here; one 32-byte bulk OUT packet per buffer. */
    uint16_t program_done;
    uint16_t program_streamed;  /* VAR16_FW_STREAMED_CHUNKS */
    uint16_t usb_irq_state;     /* VAR16_FW_USB_IRQ; armed flag | fault << 8  */
#if FW_USB_CDC
    uint16_t usb_desc_state;    /* VAR16_FW_USB_DESC; BL_USB_DESC_SET_*       */
#endif
    uint16_t pump_stalls;       /* VAR16_FW_PUMP_STALLS */
    uint8_t  program_stream_err;    /* failed chunk, ACKed at block end */


    uint8_t  flash_write_requested;
    uint32_t flash_write_addr;
    uint16_t flash_write_val;
    uint8_t  flash_write_is_agb;

    uint8_t  reset_requested;   /* main.c writes the magic word, then SYSRESETREQ */
} fw_state_t;

void fw_proto_init(fw_state_t *st);

uint32_t fw_proto_state_size(void);
uint32_t fw_proto_state_tail_off(void);

/* Returns bytes written into `out`, 0 while still accumulating a command.
 * `out` must have room for FW_MAX_TRANSFER bytes. */
uint32_t fw_proto_feed(fw_state_t *st, uint8_t b, uint8_t *out);

/* Payload bytes still outstanding, or 0 when no payload is open. */
uint32_t fw_proto_payload_remaining(void);

/* Non-zero when the parser is between commands. fw_main() must check it before
 * handing a byte to LK.c's dispatcher, which blocks pulling its own arguments. */
int fw_proto_idle(void);

/* FLASH_PROGRAM bytes landed in pay[] so far; 0 for any other command. */
uint32_t fw_proto_payload_filled(void);

/* Bulk intake for an open payload; *consumed 0 means use fw_proto_feed(). */
uint32_t fw_proto_feed_bulk(fw_state_t *st, const uint8_t *buf, uint32_t len,
                            uint8_t *out, uint32_t *consumed);

/* Abandon a mid-command parser the host stopped feeding. */
void fw_proto_tick(fw_state_t *st, uint32_t now_ms);

/* Abandon it immediately: the USB link was reset or re-enumerated. */
void fw_proto_link_reset(fw_state_t *st);

#define FW_PARSER_TIMEOUT_MS  2000u

/* Once the host stops reading mid-transfer, bl_usb_tx() returns 0 until the
 * endpoint drains, so this must stay past the host's 1.5 s command timeout. */
#define FW_TX_STALL_MS        3000u

/* The payload a FLASH_PROGRAM collected, valid while program_requested is set. */
const uint8_t *fw_proto_payload(void);

/* Largest inbound payload: the host's MAX_BUFFER_WRITE. */
#ifndef FW_PAYLOAD_MAX
#define FW_PAYLOAD_MAX  0x800u
#endif

uint32_t fw_proto_fw_info(const fw_state_t *st, uint8_t *out);

#endif /* FW_PROTO_H */
