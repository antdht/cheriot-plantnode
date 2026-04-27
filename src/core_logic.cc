// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#include "core_logic.h"
#include "comms.h"
#include "crypto.h"
#include "data_processing.h"
#include "plantnode_types.h"
#include "policy_engine.h"
#include <debug.hh>
#include <fail-simulator-on-error.h>
#include <thread.h>
#include <tick_macros.h>

using Debug = ConditionalDebug<true, "PlantNode">;

void __cheri_compartment("core_logic") core_entry()
{
	Debug::log("=== PlantNode core starting ===");

	comms_connect();

	// ── Key distribution (one-time at startup) ────────────────────────────
	// Perform Noise-N step 1: derive session TX/RX keys and publish packet1
	// so the verifier can recover session keys and decrypt telemetry.
	{
		uint8_t kxPacket[CryptoKxPacketLen];
		size_t  kxLen = 0;
		int     ret   = crypto_init_session(kxPacket, &kxLen);
		if (ret != 0)
		{
			Debug::log("crypto_init_session failed: {}", ret);
		}
		else
		{
			ret = comms_publish_key_packet(kxPacket, kxLen);
			if (ret != 0)
			{
				Debug::log("comms_publish_key_packet failed: {}", ret);
			}
			else
			{
				Debug::log("Key packet published ({} bytes)", kxLen);
			}
		}
	}

	// TODO: configure policy thresholds from persistent storage

	while (true)
	{
		SensorReading reading{};

		data_read_sensors(&reading);

		// policy_evaluate(&reading);

		// ── Publish telemetry (comms encrypts via crypto compartment) ─────
		{
			int ret = comms_publish_telemetry(&reading);
			if (ret != 0)
			{
				Debug::log("comms_publish_telemetry failed: {}", ret);
			}
		}

		comms_poll();

		Timeout t{MS_TO_TICKS(10000)};
		thread_sleep(&t);
	}
}
