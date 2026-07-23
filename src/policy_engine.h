// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#pragma once

#include "plantnode_types.h"
#include <compartment.h>
#include <stdint.h>

/**
 * Evaluate a moisture reading against the current thresholds and act
 * accordingly (e.g. activate the pump via the pump driver).
 *
 * This is the ONLY compartment permitted to call the pump_driver compartment.
 *
 * @param moistureRaw  The latest raw moisture reading.
 * @param timestamp    UNIX timestamp at which the reading was taken.
 *
 * Returns the PolicyOutcome that was applied.
 */
PolicyOutcome __cheri_compartment("policy_engine")
  policy_evaluate(uint16_t moistureRaw, uint32_t timestamp);

/**
 * Parse and apply a remote threshold-update command received on
 * plantnode/commands (already decrypted by the caller).
 *
 * Expected plaintext format (fixed key order, no other JSON accepted):
 *   {"timestamp":<uint32>,"threshold":<uint16>}
 *
 * The command is applied only if its timestamp is strictly newer than the
 * last command this function applied; otherwise it is treated as stale or
 * replayed and dropped without changing any state.
 *
 * @param packet     Decrypted plaintext bytes.
 * @param packetLen  Length of packet, in bytes.
 *
 * Returns 0 if applied, -EINVAL if the packet is malformed or the threshold
 * value is out of range, -EALREADY if the command was dropped as stale.
 */
int __cheri_compartment("policy_engine")
  policy_update_threshold(const uint8_t *packet, size_t packetLen);
