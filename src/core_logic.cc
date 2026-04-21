// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#include "attestation.h"
#include "comms.h"
#include "core_logic.h"
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

	// TODO: configure policy thresholds from persistent storage

	while (true)
	{
		SensorReading reading{};

		// WARNING: commented to debug comms
		// data_read_sensors(&reading);

		// WARNING: commented to debug comms
		// policy_evaluate(&reading);

		// 3. Sign the reading for authenticated telemetry
		uint8_t sig[AttestationSignatureMaxLength];
		size_t  sig_len = 0;
		attestation_sign(&reading, sizeof(reading), sig, &sig_len);

		// 4. Publish telemetry to monitoring station
		comms_publish_telemetry(&reading);

		// 5. Publish attestation blob to remote verifier
		if (sig_len > 0)
		{
			comms_publish_attestation(sig, sig_len);
		}

		// 6. Drive MQTT event loop (process incoming commands)
		comms_poll();

		Timeout t{MS_TO_TICKS(30000)};
		thread_sleep(&t);
	}
}
