/* Seam to Lesserkuma's LK.c: opcodes named by fw_lk_routes() leave the stream
 * before the parser and run in lk_loop(). AGB never routes; LK's recv blocks. */

#ifndef FW_LK_GLUE_H
#define FW_LK_GLUE_H

#include <stdint.h>
#include "proto.h"

#ifndef FW_LK_ROUTE_DMG_READ
#define FW_LK_ROUTE_DMG_READ 1  /* Makefile:317 ships 0 */
#endif

#ifndef FW_LK_ROUTE
#define FW_LK_ROUTE 1
#endif

/* Fills a payload the host cut short: LK.c:355 writes the buffer to the cart. */
#define FW_LK_RECV_FILL 0xFFu

void fw_lk_init(fw_state_t *st);

int fw_lk_routes(const fw_state_t *st, uint8_t op);

/* Runs one LK command; returns the `tail` bytes the caller must skip.
 * CART_WRITE_FLASH_CMD is refused: LK.c:541-545 indexes _lk_flashcmd_*[x+16]
 * with a host-supplied pair count it never bounds. */
uint32_t fw_lk_dispatch(fw_state_t *st, uint8_t op,
                        const uint8_t *tail, uint32_t tail_len);

int fw_lk_recv_truncated(void);
int fw_lk_send_stalled(void);

void fw_lk_led_idle(void);      /* PB12 low = lit */

void fw_cart_wait_ready(void);
void fw_lk_bootloader_reset(void);      /* 0xAA55BB01 -> 0x20000090 */

void fw_lk_conn_recv(uint8_t *data, uint16_t count);
void fw_lk_conn_send(const uint8_t *data, uint16_t count);
void fw_lk_conn_send_byte(uint8_t data);
void fw_lk_delay_ms(uint32_t ms);
void fw_lk_voltage_5v(void);
uint8_t fw_lk_pcb_ver(void);

#endif /* FW_LK_GLUE_H */
