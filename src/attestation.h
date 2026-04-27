// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#pragma once

#include "plantnode_types.h"
#include <compartment.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Write the device identity string (e.g. "plantnode-001") to id_out.
 * id_out must be at least DeviceIdMaxLength bytes.
 * Returns 0 on success, negative errno on failure.
 */
int __cheri_compartment("attestation")
  attestation_get_device_id(char *id_out, size_t *id_len);

/**
 * Sign data using the device's private key.
 * TODO: not yet implemented — returns -ENOSYS.
 * sig_out must be at least AttestationSignatureMaxLength bytes.
 * Returns 0 on success, negative errno on failure.
 */
int __cheri_compartment("attestation") attestation_sign(const void *data,
                                                        size_t      len,
                                                        uint8_t    *sig_out,
                                                        size_t     *sig_len);
