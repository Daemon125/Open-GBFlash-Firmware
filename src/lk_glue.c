/* The seam; include/lk_glue.h documents each side. The mirror is
 * one-way into LK's _lk_var*, plus a read-back of the three cells LK advances.
 * Framing stays with our parser, so payload bytes never read as opcodes. */

#include <stdint.h>

#include "fw_config.h"
#include "proto.h"
#include "cart.h"
#include "usb.h"
#include "timebase.h"
#include "lk_glue.h"

/* LK.h pulls in the board header via -DLK_DEVICE_HEADER. */
#include "LK.h"

static fw_state_t *g_st;

void fw_lk_init(fw_state_t *st)
{
    g_st = st;
}

/* Bytes already pulled off USB that LK has not consumed, drained before
 * bl_usb_rx(). Points into the caller's buffer, valid for one dispatch. */
static const uint8_t *g_pend;
static uint32_t g_pend_len;
static uint32_t g_pend_used;

/* Header bytes lk_admit() read ahead of lk_loop(), handed back at the head of
 * the receive path. Sized to CART_WRITE_FLASH_CMD's two; a third is a build error. */
static uint8_t  g_push[2];
static uint8_t  g_push_len;
static uint8_t  g_push_used;

static uint8_t g_recv_short;
static uint8_t g_send_stall;

int fw_lk_recv_truncated(void) { return g_recv_short != 0u; }
int fw_lk_send_stalled(void)   { return g_send_stall != 0u; }

/* Deadline enforced here: lk_conn_recv() is void and the main-loop watchdog
 * cannot run inside an LK command. Polling answers bus resets (usb.h:1-6). */
void fw_lk_conn_recv(uint8_t *data, uint16_t count)
{
    uint32_t want = count;
    uint32_t got = 0u;
    uint32_t deadline;

    while ((got < want) && (g_push_used < g_push_len)) {
        data[got++] = g_push[g_push_used++];
    }
    while ((got < want) && (g_pend_used < g_pend_len)) {
        data[got++] = g_pend[g_pend_used++];
    }
    if (got >= want) {
        return;
    }

    deadline = bl_time_ms() + FW_PARSER_TIMEOUT_MS;
    while (got < want) {
        uint32_t k;
        bl_usb_poll();
        k = bl_usb_rx(&data[got], want - got);
        if (k != 0u) {
            got += k;
            deadline = bl_time_ms() + FW_PARSER_TIMEOUT_MS;
        } else if ((int32_t)(bl_time_ms() - deadline) >= 0) {
            break;
        }
    }

    if (got < want) {
        /* Do not hand LK the previous command's bytes. */
        g_recv_short = 1u;
        while (got < want) {
            data[got++] = FW_LK_RECV_FILL;
        }
    }
}

/* Unguarded: sets g_send_stall, never reads it. The guards are in
 * fw_lk_conn_send() and fw_lk_dispatch(), so a refusal still goes out. */
static void conn_send_raw(const uint8_t *data, uint32_t n)
{
    uint32_t sent = 0u;
    uint32_t deadline;

    deadline = bl_time_ms() + FW_TX_STALL_MS;
    while (sent < n) {
        uint32_t k;
        bl_usb_poll();
        k = bl_usb_tx(&data[sent], n - sent);
        if (k != 0u) {
            sent += k;
            deadline = bl_time_ms() + FW_TX_STALL_MS;
        } else if ((int32_t)(bl_time_ms() - deadline) >= 0) {
            g_send_stall = 1u;
            return;
        }
    }
}

/* LK.c has no short-read branch: a command whose input never arrived reports
 * success, and the fill bytes still reach the cartridge below this seam. */
void fw_lk_conn_send(const uint8_t *data, uint16_t count)
{
    if (g_recv_short || g_send_stall) {
        return;
    }
    conn_send_raw(data, count);
}

void fw_lk_conn_send_byte(uint8_t data)
{
    fw_lk_conn_send(&data, 1u);
}

