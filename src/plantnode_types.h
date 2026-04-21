// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/**
 * Aggregated sensor reading passed between compartments.
 * Plain data — no pointers — safe for cross-compartment transfer.
 */
struct SensorReading
{
	uint16_t moisture_raw;      // Seesaw 0x36: raw capacitance (dry≈200, wet≈2000)
	int16_t  temperature_cx10; // AHT20 0x38:  °C × 10  (e.g. 235 = 23.5 °C)
	uint16_t humidity_rx10;    // AHT20 0x38:  %RH × 10 (e.g. 455 = 45.5 %RH)
	uint32_t timestamp;        // UNIX timestamp at time of reading
	bool     valid;            // false if either sensor read failed
};

/**
 * Outcome returned by the policy engine after evaluating a reading.
 */
enum class PolicyOutcome : uint8_t
{
	NoAction,
	ActivatePump,
	DeactivatePump,
	SendAlert,
};

/**
 * Maximum length of the device attestation identifier string (incl. NUL).
 */
static constexpr size_t DeviceIdMaxLength = 64;

/**
 * Maximum length of a detached signature produced by the attestation
 * compartment.
 */
static constexpr size_t AttestationSignatureMaxLength = 64;
