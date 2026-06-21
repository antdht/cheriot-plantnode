// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#pragma once

#include "plantnode_types.h"
#include <compartment.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Initialise the network stack, synchronise time via SNTP, and connect to the
 * MQTT broker. Displays "Connecting to network…" then "Connected!" on the LCD.
 * Must be called once before any publish/poll calls.
 *
 * This is the ONLY compartment permitted to call into the network driver
 * compartments (TCPIP, Firewall, MQTT).
 *
 * Returns 0 on success, negative errno on failure.
 */
int __cheri_compartment("comms") comms_connect();

/**
 * Publish a raw payload to an arbitrary MQTT topic.
 * Prefer the typed wrappers below when the destination is known.
 *
 * Returns 0 on success, negative errno on failure.
 */
int __cheri_compartment("comms")
  comms_publish(const char *topic, const void *payload, size_t payloadLen);

/**
 * Publish the 48-byte Noise-N packet1 to the key topic
 * (plantnode/keys/plantnode-001) as a retained QoS-1 message.
 * Called once at startup so the verifier can derive the session rx-key.
 *
 * Returns 0 on success, negative errno on failure.
 */
int __cheri_compartment("comms")
  comms_publish_key_packet(const uint8_t *packet, size_t packetLen);

/**
 * Encrypt reading via the crypto compartment and publish to
 * plantnode/telemetry. Returns 0 on success,
 * negative errno on failure.
 */
int __cheri_compartment("comms")
  comms_publish_telemetry(const SensorReading *reading);

/**
 * Publish a raw remote-attestation message to the verifier topic
 * (plantnode/attestation). The bytes are already secretbox-encrypted by the
 * crypto compartment (a nonce reply or a serialised, signed quote); this just
 * puts them on the wire.
 *
 * Returns 0 on success, negative errno on failure.
 */
int __cheri_compartment("comms")
  comms_publish_attestation(const uint8_t *bytes, size_t len);

/**
 * Drain the most recent decrypted remote-attestation message received on
 * plantnode/attestation into `out` (at least 256 bytes). Returns 0 and sets
 * *outLen when a message was pending, -ENOENT when none is, negative errno on
 * bad arguments. Called from the core_logic loop, not from the MQTT callback.
 */
int __cheri_compartment("comms")
  comms_take_ra_message(uint8_t *out, size_t *outLen);

/**
 * Drive the MQTT event loop (process ACKs and incoming messages).
 * Should be called regularly from the core logic loop.
 *
 * Returns 0 on success, negative errno on failure.
 */
int __cheri_compartment("comms") comms_poll();

/**
 * Gracefully disconnect from the MQTT broker.
 *
 * Returns 0 on success, negative errno on failure.
 */
int __cheri_compartment("comms") comms_disconnect();
