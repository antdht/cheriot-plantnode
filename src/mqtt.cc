// Copyright SCI Semiconductor and CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#include <NetAPI.h>
#if __has_include(<allocator.h>)
#	include <allocator.h>
#endif
#include <cstdlib>
#include <debug.hh>
#include <errno.h>
#include <fail-simulator-on-error.h>
#include <mqtt.h>
#include <sntp.h>
#include <tick_macros.h>

#include "mosquitto.org.h"

using CHERI::Capability;

using Debug            = ConditionalDebug<true, "PlantNode MQTT">;
constexpr bool UseIPv6 = CHERIOT_RTOS_OPTION_IPv6;

constexpr size_t                          MQTTMaximumClientLength = 23;
constexpr std::string_view                clientIDPrefix{"cheriotMQTT"};
std::array<char, MQTTMaximumClientLength> clientID;
static_assert(clientIDPrefix.size() < clientID.size());

constexpr const size_t networkBufferSize    = 1024;
constexpr const size_t incomingPublishCount = 20;
constexpr const size_t outgoingPublishCount = 20;

DECLARE_AND_DEFINE_CONNECTION_CAPABILITY(MosquittoOrgMQTT,
                                         "test.mosquitto.org",
                                         8883,
                                         ConnectionTypeTCP);

DECLARE_AND_DEFINE_ALLOCATOR_CAPABILITY(mqttMalloc, 32 * 1024);

constexpr std::string_view testTopic{"plantnode/test"};
constexpr std::string_view testPayload{"hello from plantnode"};

static int ackReceived     = 0;
static int publishReceived = 0;

void __cheri_callback publishCallback(const char *topicName,
                                      size_t      topicNameLength,
                                      const void *payload,
                                      size_t      payloadLength)
{
	Timeout t{MS_TO_TICKS(5000)};
	if (heap_claim_ephemeral(&t, topicName) != 0 ||
	    !CHERI::check_pointer(topicName, topicNameLength))
	{
		Debug::log("Cannot claim or verify PUBLISH callback topic name pointer.");
		return;
	}

	if (heap_claim_ephemeral(&t, payload) != 0 ||
	    !CHERI::check_pointer(payload, payloadLength))
	{
		Debug::log("Cannot claim or verify PUBLISH callback payload pointer.");
		return;
	}

	Debug::log("Got a PUBLISH for topic {}",
	           std::string_view{topicName, topicNameLength});
	publishReceived++;
}

void __cheri_callback ackCallback(uint16_t packetID, bool isReject)
{
	Debug::log("Got an ACK for packet {}", packetID);

	if (isReject)
	{
		Debug::log("The ACK is a SUBSCRIBE REJECT notification");
	}

	ackReceived++;
}

void __cheri_compartment("mqtt") mqtt_entry()
{
	int     ret;
	Timeout t{MS_TO_TICKS(5000)};

	Debug::log("=== PlantNode firmware starting ===");

	Debug::log("Starting network stack...");
	network_start();
	Debug::log("Network stack started.");

	Debug::log("Synchronising time via SNTP...");
	while (sntp_update(&t) != 0)
	{
		Debug::log("Failed to update NTP time, retrying...");
		t = Timeout{MS_TO_TICKS(5000)};
	}
	Debug::log("NTP time updated.");
	t = UnlimitedTimeout;

	{
		timeval tv;
		int     ret = gettimeofday(&tv, nullptr);
		if (ret != 0)
		{
			Debug::log("Failed to get time of day: {}", ret);
		}
		else
		{
			Debug::log("Current UNIX epoch time: {}", (int32_t)tv.tv_sec);
		}
	}

	memcpy(clientID.data(), clientIDPrefix.data(), clientIDPrefix.size());
	mqtt_generate_client_id(clientID.data() + clientIDPrefix.size(),
	                        clientID.size() - clientIDPrefix.size());
	Debug::log("Client ID generated: {}", std::string_view{clientID.data(), clientID.size()});

	Debug::log("Connecting to MQTT broker ({})...", testTopic);

	auto handle = mqtt_connect(&t,
	                           STATIC_SEALED_VALUE(mqttMalloc),
	                           CONNECTION_CAPABILITY(MosquittoOrgMQTT),
	                           publishCallback,
	                           ackCallback,
	                           TAs,
	                           TAs_NUM,
	                           networkBufferSize,
	                           incomingPublishCount,
	                           outgoingPublishCount,
	                           clientID.data(),
	                           clientID.size());

	if (!Capability{handle}.is_valid())
	{
		Debug::log("Failed to connect to MQTT broker.");
		return;
	}

	Debug::log("Connected to MQTT broker!");

	Debug::log("Subscribing to topic '{}'.", testTopic);

	ret = mqtt_subscribe(&t,
	                     handle,
	                     1,
	                     testTopic.data(),
	                     testTopic.size());

	if (ret < 0)
	{
		Debug::log("Failed to subscribe, error {}.", ret);
		mqtt_disconnect(&t, STATIC_SEALED_VALUE(mqttMalloc), handle);
		return;
	}

	while (ackReceived == 0)
	{
		t   = Timeout{MS_TO_TICKS(100)};
		ret = mqtt_run(&t, handle);

		if (ret < 0)
		{
			Debug::log("Failed to wait for SUBACK, error {}.", ret);
			mqtt_disconnect(&t, STATIC_SEALED_VALUE(mqttMalloc), handle);
			return;
		}
	}

	Debug::log("Subscribed! Publishing to topic '{}'.", testTopic);

	t   = Timeout{MS_TO_TICKS(5000)};
	ret = mqtt_publish(&t,
	                   handle,
	                   1,
	                   testTopic.data(),
	                   testTopic.size(),
	                   static_cast<const void *>(testPayload.data()),
	                   testPayload.size());

	if (ret < 0)
	{
		Debug::log("Failed to publish, error {}.", ret);
		mqtt_disconnect(&t, STATIC_SEALED_VALUE(mqttMalloc), handle);
		return;
	}

	while (ackReceived == 1 || publishReceived == 0)
	{
		t   = Timeout{MS_TO_TICKS(100)};
		ret = mqtt_run(&t, handle);

		if (ret < 0)
		{
			Debug::log("Failed to wait for PUBACK/PUBLISH, error {}.", ret);
			mqtt_disconnect(&t, STATIC_SEALED_VALUE(mqttMalloc), handle);
			return;
		}
	}

	Debug::log("MQTT round-trip complete. Disconnecting.");

	t   = Timeout{MS_TO_TICKS(5000)};
	ret = mqtt_disconnect(&t, STATIC_SEALED_VALUE(mqttMalloc), handle);

	if (ret < 0)
	{
		Debug::log("Failed to disconnect, error {}.", ret);
		return;
	}

	Debug::log("Done.");
}
