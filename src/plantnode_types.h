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
	uint16_t moisture_raw; // Seesaw 0x36: raw capacitance (dry≈200, wet≈2000)
	int16_t  temperature_cx10; // AHT20 0x38:  °C × 10  (e.g. 235 = 23.5 °C)
	uint16_t humidity_rx10;    // AHT20 0x38:  %RH × 10 (e.g. 455 = 45.5 %RH)
	uint32_t timestamp;        // UNIX timestamp at time of reading
	uint32_t last_watering; // UNIX timestamp of most recent pump activation (0
	                        // = never)
	bool valid;             // false if either sensor read failed
};

/**
 * Outcome returned by the policy engine after evaluating a reading.
 */
enum class PolicyOutcome : uint8_t
{
	NoAction,
	PumpActivation,
	TempAlert,
};

/**
 * Maximum length of the device attestation identifier string (incl. NUL).
 */
static constexpr size_t DeviceIdMaxLength = 64;

/** Maximum length for detached signature produced by attestation compartment */
static constexpr size_t AttestationSignatureMaxLength = 64; // hydro_sign_BYTES

/** Length of the firmware-image measurement (hydro_hash_BYTES). */
static constexpr size_t AttestationImageHashLength = 32;

/** Length of the verifier-supplied freshness challenge. */
static constexpr size_t AttestationNonceLength = 32;

/** Length of the digest that fake_tpm actually signs (hydro_hash_BYTES). */
static constexpr size_t AttestationDigestLength = 32;

/**
 * Remote-attestation quote produced by the attestation compartment.
 *
 * Pointer-free plain data, safe for cross-compartment transfer and for
 * serialisation onto the wire. The verifier reconstructs the signed digest
 * from these fields (see attestation_quote_digest) and checks `signature`
 * against the device public key, then compares `image_hash` to the expected
 * hash of the audited firmware build.
 */
struct AttestationQuote
{
	uint8_t slot;          // booted software-slot index ("slot 2" == index 1)
	uint8_t device_id_len; // bytes of device_id actually in use
	char    device_id[DeviceIdMaxLength];           // e.g. "plantnode-001"
	uint8_t image_hash[AttestationImageHashLength]; // hydro_hash of loaded ELF
	uint8_t nonce[AttestationNonceLength];          // verifier freshness nonce
	uint8_t signature[AttestationSignatureMaxLength]; // fake_tpm signature
};

/**
 * Serialised wire length of an AttestationQuote:
 *   slot(1) + device_id_len(1) + device_id + image_hash + nonce + signature.
 */
static constexpr size_t AttestationQuoteWireMaxLength =
  1 + 1 + DeviceIdMaxLength + AttestationImageHashLength +
  AttestationNonceLength + AttestationSignatureMaxLength;

/**
 * Remote-attestation handshake message-type tags. The first plaintext byte of
 * every (secretbox-encrypted) RA message on plantnode/attestation. Both
 * directions share the topic; the directional session keys ensure a party only
 * decrypts the other side's messages, so the verifier (V→D) and device (D→V)
 * tag spaces are independent.
 */
// Verifier -> device:
static constexpr uint8_t RaChallenge1 = 0x01; // payload: nonce_V[32]
static constexpr uint8_t RaChallenge2 = 0x02; // payload: combined[32]
static constexpr uint8_t RaApproved   = 0x03; // payload: combined[32] (verdict OK)
// Device -> verifier:
static constexpr uint8_t RaNonceReply = 0x01; // payload: nonce_V[32] ‖ nonce_D[32]
static constexpr uint8_t RaQuote      = 0x02; // payload: serialised AttestationQuote

/**
 * 8-byte hydro_hash context used to combine the verifier and device nonces into
 * the single nonce the quote is bound to. Must match the verifier exactly.
 *   combined = hydro_hash(ctx="PN-COMB1", nonce_V ‖ nonce_D, 32)
 */
static constexpr char AttestationCombineContext[8] = {
  'P', 'N', '-', 'C', 'O', 'M', 'B', '1'};

/**
 * Largest RA handshake plaintext: 1-byte type tag + a fully serialised quote.
 */
static constexpr size_t RaPlaintextMaxLength = 1 + AttestationQuoteWireMaxLength;
