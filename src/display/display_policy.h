// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#pragma once

// Display functions callable ONLY by the policy_engine compartment.
//
// CHERIoT enforcement: including only this header means policy_engine
// receives a sealed cross-compartment call capability solely for
// display_pump_activation(). It cannot call functions in display_comms.h
// or display_data.h.

#include <compartment.h>
#include <stdbool.h>

/**
 * Show a pump activation or deactivation notice on screen.
 *
 * @param activating  true  -> pump is being turned ON  (watering started)
 *                    false -> pump is being turned OFF (watering done)
 *
 * Called by policy_engine when the pump state changes.
 */
void __cheri_compartment("display") display_pump_activation(bool activating);
