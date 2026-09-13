/* The cartridge bus, transcribed from re/symbols-cartio.md sections 1 and 4.
 * The nops in each strobe are pulse widths; sampling early gives a subtly wrong
 * dump on marginal cartridges only. */

#include <stdint.h>
#include "cart.h"
#include "timebase.h"

/* Overridable so host/test_cart.py can redirect every access to RAM. */
#ifndef REG32
#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#endif

/* %c0 rather than stringification: the scaled count is a C expression and some
 * knobs carry a u suffix, which the assembler cannot parse in a .rept. */
#define BUS_NOPS_(n) __asm__ volatile (".rept %c0\n\tnop\n.endr"\
                                       :: "i"((int)(n)) : "memory")

/* Intervals below are cycle counts, so a faster Fsys shortens them in
 * wall-clock terms. BUS_SCALE pads the nop run by FW_BUS_NOP_NUM/DEN and
 * scales the nops only: a window of N cycles holding n nops still shortens by
 * up to (N-n)/N. */
#ifndef FW_BUS_NOP_NUM
#define FW_BUS_NOP_NUM 1
#endif
#ifndef FW_BUS_NOP_DEN
#define FW_BUS_NOP_DEN 1
#endif
#define BUS_SCALE(n) (((n) * FW_BUS_NOP_NUM + FW_BUS_NOP_DEN - 1) / FW_BUS_NOP_DEN)

/* AGB_RPT and LEAF_RPT are unscaled: the leaf's settle carries a fixed
 * 20-cycle part that its own assembly-time check re-counts, so scaling the
 * nops alone trips it. The AGB intervals below are raw cycle counts swept at
 * 40 MHz, so another Fsys moves their wall-clock times. */

#define BUS_NOPS(n)  BUS_NOPS_(BUS_SCALE(n))

/* Backoff between polls, not an interval the cartridge measures, so it is not
 * scaled: its length is not a correctness property, and scaling it grew a
 * 400-iteration loop past a Bcc's reach. */
#define BUS_NOPS_RAW(n)  BUS_NOPS_(n)

/* A window of n nops plus f fixed instruction cycles, scaled whole; BUS_NOPS
 * would pad the nops alone and leave it short by f*(NUM-DEN)/DEN. Use where
 * the cartridge measures the interval and instructions sit inside it. */
#define BUS_NOPS_FIXED(n, f) \
    BUS_NOPS_(BUS_SCALE((n) + (f)) > (f) ? BUS_SCALE((n) + (f)) - (f) : 0)

/* Settle between the AGB address latch and the first /RD; 0 means 72 cycles of
 * call frame, not nops. FW_AGB_LEAF=0 only. No `u' suffix: it feeds a .rept. */
#ifndef FW_AGB_LATCH_NOPS
#define FW_AGB_LATCH_NOPS 0
#endif

void fw_cart_init(void)
{
    /* PB_AUDIO is cart pin 31, AGB /IRQ and DMG audio-in, which a cartridge
     * may drive, so it stays an input until asked (LK.c:228-233, :235-248). */
    REG32(R32_PB_DIR) |= PB_LED | PB_WR | PB_RD | PB_CS | PB_CLK
                       | PB_CS2 | PB_VCC_EN | PB_VSEL;
    REG32(R32_PB_DIR) &= ~PB_AUDIO;

    /* PB21 low is 3.3 V (cart.h), before anything can be energised. */
    REG32(R32_PB_CLR) = PB_VSEL;
    REG32(R32_PB_DIR) &= ~PB_BUTTON;
    REG32(R32_PB_PU)  |= PB_BUTTON;

    /* Active low: between the arm above and this store /CS and /WR assert. */
    REG32(R32_PB_OUT) |= PB_CTRL_IDLE;

    /* CLK idles LOW. */
    REG32(R32_PB_CLR) = PB_CLK;

    REG32(R32_PA_DIR) &= ~PA_AD_MASK;
    REG32(R32_PB_DIR) &= ~PB_ADDR_HI;

    REG32(R32_PB_CLR) = PB_VCC_EN;
}

/* Cartridge-facing lines, released on power-off: {0..7} data / high address,
 * {13,14,15,19,20} /WR /RD /CS /CS2 AUDIO, as stock sub_4EA0. PB21 excluded. */
#define PB_CART_FACING  (PB_ADDR_HI | (0xC7u << 13))

/* On, PB20 (cart pin 31) is armed only when asked, as in LK (LK.c:858-895).
 * Off, every power-on re-arms it push-pull against what an AGB cart drives. */
#ifndef FW_CART_AUDIO_HONOUR
#define FW_CART_AUDIO_HONOUR 0
#endif

#if FW_CART_AUDIO_HONOUR
/* Nonzero once the host has asked for PB20 to be DRIVEN. */
static uint8_t g_audio_out;
#endif

void fw_cart_power(int on)
{
    if (on) {
        /* VCC before drive: the control lines idle HIGH, so arming them first
         * back-powers an unpowered cartridge. Stock sub_4EA0 0x4EC4-0x4ED4. */
        REG32(R32_PB_OUT) |= PB_VCC_EN;
        fw_cart_settle();
        REG32(R32_PB_OUT) |= PB_CTRL_IDLE;
#if FW_CART_AUDIO_HONOUR
        /* The latch above parked PB_AUDIO high, so this arms it deasserted. */
        REG32(R32_PB_DIR) |= (PB_WR | PB_RD | PB_CS | PB_CS2);
        if (g_audio_out) {
            REG32(R32_PB_DIR) |= PB_AUDIO;
        }
#else
        REG32(R32_PB_DIR) |= (0xC7u << 13);
#endif
    } else {
        /* A line held high into a falling supply back-powers the same way. */
        REG32(R32_PB_DIR) &= ~PB_CART_FACING;
        REG32(R32_PA_DIR) &= ~PA_AD_MASK;
        REG32(R32_PB_CLR) = PB_VCC_EN;

        /* A cartridge gets swapped here; a latched 5 V select would wait. */
        REG32(R32_PB_CLR) = PB_VSEL;
        fw_cart_settle();
    }
}

int fw_cart_voltage(int five_volt, int dmg_mode)
{
    /* HAZARD: a 3.3 V AGB cartridge on a 5 V rail is permanent damage, and
     * FlashGBX asks for 5 V mid mode-change and after a failed detect. */
    if (five_volt && dmg_mode) {
        REG32(R32_PB_OUT) |= PB_VSEL;
        fw_cart_settle();
        return 1;
    }
    REG32(R32_PB_CLR) = PB_VSEL;
    fw_cart_settle();
    return !five_volt;
}

void fw_cart_tristate(void)
{
    REG32(R32_PA_DIR) &= ~PA_AD_MASK;
    REG32(R32_PB_DIR) &= ~PB_ADDR_HI;
}

/* On, 0xAB/0xAC as stock has them (0x9828, 0x9880): float both buses and give
 * them 5 ms to settle. Off, only `PA_PU |= 0xFFFF', answered bus-still-driven. */
#ifndef FW_CART_PULLUPS_FULL
#define FW_CART_PULLUPS_FULL 0
#endif

/* Stock's 5000-iteration 32-cycle loop at 0x983C at 32 MHz; LK.c:311. */
#define FW_CART_PULLUP_SETTLE_MS  5u

void fw_cart_pullups(int on)
{
    /* PA is A0..A15 (DMG) / AD0..AD15 (AGB); pull-ups act on a parked bus. */
    if (on) {
        REG32(R32_PA_PU) |= PA_AD_MASK;
#if FW_CART_PULLUPS_FULL
        REG32(R32_PB_PU) |= PB_ADDR_HI;
#endif
    } else {
        REG32(R32_PA_PU) &= ~PA_AD_MASK;
#if FW_CART_PULLUPS_FULL
        REG32(R32_PB_PU) &= ~PB_ADDR_HI;
#endif
    }
#if FW_CART_PULLUPS_FULL
    /* Float the bus so the pull-ups act on something; every routine re-arms. */
    REG32(R32_PA_DIR) &= ~PA_AD_MASK;
    REG32(R32_PB_DIR) &= ~PB_ADDR_HI;
    {
        uint32_t deadline = bl_time_ms() + FW_CART_PULLUP_SETTLE_MS;
        while ((int32_t)(bl_time_ms() - deadline) < 0) {
        }
    }
#endif
}

/* 10 ms, stock's figure; a read before the supply is stable looks like data.
 * Do not go back to a spin count, the one this replaced landed near 11 ms. */
void fw_cart_settle(void)
{
    uint32_t deadline = bl_time_ms() + FW_CART_SETTLE_MS;
    while ((int32_t)(bl_time_ms() - deadline) < 0) {
    }
}

/* The AGB bus multiplexes: AD0..AD15 carry A0..A15 during the latch and the
 * data word during the read, A16..A23 stay on PB0..PB7. /CS low latches the
 * address and the cartridge auto-increments on each /RD. */

void fw_cart_agb_open(uint32_t hwaddr)
{
    /* Both parks drive AD and A16..A23 to a known state before the direction
     * flip. Remove them and the dump shifts one halfword. */
    REG32(R32_PA_OUT) = 0;
    REG32(R32_PB_CLR) = PB_ADDR_HI;

    REG32(R32_PA_DIR) |= PA_AD_MASK;
    REG32(R32_PB_DIR) |= PB_ADDR_HI;

    REG32(R32_PA_OUT) = hwaddr & PA_AD_MASK;
    REG32(R32_PB_CLR) = PB_ADDR_HI;
    REG32(R32_PB_OUT) |= (hwaddr >> 16) & PB_ADDR_HI;

    REG32(R32_PB_CLR) = PB_CS;          /* /CS low: cartridge latches it   */

    REG32(R32_PA_OUT) = 0;

    /* No pcb_ver < 0x0D bus pre-charge, as stock skips it at >= 0x0D. Put it
     * back first if a v1.2 board reads garbage: re/symbols-cartio.md 4.1. */

    REG32(R32_PA_DIR) &= ~PA_AD_MASK;

    /* Without a settle every read comes back shifted forward one halfword.
     * Live for peek() even under FW_AGB_LEAF=1. */
    BUS_NOPS(FW_AGB_LATCH_NOPS);
}

/* In assembly because in C the compiler may move the sample store across the
 * /RD stores. Failure is silent; verify with tools/verify_agb_methods.py. */
#ifndef FW_AGB_FAST_BURST
#define FW_AGB_FAST_BURST 0
#endif

/* /RD low -> sample: the ROM's access time. 2 is the floor: the strobe spends
 * both nops on `adds'/`subs'. Swept 9..2 on an S29GL256N at 40 MHz, byte-exact
 * at every point over a full 16 MiB on all three read methods. Raise this and
 * FW_AGB_LEAF_CSHI_CYCLES first if a cartridge reads shifted; BUS_SCALE(7) and
 * BUS_SCALE(40) are the wider pair these replaced. */
#ifndef FW_AGB_RD_LO_NOPS
#define FW_AGB_RD_LO_NOPS    2
#endif
/* Sample -> /RD high. 6 -> 0: /RD low 17 -> 11 cycles, 344 ns, against a GBA's
 * own ~119 ns sequential access; the sample point does not move. */
#ifndef FW_AGB_RD_HOLD_NOPS
#define FW_AGB_RD_HOLD_NOPS  0
#endif
/* Bus recovery. 1 -> 0: /RD high 10 -> 9 cycles, 281 ns, against a console's
 * ~60-119 ns. */
#ifndef FW_AGB_RD_HI_NOPS
#define FW_AGB_RD_HI_NOPS    0
#endif

#if FW_AGB_FAST_BURST

#define AGB_RPT_(n)  ".rept " #n "\n\tnop\n\t.endr\n\t"
#define AGB_RPT(n)   AGB_RPT_(n)

#if defined(__ARM_ARCH_6M__)

/* One base register covers PA_PIN [+0], PB_OUT [+36], PB_CLR [+40]. */
__extension__ _Static_assert(R32_PB_OUT - R32_PA_PIN == 36u,
                             "agb_burst_fast: PB_OUT is not PA_PIN + 36");
__extension__ _Static_assert(R32_PB_CLR - R32_PA_PIN == 40u,
                             "agb_burst_fast: PB_CLR is not PA_PIN + 40");

