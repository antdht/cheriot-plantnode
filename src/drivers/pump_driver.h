// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#pragma once

#include <compartment.h>
#include <stdbool.h>

/**
 * Driver for the irrigation pump, controlled via GPIO.
 * Sole holder of the GPIO MMIO capability for the pump pin.
 * Only policy_engine is permitted to call this compartment.
 */

/**
 * Activate the pump.
 *
 * Returns 0 on success, negative errno on failure.
 */
int __cheri_compartment("pump_driver") pump_on();

/**
 * Deactivate the pump.
 *
 * Returns 0 on success, negative errno on failure.
 */
int __cheri_compartment("pump_driver") pump_off();

/**
 * Query the current pump state.
 *
 * @param state_out  Set to true if the pump is currently active.
 *
 * Returns 0 on success, negative errno on failure.
 */
int __cheri_compartment("pump_driver") pump_get_state(bool *state_out);
