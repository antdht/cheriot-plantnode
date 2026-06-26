// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#include "tpm.h"
#include <cheri.hh>
#include <debug.hh>
#include <errno.h>
#include <string.h>

#include "plantnode_types.h"

using Debug = ConditionalDebug<true, "PlantNode TPM">;

// MOCK ONLY. Canned opaque "attestation token" the mock hands out in place of
// whatever a real secure element would produce. There is no key and no real
// attestation behind it.
static const uint8_t KMockToken[AttestationTokenLength] = {
  0x70, 0x6c, 0x61, 0x6e, 0x74, 0x6e, 0x6f, 0x64, 0x65, 0x2d, 0x6d,
  0x6f, 0x63, 0x6b, 0x2d, 0x74, 0x6f, 0x6b, 0x65, 0x6e, 0x2d, 0x76,
  0x31, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21};

int __cheri_compartment("tpm") tpm_attest(uint8_t *tokenOut, size_t *tokenLen)
{
	if (!tokenOut || !tokenLen)
	{
		return -EINVAL;
	}
	if (!CHERI::check_pointer(tokenOut, AttestationTokenLength))
	{
		return -EINVAL;
	}
	// MOCK: return the canned token. No real cryptography.
	memcpy(tokenOut, KMockToken, AttestationTokenLength);
	*tokenLen = AttestationTokenLength;
	Debug::log("tpm_attest (MOCK): returned canned attestation token");
	return 0;
}
