// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#pragma once

#include "plantnode_types.h"
#include <compartment.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Sign @p data using the device's private key.
 *
 * The key material never leaves this compartment — only the resulting
 * signature is written to @p sig_out. The hypothesis is that a secure
 * element / sealed-capability store backs the key access.
 *
 * @param data      Data buffer to sign.
 * @param len       Length of data in bytes.
 * @param sig_out   Caller-allocated output buffer of at least
 *                  AttestationSignatureMaxLength bytes.
 * @param sig_len   Populated with the actual signature length on success.
 *
 * Returns 0 on success, negative errno on failure.
 */
int __cheri_compartment("attestation") attestation_sign(const void *data,
                                                        size_t      len,
                                                        uint8_t    *sig_out,
                                                        size_t     *sig_len);

/**
 * Write the device identity string (e.g. a fingerprint or serial) to
 * @p id_out.
 *
 * @param id_out   Caller-allocated buffer of at least DeviceIdMaxLength bytes.
 * @param id_len   Populated with the number of bytes written (excl. NUL).
 *
 * Returns 0 on success, negative errno on failure.
 */
int __cheri_compartment("attestation")
  attestation_get_device_id(char *id_out, size_t *id_len);

/**
 * Verify a signature against data, e.g. to authenticate an incoming command.
 *
 * @param data     Data that was supposedly signed.
 * @param data_len Length of data in bytes.
 * @param sig      Signature bytes.
 * @param sig_len  Length of signature in bytes.
 *
 * Returns 0 if the signature is valid, -EBADMSG otherwise.
 */
int __cheri_compartment("attestation")
  attestation_verify(const void    *data,
                     size_t         data_len,
                     const uint8_t *sig,
                     size_t         sig_len);
