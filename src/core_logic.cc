// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#include "core_logic.h"
#include "attestation.h"
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

	// Key distribution (one-time at startup)
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

	// Remote attestation (one-shot, triggered at network connection).
	//
	// Authentic-execution path: core_logic (no capabilities) -> attestation
	// (sole holder of the SPI-flash capability; measures the booted slot) ->
	// back to core_logic -> comms (sole holder of the MQTT handle; publishes).
	//
	// For now we publish only the raw image measurement (the bare hash of the
	// booted slot's ELF) on plantnode/attestation, so the verifier can confirm
	// the running firmware by recomputing the same hash offline. The full signed
	// quote (attestation_quote -> fake_tpm) stays available for the next step,
	// when a verifier nonce arrives on plantnode/commands.
	{
		uint8_t imageHash[AttestationImageHashLength];
		size_t  hashLen = 0;
		int     ret     = attestation_measure_image(imageHash, &hashLen);
		if (ret != 0)
		{
			Debug::log("attestation_measure_image failed: {}", ret);
		}
		else
		{
			ret = comms_publish_attestation(imageHash, hashLen);
			if (ret != 0)
			{
				Debug::log("comms_publish_attestation failed: {}", ret);
			}
			else
			{
				Debug::log("Image hash published ({} bytes)", hashLen);
			}
		}
	}

	// Timestamp of the most recent pump activation; surfaced in every
	// telemetry payload via the reading's last_watering field. 0 = never.
	uint32_t lastWatering = 0;

	while (true)
	{
		SensorReading reading{};

		data_read_sensors(&reading);

		PolicyOutcome out = policy_evaluate(&reading);

		switch (out)
		{
			case PolicyOutcome::NoAction:
				break;
			case PolicyOutcome::TempAlert:
				break;
			case PolicyOutcome::PumpActivation:
				// Record the watering; reported in the telemetry payload below.
				lastWatering = reading.timestamp;
				break;
			default:
				Debug::log("outcome not recognized: {}", out);
				break;
		}

		// Publish telemetry (comms encrypts via crypto compartment)
		{
			reading.last_watering = lastWatering;
			int ret               = comms_publish_telemetry(&reading);
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
