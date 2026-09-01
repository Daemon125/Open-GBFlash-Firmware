/* FlashGBX command dispatcher. No length prefix on the wire and nothing to
 * resynchronise on: every path must return an exact reply count, or every
 * later reply is read shifted by one. */

#include <stdint.h>
#include "fw_config.h"
#include "proto.h"

/* Word-wise payload copy on the FLASH_PROGRAM receive path. The Makefile
 * defaults it to 1; the 0 here is the standalone-host fallback. */
#ifndef FW_PROTO_FAST_COPY
#define FW_PROTO_FAST_COPY 0
#endif

/* Var-state blob byte 20 carries flash_we_pin_var; 0 restores flash_we_pin. */
#ifndef FW_VARSTATE_WE_PIN
#define FW_VARSTATE_WE_PIN 1
#endif

static struct {
    uint8_t  op;        /* opcode being accumulated, 0 when idle            */
    uint8_t  need;
    uint8_t  got;
    uint8_t  arg[FW_CMD_BUF_LEN];

    /* Payload phase: pay_need is host-declared and can exceed pay[]. */
    uint16_t pay_need;
    uint16_t pay_got;
    uint8_t  pay_over;  /* declared more than pay[] holds; command must fail */

    /* Read by fw_proto_tick() to tell a mid-command host from a dead one. */
    uint32_t bytes;

    uint8_t  pay[FW_PAYLOAD_MAX];
} P;

const uint8_t *fw_proto_payload(void)
{
    return P.pay;
}

static void parser_reset(void)
{
    P.op = 0;
    P.need = 0;
    P.got = 0;
    P.pay_need = 0;
    P.pay_got = 0;
    P.pay_over = 0;
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
    return 4u;
}

static uint32_t put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
    return 2u;
}

/* The host suite mirrors fw_state_t in ctypes; drift garbles later fields. */
uint32_t fw_proto_state_size(void)
{
    return (uint32_t)sizeof(fw_state_t);
}

/* sizeof() alone misses a field added into existing padding. */
uint32_t fw_proto_state_tail_off(void)
{
    return (uint32_t)((const char *)&((const fw_state_t *)0)->reset_requested
                      - (const char *)0);
}

void fw_proto_init(fw_state_t *st)
{
    uint32_t i;
    uint8_t *p = (uint8_t *)st;

    for (i = 0; i < sizeof(*st); i++) {
        p[i] = 0;
    }
    /* 0 would make a read issued before the host sets it return nothing. */
    st->transfer_size = 64u;
    st->cart_step = FW_CART_STEP_DEFAULT;
    st->cart_latch = FW_CART_LATCH;
    st->cart_step_pin = 0u;
    st->mode = FW_MODE_NONE;
    parser_reset();
}

/* QUERY_FW_INFO, hw_GBFlash.py LoadFirmwareVersion(). The leading 8 is the
 * length of the block that follows, not part of it. Name and flags need fw 12. */

uint32_t fw_proto_fw_info(const fw_state_t *st, uint8_t *out)
{
    static const char name[] = FW_PCB_NAME;
    uint32_t n = 0;
    uint32_t i;

    out[n++] = 8u;
    out[n++] = (uint8_t)FW_CFW_ID;
    n += put_be16(&out[n], (uint16_t)FW_VERSION);
    out[n++] = st->pcb_ver;              
    n += put_be32(&out[n], (uint32_t)FW_TIMESTAMP);

    out[n++] = (uint8_t)(sizeof(name) - 1u);
    for (i = 0; i + 1u < sizeof(name); i++) {
        out[n++] = (uint8_t)name[i];
    }
    out[n++] = (uint8_t)FW_CAP1;
    out[n++] = (uint8_t)FW_CAP2;
    return n;
}

/* Switch on size before key: ADDRESS and TRANSFER_SIZE are both key 0. */

