// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#include "policy_engine.h"
#include "display/display_policy.h"
#include "drivers/pump_driver.h"
#include <cctype>
#include <cstring>
#include <debug.hh>
#include <errno.h>
#include <thread.h>

using Debug = ConditionalDebug<true, "PlantNode Policy">;

static uint16_t sMoistureLow          = 300;
static uint32_t sLastCommandTimestamp = 0;

PolicyOutcome __cheri_compartment("policy_engine")
  policy_evaluate(uint16_t moistureRaw, uint32_t timestamp)
{
	(void)timestamp; // not consulted by the current decision logic

	if (moistureRaw < sMoistureLow)
	{
		Debug::log("Moisture too low ({} < {}), activating pump.",
		           moistureRaw,
		           sMoistureLow);
		display_pump_activation(true);
		// Watering pulse: pump_on() is a placeholder (no relay pin wired yet,
		// returns -ENOSYS) but does light an onboard LED as a POC. This call
		// blocks the irrigation thread for ~2.5s while the pulse runs.
		// TODO: once the relay pin exists, replace this blocking pulse with a
		// non-blocking timer so watering doesn't stall the sense/policy
		// cadence.
		pump_on();
		thread_millisecond_wait(2500);
		pump_off();
		display_pump_activation(false);
		return PolicyOutcome::PumpActivation;
	}

	return PolicyOutcome::NoAction;
}

// Parses a fixed-format "{"timestamp":<uint32>,"threshold":<uint16>}"
// buffer. Returns true and fills *outValue/*outPos on success; does not
// tolerate whitespace, reordered keys, or trailing garbage before '}'.
static bool parse_uint_field(const uint8_t *buf,
                             size_t         len,
                             size_t         pos,
                             const char    *key,
                             size_t         keyLen,
                             uint32_t      *outValue,
                             size_t        *outPos)
{
	if (pos + keyLen > len || memcmp(buf + pos, key, keyLen) != 0)
	{
		return false;
	}
	pos += keyLen;

	if (pos >= len || !isdigit(buf[pos]))
	{
		return false;
	}
	uint32_t value = 0;
	while (pos < len && isdigit(buf[pos]))
	{
		uint32_t digit = static_cast<uint32_t>(buf[pos] - '0');
		// Check for overflow before accumulation
		if (value > (UINT32_MAX - digit) / 10)
		{
			return false;
		}
		value = value * 10 + digit;
		pos++;
	}
	*outValue = value;
	*outPos   = pos;
	return true;
}

int __cheri_compartment("policy_engine")
  policy_update_threshold(const uint8_t *packet, size_t packetLen)
{
	if (packet == nullptr)
	{
		return -EINVAL;
	}

	static constexpr char   TimestampKey[]  = "{\"timestamp\":";
	static constexpr char   ThresholdKey[]  = ",\"threshold\":";
	static constexpr size_t TimestampKeyLen = sizeof(TimestampKey) - 1;
	static constexpr size_t ThresholdKeyLen = sizeof(ThresholdKey) - 1;

	uint32_t timestamp = 0;
	uint32_t threshold = 0;
	size_t   pos       = 0;

	if (!parse_uint_field(packet,
	                      packetLen,
	                      pos,
	                      TimestampKey,
	                      TimestampKeyLen,
	                      &timestamp,
	                      &pos))
	{
		Debug::log("policy_update_threshold: malformed packet (timestamp)");
		return -EINVAL;
	}
	if (!parse_uint_field(packet,
	                      packetLen,
	                      pos,
	                      ThresholdKey,
	                      ThresholdKeyLen,
	                      &threshold,
	                      &pos))
	{
		Debug::log("policy_update_threshold: malformed packet (threshold)");
		return -EINVAL;
	}
	if (pos >= packetLen || packet[pos] != '}')
	{
		Debug::log("policy_update_threshold: malformed packet (trailing)");
		return -EINVAL;
	}
	if (threshold > UINT16_MAX)
	{
		Debug::log("policy_update_threshold: threshold {} out of range",
		           threshold);
		return -EINVAL;
	}

	if (timestamp <= sLastCommandTimestamp)
	{
		Debug::log("policy_update_threshold: dropping stale command "
		           "(ts={}, last={})",
		           timestamp,
		           sLastCommandTimestamp);
		return -EALREADY;
	}

	sMoistureLow          = static_cast<uint16_t>(threshold);
	sLastCommandTimestamp = timestamp;
	Debug::log("policy_update_threshold: applied new threshold {} (ts={})",
	           sMoistureLow,
	           timestamp);
	return 0;
}
