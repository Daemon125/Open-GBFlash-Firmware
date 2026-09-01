/* Never reached by the core, which fetches from 0x0000: the bootloader
 * forwards, the official one from a copy at 0x20000000 (ld: .bl_lowram). */

    .syntax unified
    .cpu    cortex-m0
    .thumb

    .section .vectors, "a", %progbits
    .align  2
    .global __vectors
    .type   __vectors, %object
__vectors:
    /* [0] must satisfy (sp & 0x2FFE0000) == 0x20000000, gated by both
     * bootloaders. [1] the official bootloader BLXes unexamined (0x1EA0). */
    .word   __stack_top             /*  0  Initial MSP                        */
    .word   Reset_Handler           /*  1  Reset                              */
    .word   fw_fault_handler        /*  2  NMI                                */
    .word   fw_fault_handler        /*  3  HardFault                          */

    /* Non-zero with the Thumb bit set: mkimage.py rejects a zero entry. */
    .rept   7
    .word   fw_fault_handler        /*  4..10  reserved                       */
    .endr

    .word   fw_fault_handler        /* 11  SVCall                             */
    .word   fw_fault_handler        /* 12  reserved                           */
    .word   fw_fault_handler        /* 13  reserved                           */
    .word   fw_fault_handler        /* 14  PendSV                             */
    .word   fw_fault_handler        /* 15  SysTick                            */

    /* Both bootloaders keep the exception return for themselves, so the USB
     * handler is a plain C function: no interrupt attribute, no prologue. */

    /* -DFW_USB_IRQ goes to the assembler too; usb.c must see the same value. */
#ifndef FW_USB_IRQ
#define FW_USB_IRQ 0
#endif

    .rept   6
    .word   fw_fault_handler        /* 16..21  IRQ0..IRQ5                     */
    .endr

#if FW_USB_IRQ
    .word   fw_usb_irq_handler      /* 22      IRQ6 = USB                     */
#else
    .word   fw_fault_handler        /* 22      IRQ6 = USB, masked             */
#endif

    .rept   13
    .word   fw_fault_handler        /* 23..35  IRQ7..IRQ19                    */
    .endr

    .size   __vectors, . - __vectors

/* Spins on purpose: power cycle plus U22 recovers, a reset would boot-loop. */
    .section .text.fw_fault_handler, "ax", %progbits
    .align  1
    .thumb_func
    .global fw_fault_handler
    .type   fw_fault_handler, %function
fw_fault_handler:
    b       .
    .size   fw_fault_handler, . - fw_fault_handler