static void set_variable(fw_state_t *st, uint8_t size, uint32_t key,
                         uint32_t value)
{
    switch (size) {
    case VAR_SIZE_32:
        switch (key) {
        case VAR32_ADDRESS: st->address = value; break;
        default: break;
        }
        break;

    case VAR_SIZE_16:
        switch (key) {
        case VAR16_TRANSFER_SIZE:
            /* Clamped, not rejected: a bad value drives the read loop off the
             * end of the output buffer. */
            st->transfer_size = (value > FW_MAX_TRANSFER)
                                ? (uint16_t)FW_MAX_TRANSFER : (uint16_t)value;
            break;
        case VAR16_BUFFER_SIZE:   st->buffer_size = (uint16_t)value; break;
        case VAR16_DMG_ROM_BANK:
            st->dmg_rom_bank = (uint16_t)value;
            /* Mirror into LAST_BANK_ACCESSED (LK.c:252-254): the host sets
             * only DMG_ROM_BANK, so without this the bank-1 unlock excursion
             * restores bank 0 and banks 2..N land where the mapper maps 0. */
            st->last_bank_accessed = (uint16_t)(value & 0xFFu);
            break;
        case VAR16_STATUS_REGISTER: st->status_register = (uint16_t)value; break;
        case VAR16_LAST_BANK_ACCESSED:
            st->last_bank_accessed = (uint16_t)value; break;
        case VAR16_STATUS_REGISTER_MASK:
            st->status_reg_mask = (uint16_t)value; break;
        case VAR16_STATUS_REGISTER_VALUE:
            st->status_reg_value = (uint16_t)value; break;
        default: break;
        }
        break;

    case VAR_SIZE_8:
        switch (key) {
        case VAR8_CART_MODE:       st->cart_mode = (uint8_t)value; break;
        case VAR8_DMG_ACCESS_MODE: st->dmg_access_mode = (uint8_t)value; break;
        case VAR8_DMG_READ_METHOD: st->dmg_read_method = (uint8_t)value; break;
        case VAR8_DMG_READ_CS_PULSE:  st->dmg_read_cs_pulse = (uint8_t)value; break;
        case VAR8_DMG_WRITE_CS_PULSE: st->dmg_write_cs_pulse = (uint8_t)value; break;
        case VAR8_AGB_READ_METHOD: st->agb_read_method = (uint8_t)value; break;
        case VAR8_CART_POWERED:    st->cart_powered = (uint8_t)value; break;
        case VAR8_PULLUPS_ENABLED: st->pullups_enabled = (uint8_t)value; break;
        case VAR8_FLASH_COMMAND_SET: st->flash_command_set = (uint8_t)value; break;
        case VAR8_FLASH_METHOD:      st->flash_method = (uint8_t)value; break;
        case VAR8_FLASH_WE_PIN:
            st->flash_we_pin_var = (uint8_t)value;
            st->we_pin_requested = 1u;
            break;
        case VAR8_FLASH_PULSE_RESET: st->flash_pulse_reset = (uint8_t)value; break;
        case VAR8_FLASH_COMMANDS_BANK_1:
            st->flash_commands_bank_1 = (uint8_t)value; break;
        case VAR8_FLASH_SHARP_VERIFY_SR:
            st->flash_sharp_verify_sr = (uint8_t)value; break;
        case VAR8_FLASH_DOUBLE_DIE:  st->flash_double_die = (uint8_t)value; break;
        case VAR8_AGB_IRQ_ENABLED:
            st->agb_irq_enabled = (uint8_t)value;
            /* Gated on mode, as LK.c:235 gates on CART_MODE == AGB. */
            if (st->mode == FW_MODE_AGB) {
                st->audio_requested = 1u;
            }
            break;
        case VAR8_DMG_AUDIO_ENABLED:
            st->dmg_audio_enabled = (uint8_t)value;
            if (st->mode == FW_MODE_DMG) {   /* LK.c:242 */
                st->audio_requested = 1u;
            }
            break;
        default: break;
        }
        break;

    default:
        break;
    }
}

/* GET_VARIABLE: 0xAD <size> <u32 key BE> -> always four bytes big-endian
 * whatever the declared size; LK_Device._get_fw_variable unpacks ">I". */
static uint32_t get_variable(const fw_state_t *st, uint8_t size, uint32_t key)
{
    switch (size) {
    case VAR_SIZE_32:
        switch (key) {
        case VAR32_ADDRESS: return st->address;
        case VAR32_AUTO_POWEROFF_TIME: return 0u;
        default: break;
        }
        break;
    case VAR_SIZE_16:
        switch (key) {
        case VAR16_TRANSFER_SIZE: return st->transfer_size;
        case VAR16_BUFFER_SIZE:   return st->buffer_size;
        case VAR16_DMG_ROM_BANK:  return st->dmg_rom_bank;
        case VAR16_STATUS_REGISTER: return st->status_register;
        case VAR16_FW_STREAMED_CHUNKS: return st->program_streamed;
        case VAR16_FW_USB_IRQ: return st->usb_irq_state;
#if FW_USB_CDC
        case VAR16_FW_USB_DESC: return st->usb_desc_state;
#endif
        case VAR16_FW_PUMP_STALLS: return st->pump_stalls;
        case VAR16_LAST_BANK_ACCESSED: return st->last_bank_accessed;
        case VAR16_STATUS_REGISTER_MASK:  return st->status_reg_mask;
        case VAR16_STATUS_REGISTER_VALUE: return st->status_reg_value;
        default: break;
        }
        break;
    case VAR_SIZE_8:
        switch (key) {
        case VAR8_CART_MODE:       return st->cart_mode;
        /* No auto-power-off here; 0 stops the host retrying. */
        case VAR8_AUTO_POWEROFF_EN: return 0u;
        case VAR8_DMG_ACCESS_MODE: return st->dmg_access_mode;
        case VAR8_DMG_READ_METHOD: return st->dmg_read_method;
        case VAR8_DMG_READ_CS_PULSE:  return st->dmg_read_cs_pulse;
        case VAR8_DMG_WRITE_CS_PULSE: return st->dmg_write_cs_pulse;
        case VAR8_AGB_READ_METHOD: return st->agb_read_method;
        case VAR8_CART_POWERED:    return st->cart_powered;
        case VAR8_PULLUPS_ENABLED: return st->pullups_enabled;
        case VAR8_FLASH_COMMAND_SET: return st->flash_command_set;
        case VAR8_FLASH_METHOD:      return st->flash_method;
        case VAR8_FLASH_WE_PIN:      return st->flash_we_pin_var;
        case VAR8_FLASH_PULSE_RESET: return st->flash_pulse_reset;
        case VAR8_FLASH_COMMANDS_BANK_1: return st->flash_commands_bank_1;
        case VAR8_FLASH_SHARP_VERIFY_SR: return st->flash_sharp_verify_sr;
        case VAR8_FLASH_DOUBLE_DIE:  return st->flash_double_die;
        case VAR8_AGB_IRQ_ENABLED:   return st->agb_irq_enabled;
        case VAR8_DMG_AUDIO_ENABLED: return st->dmg_audio_enabled;
        default: break;
        }
        break;
    default:
        break;
    }
    
    return 0u;
}