/* Caller guarantees halfwords != 0 and a halfword-aligned dst. */
static void agb_burst_fast(uint16_t *dst, uint32_t halfwords)
{
    uint32_t base = (uint32_t)R32_PA_PIN;
    uint32_t rd   = (uint32_t)PB_RD;
    /* Hoisted; PB_CLR is write-1-to-clear, so this re-drives the other bits. */
    uint32_t hi   = REG32(R32_PB_OUT) | (uint32_t)PB_RD;
    uint32_t t;

    __asm__ volatile (
        ".syntax unified\n\t"
        "1:\n\t"
        "str   %[rd], [%[b], #40]\n\t"   /* PB_CLR = PB_RD   -> /RD low      */
        AGB_RPT(FW_AGB_RD_LO_NOPS)
        "ldr   %[t], [%[b], #0]\n\t"     /* PA_PIN           -> sample       */
        AGB_RPT(FW_AGB_RD_HOLD_NOPS)
        "str   %[hi], [%[b], #36]\n\t"   /* PB_OUT = cached  -> /RD high     */
        "strh  %[t], [%[d]]\n\t"
        "adds  %[d], #2\n\t"
        "subs  %[c], #1\n\t"
        AGB_RPT(FW_AGB_RD_HI_NOPS)       /* nop does not touch the flags     */
        "bne   1b\n\t"
        : [t] "=&l" (t), [d] "+l" (dst), [c] "+l" (halfwords)
        : [b] "l" (base), [rd] "l" (rd), [hi] "l" (hi)
        : "cc", "memory");
    (void)t;
}

#else  /* not Cortex-M0: the host suite's MMIO shim build */

/* Same pin sequence and byte order, no timing guarantee: there is no bus. */
static void agb_burst_fast(uint16_t *dst, uint32_t halfwords)
{
    uint32_t hi = REG32(R32_PB_OUT) | (uint32_t)PB_RD;

    while (halfwords-- != 0u) {
        uint32_t w;
        REG32(R32_PB_CLR) = PB_RD;
        BUS_NOPS(FW_AGB_RD_LO_NOPS);
        w = REG32(R32_PA_PIN);
        BUS_NOPS(FW_AGB_RD_HOLD_NOPS);
        REG32(R32_PB_OUT) = hi;
        *dst++ = (uint16_t)w;
    }
}

#endif /* __ARM_ARCH_6M__ */
#endif /* FW_AGB_FAST_BURST */

/* One naked leaf does the latch, the strobes and the deselect for a whole
 * request, replacing the open/burst/close nest in do_cart_read(). Every
 * interval that path spent inside a call frame is paid back here as a counted
 * constant; the three-call values are ADDR 17, SETTLE 72, CSHI 86, RDHI 49. */
#ifndef FW_AGB_LEAF
#define FW_AGB_LEAF 0
#endif

#if FW_AGB_LEAF

#if !FW_AGB_FAST_BURST
#error "FW_AGB_LEAF ships agb_burst_fast()'s strobe body verbatim, with the \
same three FW_AGB_RD_* knobs, so FW_AGB_FAST_BURST=0 would be silently \
ignored: the build would still run the fast waveform while the switch says it \
is off. Build FW_AGB_LEAF=1 with FW_AGB_FAST_BURST=1, or not at all."
#endif

/* 13 is the sequence's own cost, so this is its floor and carries no pad. */
#ifndef FW_AGB_LEAF_ADDR_CYCLES
#define FW_AGB_LEAF_ADDR_CYCLES     13
#endif
/* Not composable: FW_AGB_LATCH_NOPS pads inside fw_cart_agb_open(). */
#if FW_AGB_LEAF && defined(FW_AGB_LATCH_NOPS) && (FW_AGB_LATCH_NOPS != 0)
#error "FW_AGB_LATCH_NOPS has no effect when FW_AGB_LEAF=1: the leaf does not call fw_cart_agb_open(). Use FW_AGB_LEAF_SETTLE_CYCLES instead; 72 is the three-call figure, not this build's, whose settle is 20."
#endif

#ifndef FW_AGB_LEAF_SETTLE_CYCLES
/* Stock's shortest settle (Single 23, MemCpy 22, Stream 31). 20 is this loop's
 * floor: the /CS-low-to-/RD-low sequence costs 20 cycles by itself, so the pad
 * is zero here. Swept 28/24/20 on an S29GL256N at 40 MHz, each byte-exact over
 * a full 16 MiB on all three read methods against a vendor-firmware dump.
 * Raise toward BUS_SCALE(22) if a cartridge reads shifted by one halfword. */
#define FW_AGB_LEAF_SETTLE_CYCLES   20
#endif
#ifndef FW_AGB_LEAF_CSHI_CYCLES
/* Stock's /CS-high-to-next-/CS-low is 80. 36 + the address pad is this loop's
 * floor, 900 ns at 40 MHz against a recovery specified in tens of ns. Raise
 * first if a cartridge reads shifted: 50, then 100 for stock's wall-clock. */
#define FW_AGB_LEAF_CSHI_CYCLES     36
#endif
#ifndef FW_AGB_LEAF_RDHI_CYCLES
/* 12 is the leaf's own cost, 49 what the three-call path leaves. Not a flat
 * 12: FW_AGB_RD_HI_NOPS sits inside this region, so the floor moves with it. */
#define FW_AGB_LEAF_RDHI_CYCLES     (12 + FW_AGB_RD_HI_NOPS)
#endif

/* Pad emitted is (wanted - fixed); unpadded the leaf costs ADDR 13, SETTLE 20,
 * CSHI 36 + ADDR pad, RDHI 12 + FW_AGB_RD_HI_NOPS. min(grp, left) is computed
 * in CSHI, branchless, so a short FINAL group cannot shorten the settle. */
#define AGB_LEAF_ADDR_NOPS      (FW_AGB_LEAF_ADDR_CYCLES - 13)
#define AGB_LEAF_SETTLE_NOPS    (FW_AGB_LEAF_SETTLE_CYCLES - 20)
/* `bne 1b' buys a cycle of CSHI but puts the whole group body inside the
 * branch's +-254-byte reach; over budget it is `beq 4f' + `b 1b', 37/41. */
#define AGB_LEAF_CSHI_NOPS_BNE  (FW_AGB_LEAF_CSHI_CYCLES - 36 - AGB_LEAF_ADDR_NOPS)
#define AGB_LEAF_RDHI_NOPS      (FW_AGB_LEAF_RDHI_CYCLES - 12 - FW_AGB_RD_HI_NOPS)
/* Two /RD-low nops carry `adds r6,#1' and `subs r2,#1', one cycle for one. */
#define AGB_LEAF_RD_LO_FILL     (FW_AGB_RD_LO_NOPS - 2)

#define AGB_LEAF_PAD_TOTAL_BNE  (AGB_LEAF_ADDR_NOPS + AGB_LEAF_SETTLE_NOPS \
                               + AGB_LEAF_CSHI_NOPS_BNE + AGB_LEAF_RDHI_NOPS \
                               + AGB_LEAF_RD_LO_FILL + FW_AGB_RD_HOLD_NOPS \
                               + FW_AGB_RD_HI_NOPS)
/* Budget only; a CSHI below its floor only shrinks the total. */
#if AGB_LEAF_PAD_TOTAL_BNE <= 72
#define AGB_LEAF_CLOSE_BNE      1
#define AGB_LEAF_CSHI_NOPS      AGB_LEAF_CSHI_NOPS_BNE
#else
#define AGB_LEAF_CLOSE_BNE      0
#define AGB_LEAF_CSHI_NOPS      (FW_AGB_LEAF_CSHI_CYCLES - 37 - AGB_LEAF_ADDR_NOPS)
#if AGB_LEAF_PAD_TOTAL_BNE > 72
/* The floor moved because the loop close changed, not because /CS high did. */
__extension__ _Static_assert(FW_AGB_LEAF_CSHI_CYCLES >= 37 + AGB_LEAF_ADDR_NOPS,
    "the other pads grew past what `bne 1b' reaches, so the loop is now closed "
    "with `beq 4f' + `b 1b' and the /CS-high floor is 41 rather than 40 at the "
    "default address pad. Raise FW_AGB_LEAF_CSHI_CYCLES by one, or narrow "
    "whichever of FW_AGB_LEAF_SETTLE_CYCLES / FW_AGB_LEAF_RDHI_CYCLES / "
    "FW_AGB_LEAF_ADDR_CYCLES / the three FW_AGB_RD_* knobs grew");
#endif
#endif


__extension__ _Static_assert(FW_AGB_LEAF_SETTLE_CYCLES >= 20,
    "FW_AGB_LEAF_SETTLE_CYCLES below 20, the cost of the /CS-low-to-/RD-low "
    "sequence itself, so the pad cannot go lower: the failure mode is "
    "a dump shifted forward by one halfword, stable across passes and identical "
    "across all three read methods, so tools/verify_agb_methods.py cannot see it");
__extension__ _Static_assert(AGB_LEAF_ADDR_NOPS >= 0,
    "FW_AGB_LEAF_ADDR_CYCLES is below the 13 cycles the sequence already costs");
__extension__ _Static_assert(AGB_LEAF_CSHI_NOPS >= 0,
    "FW_AGB_LEAF_CSHI_CYCLES is below the 36 + address pad the loop already costs");
__extension__ _Static_assert(AGB_LEAF_RDHI_NOPS >= 0,
    "FW_AGB_LEAF_RDHI_CYCLES is below the 12 + FW_AGB_RD_HI_NOPS the strobe tail "
    "and the deselect already cost");
__extension__ _Static_assert(AGB_LEAF_RD_LO_FILL >= 0,
    "FW_AGB_RD_LO_NOPS is below 2: the strobe spends two of the /RD-low nops on "
    "`adds r6,#1' and `subs r2,#1', which is what keeps the address advance and "
    "the remaining-count decrement out of the /CS-high interval. Below 2 there "
    "is nothing to spend, and putting them back on the /CS-high side costs 3 "
    "cycles of CSHI per group, which requires re-counting AGB_LEAF_CSHI_NOPS");
/* 108 bytes of body, +2 per nop, +4 of PC offset, against Bcc's -256 reach. */
#if AGB_LEAF_CLOSE_BNE
__extension__ _Static_assert(AGB_LEAF_PAD_TOTAL_BNE <= 72,
    "the four interval knobs and the three /RD knobs between them pad the group "
    "body past the +-254 bytes `bne 1b' reaches. The fallback to "
    "`beq 4f' + `b 1b' did not engage, so this bound and the #if above "
    "it have drifted apart");
#else
/* Only the /CS-high pad and `b 1b' sit inside the `beq 4f' reach here. */
__extension__ _Static_assert(AGB_LEAF_CSHI_NOPS <= 121,
    "FW_AGB_LEAF_CSHI_CYCLES is so large that `beq 4f' can no longer reach past "
    "the /CS-high pad (Thumb-1 conditional branches reach +254 bytes)");
#endif

#define LEAF_RPT_(n)  ".rept " #n "\n\tnop\n\t.endr\n\t"
#define LEAF_RPT(n)   LEAF_RPT_(n)
#define LEAF_STR_(n)  #n
#define LEAF_STR(n)   LEAF_STR_(n)

#if defined(__ARM_ARCH_6M__)

/* One base register reaches every register this function touches: PA_DIR [+0],
 * PA_PIN [+4], PA_OUT [+8], PB_DIR [+32], PB_OUT [+40], PB_CLR [+44]. The masks
 * and control bits are built with movs/lsls below, so their shapes bind too. */
__extension__ _Static_assert(R32_PA_PIN - R32_PA_DIR ==  4u,
                             "agb_read_leaf: PA_PIN is not PA_DIR + 4");
__extension__ _Static_assert(R32_PA_OUT - R32_PA_DIR ==  8u,
                             "agb_read_leaf: PA_OUT is not PA_DIR + 8");
__extension__ _Static_assert(R32_PB_DIR - R32_PA_DIR == 32u,
                             "agb_read_leaf: PB_DIR is not PA_DIR + 32");
__extension__ _Static_assert(R32_PB_OUT - R32_PA_DIR == 40u,
                             "agb_read_leaf: PB_OUT is not PA_DIR + 40");
__extension__ _Static_assert(R32_PB_CLR - R32_PA_DIR == 44u,
                             "agb_read_leaf: PB_CLR is not PA_DIR + 44");
__extension__ _Static_assert(R32_PA_DIR == 0x400010A0u,
                             "agb_read_leaf: the base is built, not loaded");
__extension__ _Static_assert(PB_RD == (1u << 14) && PB_CS == (PB_RD << 1),
                             "agb_read_leaf: PB_CS is built as PB_RD << 1");
__extension__ _Static_assert(PA_AD_MASK == 0x0000FFFFu &&
                             PB_ADDR_HI == 0x000000FFu,
                             "agb_read_leaf: the masks are built by shifting -1");

/* Caller guarantees halfwords != 0, grp_halfwords != 0, dst halfword-aligned.
 * r12 is hardware-stacked, so an interrupt in the settle only lengthens it. */
