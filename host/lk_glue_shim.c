/* Stand-in for LK.c and the transport so src/lk_glue.c can be compiled and
 * called on the host. lk_loop() here is transcribed from LK.c with two
 * differences: the out-of-range store at LK.c:541-545 is counted and refused
 * rather than performed, and the cartridge writes become a byte counter and a
 * capture buffer. Linking the real LK.c would reproduce that store, and a
 * harness that must survive the bug it tests for reports segfaults, not
 * failures. Byte counts, order, CONN_RECV calls and the LK_STATUS_OK reply
 * match upstream; nothing verifies that, since test_lk_seam.py compares LK.c
 * against the framer, not against this file. */

#include <stdint.h>
#include <string.h>

#include "proto.h"
#include "lk_glue.h"
#include "LK.h"

/* Dimensioned from LK.h, as LK.c's own definitions are, so these cannot drift
 * from the sizes lk_glue.c's bounds derive from. */
u32  lk_runtime;
u32  _lk_var32[LK_NUM_OF_VARIABLES_32BIT];
u16  _lk_var16[LK_NUM_OF_VARIABLES_16BIT];
u8   _lk_var8[LK_NUM_OF_VARIABLES_8BIT];
u32  _lk_flashcmd_addr[32];
u16  _lk_flashcmd_data[32];
u8   _lk_bankcmd_num;
u32  _lk_bankcmd_addr[3];
u8   _lk_bankcmd_mode[3];
u8   data_buffer[0x1000];
u16  flash_write_cycle[3][2];
u32  time_start;
bool auto_off_timer_suspended;
bool activity_done;
u32  activity_last_run;

#define RXQ 8192u
#define TXQ 8192u

static uint8_t  rxq[RXQ];
static uint32_t rx_len, rx_used;
static uint8_t  txq[TXQ];
static uint32_t tx_len;
static int      tx_dead;
static uint32_t rx_chunk = 64u;

static uint32_t lk_calls;
static uint8_t  lk_last_op;
static uint32_t batch_num, batch_flash;
static uint32_t overflow_stores;
static uint32_t overflow_first_index;
static uint32_t cart_bytes;
static uint8_t  cart_last[256];
static uint32_t cart_last_len;
static uint32_t wait_ready_calls;

void bl_usb_poll(void) { }

uint32_t bl_usb_rx(uint8_t *buf, uint32_t max)
{
    uint32_t n = rx_len - rx_used;

    if (n > max)      { n = max; }
    if (n > rx_chunk) { n = rx_chunk; }
    memcpy(buf, &rxq[rx_used], n);
    rx_used += n;
    return n;
}

uint32_t bl_usb_tx(const uint8_t *buf, uint32_t n)
{
    if (tx_dead) {
        return 0u;
    }
    if (n > TXQ - tx_len) {
        n = TXQ - tx_len;
    }
    memcpy(&txq[tx_len], buf, n);
    tx_len += n;
    return n;
}

int fw_cart_voltage(int five_volt, int dmg_mode)
{
    return five_volt && dmg_mode;      /* cart.c's rule */
}

void fw_cart_wait_ready(void) { wait_ready_calls++; }

/* LK's receive primitives, big-endian like upstream (LK.c:928-944). */
static u8 rx_u8(void)
{
    u8 t;
    fw_lk_conn_recv(&t, 1);
    return t;
}

static u16 rx_u16(void)
{
    u8 t[2];
    fw_lk_conn_recv(t, 2);
    return (u16)(((u16)t[0] << 8) | (u16)t[1]);
}

static u32 rx_u32(void)
{
    u8 t[4];
    fw_lk_conn_recv(t, 4);
    return ((u32)t[0] << 24) | ((u32)t[1] << 16) | ((u32)t[2] << 8) | (u32)t[3];
}

static void tx_u8(u8 v) { fw_lk_conn_send_byte(v); }

