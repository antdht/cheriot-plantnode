// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#pragma once

#include <compartment.h>

/**
 * Telemetry thread entry point for the plant node.
 *
 * Owns the telemetry application thread (priority 1). Reads moisture,
 * temperature and humidity directly from the sensor compartments each tick
 * and publishes them via comms, together with the most recent
 * pump-activation timestamp read from the control_loop mailbox. Holds no
 * hardware capabilities itself; device access is mediated entirely by the
 * moisture_sensor and temperature_sensor compartments.
 */
void __cheri_compartment("core_logic") telemetry_entry();
