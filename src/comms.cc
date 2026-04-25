// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#include "comms.h"
#include "display/display_comms.h"
#include "plantnode_types.h"

#include <NetAPI.h>
#if __has_include(<allocator.h>)
#	include <allocator.h>
#endif
#include <array>
#include <cheri.hh>
#include <cstdlib>
#include <debug.hh>
#include <errno.h>
#include <mqtt.h>
#include <sntp.h>
#include <string_view>
#include <thread.h>
#include <tick_macros.h>

// ── MQTT broker selection ──────────────────────────────────────────────────
// Define MQTT_USE_LOCAL_BROKER at build time to connect to a local broker
// instead of test.mosquitto.org. The local broker's CA is in local_ip.h.
// Override MQTT_LOCAL_BROKER_HOST / MQTT_LOCAL_BROKER_PORT as needed.

#define MQTT_USE_LOCAL_BROKER

#ifdef MQTT_USE_LOCAL_BROKER
#	include "local_ip.h"
#	ifndef MQTT_LOCAL_BROKER_HOST
#		define MQTT_LOCAL_BROKER_HOST "mos.waos.space"
#	endif
#	ifndef MQTT_LOCAL_BROKER_PORT
#		define MQTT_LOCAL_BROKER_PORT 8883
#	endif
DECLARE_AND_DEFINE_CONNECTION_CAPABILITY(MosquittoOrgMQTT,
                                         MQTT_LOCAL_BROKER_HOST,
                                         MQTT_LOCAL_BROKER_PORT,
                                         ConnectionTypeTCP);
#else
#	include "mosquitto.org.h"
DECLARE_AND_DEFINE_CONNECTION_CAPABILITY(MosquittoOrgMQTT,
                                         "test.mosquitto.org",
                                         8883,
                                         ConnectionTypeTCP);
#endif

using CHERI::Capability;
using Debug            = ConditionalDebug<true, "PlantNode Comms">;
constexpr bool UseIPv6 = CHERIOT_RTOS_OPTION_IPv6;

// ── MQTT infrastructure ────────────────────────────────────────────────────

// Separate heap quota for all network allocations.
DECLARE_AND_DEFINE_ALLOCATOR_CAPABILITY(mqttMalloc, 32 * 1024);

constexpr size_t                                 MQTTMaximumClientLength = 23;
constexpr std::string_view                       clientIDPrefix{"cheriotMQTT"};
static std::array<char, MQTTMaximumClientLength> clientID;
static_assert(clientIDPrefix.size() < clientID.size());

constexpr size_t networkBufferSize    = 1024;
constexpr size_t incomingPublishCount = 20;
constexpr size_t outgoingPublishCount = 20;

// ── Topic constants ────────────────────────────────────────────────────────

// Outbound: periodic sensor data sent to the monitoring station.
constexpr std::string_view TopicTelemetry{"plantnode/telemetry"};
// Outbound: signed attestation blob sent to the remote verifier.
constexpr std::string_view TopicAttestation{"plantnode/attestation"};
// Inbound: commands / verification responses from the remote verifier.
constexpr std::string_view TopicCommands{"plantnode/commands"};
// Outbound: ping message to show connection to the broker.
constexpr std::string_view TopicPing{"plantnode/ping"};

// ── Persistent MQTT state (private to this compartment) ───────────────────

// The sealed MQTT handle never leaves this compartment.
// core_logic holds no capability to it — it cannot publish directly.
static MQTTConnection s_handle       = nullptr;
static volatile int   s_ack_received = 0;

// ── Callbacks ─────────────────────────────────────────────────────────────

void __cheri_callback publishCallback(const char *topicName,
                                      size_t      topicNameLength,
                                      const void *payload,
                                      size_t      payloadLength)
{
	Timeout t{MS_TO_TICKS(5000)};
	if (heap_claim_ephemeral(&t, topicName) != 0 ||
	    !CHERI::check_pointer(topicName, topicNameLength))
	{
		Debug::log("Cannot claim/verify incoming topic pointer.");
		return;
	}
	if (heap_claim_ephemeral(&t, payload) != 0 ||
	    !CHERI::check_pointer(payload, payloadLength))
	{
		Debug::log("Cannot claim/verify incoming payload pointer.");
		return;
	}

	Debug::log("Incoming message on topic '{}'",
	           std::string_view{topicName, topicNameLength});
	// TODO: dispatch incoming commands from the remote verifier
}

void __cheri_callback ackCallback(uint16_t packetID, bool isReject)
{
	if (isReject)
	{
		Debug::log("SUBSCRIBE REJECT for packet {}", packetID);
	}
	else
	{
		Debug::log("ACK for packet {}", packetID);
	}
	s_ack_received++;
}

// ── Internal helpers ───────────────────────────────────────────────────────

static int wait_for_ack(int expected_acks, uint32_t timeout_ms = 5000)
{
	Timeout t{MS_TO_TICKS(timeout_ms)};
	while (s_ack_received < expected_acks)
	{
		t       = Timeout{MS_TO_TICKS(100)};
		int ret = mqtt_run(&t, s_handle);
		if (ret < 0)
		{
			return ret;
		}
	}
	return 0;
}

