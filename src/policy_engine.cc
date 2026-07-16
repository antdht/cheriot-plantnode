// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#include "policy_engine.h"
#include "display/display_policy.h"
#include "drivers/pump_driver.h"
#include <debug.hh>
#include <errno.h>
#include <thread.h>

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
		// Watering pulse: pump_on() is a placeholder (no relay pin wired yet,
		// returns -ENOSYS) but does light an onboard LED as a POC. This call
		// blocks the irrigation thread for ~2.5s while the pulse runs; see
		// DESIGN.md for the tradeoff.
		// TODO: once the relay pin exists, replace this blocking pulse with a
		// non-blocking timer so watering doesn't stall the sense/policy
		// cadence.
		pump_on();
		thread_millisecond_wait(2500);
		pump_off();
		display_pump_activation(false);
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
