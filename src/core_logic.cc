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
#include <string.h>
#include <thread.h>
#include <tick_macros.h>

using Debug = ConditionalDebug<true, "PlantNode">;

namespace
{

	// Remote-attestation handshake state. The verifier drives a dual-nonce
	// challenge-response over plantnode/attestation; this device side runs:
	//   Idle --RaChallenge1--> AwaitingCombined --RaChallenge2--> (quote) -->
	//   Idle
	enum class RaState
	{
		Idle,
		AwaitingCombined,
		AwaitingApproval,
	};

	RaState s_raState = RaState::Idle;
	uint8_t s_nonceV[AttestationNonceLength]; // verifier nonce from challenge 1
	uint8_t s_nonceD[AttestationNonceLength]; // our own nonce sent in the reply
	uint8_t
	  s_lastCombined[AttestationNonceLength]; // combined nonce of last quote

	// Latched once the verifier approves a quote. The board withholds all
	// telemetry until this is set, so no sensor data leaves the device until it
	// has proven what firmware it is running and the verifier has accepted it.
	bool s_attested = false;

	// RA scratch buffers kept off the (small, TLS-shared) core_logic stack.
	// Safe as statics: only the single core_logic thread touches them, and the
	// handshake is strictly sequential (one message handled at a time).
	uint8_t s_raPlain[RaPlaintextMaxLength];  // plaintext [type][payload]
	uint8_t s_raEnc[CryptoRaEncryptedMaxLen]; // secretbox-encrypted message
	uint8_t s_raWire[AttestationQuoteWireMaxLength]; // serialised quote
	uint8_t s_raDrain[256];     // most recent message drained from comms
	AttestationQuote s_raQuote; // quote being built

	// Handle one decrypted RA message (plaintext = [type][payload]). Runs in
	// the core_logic loop, never inside the MQTT callback, so publishing a
	// reply does not re-enter mqtt_run.
	void handle_ra_message(const uint8_t *plain, size_t len)
	{
		if (len < 1)
		{
			return;
		}
		uint8_t        type       = plain[0];
		const uint8_t *payload    = plain + 1;
		size_t         payloadLen = len - 1;

		switch (type)
		{
			case RaChallenge1:
			{
				if (payloadLen != AttestationNonceLength)
				{
					Debug::log("RA challenge1: bad payload length {}",
					           payloadLen);
					return;
				}
				memcpy(s_nonceV, payload, AttestationNonceLength);
				if (crypto_gen_nonce(s_nonceD, AttestationNonceLength) != 0)
				{
					Debug::log("RA challenge1: nonce generation failed");
					return;
				}

				// Reply: [RaNonceReply][nonce_V][nonce_D]
				size_t msgLen       = 0;
				s_raPlain[msgLen++] = RaNonceReply;
				memcpy(s_raPlain + msgLen, s_nonceV, AttestationNonceLength);
				msgLen += AttestationNonceLength;
				memcpy(s_raPlain + msgLen, s_nonceD, AttestationNonceLength);
				msgLen += AttestationNonceLength;

				size_t encLen = 0;
				if (crypto_encrypt_bytes(s_raPlain, msgLen, s_raEnc, &encLen) !=
				    0)
				{
					Debug::log("RA challenge1: encrypt reply failed");
					return;
				}
				if (comms_publish_attestation(s_raEnc, encLen) != 0)
				{
					Debug::log("RA challenge1: publish reply failed");
					return;
				}
				s_raState = RaState::AwaitingCombined;
				Debug::log("RA: nonce reply sent, awaiting combined nonce");
				break;
			}

			case RaChallenge2:
			{
				if (s_raState != RaState::AwaitingCombined)
				{
					Debug::log(
					  "RA challenge2: unexpected (no pending challenge1)");
					return;
				}
				if (payloadLen != AttestationNonceLength)
				{
					Debug::log("RA challenge2: bad payload length {}",
					           payloadLen);
					s_raState = RaState::Idle;
					return;
				}

				uint8_t expected[AttestationNonceLength];
				if (crypto_combine_nonce(s_nonceV, s_nonceD, expected) != 0)
				{
					Debug::log("RA challenge2: combine failed");
					s_raState = RaState::Idle;
					return;
				}
				if (memcmp(expected, payload, AttestationNonceLength) != 0)
				{
					Debug::log("RA challenge2: combined nonce mismatch");
					s_raState = RaState::Idle;
					return;
				}

				// Freshness proven on both sides — measure, build and sign a
				// quote bound to the combined nonce.
				if (attestation_quote(
				      expected, AttestationNonceLength, &s_raQuote) != 0)
				{
					Debug::log("RA challenge2: attestation_quote failed");
					s_raState = RaState::Idle;
					return;
				}

				size_t wireLen =
				  attestation_quote_serialize(&s_raQuote, s_raWire);

				size_t msgLen       = 0;
				s_raPlain[msgLen++] = RaQuote;
				memcpy(s_raPlain + msgLen, s_raWire, wireLen);
				msgLen += wireLen;

				size_t encLen = 0;
				if (crypto_encrypt_bytes(s_raPlain, msgLen, s_raEnc, &encLen) !=
				    0)
				{
					Debug::log("RA challenge2: encrypt quote failed");
					s_raState = RaState::Idle;
					return;
				}
				if (comms_publish_attestation(s_raEnc, encLen) != 0)
				{
					Debug::log("RA challenge2: publish quote failed");
					s_raState = RaState::Idle;
					break;
				}
				Debug::log("RA: signed quote published ({} bytes), awaiting "
				           "approval",
				           encLen);
				// Remember which combined nonce this quote was bound to so the
				// approval can be matched to it, and wait for the verdict.
				memcpy(s_lastCombined, expected, AttestationNonceLength);
				s_raState = RaState::AwaitingApproval;
				break;
			}

			case RaApproved:
			{
				if (s_raState != RaState::AwaitingApproval)
				{
					Debug::log("RA approval: unexpected (no quote awaiting)");
					return;
				}
				if (payloadLen != AttestationNonceLength)
				{
					Debug::log("RA approval: bad payload length {}",
					           payloadLen);
					return;
				}
				if (memcmp(payload, s_lastCombined, AttestationNonceLength) !=
				    0)
				{
					Debug::log(
					  "RA approval: combined nonce mismatch — ignored");
					return;
				}
				s_attested = true;
				s_raState  = RaState::Idle;
				Debug::log("RA: attestation approved — telemetry enabled");
				break;
			}

			default:
				Debug::log("RA: unknown message type {}", type);
				break;
		}
	}