/* GET_VAR_STATE / SET_VAR_STATE (0xAE / 0xAF), around a USB re-plug (LK:793).
 * Cart power, mode and voltage stay out of the blob; the host re-sends
 * SetMode() first, and restoring voltage would reach 5 V without
 * SET_VOLTAGE_5V's dmg_mode guard. Byte 20 must be the live flash_we_pin_var,
 * or AUDIO-WE DMG profiles come back strobing PB13 with AUDIO an input. */

static uint32_t get_var_state(const fw_state_t *st, uint8_t *out)
{
    uint32_t i;

    for (i = 0; i < FW_VAR_STATE_LEN; i++) {
        out[i] = 0u;
    }
    out[0] = 1u;                        /* blob version */
    out[1] = st->cart_mode;
    out[2] = st->dmg_access_mode;
    out[3] = st->dmg_read_method;
    out[4] = st->dmg_read_cs_pulse;
    out[5] = st->dmg_write_cs_pulse;
    out[6] = st->agb_read_method;
    out[7] = st->pullups_enabled;
    out[8]  = (uint8_t)(st->address >> 24);
    out[9]  = (uint8_t)(st->address >> 16);
    out[10] = (uint8_t)(st->address >> 8);
    out[11] = (uint8_t)st->address;
    out[12] = (uint8_t)(st->transfer_size >> 8);
    out[13] = (uint8_t)st->transfer_size;
    out[14] = (uint8_t)(st->buffer_size >> 8);
    out[15] = (uint8_t)st->buffer_size;
    out[16] = (uint8_t)(st->dmg_rom_bank >> 8);
    out[17] = (uint8_t)st->dmg_rom_bank;
    out[18] = st->flash_command_set;
    out[19] = st->flash_method;
#if FW_VARSTATE_WE_PIN
    out[20] = st->flash_we_pin_var;
#else
    out[20] = st->flash_we_pin;
#endif
    for (i = 0; i < 6u; i++) {
        out[21 + i * 4u]     = (uint8_t)(st->flash_cmd_addr[i] >> 24);
        out[21 + i * 4u + 1] = (uint8_t)(st->flash_cmd_addr[i] >> 16);
        out[21 + i * 4u + 2] = (uint8_t)(st->flash_cmd_addr[i] >> 8);
        out[21 + i * 4u + 3] = (uint8_t)st->flash_cmd_addr[i];
        out[45 + i * 2u]     = (uint8_t)(st->flash_cmd_val[i] >> 8);
        out[45 + i * 2u + 1] = (uint8_t)st->flash_cmd_val[i];
    }
    return FW_VAR_STATE_LEN;
}

static uint32_t set_var_state(fw_state_t *st, const uint8_t *in)
{
    uint32_t i;

    /* Unknown version: nothing restored, but its bytes are already consumed so
     * the stream stays in sync. The host reads nothing back below fw 15. */
    if (in[0] != 1u) {
        return 0u;
    }
    st->cart_mode          = in[1];
    st->dmg_access_mode    = in[2];
    st->dmg_read_method    = in[3];
    st->dmg_read_cs_pulse  = in[4];
    st->dmg_write_cs_pulse = in[5];
    st->agb_read_method    = in[6];
    st->pullups_enabled    = in[7];
    st->address       = be32(&in[8]);
    st->transfer_size = (uint16_t)((in[12] << 8) | in[13]);
    if (st->transfer_size > FW_MAX_TRANSFER) {
        st->transfer_size = (uint16_t)FW_MAX_TRANSFER;
    }
    st->buffer_size   = (uint16_t)((in[14] << 8) | in[15]);
    st->dmg_rom_bank  = (uint16_t)((in[16] << 8) | in[17]);
    st->flash_command_set = in[18];
    st->flash_method      = in[19];
    st->flash_we_pin      = in[20];
#if FW_VARSTATE_WE_PIN
    /* The pin is armed only when main.c drains we_pin_requested. */
    st->flash_we_pin_var  = in[20];
    st->we_pin_requested  = 1u;
#endif
    for (i = 0; i < 6u; i++) {
        st->flash_cmd_addr[i] = be32(&in[21 + i * 4u]);
        st->flash_cmd_val[i]  = (uint16_t)((in[45 + i * 2u] << 8)
                                           | in[45 + i * 2u + 1]);
    }
    return 0u;
}

/* True for bytes in the host's DEVICE_CMD table; error reply over silence. */
static int known_opcode(uint8_t op)
{
    switch (op) {
    case 0x30: case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4:
    case 0xA5: case 0xA6: case 0xA7: case 0xA8: case 0xA9: case 0xAB:
    case 0xAC: case 0xAD: case 0xAE: case 0xAF:
    case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6:
    case 0xB7: case 0xB8: case 0xB9: case 0xBA:
    case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5: case 0xC6:
    case 0xC7: case 0xC8: case 0xC9: case 0xCA:
    case 0xD1: case 0xD2: case 0xD3: case 0xD4: case 0xD5:
    case 0xF1: case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6:
    case 0xFE:
#if FW_CLKPROBE
    case 0xEE:
#endif
        return 1;
    default:
        return 0;
    }
}

/* PING is a bare 0xFE below fw_ver 15; from 15 it is FE <challenge> answered
 * with (~challenge) & 0xFF (LK_Device.py:250-256). CheckActive() sends it on a
 * timer, so a wrong answer takes down every operation rather than one. */
#if FW_VERSION < 15
#error "PING here answers a challenge; below fw_ver 15 the host sends a bare \
0xFE and this would consume the next opcode as its argument"
#endif

