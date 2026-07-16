// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#include "comms.h"
#include "crypto.h"
#include "display/display_comms.h"
#include "plantnode_types.h"

#include <NetAPI.h>
#if __has_include(<allocator.h>)
#	include <allocator.h>
#endif
#include <array>
#include <cheri.hh>
#include <cstdio>
#include <cstdlib>
#include <debug.hh>
#include <errno.h>
#include <mqtt.h>
#include <sntp.h>
#include <string_view>
#include <thread.h>
#include <tick_macros.h>

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

// MQTT infrastructure

// Separate heap quota for all network allocations.
DECLARE_AND_DEFINE_ALLOCATOR_CAPABILITY(mqttMalloc, 32 * 1024);

constexpr size_t                                 MQTTMaximumClientLength = 23;
constexpr std::string_view                       ClientIdPrefix{"cheriotMQTT"};
static std::array<char, MQTTMaximumClientLength> clientID;
static_assert(ClientIdPrefix.size() < clientID.size());

constexpr size_t NetworkBufferSize    = 1024;
constexpr size_t IncomingPublishCount = 20;
constexpr size_t OutgoingPublishCount = 20;

// Topic constants

// Outbound: periodic sensor data sent to the monitoring station.
constexpr std::string_view TopicTelemetry{"plantnode/telemetry"};
// Outbound: Noise-N packet1 for session key distribution (retained).
constexpr std::string_view TopicKey{"plantnode/keys/plantnode-001"};
// Outbound: signed attestation blob sent to the remote verifier.
constexpr std::string_view TopicAttestation{"plantnode/attestation"};
// Inbound: commands / verification responses from the remote verifier.
constexpr std::string_view TopicCommands{"plantnode/commands"};
// Outbound: ping message to show connection to the broker.
constexpr std::string_view TopicPing{"plantnode/ping"};

// Persistent MQTT state (private to this compartment)

// The sealed MQTT handle never leaves this compartment.
// telemetry holds no capability to it — it cannot publish directly.
static MQTTConnection sHandle      = nullptr;
static volatile int   sAckReceived = 0;

// Single-slot buffer for the most recent decrypted remote-attestation message
// received on plantnode/attestation. telemetry drains it via
// comms_take_ra_message and runs the handshake state machine outside the MQTT
// callback (avoiding re-entrant mqtt_run during a publish).
static uint8_t       sRaBuf[256];
static size_t        sRaLen     = 0;
static volatile bool sRaPending = false;

// Callbacks

void __cheri_callback publish_callback(const char *topicName,
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

	std::string_view topic{topicName, topicNameLength};

	if (topic == TopicCommands)
	{
		// Reserved for a future remote-control feature, unrelated to
		// attestation. Decrypt and drop for now.
		uint8_t plainBuf[256];
		size_t  plainLen = 0;
		int     ret      = crypto_decrypt(static_cast<const uint8_t *>(payload),
		                                  payloadLength,
		                                  plainBuf,
		                                  &plainLen);
		if (ret != 0)
		{
			Debug::log("crypto_decrypt failed (err={})", ret);
			return;
		}
		Debug::log("Decrypted command ({} bytes) - dispatch TODO", plainLen);
		// TODO: parse and dispatch plaintext command to policy_engine
	}
	else if (topic == TopicAttestation)
	{
		// MOCK attestation API: this topic now carries plaintext JSON, both
		// directions. The device publishes a {"query":"am_i_attested",...}
		// request and the verifier replies {"attested":true,...}. Because the
		// topic is shared, we also receive our own request echoed back — drop
		// it by its "query" marker (note: the request body also contains the
		// substring "attested", inside "am_i_attested", so we must key off
		// "query", not "attested", to tell the two apart).
		std::string_view msg{static_cast<const char *>(payload), payloadLength};
		if (msg.find("\"query\"") != std::string_view::npos)
		{
			// Our own echoed request — ignore quietly.
			return;
		}
		if (sRaPending)
		{
			Debug::log("RA message dropped: previous one not yet drained");
			return;
		}
		if (payloadLength > sizeof(sRaBuf))
		{
			Debug::log("RA message too large ({} bytes)", payloadLength);
			return;
		}
		memcpy(sRaBuf, payload, payloadLength);
		sRaLen     = payloadLength;
		sRaPending = true;
		Debug::log("RA response buffered ({} bytes)", payloadLength);
	}
}

