// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#include "attestation.h"
#include <debug.hh>
#include <errno.h>
#include <string.h>

using Debug = ConditionalDebug<true, "PlantNode Attest">;

// Hypothesis: a sealed capability or secure element provides access to the
// device private key. The key bytes never leave this compartment.

int __cheri_compartment("attestation") attestation_sign(const void *data,
                                                        size_t      len,
                                                        uint8_t    *sig_out,
                                                        size_t     *sig_len)
{
	if (!data || !sig_out || !sig_len)
	{
		return -EINVAL;
	}

	Debug::log("attestation_sign: TODO (len={})", len);
	// TODO: unseal device key via sealed capability, run ECDSA/HMAC, write to
	// sig_out
	*sig_len = 0;
	return -ENOSYS;
}

int __cheri_compartment("attestation")
  attestation_get_device_id(char *id_out, size_t *id_len)
{
	if (!id_out || !id_len)
	{
		return -EINVAL;
	}

	Debug::log("attestation_get_device_id: TODO");
	// TODO: derive or retrieve the device identifier from secure storage
	*id_len = 0;
	return -ENOSYS;
}

int __cheri_compartment("attestation") attestation_verify(const void *data,
                                                          size_t      data_len,
                                                          const uint8_t *sig,
                                                          size_t sig_len)
{
	if (!data || !sig)
	{
		return -EINVAL;
	}

	Debug::log(
	  "attestation_verify: TODO (data_len={}, sig_len={})", data_len, sig_len);
	// TODO: verify signature against the server's public key
	return -ENOSYS;
}