static uint8_t arg_len(uint8_t op)
{
    switch (op) {
    case CMD_PING:          return 1u;   /* challenge, answered complemented  */
    case CMD_FW_SET_STEP:   return 1u;
    case CMD_SET_PIN:       return 5u;   /* mask32 + high8                   */
    case CMD_DMG_CART_WRITE: return 5u;  /* addr32 + value8                  */
    case CMD_CLK_TOGGLE:    return 4u;   /* count32                          */
    case CMD_SET_FLASH_CMD: return 39u;  /* set+method+we + 6x(addr32,val16) */
    case CMD_CART_WRITE_FLASH_CMD: return 2u;  /* flashcart + count, then pairs */
    case CMD_DMG_FLASH_WRITE_BYTE: return 5u;  /* addr32 + value8            */
    case CMD_AGB_FLASH_WRITE_SHORT: return 6u; /* addr32 + value16           */
    case CMD_FW_SET_LATCH:  return 1u;
    case CMD_SET_VARIABLE:  return 9u;   /* size + key32 + value32           */
    case CMD_GET_VARIABLE:  return 5u;   /* size + key32                     */

    /* Counts from the LK call sites cited; too long desyncs as badly as too
     * short. */
    case CMD_AGB_CART_WRITE:            return 6u;  /* addr32+val16 LK:709  */
    case CMD_CALC_CRC32:                return 4u;  /* len32        LK:2271 */
    case CMD_AGB_CART_READ_EEPROM:      return 1u;  /* size sel     LK:3456 */
    case CMD_AGB_CART_WRITE_EEPROM:     return 1u;  /* size sel     LK:3456 */
    case CMD_AGB_CART_WRITE_FLASH_DATA: return 1u;  /* size sel     LK:3459 */
    case CMD_DMG_SET_BANK_CHANGE_CMD:   return 1u;  /* count        LK:4333 */
#if FW_CLKPROBE
    case 0xEEu:                         return 2u;
#endif

    default:                return 0u;
    }
}

/* Last completed command's payload byte count, for FW_DUMP_PAYLOAD. */
static uint16_t g_last_pay_got;

