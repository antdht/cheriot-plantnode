// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#include "telemetry.h"
#include "attestation.h"
#include "comms.h"
#include "crypto.h"
#include "display/display_data.h"
#include "drivers/moisture_sensor.h"
#include "drivers/temperature_sensor.h"
#include "irrigation.h"
#include "plantnode_types.h"
#include <debug.hh>
#include <fail-simulator-on-error.h>
#include <sntp.h>
#include <string.h>
#include <string_view>
#include <thread.h>
#include <tick_macros.h>

using Debug = ConditionalDebug<true, "PlantNode">;

namespace
{

	// MOCK attestation API.
	//
	// The real dual-nonce, signed remote-attestation handshake has been
	// replaced by a simple plaintext request/response on plantnode/attestation:
	// the device asks "am i attested?" and the verifier (charter) replies
	// "yes". The cross-compartment call chain is kept intact and mocked end to
	// end:
	//   telemetry -> attestation (mock measure) -> tpm (mock sign) ->
	//   telemetry
	//   -> comms (publishes the JSON query).
	// Telemetry stays gated: no sensor data leaves the device until the
	// verifier confirms attestation.

	// Latched once the verifier confirms attestation. The board withholds all
	// telemetry until this is set.
	bool sAttested = false;

	// Scratch buffers kept off the (small, TLS-shared) telemetry stack. Safe
	// as statics: only the single telemetry thread touches them.
	AttestationEvidence sEvidence;    // mock evidence gathered for each query
	uint8_t             sReqBuf[256]; // JSON "am_i_attested" request to publish
	uint8_t sRespBuf[256];            // most recent response drained from comms

	bool append_literal(uint8_t    *buf,
	                    size_t     &pos,
	                    size_t      cap,
	                    const char *s,
	                    size_t      n)
	{
		if (pos + n > cap)
		{
			return false;
		}
		memcpy(buf + pos, s, n);
		pos += n;
		return true;
	}

	bool append_hex(uint8_t       *buf,
	                size_t        &pos,
	                size_t         cap,
	                const uint8_t *data,
	                size_t         n)
	{
		static const char Hex[] = "0123456789abcdef";
		if (pos + 2 * n > cap)
		{
			return false;
		}
		for (size_t i = 0; i < n; i++)
		{
			buf[pos++] = static_cast<uint8_t>(Hex[data[i] >> 4]);
			buf[pos++] = static_cast<uint8_t>(Hex[data[i] & 0x0f]);
		}
		return true;
	}

	// Build the plaintext JSON query:
	//   {"query":"am_i_attested","device":"plantnode-001","token":"<64 hex>"}
	// Returns the number of bytes written, or 0 on overflow.
	size_t build_attestation_query(uint8_t                   *buf,
	                               size_t                     cap,
	                               const AttestationEvidence *e)
	{
		size_t pos = 0;
#define AL(s) append_literal(buf, pos, cap, s, sizeof(s) - 1)
		bool ok = AL("{\"query\":\"am_i_attested\",\"device\":\"") &&
		          append_literal(buf, pos, cap, e->deviceId, e->deviceIdLen) &&
		          AL("\",\"token\":\"") &&
		          append_hex(buf, pos, cap, e->token, AttestationTokenLength) &&
		          AL("\"}");
#undef AL
		return ok ? pos : 0;
	}

	// Gather mock evidence and publish the "am i attested?" query.
	void send_attestation_query()
	{
		if (attestation_get_evidence(&sEvidence) != 0)
		{
			Debug::log("MOCK attestation: attestation_get_evidence failed");
			return;
		}
		size_t n =
		  build_attestation_query(sReqBuf, sizeof(sReqBuf), &sEvidence);
		if (n == 0)
		{
			Debug::log("MOCK attestation: query JSON overflow");
			return;
		}
		if (comms_publish_attestation(sReqBuf, n) != 0)
		{
			Debug::log("MOCK attestation: query publish failed");
			return;
		}
		Debug::log("MOCK attestation: sent 'am i attested?' query ({} bytes)",
		           n);
	}

