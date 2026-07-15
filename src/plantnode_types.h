// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Aggregated sensor reading passed between compartments.
 */
struct SensorReading
{
	uint16_t moistureRaw; // Seesaw 0x36: raw capacitance (dry≈200, wet≈2000)
	int16_t  temperatureCx10; // AHT20 0x38:  °C × 10  (e.g. 235 = 23.5 °C)
	uint16_t humidityRx10;    // AHT20 0x38:  %RH × 10 (e.g. 455 = 45.5 %RH)
	uint32_t timestamp;       // UNIX timestamp at time of reading
	uint32_t lastWatering; // UNIX timestamp of most recent pump activation (0
	                       // = never)
	bool valid;            // false if either sensor read failed
};

/**
 * Outcome returned by the policy engine after evaluating a reading.
 */
enum class PolicyOutcome : uint8_t
{
	NoAction,
	PumpActivation,
};

/**
 * Maximum length of the device identifier string (incl. NUL).
 */
static constexpr size_t DeviceIdMaxLength = 64;

/** Length of the opaque attestation token the mock flow produces. */
static constexpr size_t AttestationTokenLength = 32;

/**
 * Attestation evidence produced by the (mock) attestation compartment.
 *
 * NOTE: this is mock data — see attestation.h. The token is an opaque blob: it
 * deliberately stands in for "whatever a real attestation would attach" without
 * modelling any particular measurement or signature scheme. Pointer-free plain
 * data, safe for cross-compartment transfer. core_logic reads these fields to
 * build the plaintext JSON "am_i_attested" query on plantnode/attestation.
 */
struct AttestationEvidence
{
	uint8_t deviceIdLen;                   // bytes of deviceId actually in use
	char    deviceId[DeviceIdMaxLength];   // e.g. "plantnode-001"
	uint8_t token[AttestationTokenLength]; // opaque mock attestation token
};
