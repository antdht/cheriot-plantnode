// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#include "attestation.h"
#include "tpm.h"
#include <cheri.hh>
#include <debug.hh>
#include <errno.h>
#include <string.h>

using Debug = ConditionalDebug<true, "PlantNode Attest">;

// MOCK ONLY. Focus has moved off real attestation: this compartment no longer
// measures anything. It just gathers abstract evidence (device id + an opaque
// token from the mock tpm). The cross-compartment call into tpm is kept so the
// flow can still be exercised end to end.

static const char KDeviceId[] = "plantnode-001";

int __cheri_compartment("attestation")
  attestation_get_device_id(char *idOut, size_t *idLen)
{
	if (!idOut || !idLen)
	{
		return -EINVAL;
	}
	size_t len = sizeof(KDeviceId) - 1;
	memcpy(idOut, KDeviceId, len);
	*idLen = len;
	return 0;
}

int __cheri_compartment("attestation")
  attestation_get_evidence(AttestationEvidence *evidenceOut)
{
	if (!evidenceOut)
	{
		return -EINVAL;
	}
	if (!CHERI::check_pointer(evidenceOut, sizeof(AttestationEvidence)))
	{
		return -EINVAL;
	}

	memset(evidenceOut, 0, sizeof(*evidenceOut));

	const size_t IdLen = sizeof(KDeviceId) - 1;
	memcpy(evidenceOut->deviceId, KDeviceId, IdLen);
	evidenceOut->deviceIdLen = static_cast<uint8_t>(IdLen);

	// Ask the (mock) tpm to vouch for the device with an opaque token, keeping
	// the call chain intact.
	size_t tokenLen = 0;
	int    ret      = tpm_attest(evidenceOut->token, &tokenLen);
	if (ret != 0)
	{
		Debug::log("attestation_get_evidence (MOCK): tpm_attest failed {}",
		           ret);
		return ret;
	}

	Debug::log("attestation_get_evidence (MOCK): evidence ready");
	return 0;
}