/* usb.h:1-6 bars ~10 ms without bl_usb_poll(), and LK.c:905/:919 delay 150 and
 * 100 ms. bl_usb_poll() never enters lk_loop(). */
void fw_lk_delay_ms(uint32_t ms)
{
    uint32_t deadline = bl_time_ms() + ms;

    while ((int32_t)(bl_time_ms() - deadline) < 0) {
        bl_usb_poll();
    }
}

/* 5 V at a 3.3 V AGB cartridge is irreversible. LK.c:190-194 force-sets
 * LK_VAR8_CART_MODE to DMG one line before SET_VOLTAGE_5V(), so the guard has to
 * read st->mode, set by the parser on 0xA3 (proto.c:556-557) and never by LK. */
void fw_lk_voltage_5v(void)
{
    int dmg = (g_st != 0) && (g_st->mode == FW_MODE_DMG);
    int applied = fw_cart_voltage(1, dmg);

    if (g_st != 0) {
        g_st->voltage_is_five = (uint8_t)applied;
    }
}

/* Runtime probe here; upstream LK_PCB_VERSION is a literal. */
uint8_t fw_lk_pcb_ver(void)
{
    return (g_st != 0) ? g_st->pcb_ver : 0u;
}

/* PB12 is active low (re/led-design.md:21). LK leaves an idle device dark, but
 * stage-2 acceptance needs it parked lit (re/led-design.md:31-33). */
void fw_lk_led_idle(void)
{
    /* Through LKDEV_REG32, not a raw store: host/test_lk_glue.py compiles this. */
    LKDEV_PB_CLR = PB_LED;
}

/* DMG-mode gate. Do not route 0xD3 (LK.c:595-616's CONN_RECV is blocking and
 * forecloses agb_stream_pump()) or 0xC8 (LK.c:1762-1763 eats the ack byte
 * proto.h:15-17's page terminator needs). 0xD4's AGB arm (LK.c:520-534)
 * displaces the fw_cart_agb_sram_open/close path the 1M FLASH save-bank fix
 * needs. */
int fw_lk_routes(const fw_state_t *st, uint8_t op)
{
#if FW_LK_ROUTE
    if (st->mode != FW_MODE_DMG) {
        return 0;
    }
    switch (op) {
#if FW_LK_ROUTE_DMG_READ
    /* Routing 0xB1 halves read throughput: lk_dmg_cart_read_data() sends through
     * conn_send_raw(), not the direct region. */
    case CMD_DMG_CART_READ:                 /* 0xB1 */
#endif
    case CMD_DMG_CART_WRITE:                /* 0xB2 */
    case CMD_DMG_CART_WRITE_SRAM:           /* 0xB3 */
    case CMD_DMG_MBC_RESET:                 /* 0xB4 */
    case CMD_DMG_MBC7_READ_EEPROM:          /* 0xB5 */
    case CMD_DMG_MBC7_WRITE_EEPROM:         /* 0xB6 */
    case CMD_DMG_MBC6_MMSA_WRITE_FLASH:     /* 0xB7 */
    case CMD_DMG_EEPROM_WRITE:              /* 0xB9 */
    case CMD_DMG_FLASH_WRITE_BYTE:          /* 0xD1 */
    case CMD_CART_WRITE_FLASH_CMD:          /* 0xD4 */
        return 1;
    default:
        return 0;
    }
#else
    (void)st;
    (void)op;
    return 0;
#endif
}