void __cheri_callback ack_callback(uint16_t packetID, bool isReject)
{
	if (isReject)
	{
		Debug::log("SUBSCRIBE REJECT for packet {}", packetID);
	}
	else
	{
		Debug::log("ACK for packet {}", packetID);
	}
	sAckReceived++;
}

// Internal helpers

static int wait_for_ack(int expectedAcks, uint32_t timeoutMs = 5000)
{
	Timeout t{MS_TO_TICKS(timeoutMs)};
	while (sAckReceived < expectedAcks)
	{
		t       = Timeout{MS_TO_TICKS(100)};
		int ret = mqtt_run(&t, sHandle);
		if (ret < 0)
		{
			return ret;
		}
	}
	return 0;
}

static int
publish_and_wait(std::string_view topic, const void *payload, size_t payloadLen)
{
	int expected = sAckReceived + 1;

	Timeout t{MS_TO_TICKS(5000)};
	int     ret = mqtt_publish(
	  &t, sHandle, 1, topic.data(), topic.size(), payload, payloadLen);
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

// Public compartment functions

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
			Debug::log("UNIX epoch: {}", static_cast<int32_t>(tv.tv_sec));
		}
	}

	memcpy(clientID.data(), ClientIdPrefix.data(), ClientIdPrefix.size());
	mqtt_generate_client_id(clientID.data() + ClientIdPrefix.size(),
	                        clientID.size() - ClientIdPrefix.size());
	Debug::log("Client ID: {}",
	           std::string_view{clientID.data(), clientID.size()});

	Debug::log("Connecting to MQTT broker...");
	t       = UnlimitedTimeout;
	sHandle = mqtt_connect(&t,
	                       STATIC_SEALED_VALUE(mqttMalloc),
	                       CONNECTION_CAPABILITY(MosquittoOrgMQTT),
	                       publish_callback,
	                       ack_callback,
	                       TAs,
	                       TAs_NUM,
	                       NetworkBufferSize,
	                       IncomingPublishCount,
	                       OutgoingPublishCount,
	                       clientID.data(),
	                       clientID.size());

	if (!Capability{sHandle}.is_valid())
	{
		Debug::log("Failed to connect to MQTT broker.");
		return -ENOTCONN;
	}
	Debug::log("Connected to MQTT broker!");

	Debug::log(
	  "Subscribing to '{}' and '{}'...", TopicCommands, TopicAttestation);
	sAckReceived = 0;
	t            = UnlimitedTimeout;
	int ret      = mqtt_subscribe(
	  &t, sHandle, 1, TopicCommands.data(), TopicCommands.size());
	if (ret < 0)
	{
		Debug::log("Subscribe to commands failed: {}", ret);
		mqtt_disconnect(&t, STATIC_SEALED_VALUE(mqttMalloc), sHandle);
		sHandle = nullptr;
		return ret;
	}

	t   = UnlimitedTimeout;
	ret = mqtt_subscribe(
	  &t, sHandle, 1, TopicAttestation.data(), TopicAttestation.size());
	if (ret < 0)
	{
		Debug::log("Subscribe to attestation failed: {}", ret);
		mqtt_disconnect(&t, STATIC_SEALED_VALUE(mqttMalloc), sHandle);
		sHandle = nullptr;
		return ret;
	}

	ret = wait_for_ack(2);
	if (ret < 0)
	{
		Debug::log("SUBACK wait failed: {}", ret);
		mqtt_disconnect(&t, STATIC_SEALED_VALUE(mqttMalloc), sHandle);
		sHandle = nullptr;
		return ret;
	}
	Debug::log("Subscribed to '{}' and '{}'.", TopicCommands, TopicAttestation);

	display_connected_network();

	static const char DebugMsg[] = "plantnode-online";
	comms_publish(TopicPing.data(), DebugMsg, sizeof(DebugMsg) - 1);
	Debug::log("Sent initial ping message to broker.");

	return 0;
}

