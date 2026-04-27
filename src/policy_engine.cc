// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#include "policy_engine.h"
#include "display/display_policy.h"
#include "drivers/pump_driver.h"
#include <debug.hh>
#include <errno.h>

using Debug = ConditionalDebug<true, "PlantNode Policy">;

static uint16_t s_moisture_low  = 300;
static uint16_t s_moisture_high = 700;
static int16_t  s_temp_max_cx10 = 400; // 40.0 °C

PolicyOutcome __cheri_compartment("policy_engine")
  policy_evaluate(const SensorReading *reading)
{
	if (!reading || !reading->valid)
	{
		return PolicyOutcome::NoAction;
	}

	if (reading->temperature_cx10 > s_temp_max_cx10)
	{
		Debug::log("Temperature threshold exceeded: {} (max {})",
		           reading->temperature_cx10,
		           s_temp_max_cx10);
		return PolicyOutcome::SendAlert;
	}

	if (reading->moisture_raw < s_moisture_low)
	{
		Debug::log("Moisture too low ({}), activating pump.",
		           reading->moisture_raw);
		display_pump_activation(true);
		// TODO: pump_on();
		return PolicyOutcome::ActivatePump;
	}

	if (reading->moisture_raw > s_moisture_high)
	{
		Debug::log("Moisture sufficient ({}), deactivating pump.",
		           reading->moisture_raw);
		display_pump_activation(false);
		// TODO: pump_off();
		return PolicyOutcome::DeactivatePump;
	}

	return PolicyOutcome::NoAction;
}

int __cheri_compartment("policy_engine")
  policy_set_thresholds(uint16_t moisture_low,
                        uint16_t moisture_high,
                        int16_t  temp_max_cx10)
{
	if (moisture_low >= moisture_high)
	{
		return -EINVAL;
	}

	s_moisture_low  = moisture_low;
	s_moisture_high = moisture_high;
	s_temp_max_cx10 = temp_max_cx10;
	return 0;
}
