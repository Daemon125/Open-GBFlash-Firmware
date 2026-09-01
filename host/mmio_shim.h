/* Force-included ahead of src/cart.c (-DREG32) and src/lk_glue.c
 * (-DLKDEV_REG32) so those macro overrides expand into a real declaration. */
#ifndef FW_MMIO_SHIM_H
#define FW_MMIO_SHIM_H
#include <stdint.h>
uint32_t *fw_test_reg(uintptr_t a);
void fw_test_reset(void);
uint32_t fw_test_get(uintptr_t a);
void fw_test_set(uintptr_t a, uint32_t v);
uint32_t fw_test_oob(void);
#endif