static uint32_t execute(fw_state_t *st, uint8_t *out)
{
    uint32_t i;
    /* Sampled and cleared: only the 3D read sets it again on the way out. */
    uint8_t terminator_due = st->page_terminator_due;

    st->page_terminator_due = 0u;

    switch (P.op) {

    case CMD_QUERY_FW_INFO:
        return fw_proto_fw_info(st, out);

    case CMD_PING:
        out[0] = (uint8_t)(~P.arg[0]);
        return 1u;

    /* No presence or mode switch exists on this board and neither capability
     * bit is advertised, so the host never asks. Stock answers a constant zero
     * and so does this, rather than leaving an opcode the host knows about
     * with no reply behind it. */
    case CMD_GET_SWITCH_STATE:
        out[0] = 0u;
        return 1u;

    case CMD_SET_FLASH_CMD: {
        /* Six (address, value) pairs to replay before each word. */
        uint32_t k;
        st->flash_command_set = P.arg[0];
        st->flash_method = P.arg[1];
        /* arg[2] is the live write-enable selector, not a second copy: LK.c:272
         * writes SET_VARIABLE's cell (LK.c:226) and LK.c:277 arms AUDIO now. */
        st->flash_we_pin = P.arg[2];
        st->flash_we_pin_var = P.arg[2];
        st->we_pin_requested = 1u;
        for (k = 0; k < 6u; k++) {
            const uint8_t *e = &P.arg[3u + k * 6u];
            st->flash_cmd_addr[k] = be32(e);
            st->flash_cmd_val[k] = (uint16_t)((e[4] << 8) | e[5]);
        }
        out[0] = ACK_OK;
        return 1u;
    }

    case CMD_DMG_FLASH_WRITE_BYTE:
    case CMD_AGB_CART_WRITE:
        /* _cart_write sends this whenever flashcart is not set (LK:709-713),
         * covering every AGB Mapper write. Same bus cycle and args as 0xD2. */
    case CMD_AGB_FLASH_WRITE_SHORT:
        st->flash_write_requested = 1u;
        st->flash_write_is_agb = (uint8_t)(P.op == CMD_AGB_FLASH_WRITE_SHORT
                                           || P.op == CMD_AGB_CART_WRITE);
        st->flash_write_addr = be32(&P.arg[0]);
        st->flash_write_val = st->flash_write_is_agb
            ? (uint16_t)((P.arg[4] << 8) | P.arg[5])
            : (uint16_t)P.arg[4];
        out[0] = ACK_OK;
        return 1u;

    case CMD_DMG_CART_READ:
        /* Signalled, not answered here: main.c emits transfer_size bytes. */
        st->read_requested = 1u;
        return 0u;

    case CMD_DMG_CART_WRITE:
        /* Mapper.py builds the [address, value] pairs; no mapper logic here. */
        st->write_requested = 1u;
        st->write_addr = be32(&P.arg[0]);
        st->write_value = P.arg[4];
        out[0] = ACK_OK;
        return 1u;

    case CMD_DMG_MBC_RESET:
        st->mbc_reset_requested = 1u;
        out[0] = ACK_OK;
        return 1u;

    case CMD_CLK_TOGGLE:
        /* N pulses on PHI/CLK; MBC3+RTC detection needs it during ReadHeader. */
        st->clk_requested = 1u;
        st->clk_pulses = be32(&P.arg[0]);
        out[0] = ACK_OK;
        return 1u;

    case CMD_AGB_CART_READ:
        /* Same contract as CMD_DMG_CART_READ: main.c emits the bytes. */
        st->read_requested = 1u;
        return 0u;

    case CMD_FW_SET_STEP:
        /* Argument 0 means stop overriding, not use 32. */
        st->cart_step_pin = (uint16_t)(P.arg[0] * 16u);
        out[0] = ACK_OK;
        return 1u;

    case CMD_FW_SET_LATCH:
        st->cart_latch = P.arg[0] ? (uint16_t)(P.arg[0] * 32u)
                                  : (uint16_t)FW_CART_LATCH;
        out[0] = ACK_OK;
        return 1u;

    case CMD_FW_BENCH_TX:
        st->bench_requested = 1u;
        return 0u;

    case CMD_BOOTLOADER_RESET:
        /* BootloaderReset() reads the ACK then closes the port; main.c pumps
         * USB 20 ms first. Do not restore the two-step arm-then-confirm: on
         * hardware it stopped firing, leaving reflashing as the only recovery. */
        st->reset_requested = 1u;
        out[0] = ACK_OK;
        return 1u;

    case CMD_GET_VARIABLE:
        return put_be32(out, get_variable(st, P.arg[0], be32(&P.arg[1])));

    case CMD_SET_VARIABLE:
        set_variable(st, P.arg[0], be32(&P.arg[1]), be32(&P.arg[5]));
        /* fw_ver >= 12: the host waits for this ACK. */
        out[0] = ACK_OK;
        return 1u;

    /* Every ACK here matters: the host blocks in wait_for_ack() at fw_ver >= 12
     * and three failures latch WRITE_DELAY, 1.4 ms after every write, on. */
    case CMD_SET_MODE_AGB:
        /* Drop to 3.3 V here, not on SET_VOLTAGE_3_3V: between the two the rail
         * still carries the previous mode's 5 V, and a host that dies there
         * leaves an AGB cartridge on 5 V. DMG does not raise the rail here. */
        st->mode = FW_MODE_AGB;
        st->cart_mode = 2u;
        st->voltage_requested = 1u;
        st->voltage_five = 0u;
        out[0] = ACK_OK;
        return 1u;

    case CMD_SET_MODE_DMG:
        st->mode = FW_MODE_DMG;
        st->cart_mode = 1u;
        out[0] = ACK_OK;
        return 1u;

    /* SetMode() sends the mode before the voltage, so st->mode is already
     * correct here and the DMG-only 5 V guard never refuses a real one. */
    case CMD_SET_VOLTAGE_3_3V:
    case CMD_SET_VOLTAGE_5V:
        st->voltage_requested = 1u;
        st->voltage_five = (uint8_t)(P.op == CMD_SET_VOLTAGE_5V);
        out[0] = ACK_OK;
        return 1u;

    case CMD_ENABLE_PULLUPS:
        st->pullups_enabled = 1u;
        st->pullup_requested = 1u;
        out[0] = ACK_OK;
        return 1u;

    case CMD_DISABLE_PULLUPS:
        st->pullups_enabled = 0u;
        st->pullup_requested = 1u;
        out[0] = ACK_OK;
        return 1u;

    case CMD_SET_ADDR_AS_INPUTS:
        /* Tri-state the bus; CartPowerOff() uses it when it cannot power-cycle. */
        st->tristate_requested = 1u;
        out[0] = ACK_OK;
        return 1u;

    case CMD_SET_PIN:
        /* 0xF5 <mask u32 BE> <high u8>. Bit 0 is CART_POWER, bits 1..30 are bus
         * lines (CLK, WR, RD, CS, A0..A23, CS2, AUDIO). Only bit 0 is honoured;
         * the rest would assert /WR with no address phase behind it. */
        st->setpin_mask = be32(&P.arg[0]);
        st->setpin_high = P.arg[4];
        if (st->setpin_mask & 0x1u) {
            st->power_requested = 1u;
            st->power_target = (uint8_t)(st->setpin_high != 0u);
        }
        out[0] = ACK_OK;
        return 1u;

    case CMD_AGB_BOOTUP_SEQUENCE:
        /* AGB only: the handshake is a sequence of AGB ROM reads, and on DMG
         * the host sends DMG_MBC_RESET instead (LK_Device.py:889-894). */
        if (st->mode == FW_MODE_AGB) {
            st->bootup_requested = 1u;
        }
        out[0] = ACK_OK;
        return 1u;

    case CMD_AGB_READ_GPIO_RTC:
        /* Always eight bytes, the host reads 8 either way, or AGB_GPIO.HasRTC
         * (Mapper.py:1863) evaluates False[1:] and raises out of ReadHeader.
         * Outside AGB mode 0x80 short-circuits HasRTC at (status >> 7) == 1. */
        if (st->mode == FW_MODE_AGB) {
            st->rtc_read_requested = 1u;
        } else {
            out[0] = 0x80u;
            for (i = 1u; i < 8u; i++) {
                out[i] = 0u;
            }
        }
        return 8u;

    case CMD_CART_PWR_ON:
    case CMD_CART_PWR_OFF:
        /* main.c sets cart_powered once the rail settles, so QUERY_CART_PWR
         * reports the hardware and not the request. */
        st->power_requested = 1u;
        st->power_target = (uint8_t)(P.op == CMD_CART_PWR_ON);
        out[0] = ACK_OK;
        return 1u;

    case CMD_QUERY_CART_PWR:
        out[0] = st->cart_powered;
        return 1u;

    case CMD_RESYNC:
        if (terminator_due) {
            /* 3D Memory page terminator: close the window and step to the next
             * page. Nothing is waiting to read a reply; see proto.h. */
            st->page_end_requested = 1u;
            return 0u;
        }
        /* fall through */
    case CMD_NULL:
        
        out[0] = ACK_OK;
        return 1u;

    case CMD_CALC_CRC32:
        /* The host does _read(4) then wait_for_ack() (LK_Device.py:2270-2276);
         * main.c answers the four CRC bytes plus the ACK, so nothing here. */
        st->crc_len = be32(&P.arg[0]);
        st->crc_requested = 1u;
        return 0u;

    case CMD_AGB_CART_READ_3D_MEMORY:
        /* Opcode alone, then the host reads TRANSFER_SIZE bytes (LK:1707). */
        st->read_requested = 1u;
        st->read_is_3d = 1u;
        st->page_terminator_due = 1u;
        return 0u;

    case CMD_AGB_CART_READ_SRAM:
        /* Same contract as AGB_CART_READ. */
        st->read_requested = 1u;
        st->read_is_save = 1u;
        return 0u;

    case CMD_AGB_CART_READ_EEPROM:
        /* arg[0] is the size selector picked from the save type (LK:3456). */
        st->eeprom_sel = P.arg[0];
        st->read_requested = 1u;
        st->read_is_eeprom = 1u;
        return 0u;

    case CMD_DMG_SET_BANK_CHANGE_CMD:
        /* Reached only at count 0, which is how the host clears the sequence
         * (LK:4353); it still waits for an ACK. */
        st->dmg_bank_cmd_count = 0u;
        out[0] = ACK_OK;
        return 1u;

    case CMD_FW_DUMP_PAYLOAD: {
        /* Takes no payload: TRANSFER_SIZE bytes would overwrite the buffer it
         * shows. P.pay survives parser_reset(). */
        uint32_t k;
        uint32_t n = st->transfer_size;
        if (n > (uint32_t)sizeof(P.pay)) {
            n = (uint32_t)sizeof(P.pay);
        }
        out[0] = (uint8_t)(g_last_pay_got >> 8);
        out[1] = (uint8_t)g_last_pay_got;
        for (k = 0; k < n; k++) {
            out[2u + k] = P.pay[k];
        }
        return n + 2u;
    }

#if FW_CLKPROBE
    case 0xEEu: {
        uint32_t cfg = ((uint32_t)P.arg[0] << 8) | (uint32_t)P.arg[1];
        uint16_t back;
        if (cfg != 0xFFFFu) {
            __asm__ volatile (
                "cpsid i\n\t"
                "movs r1, #0x57\n\t"
                "movs r2, #0xA8\n\t"
                "strb r1, [%1]\n\t"
                "strb r2, [%1]\n\t"
                "strh %0, [%2]\n\t"
                "movs r1, #0\n\t"
                "strb r1, [%1]\n\t"
                "cpsie i\n\t"
                : : "r"(cfg), "r"(0x40001040u), "r"(0x40001008u)
                : "r1", "r2", "memory");
        }
        back = *(volatile uint16_t *)0x40001008u;
        out[0] = (uint8_t)(back >> 8);
        out[1] = (uint8_t)back;
        out[2] = *(volatile uint8_t *)0x4000100Au;   /* R8_HFCK_PWR_CTRL */
        return 3u;
    }
#endif

    case CMD_DEBUG:
        /* Twenty CLK pulses for a scope; stock 0x9072 loops PB18 20 times. */
        st->debug_requested = 1u;
        out[0] = ACK_OK;
        return 1u;

    case CMD_GET_VAR_STATE:
        return get_var_state(st, out);

    default:
        /* The reply waits until the whole command is consumed: WriteRAM writes
         * opcode then buffer with nothing between (LK_Device.py:1834), so an
         * early reset feeds save data in as opcodes, 0xD2 and 0xA5 among it. */
        return known_opcode(P.op) ? (out[0] = ACK_ERROR, 1u) : 0u;
    }
}

