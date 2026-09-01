/* Millisecond clock for the polled update loop. bl_time_ms() must be called at
 * least once per BL_TIME_MAX_GAP_MS (419 ms at the shipping 40 MHz, 524 at 32) or the SysTick lap is lost and
 * time under-counts by that much. Do not count COUNTFLAG on a 1 ms reload:
 * flash stalls the core for milliseconds and the sticky flag hides all but one. */

#ifndef BL_TIMEBASE_H
#define BL_TIMEBASE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fsys, set in src/start.S. Do not pull in bl_config.h; include/usb.h needs this alone. */
#ifndef BL_TIME_FSYS_HZ
#define BL_TIME_FSYS_HZ         32000000u
#endif

#define BL_TIME_CYCLES_PER_US   ((BL_TIME_FSYS_HZ) / 1000000u)
#define BL_TIME_US_PER_MS       1000u

/* Non-power-of-two Fsys/1e6 takes the reciprocal path in bl_time_ms():
 * BL_TIME_US_RECIP is the Q16 multiplier, BL_TIME_US_BLOCK the microseconds
 * peeled per pass to keep that multiply inside 32 bits. Same expansion,
 * different roles. Shifting anyway made every deadline 20% short at 40 MHz. */
#define BL_TIME_CYC_IS_POW2     (((BL_TIME_CYCLES_PER_US) & \
                                  ((BL_TIME_CYCLES_PER_US) - 1u)) == 0u)
#if BL_TIME_CYC_IS_POW2
#define BL_TIME_US_SHIFT        5u      /* log2(BL_TIME_CYCLES_PER_US)        */
#else
#define BL_TIME_US_RECIP        (0x10000u / (BL_TIME_CYCLES_PER_US))
#define BL_TIME_US_BLOCK        (0x10000u / (BL_TIME_CYCLES_PER_US))
#endif

#define BL_TIME_SYST_CSR        0xE000E010u
#define BL_TIME_SYST_RVR        0xE000E014u
#define BL_TIME_SYST_CVR        0xE000E018u

#define BL_TIME_SYST_MAX        0x00FFFFFFu

#define BL_TIME_CSR_ENABLE      (1u << 0)
#define BL_TIME_CSR_TICKINT     (1u << 1)       /* never set, polled          */
#define BL_TIME_CSR_CLKSOURCE   (1u << 2)       /* 1 = processor clock        */

#define BL_TIME_CSR_RUN         (BL_TIME_CSR_ENABLE | BL_TIME_CSR_CLKSOURCE)

#define BL_TIME_MAX_GAP_MS      (((BL_TIME_SYST_MAX + 1u) \
                                  / BL_TIME_CYCLES_PER_US) / BL_TIME_US_PER_MS)

void bl_time_init(void);        /* idempotent; will not restart a live epoch */
uint32_t bl_time_ms(void);      /* wraps at 2^32 ms; compare differences     */

#ifdef __cplusplus
}
#endif

#endif /* BL_TIMEBASE_H */
