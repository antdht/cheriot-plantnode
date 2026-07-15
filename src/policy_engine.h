// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#pragma once

#include "plantnode_types.h"
#include <compartment.h>
#include <stdint.h>

/**
 * Evaluate a moisture reading against the current thresholds and act
 * accordingly (e.g. activate the pump via the pump driver).
 *
 * This is the ONLY compartment permitted to call the pump_driver compartment.
 *
 * @param moistureRaw  The latest raw moisture reading.
 * @param timestamp    UNIX timestamp at which the reading was taken.
 *
 * Returns the PolicyOutcome that was applied.
 */
PolicyOutcome __cheri_compartment("policy_engine")
  policy_evaluate(uint16_t moistureRaw, uint32_t timestamp);

/**
 * Update the decision thresholds used by the policy engine.
 *
 * @param moisture_low   Moisture raw value below which the pump activates.
 * @param moisture_high  Moisture raw value above which the pump deactivates.
 *
 * Returns 0 on success, negative errno on failure.
 */
int __cheri_compartment("policy_engine")
  policy_set_thresholds(uint16_t moistureLow, uint16_t moistureHigh);
