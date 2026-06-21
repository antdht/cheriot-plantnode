// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#include "policy_engine.h"
#include "display/display_policy.h"
#include <debug.hh>
#include <errno.h>

using Debug = ConditionalDebug<true, "PlantNode Policy">;

static uint16_t sMoistureLow  = 300;
static uint16_t sMoistureHigh = 1500;
static int16_t  sTempMaxCx10  = 280; // 40.0 °C

PolicyOutcome __cheri_compartment("policy_engine")
  policy_evaluate(const SensorReading *reading)
{
	if (!reading || !reading->valid)
	{
		return PolicyOutcome::NoAction;
	}

	if (reading->temperatureCx10 > sTempMaxCx10)
	{
		Debug::log("Temperature threshold exceeded: {} (max {})",
		           reading->temperatureCx10,
		           sTempMaxCx10);
		return PolicyOutcome::TempAlert;
	}

	if (reading->moistureRaw < sMoistureLow)
	{
		Debug::log("Moisture too low ({}), activating pump.",
		           reading->moistureRaw);
		display_pump_activation(true);
		// TODO: pump_on() for a short watering pulse (~2-3 s), then record the
		// watering timestamp so we can throttle re-watering with a minimum
		// interval instead of waiting for a high-moisture reading.
		return PolicyOutcome::PumpActivation;
	}

	return PolicyOutcome::NoAction;
}

int __cheri_compartment("policy_engine")
  policy_set_thresholds(uint16_t moistureLow,
                        uint16_t moistureHigh,
                        int16_t  tempMaxCx10)
{
	if (moistureLow >= moistureHigh)
	{
		return -EINVAL;
	}

	sMoistureLow  = moistureLow;
	sMoistureHigh = moistureHigh;
	sTempMaxCx10  = tempMaxCx10;
	return 0;
}