static void mirror_to_lk(const fw_state_t *st)
{
    uint32_t i;

    _lk_var32[LK_VAR32_ADDRESS] = st->address;

    _lk_var16[LK_VAR16_TRANSFER_SIZE]        = st->transfer_size;
    _lk_var16[LK_VAR16_BUFFER_SIZE]          = st->buffer_size;
    _lk_var16[LK_VAR16_DMG_ROM_BANK]         = st->dmg_rom_bank;
    _lk_var16[LK_VAR16_STATUS_REGISTER]      = st->status_register;
    _lk_var16[LK_VAR16_LAST_BANK_ACCESSED]   = st->last_bank_accessed;
    _lk_var16[LK_VAR16_STATUS_REGISTER_MASK] = st->status_reg_mask;
    _lk_var16[LK_VAR16_TRANSFER_SIZE]         = st->transfer_size;
    _lk_var16[LK_VAR16_BUFFER_SIZE]           = st->buffer_size;
    _lk_var16[LK_VAR16_DMG_ROM_BANK]          = st->dmg_rom_bank;
    _lk_var16[LK_VAR16_STATUS_REGISTER]       = st->status_register;
    _lk_var16[LK_VAR16_LAST_BANK_ACCESSED]    = st->last_bank_accessed;
    _lk_var16[LK_VAR16_STATUS_REGISTER_MASK]  = st->status_reg_mask;
    _lk_var16[LK_VAR16_STATUS_REGISTER_VALUE] = st->status_reg_value;

    /* Written out, not cast: our fw_mode_t is 0/1/2 and LK's is 1/2. */
    _lk_var8[LK_VAR8_CART_MODE] =
        (st->mode == FW_MODE_DMG) ? LK_MODE_DMG :
        (st->mode == FW_MODE_AGB) ? LK_MODE_AGB : 0u;

    _lk_var8[LK_VAR8_DMG_ACCESS_MODE]       = st->dmg_access_mode;
    _lk_var8[LK_VAR8_FLASH_COMMAND_SET]     = st->flash_command_set;
    _lk_var8[LK_VAR8_FLASH_METHOD]          = st->flash_method;
    /* flash_we_pin_var, not flash_we_pin (proto.c:452-453): the other one puts
     * LK's DMG flash writes on whatever strobe the last SET_FLASH_CMD chose. */
    _lk_var8[LK_VAR8_FLASH_WE_PIN]          = st->flash_we_pin_var;
    _lk_var8[LK_VAR8_FLASH_PULSE_RESET]     = st->flash_pulse_reset;
    _lk_var8[LK_VAR8_FLASH_COMMANDS_BANK_1] = st->flash_commands_bank_1;
    _lk_var8[LK_VAR8_FLASH_SHARP_VERIFY_SR] = st->flash_sharp_verify_sr;
    _lk_var8[LK_VAR8_DMG_READ_CS_PULSE]     = st->dmg_read_cs_pulse;
    _lk_var8[LK_VAR8_DMG_WRITE_CS_PULSE]    = st->dmg_write_cs_pulse;
    _lk_var8[LK_VAR8_FLASH_DOUBLE_DIE]      = st->flash_double_die;
    _lk_var8[LK_VAR8_DMG_READ_METHOD]       = st->dmg_read_method;
    _lk_var8[LK_VAR8_AGB_READ_METHOD]       = st->agb_read_method;
    _lk_var8[LK_VAR8_CART_POWERED]          = st->cart_powered;
    _lk_var8[LK_VAR8_PULLUPS_ENABLED]       = st->pullups_enabled;
    _lk_var8[LK_VAR8_AUTO_POWEROFF_ENABLED] = 0u;
    _lk_var8[LK_VAR8_AGB_IRQ_ENABLED]       = st->agb_irq_enabled;
    _lk_var8[LK_VAR8_DMG_AUDIO_ENABLED]     = st->dmg_audio_enabled;

    /* No reader on the routed path; kept to match LK.c:278-281. */
    for (i = 0; i < 6u; i++) {
        _lk_flashcmd_addr[i] = st->flash_cmd_addr[i];
        _lk_flashcmd_data[i] = st->flash_cmd_val[i];
    }
    for (i = 0; i < 3u; i++) {
        flash_write_cycle[i][0] = (uint16_t)st->flash_cmd_addr[i];
        flash_write_cycle[i][1] = st->flash_cmd_val[i];
    }

    /* LK declares _lk_bankcmd_addr[3] (LK.h:177-179) and FW_BANK_CMD_MAX is 8, so
     * an unclamped 4 writes off the end. Over 3, the host gets the first 3. */
    {
        uint8_t n = st->dmg_bank_cmd_count;
        if (n > 3u) {
            n = 3u;
        }
        _lk_bankcmd_num = n;
        for (i = 0; i < 3u; i++) {
            _lk_bankcmd_addr[i] = (i < n) ? st->dmg_bank_cmd_val[i] : 0u;
            _lk_bankcmd_mode[i] = (i < n) ? st->dmg_bank_cmd_type[i] : 0u;
        }
    }
}