void lk_loop(u8 command)
{
    lk_calls++;
    lk_last_op = command;

    switch (command) {

    case LK_CMD_CART_WRITE_FLASH_CMD: {          /* 0xD4, LK.c:517-563 */
        u8 flash = rx_u8();
        u8 num   = rx_u8();
        u8 x;

        batch_flash = flash;
        batch_num   = num;
        for (x = 0; x < num; x++) {
            u32 a = rx_u32();
            u16 d = rx_u16();
            /* Upstream is `_lk_flashcmd_addr[x+16] = ...` with no bound. */
            if ((unsigned)x + 16u >= (sizeof _lk_flashcmd_addr /
                                      sizeof _lk_flashcmd_addr[0])) {
                if (overflow_stores == 0u) {
                    overflow_first_index = (unsigned)x + 16u;
                }
                overflow_stores++;
            } else {
                _lk_flashcmd_addr[x + 16] = a;
                _lk_flashcmd_data[x + 16] = d;
            }
        }
        cart_bytes += num;
        tx_u8(LK_STATUS_OK);
        break;
    }

    case LK_CMD_DMG_CART_WRITE: {                /* 0xB2, LK.c:344-352 */
        u32 addr = rx_u32();
        u8  data = rx_u8();
        cart_last_len = 0u;
        cart_last[cart_last_len++] = (u8)(addr >> 8);
        cart_last[cart_last_len++] = data;
        cart_bytes += 1u;
        tx_u8(LK_STATUS_OK);
        break;
    }

    case LK_CMD_DMG_CART_WRITE_SRAM: {           /* 0xB3, LK.c:355-363 */
        u16 n = _lk_var16[LK_VAR16_TRANSFER_SIZE];
        u16 i;
        fw_lk_conn_recv(data_buffer, n);
        /* Upstream writes all n bytes to the cartridge unconditionally. */
        cart_last_len = 0u;
        for (i = 0; i < n; i++) {
            if (cart_last_len < sizeof cart_last) {
                cart_last[cart_last_len++] = data_buffer[i];
            }
        }
        cart_bytes += n;
        tx_u8(LK_STATUS_OK);
        break;
    }

    default:
        tx_u8(LK_STATUS_ERROR);
        break;
    }

    activity_done = true;
    ACTIVITY_LED_OFF();
}

static fw_state_t st;

fw_state_t *shim_state(void)                 { return &st; }
void shim_state_mode(int m)                  { st.mode = (uint8_t)m; }
void shim_state_transfer_size(uint32_t n)    { st.transfer_size = (uint16_t)n; }

void shim_reset(void)
{
    memset(&st, 0, sizeof st);
    memset(rxq, 0, sizeof rxq);
    memset(txq, 0, sizeof txq);
    memset(_lk_flashcmd_addr, 0, sizeof _lk_flashcmd_addr);
    memset(_lk_flashcmd_data, 0, sizeof _lk_flashcmd_data);
    memset(data_buffer, 0, sizeof data_buffer);
    rx_len = rx_used = tx_len = 0u;
    tx_dead = 0;
    rx_chunk = 64u;
    lk_calls = 0u;
    lk_last_op = 0u;
    batch_num = batch_flash = 0u;
    overflow_stores = overflow_first_index = 0u;
    cart_bytes = cart_last_len = 0u;
    wait_ready_calls = 0u;
    fw_lk_init(&st);
}

void shim_feed(const uint8_t *b, uint32_t n)
{
    if (n > RXQ - rx_len) {
        n = RXQ - rx_len;
    }
    memcpy(&rxq[rx_len], b, n);
    rx_len += n;
}

void     shim_tx_dead(int dead)   { tx_dead = dead ? 1 : 0; }
uint32_t shim_tx_len(void)        { return tx_len; }
uint32_t shim_tx_at(uint32_t i)   { return (i < tx_len) ? txq[i] : 0x100u; }
uint32_t shim_rx_unread(void)     { return rx_len - rx_used; }

uint32_t shim_lk_calls(void)      { return lk_calls; }
uint32_t shim_lk_last_op(void)    { return lk_last_op; }
uint32_t shim_batch_num(void)     { return batch_num; }
uint32_t shim_batch_flash(void)   { return batch_flash; }
uint32_t shim_overflow(void)      { return overflow_stores; }
uint32_t shim_overflow_index(void){ return overflow_first_index; }
uint32_t shim_cart_bytes(void)    { return cart_bytes; }
uint32_t shim_cart_len(void)      { return cart_last_len; }
uint32_t shim_cart_at(uint32_t i) { return (i < cart_last_len) ? cart_last[i] : 0x100u; }
uint32_t shim_wait_ready(void)    { return wait_ready_calls; }
uint32_t shim_flashcmd_addr(uint32_t i)
{
    return (i < sizeof _lk_flashcmd_addr / sizeof _lk_flashcmd_addr[0])
           ? _lk_flashcmd_addr[i] : 0u;
}
uint32_t shim_flashcmd_data(uint32_t i)
{
    return (i < sizeof _lk_flashcmd_data / sizeof _lk_flashcmd_data[0])
           ? _lk_flashcmd_data[i] : 0u;
}