	// True if the verifier's JSON response confirms attestation.
	bool response_is_attested(const uint8_t *buf, size_t len)
	{
		std::string_view sv{reinterpret_cast<const char *>(buf), len};
		return sv.find("\"attested\":true") != std::string_view::npos ||
		       sv.find("\"attested\": true") != std::string_view::npos;
	}

	// Drain and handle every attestation response buffered by comms.
	void drain_ra_messages()
	{
		size_t len = 0;
		while (comms_take_ra_message(sRespBuf, &len) == 0)
		{
			if (response_is_attested(sRespBuf, len))
			{
				if (!sAttested)
				{
					Debug::log("MOCK attestation: verifier says YES, telemetry "
					           "enabled");
				}
				sAttested = true;
			}
			else
			{
				Debug::log(
				  "MOCK attestation: response did not confirm attestation");
			}
		}
	}

} // namespace

void __cheri_compartment("telemetry") telemetry_loop()
{
	Debug::log("=== PlantNode core starting ===");

	// MQTT must be up before anything else runs (key exchange, attestation,
	// telemetry all depend on a live broker connection). Keep retrying until
	// it succeeds instead of falling through to attestation on a dead link.
	int connectRet;
	while ((connectRet = comms_connect()) != 0)
	{
		Debug::log("comms_connect failed (err={}), retrying...", connectRet);
		Timeout retryDelay{MS_TO_TICKS(3000)};
		thread_sleep(&retryDelay, ThreadSleepNoEarlyWake);
	}

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

	while (true)
	{
		// Keep asking until the verifier confirms attestation.
		if (!sAttested)
		{
			send_attestation_query();
		}

		// Publish telemetry (comms encrypts via crypto compartment), but only
		// once the verifier has confirmed attestation. No sensor data leaves
		// the device before then. Read latest reading from irrigation
		// mailbox.
		if (sAttested)
		{
			SensorReading reading{};
			int moistureRet = moisture_read_raw_mock(&reading.moistureRaw);
			int tempRet     = temperature_read_both(&reading.temperatureCx10,
			                                        &reading.humidityRx10);
			reading.valid   = (moistureRet == 0) && (tempRet == 0);

			struct timeval tv;
			reading.timestamp = (gettimeofday(&tv, nullptr) == 0)
			                      ? static_cast<uint32_t>(tv.tv_sec)
			                      : 0;
			irrigation_get_last_watering(&reading.lastWatering);

			if (reading.valid)
			{
				int ret = comms_publish_telemetry(&reading);
				if (ret != 0)
				{
					Debug::log("comms_publish_telemetry failed: {}", ret);
				}
				display_sensor_readings(&reading);
			}
			else
			{
				Debug::log("Sensor read failed (moisture={}, temp={}); "
				           "skipping telemetry tick",
				           moistureRet,
				           tempRet);
			}
		}
		else
		{
			Debug::log("Telemetry withheld: awaiting attestation confirmation");
		}

		// Wait ~10 s until the next telemetry tick, but poll the network often
		// and drain attestation responses so a reply is picked up within a poll
		// interval rather than one per telemetry tick.
		//
		// ThreadSleepNoEarlyWake is required here: without it, thread_sleep()
		// may return as soon as no other thread is runnable (see thread.h),
		// which on this board happens constantly between network events and
		// turns each "250 ms" sleep into a near-instant yield.
		constexpr int PollIntervalMs = 250;
		constexpr int PollsPerCycle  = 2500 / PollIntervalMs;
		for (int i = 0; i < PollsPerCycle; i++)
		{
			comms_poll();
			drain_ra_messages();

			Timeout t{MS_TO_TICKS(PollIntervalMs)};
			thread_sleep(&t, ThreadSleepNoEarlyWake);
		}
	}
}