/* LK_VAR32_ADDRESS must come back: every routed byte-mover auto-advances it
 * (0xB1 LK.c:1137, 0xB3 :361, 0xB5 :1172, 0xB6 :1184, 0xB7 :985, 0xB9 :1057) and
 * the host sets it once then reads many blocks. The other two are a no-op. */
static void mirror_from_lk(fw_state_t *st)
{
    st->address           = _lk_var32[LK_VAR32_ADDRESS];
    st->address            = _lk_var32[LK_VAR32_ADDRESS];
    st->last_bank_accessed = _lk_var16[LK_VAR16_LAST_BANK_ACCESSED];
    st->status_register    = _lk_var16[LK_VAR16_STATUS_REGISTER];
    st->status_register   = _lk_var16[LK_VAR16_STATUS_REGISTER];
}

/* LK.c:541-545 indexes _lk_flashcmd_addr[x+16] / _lk_flashcmd_data[x+16], both
 * 32 elements (LK.h:174-175), off an unbounded host count: num=17 lands on
 * _lk_var8[CART_MODE..FLASH_METHOD] (LK.h:150-153), num=255 past __bss_end.
 * FW_LK_BATCH_BASE is LK.c:543-544's offset. */
#define FW_LK_FLASHCMD_ADDR_N (sizeof(_lk_flashcmd_addr) / sizeof(_lk_flashcmd_addr[0]))
#define FW_LK_FLASHCMD_DATA_N (sizeof(_lk_flashcmd_data) / sizeof(_lk_flashcmd_data[0]))
#define FW_LK_FLASHCMD_N      ((FW_LK_FLASHCMD_ADDR_N < FW_LK_FLASHCMD_DATA_N) \
                               ? FW_LK_FLASHCMD_ADDR_N : FW_LK_FLASHCMD_DATA_N)
#define FW_LK_BATCH_BASE      16u
#define FW_LK_BATCH_LKMAX     (FW_LK_FLASHCMD_N - FW_LK_BATCH_BASE)
#define FW_LK_BATCH_ADMIT     ((FW_LK_BATCH_LKMAX < (uint32_t)FW_BATCH_MAX) \
                               ? FW_LK_BATCH_LKMAX : (uint32_t)FW_BATCH_MAX)

/* CART_WRITE_FLASH_CMD's header: the flashcart selector and the pair count. */
#define FW_LK_BATCH_HDR       2u

/* 0xB3 LK.c:358, 0xB6 :401, 0xB7 :370, 0xB9 :380 recv TRANSFER_SIZE into
 * data_buffer unchecked; TRANSFER_SIZE clamps to 0x4000, data_buffer is 0x1000,
 * and LK_VAR8_CART_MODE is 4308 bytes past its end. */
#define FW_LK_PAYLOAD_ADMIT   ((uint32_t)sizeof(data_buffer))

/* fw_lk_batch_sane keeps FW_LK_BATCH_LKMAX's subtraction from wrapping. */
typedef char fw_lk_push_fits[(sizeof(g_push) >= FW_LK_BATCH_HDR) ? 1 : -1];
typedef char fw_lk_batch_sane[(FW_LK_FLASHCMD_N > FW_LK_BATCH_BASE) ? 1 : -1];