/* Every FLASH_PROGRAM block passes here, finished or not. Resetting from
 * main.c's pump instead wraps its loop condition on an abandoned block. */
static void payload_opened(fw_state_t *st)
{
    if (P.op == CMD_FLASH_PROGRAM) {
        st->program_done = 0u;
        st->program_stream_err = 0u;
    }
}

static uint16_t payload_len(const fw_state_t *st)
{
    switch (P.op) {
    case CMD_CART_WRITE_FLASH_CMD:
        /* arg[0] = flashcart flag, arg[1] = pair count, then 6 bytes each. */
        return (uint16_t)(P.arg[1] * 6u);
    case CMD_FLASH_PROGRAM:
    case CMD_FW_ECHO_PAYLOAD:
        /* No length on the wire; the host sends exactly TRANSFER_SIZE bytes. */
        return st->transfer_size;

    /* WriteRAM's shape (LK_Device.py:1834-1836): opcode alone, then
     * TRANSFER_SIZE raw bytes, then one reply, nothing between the two writes. */
    case CMD_DMG_CART_WRITE_SRAM:           /* LK:692, 1829, 1834           */
    case CMD_AGB_CART_WRITE_SRAM:           /* LK:703, 1832, 1834           */
    case CMD_DMG_MBC7_WRITE_EEPROM:         /* LK:1927-1928                 */
    case CMD_DMG_MBC6_MMSA_WRITE_FLASH:     /* LK:1874-1875                 */
    case CMD_DMG_EEPROM_WRITE:              /* LK:2215-2216                 */
    case CMD_AGB_CART_WRITE_EEPROM:         /* LK:3456 + WriteRAM           */
        return st->transfer_size;

    case CMD_AGB_CART_WRITE_FLASH_DATA:     /* LK:3459 + WriteRAM           */
        return st->transfer_size;

    case CMD_DMG_SET_BANK_CHANGE_CMD:
        /* count, then count x (u32 value-or-address + u8 type), LK:4333-4348.
         * count == 0 is common (LK:4353) and yields no payload at all. */
        return (uint16_t)(P.arg[0] * 5u);

    case CMD_SET_VAR_STATE:
        return (uint16_t)FW_VAR_STATE_LEN;

    default:
        return 0u;
    }
}