	// Drain and handle every RA message buffered by the comms compartment.
	void drain_ra_messages()
	{
		size_t len = 0;
		while (comms_take_ra_message(s_raDrain, &len) == 0)
		{
			handle_ra_message(s_raDrain, len);
		}
	}

} // namespace

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

	// Remote attestation is now verifier-initiated: the verifier drives a
	// dual-nonce challenge-response over plantnode/attestation (handled by
	// drain_ra_messages below). The authentic-execution path is unchanged:
	// core_logic (no capabilities) -> attestation (sole SPI-flash holder;
	// measures the booted slot) -> fake_tpm (sole key holder; signs) ->
	// core_logic -> comms (sole MQTT holder; publishes).

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

		// Publish telemetry (comms encrypts via crypto compartment), but only
		// once remote attestation has completed and the verifier has approved
		// the quote. No sensor data leaves the device before then.
		if (s_attested)
		{
			reading.last_watering = lastWatering;
			int ret               = comms_publish_telemetry(&reading);
			if (ret != 0)
			{
				Debug::log("comms_publish_telemetry failed: {}", ret);
			}
		}
		else
		{
			Debug::log("Telemetry withheld: awaiting attestation approval");
		}

		// Wait ~10 s until the next telemetry tick, but poll the network often
		// and drain attestation messages so the multi-round handshake completes
		// in seconds rather than one leg per telemetry tick.
		constexpr int PollIntervalMs = 250;
		constexpr int PollsPerCycle  = 10000 / PollIntervalMs;
		for (int i = 0; i < PollsPerCycle; i++)
		{
			comms_poll();
			drain_ra_messages();

			Timeout t{MS_TO_TICKS(PollIntervalMs)};
			thread_sleep(&t);
		}
	}
}
