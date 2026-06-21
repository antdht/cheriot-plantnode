// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#pragma once

#include "plantnode_types.h"
#include <compartment.h>
#include <stddef.h>
#include <stdint.h>

/**
 * fake_tpm, software stand-in for a hardware TPM / secure key store.
 *
 * Holds the device's Ed25519 signing key and NOTHING else. Its only job is to
 * sign. It never reads sensors, flash, or the network. The private key is
 * created inside this compartment and is never exported across a compartment
 * boundary, so even a fully compromised application compartment cannot extract
 * it.
 *
 * Trust boundary: a cheriot-audit policy asserts that fake_tpm_sign() is
 * imported ONLY by the attestation compartment, so no other compartment can
 * reach the signing oracle at all.
 */

/**
 * Sign a 32-byte attestation digest with the device key.
 *
 * Accepts ONLY a fixed-size digest (AttestationDigestLength). This is a
 * deliberate guard: fake_tpm cannot be repurposed as a general-purpose signing
 * oracle for arbitrary data. Every signature it ever produces is over a
 * quote-shaped digest. sigOut must be at least AttestationSignatureMaxLength
 * bytes.
 *
 * Returns 0 on success, negative errno on failure.
 */
int __cheri_compartment("fake_tpm") fake_tpm_sign(const uint8_t *digest,
                                                  size_t         digestLen,
                                                  uint8_t       *sigOut,
                                                  size_t        *sigLen);

/**
 * Write the device public key (verification key) to pkOut.
 * pkOut must be at least 32 bytes. The verifier needs this key (or derives it
 * from the same seed) to check quotes.
 *
 * Returns 0 on success, negative errno on failure.
 */
int __cheri_compartment("fake_tpm")
  fake_tpm_get_public_key(uint8_t *pkOut, size_t *pkLen);
