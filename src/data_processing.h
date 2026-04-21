// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#pragma once

#include "plantnode_types.h"
#include <compartment.h>

/**
 * Read both sensors, apply any calibration, and populate @p out.
 *
 * This is the ONLY compartment permitted to call the moisture_sensor and
 * temperature_sensor driver compartments.
 *
 * @param out  Caller-allocated SensorReading to fill.
 *
 * Returns 0 on success, negative errno on failure. On failure out->valid is
 * set to false.
 */
int __cheri_compartment("data_processing")
  data_read_sensors(SensorReading *out);
