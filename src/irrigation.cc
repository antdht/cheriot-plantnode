// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT
#include "irrigation.h"
#include "drivers/moisture_sensor.h"
#include "policy_engine.h"
#include <debug.hh>
#include <errno.h>
#include <fail-simulator-on-error.h>
#include <locks.hh>
#include <sntp.h>
#include <thread.h>
#include <tick_macros.h>

using Debug = ConditionalDebug<true, "PlantNode Control">;

namespace
{
	FlagLockPriorityInherited sMailboxLock;
	uint32_t                  sLastWatering = 0; // most recent pump activation
} // namespace

int __cheri_compartment("irrigation")
  irrigation_get_last_watering(uint32_t *out)
{
	if (out == nullptr)
	{
		return -EINVAL;
	}
	LockGuard g{sMailboxLock};
	*out = sLastWatering;
	return 0;
}

void __cheri_compartment("irrigation") irrigation_loop()
{
	Debug::log("=== PlantNode control loop starting (prio 3) ===");
	while (true)
	{
		uint16_t moistureRaw = 0;
		// POC: fake, self-decrementing reading (see moisture_read_raw_mock) so
		// the watering pulse / LED can be exercised without real hardware.
		int ret = moisture_read_raw_mock(&moistureRaw);
		if (ret < 0)
		{
			Debug::log("Moisture read failed: {}", ret);
		}
		else
		{
			struct timeval tv;
			uint32_t       timestamp = (gettimeofday(&tv, nullptr) == 0)
			                             ? static_cast<uint32_t>(tv.tv_sec)
			                             : 0;

			PolicyOutcome outcome = policy_evaluate(moistureRaw, timestamp);
			switch (outcome)
			{
				case PolicyOutcome::PumpActivation:
				{
					LockGuard g{sMailboxLock};
					sLastWatering = timestamp;
					break;
				}
				case PolicyOutcome::NoAction:
					break;
				default:
					Debug::log("outcome not recognized: {}", outcome);
					break;
			}
		}

		// Control cadence: faster than telemetry; loop sleeps (yields CPU to
		// the network stack) between ticks.
		Timeout t{MS_TO_TICKS(6000)};
		thread_sleep(&t);
	}
}
