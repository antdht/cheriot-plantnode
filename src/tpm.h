// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#pragma once

#include <compartment.h>
#include <stddef.h>
#include <stdint.h>

/**
 * tpm MOCK API.
 *
 * NOTE: this is NOT a real TPM and performs NO real cryptography. It is an
 * abstract stand-in for a secure element that "vouches" for the device, so the
 * attestation flow (telemetry -> attestation -> tpm) can be exercised end to
 * end without any real measurement or signing. The focus has moved off real
 * attestation; this just marks where a hardware root of trust would sit.
 *
 * It keeps the shape of the real thing: it never touches sensors/flash/network
 * and the cross-compartment call boundary is preserved on purpose.
 */

/**
 * MOCK: produce the device's opaque attestation token. Returns a canned,
 * deterministic blob — there is no real key and no real attestation behind it.
 * tokenOut must be at least AttestationTokenLength bytes.
 *
 * Returns 0 on success, negative errno on failure.
 */
int __cheri_compartment("tpm") tpm_attest(uint8_t *tokenOut, size_t *tokenLen);