__attribute__((naked, noinline))
void fw_cart_agb_read_leaf(uint32_t hwaddr    __attribute__((unused)),
                           uint16_t *dst      __attribute__((unused)),
                           uint32_t halfwords __attribute__((unused)),
                           uint32_t grp_halfwords __attribute__((unused)))
{
    __asm__ volatile (
        ".syntax unified\n\t"

        "push  {r4,r5,r6,r7,lr}\n\t"
        "mov   r4, r8\n\t"
        "mov   r5, r9\n\t"
        "mov   r6, r10\n\t"
        "mov   r7, r11\n\t"
        "push  {r4,r5,r6,r7}\n\t"

        /* Zero length must leave here: the strobe runs until dst reaches the
         * group-end pointer, so n == 0 walks SRAM and on into MMIO. */
        "cmp   r2, #0\n\t"
        "bne   8f\n\t"
        "b     4f\n\t"                 /* B reaches +-2048; Bcc only +-256 */
        "8:\n\t"
        "cmp   r3, #0\n\t"
        "bne   7f\n\t"
        "b     4f\n\t"
        "7:\n\t"
        "movs  r6, r0\n\t"              /* r6 = addr                     */
        "movs  r7, r3\n\t"              /* r7 = grp, low: CSHI reads it  */

        /* n for the FIRST group; later groups compute it on the /CS-high side. */
        "movs  r4, r7\n\t"
        "cmp   r2, r7\n\t"
        "bhs   0f\n\t"
        "movs  r4, r2\n\t"
        "0:\n\t"
        "mov   r12, r4\n\t"             /* r12 = n                       */

        /* Built, not loaded: this function is past LDR-literal range. */
        "movs  r0, #1\n\t"
        "lsls  r0, r0, #30\n\t"         /* 0x40000000                    */
        "movs  r5, #0x85\n\t"
        "lsls  r5, r5, #5\n\t"          /* 0x000010A0                    */
        "adds  r0, r0, r5\n\t"          /* r0 = R32_PA_DIR               */

        "movs  r5, #1\n\t"
        "lsls  r5, r5, #15\n\t"
        "mov   r9, r5\n\t"              /* r9 = PB_CS                    */

        /* Both direction shadows, read once per call. r10 is refreshed in the
         * settle; re-reading PB_DIR for r11 costs 3 where the settle has 2. */
        "ldr   r4, [r0, #0]\n\t"
        "movs  r5, #0\n\t"
        "mvns  r5, r5\n\t"
        "lsrs  r5, r5, #16\n\t"         /* PA_AD_MASK  0x0000FFFF        */
        "orrs  r4, r5\n\t"
        "mov   r10, r4\n\t"             /* r10 = PA_DIR |  PA_AD_MASK    */

        "movs  r3, #0xFF\n\t"           /* r3 = PB_ADDR_HI               */
        "ldr   r4, [r0, #32]\n\t"
        "orrs  r4, r3\n\t"
        "mov   r11, r4\n\t"             /* r11 = PB_DIR |  PB_ADDR_HI    */

        /* Latch phase, /CS high. Store for store as fw_cart_agb_open().  15 */
        "1:\n\t"
        "movs  r5, #0\n\t"
        "str   r5, [r0, #8]\n\t"        /* PA_OUT = 0   park AD          */
        "str   r3, [r0, #44]\n\t"       /* PB_CLR = PB_ADDR_HI  park hi  */
        "mov   r4, r10\n\t"
        "str   r4, [r0, #0]\n\t"        /* PA_DIR |= AD    -> output     */
        "mov   r4, r11\n\t"
        "str   r4, [r0, #32]\n\t"       /* PB_DIR |= A16.. -> output     */
        "lsls  r4, r6, #16\n\t"
        "lsrs  r4, r4, #16\n\t"         /* addr & PA_AD_MASK, no mask reg*/
        "str   r4, [r0, #8]\n\t"        /* PA_OUT = addr & 0xFFFF        */
        /* AD valid; next store the cartridge sees is /CS low.      13 + pad */
        "str   r3, [r0, #44]\n\t"       /* PB_CLR = PB_ADDR_HI           */
        "lsrs  r4, r6, #16\n\t"
        "ands  r4, r3\n\t"
        "ldr   r5, [r0, #40]\n\t"
        "orrs  r4, r5\n\t"
        "str   r4, [r0, #40]\n\t"       /* PB_OUT |= addr >> 16          */
        "mov   r4, r9\n\t"              /* PB_CS                         */
        "movs  r5, #0\n\t"              /* staged so PA_OUT = 0 lands 2 cycles
                                         * after /CS, as fw_cart_agb_open() */
        LEAF_RPT(AGB_LEAF_ADDR_NOPS)    /* AD valid -> /CS low pad       */
        "str   r4, [r0, #44]\n\t"       /* PB_CLR = PB_CS  ==> /CS LOW   */

        /* /CS low; to the first /RD store is the settle. */
        ".global fw_agb_leaf_cs_low\n\t"
        "fw_agb_leaf_cs_low:\n\t"
        "str   r5, [r0, #8]\n\t"        /* PA_OUT = 0              (+2)  */
        "ldr   r5, [r0, #0]\n\t"        /* PA_DIR, still the OUT value   */
        "lsrs  r4, r5, #16\n\t"
        "lsls  r4, r4, #16\n\t"
        "str   r4, [r0, #0]\n\t"        /* PA_DIR: AD -> input     (+8)  */
        "mov   r10, r5\n\t"             /* refresh the PA_DIR-out shadow */
        "mov   r3, r9\n\t"
        "lsrs  r3, r3, #1\n\t"          /* r3 = PB_RD again              */
        "ldr   r4, [r0, #40]\n\t"       /* PB_OUT, read back AFTER the /CS
                                         * clear, so PB_CS is out of it  */
        "orrs  r4, r3\n\t"              /* hi = PB_OUT | PB_RD           */
        "mov   r5, r12\n\t"             /* n                             */
        "lsls  r5, r5, #1\n\t"
        "adds  r5, r1, r5\n\t"          /* group end = dst + 2n          */
        "mov   r8, r5\n\t"              /* the strobe's exit test        */
        LEAF_RPT(AGB_LEAF_SETTLE_NOPS)

        /* Fourteen 16-bit instructions and the pad. Anything else means the
         * settle changed without re-counting the 20 in AGB_LEAF_SETTLE_NOPS. */
        ".if (. - fw_agb_leaf_cs_low) != (2 * (14 + " LEAF_STR(AGB_LEAF_SETTLE_NOPS) "))\n\t"
        ".error \"AGB leaf: the /CS-low-to-/RD-low sequence changed and the 20-cycle fixed part in AGB_LEAF_SETTLE_NOPS was not re-counted. A settle that is too short produces a dump shifted forward by one halfword, stable across passes and identical across all three read methods.\"\n\t"
        ".endif\n\t"

        /* The strobe, agb_burst_fast()'s body verbatim.            20/hw
         * `adds r6,#1' and `subs r2,#1' replace two /RD-low nops ahead of the
         * sample, so /RD stays low 11 and high 9 at every setting. */
        ".global fw_agb_leaf_rd_low\n\t"
        "fw_agb_leaf_rd_low:\n\t"
        "3:\n\t"
        "str   r3, [r0, #44]\n\t"       /* PB_CLR = PB_RD  -> /RD low    */
        "adds  r6, #1\n\t"              /* addr++     (was a nop)        */
        "subs  r2, #1\n\t"              /* left--     (was a nop)        */
        LEAF_RPT(AGB_LEAF_RD_LO_FILL)
        "ldr   r5, [r0, #4]\n\t"        /* PA_PIN          -> sample     */
        LEAF_RPT(FW_AGB_RD_HOLD_NOPS)
        "str   r4, [r0, #40]\n\t"       /* PB_OUT = cached -> /RD high   */
        "strh  r5, [r1, #0]\n\t"
        "adds  r1, #2\n\t"
        "cmp   r1, r8\n\t"              /* group end?                    */
        LEAF_RPT(FW_AGB_RD_HI_NOPS)     /* nop does not touch the flags  */
        "bne   3b\n\t"

        /* Pad to /CS high, outside the loop: the waveform never moves. */
        LEAF_RPT(AGB_LEAF_RDHI_NOPS)

        /* Deselect, verbatim. `movs r3,#0xFF' is the spare cycle.     7c */
        "lsls  r3, r3, #1\n\t"          /* PB_CS, from PB_RD             */
        "ldr   r4, [r0, #40]\n\t"
        "orrs  r4, r3\n\t"
        "movs  r3, #0xFF\n\t"           /* stage PB_ADDR_HI for the latch*/
        "str   r4, [r0, #40]\n\t"       /* PB_OUT |= PB_CS ==> /CS HIGH  */

        /* /CS high, 8c. n for the next group, branchless. */
        "subs  r5, r2, r7\n\t"          /* left - grp, C = (left >= grp) */
        "sbcs  r4, r4, r4\n\t"          /* 0xFFFFFFFF iff left < grp     */
        "ands  r5, r4\n\t"
        "adds  r5, r5, r7\n\t"          /* n = min(grp,left); Z iff n==0 */
        "mov   r12, r5\n\t"             /* MOV to a high reg keeps flags */
#if AGB_LEAF_CLOSE_BNE
        /* The pad is inside the reach, hence the total-padding assert. */
        LEAF_RPT(AGB_LEAF_CSHI_NOPS)    /* /CS high pad                  */
        "bne   1b\n\t"
#else
        /* Wide-pad form, one cycle dearer. The exit test comes BEFORE the
         * pad, so the last group skips a /CS-high interval it cannot use. */
        "beq   4f\n\t"
        LEAF_RPT(AGB_LEAF_CSHI_NOPS)    /* /CS high pad                  */
        "b     1b\n\t"
#endif
        "4:\n\t"

        "pop   {r4,r5,r6,r7}\n\t"
        "mov   r8, r4\n\t"
        "mov   r9, r5\n\t"
        "mov   r10, r6\n\t"
        "mov   r11, r7\n\t"
        "pop   {r4,r5,r6,r7,pc}\n\t"
    );
}

#else  /* not Cortex-M0: the host suite's MMIO shim build */

/* Same pin sequence, per-group deselect and byte order; no timing guarantee. */
void fw_cart_agb_read_leaf(uint32_t hwaddr, uint16_t *dst,
                           uint32_t halfwords, uint32_t grp_halfwords)
{
    while (halfwords != 0u) {
        uint32_t n = (grp_halfwords < halfwords) ? grp_halfwords : halfwords;
        uint32_t hi;
        uint32_t i;

        /* fw_cart_agb_open(), store for store and in the same order. */
        REG32(R32_PA_OUT) = 0;
        REG32(R32_PB_CLR) = PB_ADDR_HI;
        REG32(R32_PA_DIR) |= PA_AD_MASK;
        REG32(R32_PB_DIR) |= PB_ADDR_HI;
        REG32(R32_PA_OUT) = hwaddr & PA_AD_MASK;
        REG32(R32_PB_CLR) = PB_ADDR_HI;
        REG32(R32_PB_OUT) |= (hwaddr >> 16) & PB_ADDR_HI;
        BUS_NOPS(AGB_LEAF_ADDR_NOPS);
        REG32(R32_PB_CLR) = PB_CS;
        REG32(R32_PA_OUT) = 0;
        REG32(R32_PA_DIR) &= ~PA_AD_MASK;
        hi = REG32(R32_PB_OUT) | (uint32_t)PB_RD;
        BUS_NOPS(AGB_LEAF_SETTLE_NOPS);

        for (i = 0; i < n; i++) {
            uint32_t w;
            REG32(R32_PB_CLR) = PB_RD;
            BUS_NOPS(FW_AGB_RD_LO_NOPS);
            w = REG32(R32_PA_PIN);
            BUS_NOPS(FW_AGB_RD_HOLD_NOPS);
            REG32(R32_PB_OUT) = hi;
            *dst++ = (uint16_t)w;
        }

        BUS_NOPS(AGB_LEAF_RDHI_NOPS);
        REG32(R32_PB_OUT) |= PB_CS;

        hwaddr    += n;
        halfwords -= n;
        BUS_NOPS(AGB_LEAF_CSHI_NOPS);
    }
}

#endif /* __ARM_ARCH_6M__ */
#endif /* FW_AGB_LEAF */

/* /CS stays low across a burst and the cartridge advances its own address. */
uint32_t fw_cart_agb_burst(uint8_t *out, uint32_t count)
{
    uint32_t i;
    uint32_t n = count & ~1u;

#if FW_AGB_FAST_BURST
    /* `strh' needs a halfword-aligned dst. Callers pass &g_reply[even] and
     * g_reply is 4-aligned; the PROTOCOL does not guarantee it. */
    if ((n != 0u) && ((((uintptr_t)out) & 1u) == 0u)) {
        agb_burst_fast((uint16_t *)(void *)out, n >> 1);
        return n;
    }
#endif

    for (i = 0; i < n; i += 2u) {
        uint32_t w;

        REG32(R32_PB_CLR) = PB_RD;
        BUS_NOPS(5);                    /* stock 0x629C */
        w = REG32(R32_PA_PIN) & PA_AD_MASK;
        REG32(R32_PB_OUT) |= PB_RD;

        out[i]      = (uint8_t)w;
        out[i + 1u] = (uint8_t)(w >> 8);
    }

    return n;
}

