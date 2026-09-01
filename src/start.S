/* Entered from bl_jump_to_app(), not reset: hardware is as the bootloader left
 * it, with NVIC enable and pending bits assumed clear. */

#ifndef FW_INIT_CLOCK
#define FW_INIT_CLOCK 1
#endif

/* R16_CLK_SYS_CFG low byte. 0x88 is int RC direct, 32 MHz, what stock ships.
 * 0x4C is int RC -> PLL480/12 = 40 MHz, the vendor's own SetSysClock case 13.
 * Neither needs the X1 crystal; bit 9 would, and retail boards lack it. */
#ifndef FW_SYS_CLK_CFG
#define FW_SYS_CLK_CFG 0x88
#endif

/* Powers the PLL before the clock switch. The shipped 0x4C is a PLL mode and
 * still leaves this 0: the bootloader hands over with R8_HFCK_PWR_CTRL already
 * 0x1C. Set it if that changes. */
#ifndef FW_SYS_CLK_PLL
#define FW_SYS_CLK_PLL 0
#endif

    .syntax unified
    .cpu    cortex-m0
    .thumb

    .section .text.Reset_Handler, "ax", %progbits
    .align  1
    .thumb_func
    .global Reset_Handler
    .type   Reset_Handler, %function
Reset_Handler:

    /* Nothing may sit between the second key and the protected store. strh, not
     * str: R8_HFCK_PWR_CTRL at 0x4000100A shares the word. */
#if FW_INIT_CLOCK
    ldr     r0, =0x40001040             /* R8_SAFE_ACCESS_SIG                 */
    ldr     r1, =0x40001008             /* R16_CLK_SYS_CFG                    */
    movs    r2, #0x57
    movs    r3, #0xA8
    movs    r4, #FW_SYS_CLK_CFG

#if FW_SYS_CLK_PLL
    /* MOD 1 takes Fsys from the PLL. R8_HFCK_PWR_CTRL 0x4000100A:
     * XT32M_PON|INT32M_PON|PLL_PON = 0x1C. Its own safe-access window, and a
     * byte: a word store lands on R16_CLK_SYS_CFG at 0x1008 too. */
    ldr     r5, =0x4000100A
    movs    r6, #0x1C
    strb    r2, [r0]
    strb    r3, [r0]
    strb    r6, [r5]
    movs    r6, #0
    strb    r6, [r0]

    ldr     r5, =2000                   /* PLL settle, blind: no ready bit    */
1:  subs    r5, #1
    bne     1b
#endif

    strb    r2, [r0]
    strb    r3, [r0]
    strh    r4, [r1]
    movs    r2, #0
    strb    r2, [r0]
#endif

    /* Both ends are 4-byte aligned by the linker script. */
    ldr     r0, =__data_load
    ldr     r1, =__data_start
    ldr     r2, =__data_end
    b       2f
1:  ldm     r0!, {r3}
    stm     r1!, {r3}
2:  cmp     r1, r2
    blo     1b

    movs    r3, #0
    ldr     r1, =__bss_start
    ldr     r2, =__bss_end
    b       4f
3:  stm     r1!, {r3}
4:  cmp     r1, r2
    blo     3b

    bl      fw_main

    .global fw_main_returned
fw_main_returned:
    b       .

    .size   Reset_Handler, . - Reset_Handler


    .end