static uint32_t payload_done(fw_state_t *st, uint8_t *out)
{
    g_last_pay_got = P.pay_got;

    uint32_t i;

    switch (P.op) {
    case CMD_CART_WRITE_FLASH_CMD: {
        uint32_t n = P.arg[1];
        if (n > FW_BATCH_MAX) {
            /* Refuse rather than apply the first FW_BATCH_MAX: a partly applied
             * unlock leaves the chip undefined and the host programs it. */
            out[0] = ACK_ERROR;
            return 1u;
        }
        for (i = 0; i < n; i++) {
            const uint8_t *e = &P.pay[i * 6u];
            st->batch_addr[i] = be32(e);
            st->batch_val[i] = (uint16_t)((e[4] << 8) | e[5]);
        }
        st->batch_count = (uint8_t)n;
        st->batch_flashcart = P.arg[0];
        st->batch_requested = 1u;
        out[0] = ACK_OK;
        return 1u;
    }

    case CMD_FLASH_PROGRAM:
        /* Only FLASH_METHOD 1 (unbuffered) and 2 (buffered) are implemented;
         * 3,4,5,8,9,0x0A,0x0B,0x0C are separate protocols. Falling through to
         * the single-write replay would write bare data and report success. */
        if (st->flash_method > 2u) {
            out[0] = ACK_ERROR;
            return 1u;
        }
        st->program_len = P.pay_got;
        st->program_requested = 1u;
        /* 0x01, not 0x03: 0x03 means stay in program mode and send the next
         * block bare. 0x01 keeps every block prefixed by its opcode. */
        out[0] = ACK_OK;
        return 1u;

    case CMD_AGB_CART_WRITE_EEPROM:
        /* One selector byte, then a TRANSFER_SIZE payload (LK:3456-3457). */
        st->save_write_eeprom = P.arg[0] ? P.arg[0] : 1u;
        st->save_write_flash = 0u;
        st->save_write_is_dmg = 0u;
        st->save_write_len = P.pay_got;
        st->save_write_requested = 1u;
        out[0] = ACK_OK;
        return 1u;

    case CMD_AGB_CART_WRITE_FLASH_DATA:
        /* arg[0] is the method chosen from the save chip ID (LK:3459). */
        st->save_write_flash = P.arg[0];
        /* Nothing else clears this and main.c tests it first: left set from an
         * earlier EEPROM restore, a flash-save restore writes serial frames. */
        st->save_write_eeprom = 0u;
        st->save_write_len = P.pay_got;
        st->save_write_is_dmg = 0u;
        st->save_write_requested = 1u;
        out[0] = ACK_OK;
        return 1u;

    case CMD_AGB_CART_WRITE_SRAM:
    case CMD_DMG_CART_WRITE_SRAM:
        st->save_write_flash = 0u;
        st->save_write_eeprom = 0u;
        st->save_write_len = P.pay_got;
        st->save_write_is_dmg = (uint8_t)(P.op == CMD_DMG_CART_WRITE_SRAM);
        st->save_write_requested = 1u;
        /* 0x01, not 0x03, for the reason FLASH_PROGRAM answers 0x01. */
        out[0] = ACK_OK;
        return 1u;

    case CMD_DMG_SET_BANK_CHANGE_CMD: {
        /* count x (u32 value-or-address, u8 type), LK:4335-4348. type 0 = an
         * address to write the bank number to, type 1 = a literal value. */
        uint32_t n = P.arg[0];
        uint32_t k;
        if (n > FW_BANK_CMD_MAX) {
            out[0] = ACK_ERROR;         
            return 1u;
        }
        for (k = 0; k < n; k++) {
            st->dmg_bank_cmd_val[k] = be32(&P.pay[k * 5u]);
            st->dmg_bank_cmd_type[k] = P.pay[k * 5u + 4u];
        }
        st->dmg_bank_cmd_count = (uint8_t)n;
        out[0] = ACK_OK;
        return 1u;
    }

    case CMD_FW_ECHO_PAYLOAD: {
        /* FNV-1a over the payload, four bytes big-endian. The reply is one
         * packet, so a mismatch is on the receive side. */
        uint32_t k;
        uint32_t sum = 0x811C9DC5u;
        for (k = 0; k < P.pay_got; k++) {
            sum = (sum ^ P.pay[k]) * 16777619u;
        }
        out[0] = (uint8_t)(sum >> 24);
        out[1] = (uint8_t)(sum >> 16);
        out[2] = (uint8_t)(sum >> 8);
        out[3] = (uint8_t)sum;
        return 4u;
    }

    case CMD_SET_VAR_STATE:
        /* From fw_ver 15 SetVarState() waits for an ack (LK_Device.py:934).
         * The restore itself cannot fail in a way the host can act on: an
         * unknown blob version leaves the state alone and still acks, because
         * the bytes are already off the wire and the stream is in sync. */
        (void)set_var_state(st, P.pay);
        out[0] = ACK_OK;
        return 1u;

    default:
        /* The error byte is owed only once the payload is off the wire. */
        return known_opcode(P.op) ? (out[0] = ACK_ERROR, 1u) : 0u;
    }
}

/* A closed port does not reach this layer: without the timeout the next
 * session's handshake bytes append to a stale payload and are programmed onto
 * the cartridge. Floor 200 ms, SetVarState sleeps that between opcode and
 * payload (LK:930). */