void fw_cart_agb_close(void)
{
    REG32(R32_PB_OUT) |= PB_CS;
}

void fw_cart_delay_nops(uint32_t n)
{
    while (n--) {
        __asm__ volatile ("nop" ::: "memory");
    }
}

uint16_t fw_cart_agb_peek(uint32_t hwaddr)
{
    uint8_t b[2];

    fw_cart_agb_open(hwaddr);
    if (fw_cart_agb_burst(b, 2u) != 2u) {
        b[0] = 0xFFu;
        b[1] = 0xFFu;
    }
    fw_cart_agb_close();
    return (uint16_t)(b[0] | ((uint16_t)b[1] << 8));
}

/* Data setup before /WR falling. [GATED] */
#ifndef FW_AGB_CMD_DS_NOPS
#define FW_AGB_CMD_DS_NOPS      4u   /* data valid -> /WR low, stock 0x6944 */
#endif
#ifndef FW_AGB_CMD_AD_NOPS
#define FW_AGB_CMD_AD_NOPS      1u   /* address valid -> /CS low          */
#endif
/* /WR low in the command phase, as a window: this many nops plus the 5 fixed
 * cycles that raise /WR again. [GATED] Swept 11/5/1 on an S29GL256N: 39.46/39.36/39.09 s per
 * 4 MiB write. At 1 a 4 MiB write reads back byte-exact and six save
 * restore/backup cycles alternating two differing images all match. */
#ifndef FW_AGB_CMD_LO_NOPS
#define FW_AGB_CMD_LO_NOPS      1u
#endif

void fw_cart_agb_write(uint32_t hwaddr, uint16_t value)
{
    /* Stock sub_6908: latch phase, the data word on the SAME AD lines, then
     * /WR. Too short a pulse and a flash cart writes something else. */
    REG32(R32_PA_OUT) = 0;
    REG32(R32_PB_CLR) = PB_ADDR_HI;

    REG32(R32_PA_DIR) |= PA_AD_MASK;
    REG32(R32_PB_DIR) |= PB_ADDR_HI;

    REG32(R32_PA_OUT) = hwaddr & PA_AD_MASK;
    BUS_NOPS(FW_AGB_CMD_AD_NOPS);
    REG32(R32_PB_CLR) = PB_ADDR_HI;
    REG32(R32_PB_OUT) |= (hwaddr >> 16) & PB_ADDR_HI;

    REG32(R32_PB_CLR) = PB_CS;              /* latch the address */

    REG32(R32_PA_OUT) = value;              /* data onto AD0..AD15 */
    BUS_NOPS(FW_AGB_CMD_DS_NOPS);
    REG32(R32_PB_CLR) = PB_WR;
    /* 5 fixed cycles raise /WR again (ldr, orrs, str), counted in build/cart.o. */
    BUS_NOPS_FIXED(FW_AGB_CMD_LO_NOPS, 5);  /* stock 0x6908, counted */
    REG32(R32_PB_OUT) |= PB_WR;
    REG32(R32_PB_OUT) |= PB_CS;
}


#ifndef FW_AGB_WR_ADDR_NOPS
/* Swept on an S29GL256N at 40 MHz: 4/15/4 -> 1/3/1 is 3.0% off a 4 MiB write
 * (41.49 -> 40.24 s), every point reading back byte-exact. The rest of a ROM
 * write is the chip's own program time. Raise these first if a cartridge
 * writes unreliably. */
#define FW_AGB_WR_ADDR_NOPS 1u
#endif
/* /WR low in the burst, as a window: this many nops plus the 5 fixed cycles
 * that raise /WR again. */
/* Swept 11/7/3/0 on an S29GL256N: 40.17/40.06/39.65/39.62 s per 4 MiB write,
 * every point read back byte-exact. 3 and 0 measure the same, so 3 keeps the
 * wider pulse for nothing. Raise toward 11 if a cartridge writes unreliably. */
#ifndef FW_AGB_WR_LO_NOPS
#define FW_AGB_WR_LO_NOPS   3u
#endif
#ifndef FW_AGB_WR_CS_NOPS
#define FW_AGB_WR_CS_NOPS   3u
#endif
#ifndef FW_AGB_WR_DS_NOPS
#define FW_AGB_WR_DS_NOPS   1u
#endif
#ifndef FW_AGB_WR_HI_NOPS
#define FW_AGB_WR_HI_NOPS   1u
#endif
#ifndef FW_AGB_WR_REC_NOPS
#define FW_AGB_WR_REC_NOPS  1u
#endif

void fw_cart_agb_write_burst(uint32_t hwaddr, const uint8_t *data,
                             uint32_t words)
{
    uint32_t i;

    REG32(R32_PA_OUT) = 0;
    REG32(R32_PB_CLR) = PB_ADDR_HI;
    REG32(R32_PA_DIR) |= PA_AD_MASK;
    REG32(R32_PB_DIR) |= PB_ADDR_HI;

    for (i = 0; i < words; i++) {
        uint32_t a = hwaddr + i;
        uint32_t d = (uint32_t)data[i * 2u]
                   | ((uint32_t)data[(i * 2u) + 1u] << 8);

        REG32(R32_PA_OUT) = a & PA_AD_MASK;
        REG32(R32_PB_CLR) = PB_ADDR_HI;
        REG32(R32_PB_OUT) |= (a >> 16) & PB_ADDR_HI;
        BUS_NOPS(FW_AGB_WR_ADDR_NOPS);
        REG32(R32_PB_CLR) = PB_CS;
        BUS_NOPS(FW_AGB_WR_CS_NOPS);
        REG32(R32_PA_OUT) = d;
        BUS_NOPS(FW_AGB_WR_DS_NOPS);
        REG32(R32_PB_CLR) = PB_WR;
        BUS_NOPS_FIXED(FW_AGB_WR_LO_NOPS, 5);
        REG32(R32_PB_OUT) |= PB_WR;
        BUS_NOPS(FW_AGB_WR_HI_NOPS);
        REG32(R32_PB_OUT) |= PB_CS;
        BUS_NOPS(FW_AGB_WR_REC_NOPS);
    }
}

/* DMG, from re/symbols-cartio.md section 5 (stock dispatcher sub_7CFE). The
 * three read methods drive DIFFERENT PINS: `RD' strobes /RD, `A15' toggles A15
 * and never touches /RD. Some carts answer only one, so honour the host. */

#define DMG_METHOD_RD       0u
#define DMG_METHOD_A15      1u
#define DMG_METHOD_SLOW_A15 2u

#define PA_A15          0x8000u

/* FLASH_WE_PIN. NOT the strobe for ordinary writes; see fw_cart_dmg_write(). */
static uint8_t g_dmg_we = FW_DMG_WE_WR;

/* Level AND direction (LK.c:235-248), against fw_cart_audio_dir()'s direction
 * only (LK.c:228-233). Folding them puts a PB_CLR on a line nobody moved. */
void fw_cart_audio_drive(uint8_t on)
{
#if FW_CART_AUDIO_HONOUR
    /* Kept so a power cycle re-applies it, as LK does (LK.c:891-895). */
    g_audio_out = (uint8_t)(on != 0u);
#endif
    if (on) {
        REG32(R32_PB_DIR) |= PB_AUDIO;
        REG32(R32_PB_OUT) |= PB_AUDIO;
    } else {
        REG32(R32_PB_CLR) = PB_AUDIO;
        REG32(R32_PB_DIR) &= ~PB_AUDIO;
    }
}

/* Direction only. LK.c:228-233. */
void fw_cart_audio_dir(uint8_t out)
{
#if FW_CART_AUDIO_HONOUR
    g_audio_out = (uint8_t)(out != 0u);
#endif
    if (out) {
        REG32(R32_PB_DIR) |= PB_AUDIO;
    } else {
        REG32(R32_PB_DIR) &= ~PB_AUDIO;
    }
}

/* An AUDIO write-enable must not come up asserted: PB20's output latch is zero
 * after fw_cart_audio_drive(0) and /WE is active low, so dmg_write_raw() would
 * hold it asserted across the address change. On AMD flash that is an erase. */
#ifndef FW_CART_AUDIO_WE_SAFE
#define FW_CART_AUDIO_WE_SAFE 0
#endif

void fw_cart_set_we_pin(uint8_t we)
{
    g_dmg_we = we;
    /* 26 shipped DMG profiles put the flash /WE on AUDIO. LK.c:228-233. */
#if FW_CART_AUDIO_WE_SAFE
    if (we == FW_DMG_WE_AUDIO) {
        /* A store to PB_OUT while PB20 is an input picks the armed level. */
        REG32(R32_PB_OUT) |= PB_AUDIO;
    }
#endif
    fw_cart_audio_dir((uint8_t)(we == FW_DMG_WE_AUDIO));
}

/* /RD is the cartridge ROM's /OE and /CS is the cart RAM's chip select, so
 * strobe falling to sample IS the part's access time. Short here because stock
 * releases D0..D7 inside that window (0x7FC0, sampling at 0x7FC8). */
#ifndef FW_DMG_CS_READ_PAD
#define FW_DMG_CS_READ_PAD 0
#endif
#ifndef FW_DMG_CS_TO_RD_NOPS
#define FW_DMG_CS_TO_RD_NOPS 3   /* /CS low -> /RD low,   -> 6, stock 7F9E */
#endif
#ifndef FW_DMG_CS_SETTLE_NOPS
/* 8 -> 0 is a further 0.6% with RD_ADDR at 0, and byte-exact on the same
 * full 2 MiB three-method gate. The 8 below it is not this knob. */
#define FW_DMG_CS_SETTLE_NOPS 0  /* on top of the 8, stock 7FA6           */
#endif
/* Address -> /RD low on the ordinary read cycle. Swept at 40 MHz on a ChisFlash
 * 2 MiB: 1 -> 0 takes a 512 KiB read from 739.7 to 776.1 KiB/s, all three read
 * methods byte-exact over a full 2 MiB against a vendor-firmware dump. */
#ifndef FW_DMG_RD_ADDR_NOPS
#define FW_DMG_RD_ADDR_NOPS 0    /* address -> /RD low,   stock 7F26 */
#endif

/* Address -> sample on the A15 read cycle, the A15 method's access time. 8 -> 0
 * takes the A15 method from 670.9 to 789.3 KiB/s, byte-exact against a vendor
 * dump with all three methods agreeing. */
#ifndef FW_DMG_A15_SETTLE_NOPS
#define FW_DMG_A15_SETTLE_NOPS 0
#endif

/* /RD low -> sample on the ordinary read cycle, the ROM's access time. Swept
 * at 40 MHz, 512 KiB per point, byte-exact against a vendor dump:
 * 8 -> 781.1 KiB/s, 5 -> 829.5, 2 -> 911.9, 0 -> 948.7. 0 is the sequence's
 * floor. Raise this knob first if a cartridge reads wrong. */
#ifndef FW_DMG_RD_SETTLE_NOPS
#define FW_DMG_RD_SETTLE_NOPS 0
#endif

void fw_cart_dmg_setup(void)
{
    /* Address out, data in. The data direction flips only for a write. */
    REG32(R32_PA_DIR) |= PA_AD_MASK;
    REG32(R32_PB_DIR) &= ~PB_ADDR_HI;
    REG32(R32_PB_OUT) |= PB_CTRL_IDLE;
}

/* In the A15 and SlowA15 variants A15 is the ONLY strobe: /RD drops once in the
 * prologue and the cycle ends when A15 goes high (stock 0x8020 / 0x808A). A15
 * high is the mask ROM's /CE, so its width is deselect recovery. Re-count: two
 * nops buy three cycles only by pushing GCC's `adds r1,#1' past that store. */
#ifndef FW_DMG_A15_PAD
#define FW_DMG_A15_PAD 0
#endif
#ifndef FW_DMG_A15_FLAT_NOPS
/* 0 is the measured floor: the method dispatch already sits between the address
 * write and the sample and carries about six cycles of settle. */
#define FW_DMG_A15_FLAT_NOPS 0
#endif
#ifndef FW_DMG_A15_HI_NOPS
#define FW_DMG_A15_HI_NOPS 2   /* -> 12, stock 0x8020..0x7FFA */
#endif

