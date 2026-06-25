// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#pragma once

#include "plantnode_types.h"
#include <compartment.h>
#include <stddef.h>

/**
 * attestation — MOCK API.
 *
 * NOTE: this performs no real measurement. The focus has moved off real
 * attestation, so this compartment just gathers abstract "attestation evidence"
 * (the device id plus an opaque token from the mock tpm) instead of measuring
 * anything. It still calls into the (mock) tpm so the cross-compartment chain
 * core_logic -> attestation -> tpm is preserved.
 *
 * It remains deliberately tiny and non-network-facing.
 */

/**
 * Write the device identity string (e.g. "plantnode-001") to idOut.
 * idOut must be at least DeviceIdMaxLength bytes.
 * Returns 0 on success, negative errno on failure.
 */
int __cheri_compartment("attestation")
  attestation_get_device_id(char *idOut, size_t *idLen);

/**
 * MOCK: gather the device's attestation evidence — fills evidenceOut with the
 * device id and an opaque token obtained from the (mock) tpm. There is no real
 * measurement or signature involved.
 * Returns 0 on success, negative errno on failure.
 */
int __cheri_compartment("attestation")
  attestation_get_evidence(AttestationEvidence *evidenceOut);