/* `drain` is what the host declared still follows and has to be consumed: left
 * on the wire, the parser's next byte is a flash address. Then one ACK_ERROR. */
static void lk_refuse(uint32_t drain)
{
    uint8_t scratch[16];
    uint8_t err = ACK_ERROR;

    /* Bounded by the receive path giving up, not by the count. */
    while ((drain != 0u) && !g_recv_short) {
        uint32_t k = (drain > sizeof scratch) ? (uint32_t)sizeof scratch : drain;
        fw_lk_conn_recv(scratch, (uint16_t)k);
        drain -= k;
    }

    /* conn_send_raw, not fw_lk_conn_send: that one goes quiet on g_recv_short. */
    conn_send_raw(&err, 1u);
}

static int lk_admit(const fw_state_t *st, uint8_t op)
{
    uint8_t hdr[FW_LK_BATCH_HDR];

    switch (op) {
    case CMD_DMG_CART_WRITE_SRAM:       /* 0xB3 */
    case CMD_DMG_MBC7_WRITE_EEPROM:     /* 0xB6 */
    case CMD_DMG_MBC6_MMSA_WRITE_FLASH: /* 0xB7 */
    case CMD_DMG_EEPROM_WRITE:          /* 0xB9 */
        /* Refuse rather than truncate: a short write to a save chip is a
         * corrupted save. The drain keeps the stream framed. */
        if ((uint32_t)st->transfer_size > FW_LK_PAYLOAD_ADMIT) {
            lk_refuse((uint32_t)st->transfer_size);
            return 0;
        }
        return 1;
    default:
        break;
    }

    if (op != CMD_CART_WRITE_FLASH_CMD) {
        return 1;
    }

    fw_lk_conn_recv(hdr, (uint16_t)FW_LK_BATCH_HDR);

    if (!g_recv_short && ((uint32_t)hdr[1] <= FW_LK_BATCH_ADMIT)) {
        g_push[0]   = hdr[0];
        g_push[1]   = hdr[1];
        g_push_len  = (uint8_t)FW_LK_BATCH_HDR;
        g_push_used = 0u;
        return 1;
    }

    /* A truncated header leaves hdr[1] at FW_LK_RECV_FILL, over the bound, so a
     * host that died mid-header takes this path too. */
    lk_refuse((uint32_t)hdr[1] * 6u);
    return 0;
}

uint32_t fw_lk_dispatch(fw_state_t *st, uint8_t op,
                        const uint8_t *tail, uint32_t tail_len)
{
    uint32_t used;

    g_pend      = tail;
    g_pend_len  = tail_len;
    g_pend_used = 0u;
    g_push_len  = 0u;
    g_push_used = 0u;
    g_recv_short = 0u;
    g_send_stall = 0u;

    /* Before the settle: a refused command drives no cartridge pin. */
    if (!lk_admit(st, op)) {
        used = g_pend_used;
        g_pend      = 0;
        g_pend_len  = 0u;
        g_pend_used = 0u;
        return used;
    }

    /* Deferred half of the power-on settle: only FW_CART_POWERON_ACK_MS of
     * FW_CART_POWERON_MS is spent in the CART_PWR_ON handler (cart.h:45-49). */
    fw_cart_wait_ready();

    mirror_to_lk(st);
    lk_loop(op);
    mirror_from_lk(st);

    /* LK answers LK_STATUS_OK even when handed FW_LK_RECV_FILL (LK.c:355-362);
     * this substitutes the ACK_ERROR. Skipped after a stall. */
    if (fw_lk_recv_truncated() && !fw_lk_send_stalled()) {
        uint8_t err = ACK_ERROR;
        conn_send_raw(&err, 1u);
    }

    /* LK turns the LED off at the bottom of lk_loop(). */
    fw_lk_led_idle();

    used = g_pend_used;
    g_pend = 0;
    g_pend_len = 0u;
    g_pend_used = 0u;
    return used;
}