uint32_t fw_cart_dmg_read(uint32_t addr, uint8_t *out, uint32_t count,
                          uint8_t method, uint8_t cs_pulse)
{
    uint32_t i;

    REG32(R32_PA_DIR) |= PA_AD_MASK;
    REG32(R32_PB_DIR) &= ~PB_ADDR_HI;

    /* Prologue, stock 0x7ECC..0x7EDE. Without it no variant reads a byte: the
     * A15 loop at 0x7FF8 never writes PB, so /RD is low across it only because
     * the prologue put it there, and the mask ROM's /OE is /RD (pin 4). */
    REG32(R32_PB_CLR) = PB_RD;
    REG32(R32_PB_CLR) = PB_CLK;

    for (i = 0; i < count; i++) {
        uint32_t a = (addr + i) & 0xFFFFu;

        if (cs_pulse) {
            /* CS-pulse variant, stock 0x7F70: the full handshake, CLK included. */
            REG32(R32_PA_OUT) = a;
            REG32(R32_PB_OUT) |= PB_CLK;
            REG32(R32_PB_CLR) = PB_CS;
#if FW_DMG_CS_READ_PAD
            BUS_NOPS(FW_DMG_CS_TO_RD_NOPS);
#endif
            REG32(R32_PB_CLR) = PB_RD;
            REG32(R32_PB_CLR) = PB_CLK;
            BUS_NOPS(8);
#if FW_DMG_CS_READ_PAD
            /* Stock's in-window PB_DIR &= ~0xFF at 0x7FC0, hoisted out. */
            BUS_NOPS(FW_DMG_CS_SETTLE_NOPS);
#endif
            out[i] = (uint8_t)(REG32(R32_PB_PIN) & PB_ADDR_HI);
            REG32(R32_PB_OUT) |= PB_CLK;
            REG32(R32_PB_OUT) |= PB_RD;
            REG32(R32_PB_OUT) |= PB_CS;
        } else if (method == DMG_METHOD_A15 || method == DMG_METHOD_SLOW_A15) {
            /* A15 variants, stock 0x7FEE / 0x802E; the address is driven
             * UNMASKED as stock does at 0x7FFA, no `& ~A15' and no uxth. */
            REG32(R32_PA_OUT) = a;
            if (method == DMG_METHOD_SLOW_A15) {
                BUS_NOPS(27);               /* stock 0x803E, and it runs... */
                BUS_NOPS(27);               /* ...twice: the loop at 0x8074  */
            } else {
#if FW_DMG_A15_NOTOGGLE
                /* /RD is /OE and the prologue holds it low, so a changed address
                 * presents new data after the part's access time. Whether the
                 * A15-high edge is a strobe the cartridge needs, or only what
                 * stock happens to emit, is what this measures. */
                BUS_NOPS(FW_DMG_A15_FLAT_NOPS);
#else
                BUS_NOPS(FW_DMG_A15_SETTLE_NOPS);  /* stock 0x7FFC           */
#endif
            }
            out[i] = (uint8_t)(REG32(R32_PB_PIN) & PB_ADDR_HI);
#if !FW_DMG_A15_NOTOGGLE
            REG32(R32_PA_OUT) = a | PA_A15;
#if FW_DMG_A15_PAD
            BUS_NOPS(FW_DMG_A15_HI_NOPS);
#endif
#endif
        } else {
            /* RD, stock 0x7F16: the ordinary cycle. */
            REG32(R32_PA_OUT) = a;
#if FW_DMG_CS_READ_PAD
            BUS_NOPS(FW_DMG_RD_ADDR_NOPS);  /* address -> /RD low, stock 7F26 */
#endif
            REG32(R32_PB_CLR) = PB_RD;
            BUS_NOPS(FW_DMG_RD_SETTLE_NOPS);/* stock 0x7F32                  */
            out[i] = (uint8_t)(REG32(R32_PB_PIN) & PB_ADDR_HI);
            REG32(R32_PB_OUT) |= PB_RD;
        }
    }

    /* Epilogue, stock's shared tail at 0x7F64. /RD left asserted output-enables
     * the ROM into the next bus write. CLK is not restored, as in stock. */
    REG32(R32_PB_OUT) |= PB_RD;
    return count;
}

/* /CS is the cart RAM's chip select (pin 27) and the write cycle is the overlap
 * of /CS and /WR, so /CS must not fall before the data bus has settled. */
#ifndef FW_DMG_WRITE_RAW_PAD
#define FW_DMG_WRITE_RAW_PAD 0
#endif
#ifndef FW_DMG_RAW_DIR_NOPS
#define FW_DMG_RAW_DIR_NOPS  1   /* PB_DIR|=0xFF -> PB_CLR=0xFF, -> 5  */
#endif
#ifndef FW_DMG_RAW_WRCLK_NOPS
#define FW_DMG_RAW_WRCLK_NOPS 1  /* /WR low -> CLK low,          -> 5  */
#endif
#ifndef FW_DMG_RAW_REL_NOPS
#define FW_DMG_RAW_REL_NOPS  3   /* PB_CLR=0xFF -> PB_DIR&=~0xFF,-> 10 */
#endif

static void dmg_write_raw(uint32_t addr, uint8_t value, uint8_t cs_pulse,
                          uint8_t we)
{
    /* Stock sub_80B4 (we == 1) and sub_8790 (any we). D0..D7 are released at
     * the end, or the next read samples what was written. /RD high first, or
     * D0..D7 drive into an output-enabled mask ROM. */
    REG32(R32_PB_OUT) |= PB_RD;

    REG32(R32_PB_OUT) |= PB_CLK;
    BUS_NOPS(11);                           /* stock 0x80D0 */

    /* Arm the address bus here, stock 0x80E6-0x80F0: the only place PA_DIR is
     * restored, while fw_cart_tristate(), power-off and init all clear it.
     * Omitted, the first write after any of those lands nowhere and the device
     * still ACKs. Not in fw_cart_power(), which would drive a stale address. */
    REG32(R32_PA_OUT) = 0;                  /* park,     stock 0x80E8 */
    REG32(R32_PA_DIR) |= PA_AD_MASK;        /* A0..A15 -> out, 0x80EE */
    REG32(R32_PA_OUT) = addr & 0xFFFFu;     /*           stock 0x80F0 */

    REG32(R32_PB_DIR) |= PB_ADDR_HI;        /* D0..D7 -> output */
#if FW_DMG_WRITE_RAW_PAD
    BUS_NOPS(FW_DMG_RAW_DIR_NOPS);          /* -> stock 0x80F6..0x80FC */
#endif
    REG32(R32_PB_CLR) = PB_ADDR_HI;
    REG32(R32_PB_OUT) |= (uint32_t)value & PB_ADDR_HI;

#if !FW_DMG_WRITE_RAW_PAD
    if (cs_pulse) {
        REG32(R32_PB_CLR) = PB_CS;
    }
#endif

    /* we == 3 ("WR+RESET") brackets the /WR pulse with /CS2 (PB19, /RESET in
     * DMG mode), stock 0x87DA, LK.c:1265-1271. we == 0 is treated as /WR. */
    BUS_NOPS(3);                            /* stock 0x8104: 3, not 5 */
#if FW_DMG_WRITE_RAW_PAD
    /* After the pad, where stock puts it (0x818A-0x818E pad, 0x8198 /CS). */
    if (cs_pulse) {
        REG32(R32_PB_CLR) = PB_CS;
    }
#endif
    if (we == FW_DMG_WE_WR_RESET) {
        REG32(R32_PB_CLR) = PB_CS2;         /* stock 0x87DA */
    }
    REG32(R32_PB_CLR) = (we == FW_DMG_WE_AUDIO) ? PB_AUDIO : PB_WR;
#if FW_DMG_WRITE_RAW_PAD
    BUS_NOPS(FW_DMG_RAW_WRCLK_NOPS);        /* -> stock 0x8112..0x8118 */
#endif
    REG32(R32_PB_CLR) = PB_CLK;
    BUS_NOPS(11);                           /* the write pulse, stock 0x811A */
    REG32(R32_PB_OUT) |= PB_CLK;
    REG32(R32_PB_OUT) |= (we == FW_DMG_WE_AUDIO) ? PB_AUDIO : PB_WR;
    if (we == FW_DMG_WE_WR_RESET) {
        REG32(R32_PB_OUT) |= PB_CS2;
    }

    if (cs_pulse) {
        REG32(R32_PB_OUT) |= PB_CS;
    }

    REG32(R32_PB_CLR) = PB_ADDR_HI;         /* stock 0x813C */
#if FW_DMG_WRITE_RAW_PAD
    BUS_NOPS(FW_DMG_RAW_REL_NOPS);          /* -> stock 0x8140..0x814C */
#endif
    REG32(R32_PB_DIR) &= ~PB_ADDR_HI;       /* release D0..D7, stock 0x8146 */
}

/* The ordinary cartridge write: every MBC bank select, RAM-enable and save
 * write, strobing /WR whatever FLASH_WE_PIN says. Merged with the flash write,
 * the 26 "write_pin":"AUDIO" profiles never change bank. */
void fw_cart_dmg_write(uint32_t addr, uint8_t value, uint8_t cs_pulse)
{
    dmg_write_raw(addr, value, cs_pulse, FW_DMG_WE_WR);
}

/* The flashcart write: the one that honours FLASH_WE_PIN. */
void fw_cart_dmg_flash_write(uint32_t addr, uint8_t value, uint8_t cs_pulse)
{
    dmg_write_raw(addr, value, cs_pulse, g_dmg_we);
}

/* The AMD single-write program sequence for one DMG byte as stock's flash write
 * does it: FLASH_METHOD 1 (sub_7BCC) calls sub_8790, not the sub_80B4 that
 * dmg_write_raw() transcribes. Scope is plain /WR, no /CS pulse,
 * flash_commands_bank_1 clear; anything else keeps dmg_write_raw(). [GATED] */
#ifndef FW_DMG_WRITE_BURST
#define FW_DMG_WRITE_BURST 0
#endif

#if FW_DMG_WRITE_BURST

/* Neither pad is the nop count stock emits: stock carries work inside both
 * windows that this routine does not. The invariant is ">= stock", not "==". */
#ifndef FW_DMG_WR_DS_NOPS
#define FW_DMG_WR_DS_NOPS       14u   /* -> 16, stock 0x87C2..0x880E */
#endif
#ifndef FW_DMG_WR_PULSE_NOPS
#define FW_DMG_WR_PULSE_NOPS    11u   /* -> 16, stock 0x880E..0x8850 */
#endif
#ifndef FW_DMG_WR_WRHI_NOPS
#define FW_DMG_WR_WRHI_NOPS     4u   /* -> 6,  stock 0x879A..0x87A2 */
#endif
#ifndef FW_DMG_WR_CLK_NOPS
#define FW_DMG_WR_CLK_NOPS      1u   /* -> 9,  stock 0x87A2..0x87AC */
#endif
#ifndef FW_DMG_WR_AD_NOPS
#define FW_DMG_WR_AD_NOPS       0u   /* -> 6,  stock 0x87AE..0x87B6 */
#endif
#ifndef FW_DMG_WR_DIR_NOPS
#define FW_DMG_WR_DIR_NOPS      2u   /* -> 5,  stock 0x87B6..0x87BC */
#endif

int g_dmg_we_is_wr(void)
{
    return (g_dmg_we == FW_DMG_WE_WR);
}

/* sub_8790, instruction for instruction, for we == /WR. The read-modify-writes
 * are stock's: plain stores shorten the windows this exists to reproduce. */
__attribute__((always_inline))
static inline void dmg_write_stock_flash(uint32_t addr, uint32_t value)
{
    REG32(R32_PB_OUT) |= PB_WR;                 /* 0x879A, no-op in steady state */
    BUS_NOPS(FW_DMG_WR_WRHI_NOPS);
    REG32(R32_PB_CLR)  = PB_CLK;                /* 0x87A2 CLK low, once and done */
    BUS_NOPS(FW_DMG_WR_CLK_NOPS);
    REG32(R32_PA_DIR) |= PA_AD_MASK;            /* 0x87AC, no PA_OUT park       */
    REG32(R32_PA_OUT)  = addr & PA_AD_MASK;     /* 0x87AE  EDGE: address        */
    BUS_NOPS(FW_DMG_WR_AD_NOPS);
    REG32(R32_PB_DIR) |= PB_ADDR_HI;            /* 0x87B6                        */
    BUS_NOPS(FW_DMG_WR_DIR_NOPS);
    REG32(R32_PB_CLR)  = PB_ADDR_HI;            /* 0x87BC  EDGE: data -> 0      */
    REG32(R32_PB_OUT) |= value & PB_ADDR_HI;    /* 0x87C2  EDGE: data valid     */
    BUS_NOPS(FW_DMG_WR_DS_NOPS);
    REG32(R32_PB_CLR)  = PB_WR;                 /* 0x880E  EDGE: /WR low        */
    /* 5 fixed cycles raise /WR again (ldr, orrs, str); the window scales whole
     * to hold stock's 32 MHz pulse width. */
    BUS_NOPS_FIXED(FW_DMG_WR_PULSE_NOPS, 5);
    REG32(R32_PB_OUT) |= PB_WR;                 /* 0x8850  EDGE: /WR high       */
}

