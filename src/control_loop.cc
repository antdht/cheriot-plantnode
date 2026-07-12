// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT
#include "control_loop.h"
#include "data_processing.h"
#include "policy_engine.h"
#include <debug.hh>
#include <errno.h>
#include <fail-simulator-on-error.h>
#include <locks.hh>
#include <thread.h>
#include <tick_macros.h>

using Debug = ConditionalDebug<true, "PlantNode Control">;

namespace
{
	FlagLockPriorityInherited sMailboxLock;
	SensorReading             sLatest{};
	bool                      sHaveReading  = false;
	uint32_t                  sLastWatering = 0; // most recent pump activation
} // namespace

int __cheri_compartment("control_loop")
  control_get_latest_reading(SensorReading *out)
{
	if (out == nullptr)
	{
		return -EINVAL;
	}
	LockGuard g{sMailboxLock};
	if (!sHaveReading)
	{
		return -EAGAIN;
	}
	*out = sLatest;
	return 0;
}

void __cheri_compartment("control_loop") control_entry()
{
	Debug::log("=== PlantNode control loop starting (prio 3) ===");
	while (true)
	{
		SensorReading reading{};
		data_read_sensors(&reading);

		PolicyOutcome out = policy_evaluate(&reading);
		switch (out)
		{
			case PolicyOutcome::PumpActivation:
				sLastWatering = reading.timestamp;
				break;
			case PolicyOutcome::NoAction:
			case PolicyOutcome::TempAlert:
				break;
			default:
				Debug::log("outcome not recognized: {}", out);
				break;
		}
		reading.lastWatering = sLastWatering;

		{
			LockGuard g{sMailboxLock};
			sLatest      = reading; // overwrite; never blocks on telemetry
			sHaveReading = true;
		}

		// Control cadence: faster than telemetry; loop sleeps (yields CPU to
		// the network stack) between ticks.
		Timeout t{MS_TO_TICKS(6000)};
		thread_sleep(&t);
	}
}
