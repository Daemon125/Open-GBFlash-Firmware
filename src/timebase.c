/* Free-running SysTick millisecond clock, TICKINT off; caller contract in
 * include/timebase.h. RVR is 0x00FFFFFF, not a 1 ms reload: COUNTFLAG says
 * "wrapped" and not how many times, and USB init and a CodeFlash erase both
 * outlast one reload. */

#include "bl_config.h"
#include "timebase.h"

_Static_assert((uint32_t)BL_FSYS_HZ == (uint32_t)BL_TIME_FSYS_HZ,
               "BL_TIME_FSYS_HZ disagrees with bl_config.h's BL_FSYS_HZ");

_Static_assert(BL_TIME_CYCLES_PER_US * 1000000u == BL_TIME_FSYS_HZ,
               "Fsys is not a whole number of cycles per microsecond");
#if !BL_TIME_CYC_IS_POW2
/* Overestimating drives the carried remainder negative and time runs fast. */
_Static_assert(BL_TIME_US_RECIP * BL_TIME_CYCLES_PER_US <= 0x10000u,
               "BL_TIME_US_RECIP overestimates 1/cycles-per-microsecond");
_Static_assert(BL_TIME_US_BLOCK * BL_TIME_CYCLES_PER_US <= 0x10000u,
               "BL_TIME_US_BLOCK would peel more cycles than it counts");
#endif
#if BL_TIME_CYC_IS_POW2
_Static_assert((1u << BL_TIME_US_SHIFT) == BL_TIME_CYCLES_PER_US,
               "BL_TIME_US_SHIFT disagrees with BL_TIME_CYCLES_PER_US");
#endif

_Static_assert(BL_TIME_SYST_MAX == 0x00FFFFFFu, "SysTick is a 24-bit counter");

/* Vector 15 is fw_fault_handler (vectors.S:29); TICKINT faults on first reload. */
_Static_assert((BL_TIME_CSR_RUN & BL_TIME_CSR_TICKINT) == 0u,
               "SysTick must never raise an exception in the bootloader");

_Static_assert(BL_TIME_MAX_GAP_MS >= 100u,
               "one lap of SysTick is too short to bridge a flash blackout");

static uint32_t tb_ms;
static uint32_t tb_us;      /* 0..999, microseconds not yet a millisecond     */
static uint32_t tb_cyc;     /* < BL_TIME_CYCLES_PER_US, not yet a microsecond */
static uint32_t tb_last;    /* the previous SYST_CVR sample, 24 bits          */
static uint32_t tb_running;

void bl_time_init(void)
{
    /* Idempotent: restarting the epoch runs a held timestamp's arithmetic back. */
    if (tb_running != 0u) {
        return;
    }

    BL_REG32(BL_TIME_SYST_CSR) = 0u;
    BL_REG32(BL_TIME_SYST_RVR) = BL_TIME_SYST_MAX;
    BL_REG32(BL_TIME_SYST_CVR) = 0u;

    tb_ms  = 0u;
    tb_us  = 0u;
    tb_cyc = 0u;

    BL_REG32(BL_TIME_SYST_CSR) = BL_TIME_CSR_RUN;

    tb_last    = BL_REG32(BL_TIME_SYST_CVR) & BL_TIME_SYST_MAX;
    tb_running = 1u;
}

uint32_t bl_time_ms(void)
{
    uint32_t now;
    uint32_t elapsed;
    uint32_t us;
    uint32_t ms;

    /* SYST_CVR's reset value is UNKNOWN (ARMv6-M B3.3.3) and a disabled SysTick
     * keeps its last count: the first call would report up to one lap. */
    if (tb_running == 0u) {
        return tb_ms;
    }

    now = BL_REG32(BL_TIME_SYST_CVR) & BL_TIME_SYST_MAX;

    elapsed = (tb_last - now) & BL_TIME_SYST_MAX;
    tb_last = now;

    /* No divide: -nostdlib, so there is no __aeabi_uidiv. */
    elapsed += tb_cyc;
#if BL_TIME_CYC_IS_POW2
    tb_cyc   = elapsed & (BL_TIME_CYCLES_PER_US - 1u);
    us       = (elapsed >> BL_TIME_US_SHIFT) + tb_us;
#else
    us = tb_us;
    while (elapsed >= 0x10000u) {
        us      += BL_TIME_US_BLOCK;
        elapsed -= BL_TIME_US_BLOCK * BL_TIME_CYCLES_PER_US;
    }
    {
        uint32_t q = (elapsed * BL_TIME_US_RECIP) >> 16;
        uint32_t r = elapsed - (q * BL_TIME_CYCLES_PER_US);
        while (r >= BL_TIME_CYCLES_PER_US) {
            q++;
            r -= BL_TIME_CYCLES_PER_US;
        }
        tb_cyc = r;
        us    += q;
    }
#endif

    ms = tb_ms;
    while (us >= BL_TIME_US_PER_MS) {
        us -= BL_TIME_US_PER_MS;
        ms++;
    }
    tb_ms = ms;
    tb_us = us;

    return ms;
}