void fw_cart_dmg_amd_program_byte(const uint32_t *cmd_addr,
                                  const uint16_t *cmd_val,
                                  uint32_t pa, uint8_t pd)
{
    /* /RD high before anything drives D0..D7; the poll runs between bytes. */
    REG32(R32_PB_OUT) |= PB_RD;

    dmg_write_stock_flash(cmd_addr[0], cmd_val[0]);
    dmg_write_stock_flash(cmd_addr[1], cmd_val[1]);
    dmg_write_stock_flash(cmd_addr[2], cmd_val[2]);
    dmg_write_stock_flash(pa,          pd);

    /* D0..D7 are left DRIVEN, as sub_8790 leaves them; released by
     * fw_cart_dmg_status_poll_open() or fw_cart_dmg_read()'s prologue. */
}

/* Split so a poll pays fw_cart_dmg_read()'s prologue once, not per sample. */
void fw_cart_dmg_status_poll_open(void)
{
    REG32(R32_PA_DIR) |= PA_AD_MASK;
    REG32(R32_PB_DIR) &= ~PB_ADDR_HI;   /* releases what the write burst drove */
    REG32(R32_PB_CLR)  = PB_RD;
    REG32(R32_PB_CLR)  = PB_CLK;
}

uint8_t fw_cart_dmg_status_poll_read(uint32_t addr)
{
    uint8_t v;

    REG32(R32_PA_OUT) = addr & PA_AD_MASK;
    REG32(R32_PB_CLR) = PB_RD;
    BUS_NOPS(8);                        /* stock 0x7F32 */
    v = (uint8_t)(REG32(R32_PB_PIN) & PB_ADDR_HI);
    REG32(R32_PB_OUT) |= PB_RD;
    return v;
}

void fw_cart_dmg_status_poll_close(void)
{
    REG32(R32_PB_OUT) |= PB_RD;
}

/* One AMD write-buffer load: unlock pair, buffer-load at SA, count-1, the data
 * run, then the confirm. Same scope as fw_cart_dmg_amd_program_byte: plain /WR,
 * no /CS pulse, bank-1 commands clear. D0..D7 are left driven; the status poll
 * releases them. [GATED] */
#if !FW_DMG_SHADOW_PB && FW_DMG_WSF_LEAN
#error "FW_DMG_WSF_LEAN needs FW_DMG_SHADOW_PB=1; it only edits the shadow path"
#endif

#if FW_DMG_SHADOW_PB
/* PB_OUT is read-modify-written three times per bus write. Nothing outside this
 * file drives port B, and neither bl_usb_poll() nor the USB handler touches
 * GPIO, so its state is held in a register across one load and stored directly.
 * The peripheral read is what costs; the stores keep the same edge order. The
 * shadow is re-read per load, which bounds staleness to one load.
 * FW_DMG_SHADOW_WRHI/DS/PULSE_PAD put back the cycles the dropped loads held in
 * the three intervals they sat in. */
#ifndef FW_DMG_SHADOW_WRHI_PAD
#define FW_DMG_SHADOW_WRHI_PAD  0u
#endif
#ifndef FW_DMG_SHADOW_DS_PAD
#define FW_DMG_SHADOW_DS_PAD    0u
#endif
#ifndef FW_DMG_SHADOW_PULSE_F
#define FW_DMG_SHADOW_PULSE_F   2
#endif

/* PA_DIR and PB_DIR do not change across a load, but sit inside the per-write
 * sequence as read-modify-writes. Hoisting both to the top of the load removes
 * two of those from each of 37 writes. They sit in intervals the cartridge acts
 * on, CLK-low to address and address to data, so this spends waveform margin:
 * putting the cycles back as nops returns the whole gain. */
#if FW_DMG_DIR_HOIST
#define DMG_WSF_PADIR()  ((void)0)
#define DMG_WSF_PBDIR()  ((void)0)
#else
#define DMG_WSF_PADIR()  REG32(R32_PA_DIR) |= PA_AD_MASK
#define DMG_WSF_PBDIR()  REG32(R32_PB_DIR) |= PB_ADDR_HI
#endif

/* Three stores per write emit nothing the cartridge can see.
 *
 *   /WR high at the top is already high: the previous write ended by raising it,
 *   and the comment on dmg_write_stock_flash calls it a no-op in steady state.
 *   CLK low is "once and done": inside a burst nothing drives it high again, so
 *   36 of 37 clears change no pin.
 *   The data byte takes a PB_CLR of all eight bits and then an OR of the value.
 *   One store of the shadow does both; the intermediate zero is an artefact of
 *   using PB_CLR, not a level the part is waiting for.
 *
 * FW_DMG_WSF_LEAN drops those three and keeps every nop interval, so no window
 * the cartridge measures changes width. The first write of a load still needs
 * the real CLK-low and /WR-high, which the load prologue now does once. */
#if FW_DMG_WSF_LEAN
#define DMG_WSF_SH(a, v) do {                                            \
    BUS_NOPS(FW_DMG_WR_WRHI_NOPS + FW_DMG_SHADOW_WRHI_PAD);              \
    BUS_NOPS(FW_DMG_WR_CLK_NOPS);                                        \
    DMG_WSF_PADIR();                                                     \
    REG32(R32_PA_OUT)  = (a) & PA_AD_MASK;                               \
    BUS_NOPS(FW_DMG_WR_AD_NOPS);                                         \
    DMG_WSF_PBDIR();                                                     \
    BUS_NOPS(FW_DMG_WR_DIR_NOPS);                                        \
    pb = (pb & ~(uint32_t)PB_ADDR_HI) | ((uint32_t)(v) & PB_ADDR_HI);    \
    REG32(R32_PB_OUT) = pb;                                              \
    BUS_NOPS(FW_DMG_WR_DS_NOPS + FW_DMG_SHADOW_DS_PAD);                  \
    REG32(R32_PB_CLR) = PB_WR;          pb &= ~(uint32_t)PB_WR;          \
    BUS_NOPS_FIXED(FW_DMG_WR_PULSE_NOPS + (5 - FW_DMG_SHADOW_PULSE_F),   \
                   FW_DMG_SHADOW_PULSE_F);                               \
    pb |= PB_WR;                        REG32(R32_PB_OUT) = pb;          \
} while (0)
#else
#define DMG_WSF_SH(a, v) do {                                            \
    pb |= PB_WR;                        REG32(R32_PB_OUT) = pb;          \
    BUS_NOPS(FW_DMG_WR_WRHI_NOPS + FW_DMG_SHADOW_WRHI_PAD);              \
    REG32(R32_PB_CLR) = PB_CLK;         pb &= ~(uint32_t)PB_CLK;         \
    BUS_NOPS(FW_DMG_WR_CLK_NOPS);                                        \
    DMG_WSF_PADIR();                                                     \
    REG32(R32_PA_OUT)  = (a) & PA_AD_MASK;                               \
    BUS_NOPS(FW_DMG_WR_AD_NOPS);                                         \
    DMG_WSF_PBDIR();                                                     \
    BUS_NOPS(FW_DMG_WR_DIR_NOPS);                                        \
    REG32(R32_PB_CLR) = PB_ADDR_HI;     pb &= ~(uint32_t)PB_ADDR_HI;     \
    pb |= (uint32_t)(v) & PB_ADDR_HI;   REG32(R32_PB_OUT) = pb;          \
    BUS_NOPS(FW_DMG_WR_DS_NOPS + FW_DMG_SHADOW_DS_PAD);                  \
    REG32(R32_PB_CLR) = PB_WR;          pb &= ~(uint32_t)PB_WR;          \
    BUS_NOPS_FIXED(FW_DMG_WR_PULSE_NOPS + (5 - FW_DMG_SHADOW_PULSE_F),   \
                   FW_DMG_SHADOW_PULSE_F);                               \
    pb |= PB_WR;                        REG32(R32_PB_OUT) = pb;          \
} while (0)
#endif

void fw_cart_dmg_amd_program_buffer(const uint32_t *cmd_addr,
                                    const uint16_t *cmd_val,
                                    uint32_t sa, uint32_t count,
                                    const uint8_t *data)
{
#if FW_DMG_WSF_LEAN
    /* /WR high and CLK low once for the load, where each write repeated them. */
    uint32_t pb = REG32(R32_PB_OUT) | PB_RD | PB_WR;
#else
    uint32_t pb = REG32(R32_PB_OUT) | PB_RD;
#endif
    uint32_t x;

    REG32(R32_PB_OUT) = pb;
#if FW_DMG_WSF_LEAN
    REG32(R32_PB_CLR) = PB_CLK; pb &= ~(uint32_t)PB_CLK;
#endif
#if FW_DMG_DIR_HOIST
    REG32(R32_PA_DIR) |= PA_AD_MASK;
    REG32(R32_PB_DIR) |= PB_ADDR_HI;
#endif

    DMG_WSF_SH(cmd_addr[0], cmd_val[0]);                /* AAA=AA */
    DMG_WSF_SH(cmd_addr[1], cmd_val[1]);                /* 555=55 */
    DMG_WSF_SH(sa,          cmd_val[2]);                /* SA=25  */
    DMG_WSF_SH(sa,          count - 1u);                /* SA=BS  */

    for (x = 0; x < count; x++) {
        DMG_WSF_SH(sa + x, data[x]);                    /* PA=PD  */
    }

    DMG_WSF_SH(sa, cmd_val[5]);                         /* SA=29  */
}
#else
void fw_cart_dmg_amd_program_buffer(const uint32_t *cmd_addr,
                                    const uint16_t *cmd_val,
                                    uint32_t sa, uint32_t count,
                                    const uint8_t *data)
{
    uint32_t x;

    REG32(R32_PB_OUT) |= PB_RD;

    dmg_write_stock_flash(cmd_addr[0], cmd_val[0]);     /* AAA=AA */
    dmg_write_stock_flash(cmd_addr[1], cmd_val[1]);     /* 555=55 */
    dmg_write_stock_flash(sa,          cmd_val[2]);     /* SA=25  */
    dmg_write_stock_flash(sa,          count - 1u);     /* SA=BS  */

    for (x = 0; x < count; x++) {
        dmg_write_stock_flash(sa + x, data[x]);         /* PA=PD  */
    }

    dmg_write_stock_flash(sa, cmd_val[5]);              /* SA=29  */
}
#endif

/* AMD unlock bypass. Between enter and exit a program is A0 then the byte, so
 * the two unlock writes drop off every byte in the run. The mode persists across
 * reads, so the status poll runs inside it. Anything else the chip is asked to
 * do, erase included, needs the exit first. */
void fw_cart_dmg_amd_bypass_enter(const uint32_t *cmd_addr,
                                  const uint16_t *cmd_val)
{
    REG32(R32_PB_OUT) |= PB_RD;
    dmg_write_stock_flash(cmd_addr[0], cmd_val[0]);     /* AAA=AA */
    dmg_write_stock_flash(cmd_addr[1], cmd_val[1]);     /* 555=55 */
    dmg_write_stock_flash(cmd_addr[0], 0x20u);          /* AAA=20 */
}

void fw_cart_dmg_amd_bypass_byte(uint16_t a0, uint32_t pa, uint8_t pd)
{
    REG32(R32_PB_OUT) |= PB_RD;
    dmg_write_stock_flash(pa, a0);                      /* PA=A0  */
    dmg_write_stock_flash(pa, pd);                      /* PA=PD  */
}

void fw_cart_dmg_amd_bypass_exit(uint32_t pa)
{
    REG32(R32_PB_OUT) |= PB_RD;
    dmg_write_stock_flash(pa, 0x90u);
    dmg_write_stock_flash(pa, 0x00u);
}

/* Release what the burst left driven: the error exit and the end of a block. */
void fw_cart_dmg_write_burst_release(void)
{
    REG32(R32_PB_CLR)  = PB_ADDR_HI;
    REG32(R32_PB_DIR) &= ~PB_ADDR_HI;
}
#endif  /* FW_DMG_WRITE_BURST */



/* FLASH_PULSE_RESET: /RESET (PB19) low between the program write and the
 * completion poll. Counts are stock's 0x7D1C-0x7DAC; LK.c:1820-1829. */
void fw_cart_dmg_pulse_reset(void)
{
    fw_cart_delay_nops(BUS_SCALE(270u));    /* stock 0x7D22-0x7D5C */
    REG32(R32_PB_CLR) = PB_CS2;             /* /RESET low          */
    fw_cart_delay_nops(BUS_SCALE(81u));     /* stock 0x7D62-0x7DA6 */
    REG32(R32_PB_OUT) |= PB_CS2;            /* stock 0x7DA8        */
}

void fw_cart_dmg_mbc_reset(void)
{
    /* PB19 is /RESET in DMG mode. The mapper comes up in bank 1. */
    REG32(R32_PB_CLR) = PB_CS2;
    fw_cart_settle();
    REG32(R32_PB_OUT) |= PB_CS2;
    fw_cart_settle();
}

void fw_cart_clk_pulses(uint32_t n)
{
    /* Stock 0x97E6. Required for DMG: MBC3+RTC detection uses it. */
    while (n--) {
        REG32(R32_PB_CLR) = PB_CLK;
        BUS_NOPS(11);
        REG32(R32_PB_OUT) |= PB_CLK;
        BUS_NOPS(11);
    }
}

