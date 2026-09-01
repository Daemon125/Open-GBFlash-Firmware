/* RAM stand-in for the CH579 GPIO block so src/cart.c compiles and runs on the
 * host; -DREG32(...) redirects every REG32() in cart.c here.
 *
 * Not a peripheral model but a flat array of 32-bit slots indexed by address: a
 * write to PB_CLR does not clear PB_OUT bits. CLR slots accumulate instead of
 * replacing, so a read answers "bits driven low at any point in this call", not
 * "the last line to touch the register". Both over-report what was driven and
 * never under-report it, so a check can fail falsely but not pass falsely. One
 * gap: nothing here lowers a PB_OUT bit, so an edit raising one between the AGB
 * read loops' snapshot and its `PB_OUT = hi` store would be missed. */
#include <stdint.h>
#include <string.h>
#include "mmio_shim.h"

#define BASE  0x40001080u
#define SLOTS 32u

#define IDX(a)  (((a) - BASE) / 4u)
#define I_PA_CLR IDX(0x400010ACu)
#define I_PB_CLR IDX(0x400010CCu)

static uint32_t regs[SLOTS];
static uint32_t clr_acc[SLOTS];
static uint32_t oob;

/* Fold what the last handed-out pointer left in a CLR slot into its
 * accumulator. Every access routes through here, so no intermediate is lost. */
static void fold(void)
{
    clr_acc[I_PA_CLR] |= regs[I_PA_CLR];
    clr_acc[I_PB_CLR] |= regs[I_PB_CLR];
    regs[I_PA_CLR] = 0u;
    regs[I_PB_CLR] = 0u;
}

uint32_t *fw_test_reg(uintptr_t a)
{
    uint32_t i = (uint32_t)((a - BASE) / 4u);
    fold();
    if (a < BASE || i >= SLOTS) {
        return &oob;
    }
    return &regs[i];
}

void fw_test_reset(void)
{
    memset(regs, 0, sizeof(regs));
    memset(clr_acc, 0, sizeof(clr_acc));
    oob = 0;
}

uint32_t fw_test_get(uintptr_t a)
{
    uint32_t i = (uint32_t)((a - BASE) / 4u);
    fold();
    if (a < BASE || i >= SLOTS) {
        return oob;
    }
    return (i == I_PA_CLR || i == I_PB_CLR) ? clr_acc[i] : regs[i];
}

void fw_test_set(uintptr_t a, uint32_t v) { fold(); *fw_test_reg(a) = v; }
uint32_t fw_test_oob(void)   { fold(); return oob; }

/* fw_cart_settle() spins until this advances, so it must not return a constant. */
static uint32_t fake_ms;
uint32_t bl_time_ms(void) { return fake_ms += 5u; }
