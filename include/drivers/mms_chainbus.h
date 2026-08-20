/* SPDX-License-Identifier: Apache-2.0 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_CHAINBUS_H_
#define ZEPHYR_INCLUDE_DRIVERS_CHAINBUS_H_

#include <zephyr/types.h>
#include <zephyr/drivers/gpio.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Claim a physical slot for a specific HAT compatible string.
 *
 * @param compatible DT compatible string of the HAT (e.g., "mms3,relay-hat")
 * @param position Desired static position, or -1 for dynamic match
 * @return Assigned physical slot index (>= 0), or negative error code
 */
int chainbus_claim_slot(const char *compatible, int position);

/**
 * @brief Assert or de-assert the CS GPIO for a specific physical slot.
 *
 * @param slot Physical slot index (0..7)
 * @param active True to drive line active, false to drive inactive
 * @return 0 on success, negative error code on failure
 */
int chainbus_assert_cs(int slot, bool active);

/**
 * @brief Get the GPIO specifier associated with a physical slot.
 *
 * @param slot Physical slot index
 * @return Pointer to gpio_dt_spec, or NULL if slot is invalid
 */
const struct gpio_dt_spec *chainbus_get_cs_spec(int slot);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_CHAINBUS_H_ */