/* A GBA cartridge's save chip is NOT on the ROM bus: an 8-bit device behind
 * /CS2 (PB19, DMG's /RESET), addressed by A0..A15 on PA with data on PB0..PB7,
 * the lines that carry A16..A23 during a ROM read. Stock sub_65D8 (read),
 * sub_6980 (write), sub_A5A4 (turnaround, 1 to enter and 2 to leave).
 * Turnaround order is a constraint: entering parks the data lines low, releases
 * them to inputs, then arms the address bus; leaving parks the address before
 * re-driving D0..D7. Either reversed puts our outputs on the save chip's. */

void fw_cart_agb_sram_open(void)
{
    /* stock sub_A5A4(1) at 0xA5D4 */
    REG32(R32_PB_CLR) = PB_ADDR_HI;         /* park D0..D7 low first        */
    REG32(R32_PB_DIR) &= ~PB_ADDR_HI;       /* then release them as inputs  */
    REG32(R32_PA_DIR) |= PA_AD_MASK;        /* A0..A15 -> output            */
}

void fw_cart_agb_sram_close(void)
{
    /* stock sub_A5A4(2) at 0xA5B2; address parked before D0..D7 turn round. */
    REG32(R32_PA_OUT) = 0;
    REG32(R32_PB_CLR) = PB_ADDR_HI;
    REG32(R32_PA_DIR) |= PA_AD_MASK;
    REG32(R32_PB_DIR) |= PB_ADDR_HI;
}

uint32_t fw_cart_agb_sram_read(uint32_t addr, uint8_t *out, uint32_t count)
{
    uint32_t i;

    /* Prologue, stock 0x65E6; the loop raises /RD again per byte. */
    REG32(R32_PB_CLR) = PB_RD;

    for (i = 0; i < count; i++) {
        uint32_t a = (addr + i) & 0xFFFFu;

        REG32(R32_PB_OUT) |= PB_RD;
        REG32(R32_PA_DIR) |= PA_AD_MASK;
        REG32(R32_PA_OUT) = a;
        REG32(R32_PB_CLR) = PB_CS2;         /* select the save chip         */
        REG32(R32_PB_CLR) = PB_RD;
        BUS_NOPS(8);                        /* stock 0x6634, counted        */
        REG32(R32_PB_DIR) &= ~PB_ADDR_HI;   /* D0..D7 in, stock 0x6644      */
        out[i] = (uint8_t)(REG32(R32_PB_PIN) & PB_ADDR_HI);
        REG32(R32_PB_OUT) |= PB_RD;
        REG32(R32_PB_OUT) |= PB_CS2;
    }

    /* A save chip left output-enabled fights the next driver of D0..D7. */
    REG32(R32_PB_OUT) |= PB_RD;
    return count;
}

void fw_cart_agb_sram_write(uint32_t addr, uint8_t value)
{
    /* stock sub_6980. The read path holds /RD low across a whole block. */
    REG32(R32_PB_OUT) |= PB_RD;

    REG32(R32_PB_DIR) |= PB_ADDR_HI;        /* D0..D7 -> output             */
    REG32(R32_PA_DIR) |= PA_AD_MASK;
    REG32(R32_PA_OUT) = addr & 0xFFFFu;
    REG32(R32_PB_CLR) = PB_ADDR_HI;
    REG32(R32_PB_OUT) |= (uint32_t)value & PB_ADDR_HI;

    REG32(R32_PB_CLR) = PB_CS2;
    REG32(R32_PB_CLR) = PB_WR;
    BUS_NOPS(8);                            /* stock 0x69CC, counted        */
    REG32(R32_PB_OUT) |= PB_WR;
    REG32(R32_PB_OUT) |= PB_CS2;
}

/* Program a GBA FLASH save chip, stock sub_6764. Method 1 is per byte AA@5555,
 * 55@2AAA, A0@5555, the byte, wait. Method 2 is the Atmel AT29LV512, which the
 * host picks only for chip 0x1F3D (LK_Device.py:3459): one unlock per 128-byte
 * sector, the sector, a much longer wait. Method 2 has never run. */
void fw_cart_agb_sram_program(uint32_t addr, const uint8_t *data, uint32_t len,
                              uint8_t method)
{
    uint32_t i;
    uint32_t j;

    fw_cart_agb_sram_open();

    if (method == 2u) {
        /* addr is ALREADY a sector index, not a byte address: the host passes
         * `int(pos/128)' (LK_Device.py:3651). Shifted again, the first 4 KiB
         * looks right and nothing above it is written. stock 0x677A. */
        uint32_t sector = addr;
        for (i = 0; i + 128u <= len; i += 128u) {
            fw_cart_agb_sram_write(0x5555u, 0xAAu);
            fw_cart_agb_sram_write(0x2AAAu, 0x55u);
            fw_cart_agb_sram_write(0x5555u, 0xA0u);
            for (j = 0; j < 128u; j++) {
                fw_cart_agb_sram_write(((sector << 7) & 0xFFFFu) | j,
                                       data[i + j]);
            }
            sector++;
            /* An AT29LV512 page program is 10-20 ms (stock 0x6898). Short,
             * the next unlock is swallowed and written as data instead. */
            {
                uint32_t deadline = bl_time_ms() + 15u;
                while ((int32_t)(bl_time_ms() - deadline) < 0) {
                }
            }
        }
        /* Partial pages are dropped, not half-programmed; the chip writes
         * whole pages and the host always sends whole ones. */
        fw_cart_agb_sram_close();
        return;
    }

    for (i = 0; i < len; i++) {
        uint32_t a = (addr + i) & 0xFFFFu;
        uint8_t got = (uint8_t)~data[i];

/* One turnaround per byte instead of four: /RD high and both direction stores
 * are idempotent, and only the readback poll turns D0..D7 back around. The gap
 * between one /CS2 rising edge and the next drops to about 0.6 us; a 128 KiB
 * restore goes 5.90 to 5.44 s on an S29GL256N with a 1M FLASH save. */
#ifndef FW_AGB_SAVE_CMD_INLINE
#define FW_AGB_SAVE_CMD_INLINE  1
#endif
#if FW_AGB_SAVE_CMD_INLINE
#define SRAM_CMD(a_, v_) do {                                           \
            REG32(R32_PA_OUT) = (a_) & 0xFFFFu;                         \
            REG32(R32_PB_CLR) = PB_ADDR_HI;                             \
            REG32(R32_PB_OUT) |= (uint32_t)(v_) & PB_ADDR_HI;           \
            REG32(R32_PB_CLR) = PB_CS2;                                 \
            REG32(R32_PB_CLR) = PB_WR;                                  \
            BUS_NOPS(8);                    /* stock 0x69CC, counted */ \
            REG32(R32_PB_OUT) |= PB_WR;                                 \
            REG32(R32_PB_OUT) |= PB_CS2;                                \
        } while (0)
        REG32(R32_PB_OUT) |= PB_RD;
        REG32(R32_PB_DIR) |= PB_ADDR_HI;
        REG32(R32_PA_DIR) |= PA_AD_MASK;
        SRAM_CMD(0x5555u, 0xAAu);
        SRAM_CMD(0x2AAAu, 0x55u);
        SRAM_CMD(0x5555u, 0xA0u);
        SRAM_CMD(a, data[i]);
#undef SRAM_CMD
#else
        fw_cart_agb_sram_write(0x5555u, 0xAAu);
        fw_cart_agb_sram_write(0x2AAAu, 0x55u);
        fw_cart_agb_sram_write(0x5555u, 0xA0u);
        fw_cart_agb_sram_write(a, data[i]);
#endif

        /* Poll; do not go back to stock's fixed ~17 us typical byte time; an
         * MX29L010 needs longer back to back. Short, the next unlock is
         * swallowed and written as data with every command still ACKed.
         * The cap is in iterations, so a faster Fsys shortens the timeout. */
        for (j = 0; j < BUS_SCALE(400u) && got != data[i]; j++) {
/* Backoff between polls of the save flash. Swept 27/13/4/0 against a 128 KiB
 * restore: 6.06/6.05/6.10/6.03 s, each reading back identical. The chip's
 * 46 us a byte is the restore, so shortening does not pay; raise it for a save
 * chip that needs a longer backoff. */
#ifndef FW_AGB_SAVE_POLL_NOPS
#define FW_AGB_SAVE_POLL_NOPS 27
#endif
            BUS_NOPS_RAW(FW_AGB_SAVE_POLL_NOPS);
            (void)fw_cart_agb_sram_read(a, &got, 1u);
        }
    }

    fw_cart_agb_sram_close();
}

/* A GBA EEPROM save is a one-wire serial device on AD0, clocked by /WR going
 * out and /RD coming back. Stock sub_6E38. Command is read = 11, write = 10 in
 * the top two bits; address is 6 bits for a 4K part and 14 for a 64K one; data
 * is 64 bits MSB first after four dummy read clocks. The host picks the size
 * with the selector on 0xC5/0xC6, 2 meaning 64K (LK_Device.py:3456-3457). */

#define EEP_AD0     0x0001u             /* the serial line is AD0 */

/* The part has no chip select and lives in ROM address space, so both stock
 * routines (0x64BC, 0x6698) park the bus on its address before any frame.
 * Omitted on the write path, 493 of 512 bytes land wrong. */
static void eeprom_bus_open(void)
{
    REG32(R32_PA_DIR) |= PA_AD_MASK;
    REG32(R32_PB_DIR) |= PB_ADDR_HI;
    REG32(R32_PA_OUT) = 0xFF80u;
    REG32(R32_PB_CLR) = PB_ADDR_HI;
    REG32(R32_PB_OUT) |= PB_ADDR_HI;
}

static void eeprom_begin(void)
{
    /* stock 0x6E50-0x6E70 */
    REG32(R32_PB_CLR) = PB_CS;          /* select                            */
    REG32(R32_PB_OUT) |= PB_CLK;
    REG32(R32_PB_CLR) = PB_CLK;
    REG32(R32_PA_DIR) |= EEP_AD0;       /* AD0 driven by us                  */
}

/* Shift `nbits` of `word` out MSB first, /WR clocked. stock 0x6E78-0x6EB2. */
static void eeprom_send(uint32_t word, uint32_t nbits)
{
    uint32_t i;

    for (i = 0; i < nbits; i++) {
        if (word & (1u << (nbits - 1u - i))) {
            REG32(R32_PA_OUT) |= EEP_AD0;
        } else {
            REG32(R32_PA_CLR) = EEP_AD0;
        }
        REG32(R32_PB_CLR) = PB_WR;
        REG32(R32_PB_OUT) |= PB_WR;
    }
}

void fw_cart_agb_eeprom_bus(void)
{
    eeprom_bus_open();
}

void fw_cart_agb_eeprom_read(uint32_t addr, uint8_t *out, uint8_t selector)
{
    uint32_t wide = (selector == 2u);
    uint32_t nbits = wide ? 16u : 8u;
    uint32_t word = (wide ? 0xC000u : 0xC0u) | (addr & (wide ? 0x3FFFu : 0x3Fu));
    uint32_t i;

    eeprom_begin();
    eeprom_send(word, nbits);

    /* Close the address phase, turn the line round. stock 0x6EBE-0x6EEC. */
    REG32(R32_PA_CLR) = EEP_AD0;
    REG32(R32_PB_CLR) = PB_WR;
    REG32(R32_PB_OUT) |= PB_WR;
    REG32(R32_PB_OUT) |= PB_CS;
    REG32(R32_PA_DIR) &= ~EEP_AD0;      /* AD0 driven by the EEPROM now      */
    REG32(R32_PB_CLR) = PB_CS;

    /* Four dummy clocks the part inserts before data. stock 0x6EF2-0x6F04. */
    for (i = 0; i < 4u; i++) {
        REG32(R32_PB_CLR) = PB_RD;
        REG32(R32_PB_OUT) |= PB_RD;
    }

    /* Stock samples PA_PIN AFTER /RD goes back high (0x6F1C). */
    for (i = 0; i < 64u; i++) {
        uint32_t bit;
        if ((i & 7u) == 0u) {
            out[i >> 3] = 0u;
        }
        REG32(R32_PB_CLR) = PB_RD;
        REG32(R32_PB_OUT) |= PB_RD;
        bit = REG32(R32_PA_PIN) & EEP_AD0;
        out[i >> 3] |= (uint8_t)(bit << (7u - (i & 7u)));
    }

    REG32(R32_PB_OUT) |= PB_CS;         /* deselect, stock 0x6F3C            */
    REG32(R32_PA_DIR) |= EEP_AD0;       /* and take the line back, 0x6F46    */
}

/* Write one 64-bit EEPROM word, stock sub_6694 driving the sub_6E38 mode-4
 * frame (0x6F50-0x6FA4) then the shared tail (0x6FA6-0x6FBC). The wait is not
 * optional; a single-word read is not repeatable, so read-back cannot replace it. */
