// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#include "attestation.h"
#include <debug.hh>
#include <errno.h>
#include <string.h>

using Debug = ConditionalDebug<true, "PlantNode Attest">;

static const char kDeviceId[] = "plantnode-001";

int __cheri_compartment("attestation")
  attestation_get_device_id(char *id_out, size_t *id_len)
{
	if (!id_out || !id_len)
	{
		return -EINVAL;
	}
	size_t len = sizeof(kDeviceId) - 1;
	memcpy(id_out, kDeviceId, len);
	*id_len = len;
	return 0;
}

int __cheri_compartment("attestation") attestation_sign(const void *data,
                                                        size_t      len,
                                                        uint8_t    *sig_out,
                                                        size_t     *sig_len)
{
	(void)data;
	(void)len;
	(void)sig_out;
	(void)sig_len;
	Debug::log("attestation_sign: not yet implemented");
	return -ENOSYS;
}
