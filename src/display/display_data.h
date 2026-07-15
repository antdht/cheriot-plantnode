// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#pragma once

// Display functions callable ONLY by the core_logic compartment.
//
// CHERIoT enforcement: including only this header means core_logic
// receives a sealed cross-compartment call capability solely for
// display_sensor_readings(). It cannot call functions in display_comms.h
// or display_policy.h.

#include "../plantnode_types.h"
#include <compartment.h>

/**
 * Show the latest sensor readings on screen.
 * Called by core_logic after a successful sensor read cycle.
 */
void __cheri_compartment("display")
  display_sensor_readings(const SensorReading *reading);