int __cheri_compartment("comms")
  comms_publish(const char *topic, const void *payload, size_t payloadLen)
{
	if (!topic || !payload)
	{
		return -EINVAL;
	}
	if (!Capability{sHandle}.is_valid())
	{
		return -ENOTCONN;
	}
	return publish_and_wait(std::string_view{topic}, payload, payloadLen);
}

int __cheri_compartment("comms")
  comms_publish_key_packet(const uint8_t *packet, size_t packetLen)
{
	if (!CHERI::check_pointer(packet, packetLen))
	{
		return -EINVAL;
	}
	if (!Capability{sHandle}.is_valid())
	{
		return -ENOTCONN;
	}
	Debug::log("Publishing Noise-N key packet ({} bytes, retained)...",
	           packetLen);

	int     expected = sAckReceived + 1;
	Timeout t{MS_TO_TICKS(5000)};
	int     ret = mqtt_publish(&t,
	                           sHandle,
	                           1,
	                           TopicKey.data(),
	                           TopicKey.size(),
	                           packet,
	                           packetLen,
	                           /*retain=*/true);
	if (ret < 0)
	{
		Debug::log("mqtt_publish key packet failed: {}", ret);
		return ret;
	}
	return wait_for_ack(expected);
}

int __cheri_compartment("comms")
  comms_publish_telemetry(const SensorReading *reading)
{
	if (!reading)
	{
		return -EINVAL;
	}
	if (!Capability{sHandle}.is_valid())
	{
		return -ENOTCONN;
	}

	uint8_t encBuf[CryptoEncryptedMaxLen];
	size_t  encLen = 0;
	int     ret    = crypto_encrypt(reading, encBuf, &encLen);
	if (ret != 0)
	{
		Debug::log("crypto_encrypt failed: {}", ret);
		return ret;
	}

	Debug::log("Publishing encrypted telemetry ({} bytes)...", encLen);
	return publish_and_wait(TopicTelemetry, encBuf, encLen);
}

int __cheri_compartment("comms")
  comms_publish_attestation(const uint8_t *bytes, size_t len)
{
	if (!CHERI::check_pointer(bytes, len))
	{
		return -EINVAL;
	}
	if (!Capability{sHandle}.is_valid())
	{
		return -ENOTCONN;
	}
	Debug::log("Publishing attestation message ({} bytes)...", len);
	return publish_and_wait(TopicAttestation, bytes, len);
}

int __cheri_compartment("comms")
  comms_take_ra_message(uint8_t *out, size_t *outLen)
{
	if (!out || !outLen)
	{
		return -EINVAL;
	}
	if (!sRaPending)
	{
		return -ENOENT;
	}
	memcpy(out, sRaBuf, sRaLen);
	*outLen    = sRaLen;
	sRaPending = false;
	return 0;
}

int __cheri_compartment("comms") comms_poll()
{
	if (!Capability{sHandle}.is_valid())
	{
		return -ENOTCONN;
	}
	Timeout t{MS_TO_TICKS(100)};
	return mqtt_run(&t, sHandle);
}

int __cheri_compartment("comms") comms_disconnect()
{
	if (!Capability{sHandle}.is_valid())
	{
		return 0;
	}
	Debug::log("Disconnecting from MQTT broker...");
	Timeout t{MS_TO_TICKS(5000)};
	int     ret = mqtt_disconnect(&t, STATIC_SEALED_VALUE(mqttMalloc), sHandle);
	sHandle     = nullptr;
	return ret;
}
