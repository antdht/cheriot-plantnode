// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#include "policy_engine.h"
#include "display/display_policy.h"
#include <debug.hh>
#include <errno.h>

using Debug = ConditionalDebug<true, "PlantNode Policy">;

static uint16_t sMoistureLow  = 300;
static uint16_t sMoistureHigh = 1500;

PolicyOutcome __cheri_compartment("policy_engine")
  policy_evaluate(uint16_t moistureRaw, uint32_t timestamp)
{
	(void)timestamp; // not consulted by the current decision logic

	if (moistureRaw < sMoistureLow)
	{
		Debug::log("Moisture too low ({}), activating pump.", moistureRaw);
		display_pump_activation(true);
		// TODO: pump_on() for a short watering pulse (~2-3 s), then record the
		// watering timestamp so we can throttle re-watering with a minimum
		// interval instead of waiting for a high-moisture reading.
		return PolicyOutcome::PumpActivation;
	}

	return PolicyOutcome::NoAction;
}

int __cheri_compartment("policy_engine")
  policy_set_thresholds(uint16_t moistureLow, uint16_t moistureHigh)
{
	if (moistureLow >= moistureHigh)
	{
		return -EINVAL;
	}

	sMoistureLow  = moistureLow;
	sMoistureHigh = moistureHigh;
	return 0;
}
