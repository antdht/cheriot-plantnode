// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#pragma once

#include "plantnode_types.h"
#include <compartment.h>
#include <stddef.h>
#include <stdint.h>

/**
 * attestation, measurer in the remote-attestation flow.
 *
 * This is the ONLY compartment that holds a capability to the SPI flash device,
 * and the ONLY compartment permitted to call fake_tpm_sign (both enforced by a
 * cheriot-audit policy). It reads the firmware image of the booted slot back
 * out of flash, hashes it, builds a quote over (device id, slot, image hash,
 * verifier nonce), and asks fake_tpm to sign the resulting digest.
 *
 * It is deliberately tiny and non-network-facing: it takes a nonce as plain
 * data and returns a quote. It never touches the network, sensors, or any
 * secret. The signing key lives only in fake_tpm.
 */

/**
 * Write the device identity string (e.g. "plantnode-001") to idOut.
 * idOut must be at least DeviceIdMaxLength bytes.
 * Returns 0 on success, negative errno on failure.
 */
int __cheri_compartment("attestation")
  attestation_get_device_id(char *idOut, size_t *idLen);

/**
 * Measure the firmware image of the booted slot: read the loaded ELF back out
 * of SPI flash and hash it (hydro_hash). hashOut must be at least
 * AttestationImageHashLength bytes. Exposed independently of quoting so the
 * measurement can be exercised on its own.
 * Returns 0 on success, negative errno on failure.
 */
int __cheri_compartment("attestation")
  attestation_measure_image(uint8_t *hashOut, size_t *hashLen);

/**
 * Produce a signed attestation quote for the given verifier nonce.
 * nonceLen must equal AttestationNonceLength. Measures the image, builds the
 * digest, and signs it via fake_tpm. Fills quoteOut.
 * Returns 0 on success, negative errno on failure.
 */
int __cheri_compartment("attestation")
  attestation_quote(const uint8_t    *nonce,
                    size_t            nonceLen,
                    AttestationQuote *quoteOut);

/**
 * Serialise a quote to its wire form (see AttestationQuoteWireMaxLength):
 *   slot(1) | device_id_len(1) | device_id | image_hash | nonce | signature
 * out must be at least AttestationQuoteWireMaxLength bytes. Returns the number
 * of bytes written. Pure data shuffling (no capabilities), so it lives in the
 * header for callers that publish the quote.
 */
static inline size_t attestation_quote_serialize(const AttestationQuote *q,
                                                 uint8_t                *out)
{
	size_t p = 0;
	out[p++] = q->slot;
	out[p++] = q->deviceIdLen;
	for (size_t i = 0; i < q->deviceIdLen; i++)
	{
		out[p++] = static_cast<uint8_t>(q->deviceId[i]);
	}
	for (unsigned char i : q->imageHash)
	{
		out[p++] = i;
	}
	for (unsigned char i : q->nonce)
	{
		out[p++] = i;
	}
	for (unsigned char i : q->signature)
	{
		out[p++] = i;
	}
	return p;
}