static int publish_and_wait(std::string_view topic,
                            const void      *payload,
                            size_t           payload_len)
{
	int expected = s_ack_received + 1;

	Timeout t{MS_TO_TICKS(5000)};
	int     ret = mqtt_publish(
	  &t, s_handle, 1, topic.data(), topic.size(), payload, payload_len);
	if (ret < 0)
	{
		Debug::log("mqtt_publish to '{}' failed: {}", topic, ret);
		return ret;
	}

	ret = wait_for_ack(expected);
	if (ret < 0)
	{
		Debug::log("PUBACK wait failed: {}", ret);
	}
	return ret;
}

// ── Public compartment functions ───────────────────────────────────────────

int __cheri_compartment("comms") comms_connect()
{
	// Start the network first — DHCP must complete before SNTP.
	Debug::log("Starting network stack...");
	network_start();
	Debug::log("Network stack started.");

	display_connecting_network();

	Debug::log("Synchronising time via SNTP...");

	Timeout t{MS_TO_TICKS(5000)};
	int     sntpRet;
	while ((sntpRet = sntp_update(&t)) != 0)
	{
		if (sntpRet == -ENOTCONN)
		{
			// DHCP not yet complete — wait before retrying
			Debug::log("Network not up yet (ENOTCONN), waiting for DHCP...");
			Timeout dhcpRetry{MS_TO_TICKS(3000)};
			thread_sleep(&dhcpRetry);
		}
		else
		{
			Debug::log("SNTP update failed (err={}), retrying...", sntpRet);
		}
		t = Timeout{MS_TO_TICKS(5000)};
	}
	Debug::log("NTP time updated.");

	{
		timeval tv;
		if (gettimeofday(&tv, nullptr) == 0)
		{
			Debug::log("UNIX epoch: {}", (int32_t)tv.tv_sec);
		}
	}

	memcpy(clientID.data(), clientIDPrefix.data(), clientIDPrefix.size());
	mqtt_generate_client_id(clientID.data() + clientIDPrefix.size(),
	                        clientID.size() - clientIDPrefix.size());
	Debug::log("Client ID: {}",
	           std::string_view{clientID.data(), clientID.size()});

	Debug::log("Connecting to MQTT broker...");
	t        = UnlimitedTimeout;
	s_handle = mqtt_connect(&t,
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

	if (!Capability{s_handle}.is_valid())
	{
		Debug::log("Failed to connect to MQTT broker.");
		return -ENOTCONN;
	}
	Debug::log("Connected to MQTT broker!");

	Debug::log("Subscribing to commands topic '{}'...", TopicCommands);
	s_ack_received = 0;
	t              = UnlimitedTimeout;
	int ret        = mqtt_subscribe(
	  &t, s_handle, 1, TopicCommands.data(), TopicCommands.size());
	if (ret < 0)
	{
		Debug::log("Subscribe failed: {}", ret);
		mqtt_disconnect(&t, STATIC_SEALED_VALUE(mqttMalloc), s_handle);
		s_handle = nullptr;
		return ret;
	}

	ret = wait_for_ack(1);
	if (ret < 0)
	{
		Debug::log("SUBACK wait failed: {}", ret);
		mqtt_disconnect(&t, STATIC_SEALED_VALUE(mqttMalloc), s_handle);
		s_handle = nullptr;
		return ret;
	}
	Debug::log("Subscribed to '{}'.", TopicCommands);

	display_connected_network();

	static const char debugMsg[] = "plantnode-online";
	comms_publish(TopicPing.data(), debugMsg, sizeof(debugMsg) - 1);
	Debug::log("Sent initial ping message to broker.");

	return 0;
}

int __cheri_compartment("comms")
  comms_publish(const char *topic, const void *payload, size_t payload_len)
{
	if (!topic || !payload)
	{
		return -EINVAL;
	}
	if (!Capability{s_handle}.is_valid())
	{
		return -ENOTCONN;
	}
	return publish_and_wait(std::string_view{topic}, payload, payload_len);
}

int __cheri_compartment("comms")
  comms_publish_telemetry(const SensorReading *reading)
{
	if (!CHERI::check_pointer(reading, sizeof(SensorReading)))
	{
		return -EINVAL;
	}
	if (!Capability{s_handle}.is_valid())
	{
		return -ENOTCONN;
	}
	Debug::log("Publishing telemetry...");
	return publish_and_wait(TopicTelemetry, reading, sizeof(SensorReading));
}

int __cheri_compartment("comms")
  comms_publish_attestation(const uint8_t *sig, size_t sig_len)
{
	if (!CHERI::check_pointer(sig, sig_len))
	{
		return -EINVAL;
	}
	if (!Capability{s_handle}.is_valid())
	{
		return -ENOTCONN;
	}
	Debug::log("Publishing attestation ({} bytes)...", sig_len);
	return publish_and_wait(TopicAttestation, sig, sig_len);
}

int __cheri_compartment("comms") comms_poll()
{
	if (!Capability{s_handle}.is_valid())
	{
		return -ENOTCONN;
	}
	Timeout t{MS_TO_TICKS(100)};
	return mqtt_run(&t, s_handle);
}

int __cheri_compartment("comms") comms_disconnect()
{
	if (!Capability{s_handle}.is_valid())
	{
		return 0;
	}
	Debug::log("Disconnecting from MQTT broker...");
	Timeout t{MS_TO_TICKS(5000)};
	int ret  = mqtt_disconnect(&t, STATIC_SEALED_VALUE(mqttMalloc), s_handle);
	s_handle = nullptr;
	return ret;
}
