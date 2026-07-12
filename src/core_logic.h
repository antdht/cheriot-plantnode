// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#pragma once

#include <compartment.h>

/**
 * Telemetry thread entry point for the plant node.
 *
 * Owns the telemetry application thread (priority 1). Reads the latest
 * SensorReading from the control_loop mailbox and publishes it via comms.
 * Holds no hardware capabilities itself.
 */
void __cheri_compartment("core_logic") telemetry_entry();