void fw_proto_tick(fw_state_t *st, uint32_t now_ms)
{
    static uint32_t seen;
    static uint32_t since;

    (void)st;

    if (P.op == 0u && P.pay_need == 0u) {
        since = now_ms;
        seen = P.bytes;
        return;
    }
    if (P.bytes != seen) {
        seen = P.bytes;
        since = now_ms;
        return;
    }
    if ((uint32_t)(now_ms - since) >= FW_PARSER_TIMEOUT_MS) {
        parser_reset();
        since = now_ms;
    }
}

/* A USB bus reset means the host let go; the half-sent command is gone. */
void fw_proto_link_reset(fw_state_t *st)
{
    (void)st;
    parser_reset();
}

int fw_proto_idle(void)
{
    /* pay_need as well as op: the payload phase leaves op set. */
    return (P.op == 0u) && (P.pay_need == 0u);
}

/* Lets fw_main() stay in a tight receive loop between USB packets. */
uint32_t fw_proto_payload_remaining(void)
{
    if (P.pay_need == 0u || P.pay_got >= P.pay_need) {
        return 0u;
    }
    return (uint32_t)P.pay_need - (uint32_t)P.pay_got;
}

/* The opcode test keeps a save restore out of ROM data, pay_need resets the
 * caller's counter between commands, !pay_over keeps dropped bytes off. */
uint32_t fw_proto_payload_filled(void)
{
    if (P.op != CMD_FLASH_PROGRAM || P.pay_need == 0u || P.pay_over) {
        return 0u;
    }
    return (uint32_t)P.pay_got;
}

/* Refuses unless a payload is already open, so every byte that could be an
 * opcode still goes through fw_proto_feed(). */
uint32_t fw_proto_feed_bulk(fw_state_t *st, const uint8_t *buf, uint32_t len,
                            uint8_t *out, uint32_t *consumed)
{
    uint32_t want;
    uint32_t fits;

    *consumed = 0u;
    if (P.pay_need == 0u || len == 0u) {
        return 0u;
    }

    want = (uint32_t)P.pay_need - (uint32_t)P.pay_got;
    if (want > len) {
        want = len;
    }

    /* Excess beyond pay[] is consumed and dropped, as in the byte path. */
    fits = 0u;
    if (P.pay_got < (uint16_t)sizeof(P.pay)) {
        fits = (uint32_t)sizeof(P.pay) - (uint32_t)P.pay_got;
        if (fits > want) {
            fits = want;
        }
#if FW_PROTO_FAST_COPY
        /* buf starts at an opcode-dependent offset in the receive batch, so the
         * unaligned tail below is reachable. */
        {
            uint8_t *dst = &P.pay[P.pay_got];
            const uint8_t *src = buf;
            uint32_t k = fits;

            if ((((uintptr_t)dst | (uintptr_t)src) & 3u) == 0u) {
                uint32_t *d4 = (uint32_t *)(void *)dst;
                const uint32_t *s4 = (const uint32_t *)(const void *)src;
                uint32_t w = k >> 2;

                k &= 3u;
                while (w-- != 0u) {
                    *d4++ = *s4++;
                }
                dst = (uint8_t *)(void *)d4;
                src = (const uint8_t *)(const void *)s4;
            }
            while (k-- != 0u) {
                *dst++ = *src++;
            }
        }
#else
        {
            uint32_t k;
            for (k = 0; k < fits; k++) {
                P.pay[P.pay_got + k] = buf[k];
            }
        }
#endif
    }
    P.pay_got = (uint16_t)(P.pay_got + want);
    P.bytes += want;
    *consumed = want;

    if (P.pay_got >= P.pay_need) {
        uint32_t n;
        if (P.pay_over) {
            out[0] = ACK_ERROR;
            n = 1u;
        } else {
            n = payload_done(st, out);
        }
        parser_reset();
        return n;
    }
    return 0u;
}

uint32_t fw_proto_feed(fw_state_t *st, uint8_t b, uint8_t *out)
{
    P.bytes++;

    if (P.pay_need != 0u) {
        /* Every declared byte is consumed whether or not it fits. Truncating
         * pay_need to the buffer size leaves the rest parsed as opcodes; for
         * FLASH_PROGRAM that is ROM data, 0xB2 / 0xD1 / 0xD2 among it. */
        if (P.pay_got < (uint16_t)sizeof(P.pay)) {
            P.pay[P.pay_got] = b;
        }
        P.pay_got++;
        if (P.pay_got >= P.pay_need) {
            uint32_t n;
            if (P.pay_over) {
                out[0] = ACK_ERROR;
                n = 1u;
            } else {
                n = payload_done(st, out);
            }
            parser_reset();
            return n;
        }
        return 0u;
    }

    if (P.op == 0u) {
        P.op = b;
        P.need = arg_len(b);
        P.got = 0;
        if (P.need == 0u) {
            uint32_t n;
            P.pay_need = payload_len(st);
            if (P.pay_need != 0u) {
                P.pay_over = (uint8_t)(P.pay_need > (uint16_t)sizeof(P.pay));
                payload_opened(st);
                return 0u;
            }
            n = execute(st, out);
            parser_reset();
            return n;
        }
        return 0u;
    }

    /* arg_len() never returns more than FW_CMD_BUF_LEN; past that, host bytes
     * would run off P.arg into the rest of P. */
    if (P.got < (uint8_t)sizeof(P.arg)) {
        P.arg[P.got] = b;
    }
    P.got++;

    if (P.got >= P.need) {
        uint32_t n;
        P.pay_need = payload_len(st);
        if (P.pay_need != 0u) {
            P.pay_over = (uint8_t)(P.pay_need > (uint16_t)sizeof(P.pay));
            payload_opened(st);
            return 0u;
        }
        n = execute(st, out);
        parser_reset();
        return n;
    }
    return 0u;
}