void fw_cart_agb_eeprom_write(uint32_t addr, const uint8_t *data,
                              uint8_t selector)
{
    uint32_t wide = (selector == 2u);
    uint32_t nbits = wide ? 16u : 8u;
    uint32_t word = (wide ? 0x8000u : 0x80u) | (addr & (wide ? 0x3FFFu : 0x3Fu));
    uint32_t i;

    eeprom_begin();
    eeprom_send(word, nbits);

    /* 64 data bits, MSB first within each byte. stock 0x6F58-0x6F90. */
    for (i = 0; i < 64u; i++) {
        if (data[i >> 3] & (1u << (7u - (i & 7u)))) {
            REG32(R32_PA_OUT) |= EEP_AD0;
        } else {
            REG32(R32_PA_CLR) = EEP_AD0;
        }
        REG32(R32_PB_CLR) = PB_WR;
        REG32(R32_PB_OUT) |= PB_WR;
    }

    /* The stop bit: a clocked zero, stock 0x6F92-0x6FA4. */
    REG32(R32_PA_CLR) = EEP_AD0;
    REG32(R32_PB_CLR) = PB_WR;
    REG32(R32_PB_OUT) |= PB_WR;

    /* Shared tail: deselect, then pulse CLK. stock 0x6FA6-0x6FBC. */
    REG32(R32_PB_OUT) |= PB_CS;
    REG32(R32_PB_OUT) |= PB_CLK;
    REG32(R32_PB_CLR) = PB_CLK;

    fw_cart_settle();               /* 10 ms; the part is programming */
}

/* The 64 MiB "3D Memory" mapper on the GBA Video carts. FlashGBX asks with
 * 0xC8 and never checks support (hw_GBFlash.py:174). Stock sub_62FC. [GATED] */

#define M3D_REG(n)      (0x4000C0u + (n))       /* halfword addresses */

/* AGB cartridge boot sequence, stock sub_5F7C, run by the host on every AGB
 * power-on (LK_Device.py:892-894). A cartridge that needs it answers open bus,
 * which looks like an empty slot. The order of the reads is the protocol. */

/* stock 0xB348: 4 groups of 4 runs of 6 halfword addresses. */
static const uint16_t agb_boot_addr[96] = {
    0x479B, 0x7426, 0x11BC, 0x6D4F, 0x11BD, 0x32F1,
    0x7FD9, 0x2CE7, 0x5DA5, 0x11BD, 0x4610, 0x5DA4,
    0x4E90, 0x6173, 0x2A84, 0x4E91, 0x106A, 0x75FE,
    0x29C8, 0x7839, 0x420E, 0x5D1B, 0x7838, 0x12A8,
    0x3F7D, 0x67B9, 0x26F3, 0x54EF, 0x7C23, 0x26F2,
    0x6BC6, 0x4137, 0x15AB, 0x730D, 0x6BC7, 0x3B4F,
    0x5F24, 0x3DDA, 0x253F, 0x1749, 0x3DDB, 0x70E6,
    0x746C, 0x30F7, 0x531F, 0x6738, 0x531E, 0x1A51,
    0x1971, 0x5B7D, 0x4ED6, 0x1970, 0x3F27, 0x75CB,
    0x3D62, 0x128C, 0x74B8, 0x2FAD, 0x74B9, 0x64FD,
    0x6C9A, 0x4F3A, 0x276D, 0x73EF, 0x38B1, 0x4F3B,
    0x571E, 0x7EA3, 0x6249, 0x3587, 0x1B7C, 0x3586,
    0x7AFB, 0x67E4, 0x5C92, 0x67E5, 0x2BCA, 0x438C,
    0x2E6F, 0x587F, 0x14B7, 0x2E6E, 0x4CB9, 0x6FA2,
    0x38F0, 0x719E, 0x475A, 0x1F3C, 0x6AD8, 0x475B,
    0x5199, 0x3264, 0x7B41, 0x49EF, 0x5198, 0x1CD7,
};

void fw_cart_agb_bootup(void)
{
    uint32_t a;
    uint32_t chk = 0;
    uint32_t idx;
    uint32_t g;

    /* stock 0x5F7E-0x5FD8; the hold is 27 nops. */
    REG32(R32_PB_CLR) = PB_WR;
    REG32(R32_PB_CLR) = PB_RD;
    BUS_NOPS(27);
    REG32(R32_PB_OUT) |= PB_RD;
    REG32(R32_PB_OUT) |= PB_WR;

    (void)fw_cart_agb_peek(0x5Au);
    for (a = 0xFFFFF0u; a <= 0xFFFFF9u; a++) {
        (void)fw_cart_agb_peek(a);
    }

    /* Header bytes 0x9D..0xB7; 0x9C is skipped. stock 0x601C-0x6056. */
    for (a = 0x9Cu; a < 0xB8u; a += 2u) {
        uint16_t w = fw_cart_agb_peek(a >> 1);
        uint32_t k;
        for (k = 0; k < 2u; k++) {
            uint32_t byte;
            if (a == 0x9Cu && k == 0u) {
                continue;
            }
            byte = (w >> (k * 8u)) & 0xFFu;
            chk = (chk >> 3) | (chk << 29);     /* ROR 3 */
            chk ^= byte;
            chk ^= byte << 8;
            chk ^= byte << 16;
            chk ^= byte << 24;
        }
    }

    /* stock 0x6058-0x606E. */
    idx = ((fw_cart_agb_peek(0x4Fu) & 3u) * 24u) + (((chk >> 3) & 3u) * 6u);

    for (a = 2u; a < 0x2Au; a++) {
        (void)fw_cart_agb_peek(a);
    }

    /* Six table-selected addresses, each with the next run of ten from 0x2A,
     * then a seventh run with no table read. stock 0x6080-0x60D2. */
    for (g = 0; g < 7u; g++) {
        uint32_t j;
        if (g < 6u) {
            (void)fw_cart_agb_peek(agb_boot_addr[idx]);
            idx++;
        }
        for (j = g * 10u; j < (g + 1u) * 10u; j++) {
            (void)fw_cart_agb_peek(0x2Au + j);
        }
    }

    (void)fw_cart_agb_peek(0u);
    (void)fw_cart_agb_peek(1u);
}

/* AGB GPIO real-time clock (S3511), mapped into ROM space past the header:
 * halfword 0x62 data (bit0 SCK, bit1 SIO, bit2 CS), 0x63 direction (1 = the
 * cartridge drives), 0x64 control (bit0 makes the registers readable). Serial,
 * MSB-first out and LSB-first in, clocked on SCK's rising edge. The repeated
 * identical register writes below are stock's clock-low and settle widths
 * (0x6CB0-0x6CE4, 0x6CF4-0x6D20); collapsing them changes the bit period. */
#define RTC_DATA    0x62u
#define RTC_DIR     0x63u
#define RTC_CTRL    0x64u

/* Shift one byte out, MSB first. stock sub_6CA8. */
static void rtc_send(uint8_t byte)
{
    uint32_t i;

    for (i = 0; i < 8u; i++) {
        uint16_t sio = (uint16_t)(((byte >> (7u - i)) & 1u) << 1);
        fw_cart_agb_write(RTC_DATA, (uint16_t)(sio | 4u));   /* SCK low */
        fw_cart_agb_write(RTC_DATA, (uint16_t)(sio | 4u));
        fw_cart_agb_write(RTC_DATA, (uint16_t)(sio | 4u));
        fw_cart_agb_write(RTC_DATA, (uint16_t)(sio | 5u));   /* SCK high */
    }
}

/* Shift one byte in, LSB first. stock sub_6CEE. */
static uint8_t rtc_recv(void)
{
    uint32_t i;
    uint8_t v = 0;

    for (i = 0; i < 8u; i++) {
        uint16_t b;
        fw_cart_agb_write(RTC_DATA, 4u);        /* SCK low, held */
        fw_cart_agb_write(RTC_DATA, 4u);
        fw_cart_agb_write(RTC_DATA, 4u);
        fw_cart_agb_write(RTC_DATA, 4u);
        fw_cart_agb_write(RTC_DATA, 4u);
        fw_cart_agb_write(RTC_DATA, 5u);        /* SCK high: the cart presents */
        b = (uint16_t)((fw_cart_agb_peek(RTC_DATA) >> 1) & 1u);
        v = (uint8_t)((v >> 1) | (b << 7));
    }
    return v;
}

/* Enable the window, drive SCK/SIO/CS, send `cmd`, release SIO. 0x6D42. */
static void rtc_begin(uint8_t cmd)
{
    fw_cart_agb_write(RTC_CTRL, 1u);            /* registers readable */
    fw_cart_agb_write(RTC_DATA, 1u);
    fw_cart_agb_write(RTC_DATA, 5u);
    fw_cart_agb_write(RTC_DIR, 7u);             /* SCK, SIO, CS all outputs */
    rtc_send(cmd);
    fw_cart_agb_write(RTC_DIR, 5u);             /* SIO becomes an input */
}

/* Drop CS and close the window. stock 0x6D7A-0x6D8C and 0x6E18-0x6E2C. */
static void rtc_end(void)
{
    fw_cart_agb_write(RTC_DATA, 1u);
    fw_cart_agb_write(RTC_DATA, 1u);
    fw_cart_agb_write(RTC_CTRL, 0u);
}

/* Eight bytes, a layout contract with AGB_GPIO.HasRTC (Mapper.py:125-157).
 * out[0] is the status register, bit 7 meaning the clock lost power. out[1..6]
 * must come from a read with the register window ENABLED; the host compares
 * them against its own window-disabled read. Commands are S3511's 0110 rrr d. */
void fw_cart_agb_rtc_read(uint8_t *out)
{
    uint32_t j;

    rtc_begin(0x63u);                   /* read status register */
    out[0] = rtc_recv();
    rtc_end();

    rtc_begin(0x65u);                   /* read date and time */
    for (j = 0; j < 7u; j++) {
        out[1u + j] = rtc_recv();
    }
    rtc_end();
}

/* The window is opened once per PAGE, not once per read: it auto-increments, so
 * re-programming the mapper per 0xC8 restarts it and the dump becomes the first
 * 512 bytes of each page eight times over. The host sends eight 0xC8s then a
 * bare 0x00 to close the page, which page_terminator_due in proto.c eats. */
#ifndef FW_M3D_SETTLE_ITERS
#define FW_M3D_SETTLE_ITERS     0x190u   /* stock 0x6352-0x6392, 27 nops each */
#endif

void fw_cart_agb_3d_open(uint32_t hwaddr, uint16_t buffer_size)
{
    uint32_t i;

    /* Program the mapper, stock 0x62FE-0x634E, through the AGB word write. */
    fw_cart_agb_write(M3D_REG(2), (uint16_t)((hwaddr << 1) & 0xFFFFu));
    fw_cart_agb_write(M3D_REG(3), (uint16_t)((hwaddr << 1) >> 16));
    fw_cart_agb_write(M3D_REG(4), 0x1000u);
    fw_cart_agb_write(M3D_REG(5), 0x0800u);
    fw_cart_agb_write(M3D_REG(6), (uint16_t)(buffer_size >> 9));
    fw_cart_agb_write(M3D_REG(7), 0x0000u);
    fw_cart_agb_write(M3D_REG(0), 0x0011u);
    fw_cart_agb_write(M3D_REG(1), 0x0000u);

    for (i = 0; i < (uint32_t)FW_M3D_SETTLE_ITERS; i++) {
        BUS_NOPS(27);
    }

    /* The address is FIXED at halfword 0x800, not the caller's. stock 0x6394. */
    REG32(R32_PA_DIR) |= PA_AD_MASK;
    REG32(R32_PB_DIR) |= PB_ADDR_HI;
    REG32(R32_PA_OUT) = 0x0800u;
    REG32(R32_PB_CLR) = PB_ADDR_HI;
    REG32(R32_PB_CLR) = PB_CS;
    REG32(R32_PA_OUT) = 0;
    REG32(R32_PB_CLR) = PB_ADDR_HI;
    REG32(R32_PA_DIR) &= ~PA_AD_MASK;
}

uint32_t fw_cart_agb_3d_read(uint8_t *out, uint32_t count)
{
    uint32_t i;
    uint32_t n = count & ~1u;

    /* 11 nops per /RD where an ordinary burst uses 5 (stock 0x6414): the
     * mapper is slower. No latch, the window self-advances. */
    for (i = 0; i < n; i += 2u) {
        uint32_t w;
        REG32(R32_PB_CLR) = PB_RD;
        BUS_NOPS(11);
        w = REG32(R32_PA_PIN) & PA_AD_MASK;
        REG32(R32_PB_OUT) |= PB_RD;
        out[i]      = (uint8_t)w;
        out[i + 1u] = (uint8_t)(w >> 8);
    }
    return n;
}

void fw_cart_agb_3d_close(void)
{
    REG32(R32_PB_OUT) |= PB_CS;
}

