// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#include "fake_tpm.h"
#include <cheri.hh>
#include <debug.hh>
#include <errno.h>
#include <platform-entropy.hh>
#include <string.h>

#include "hydrogen.h"
#include "plantnode_types.h"

using Debug = ConditionalDebug<true, "PlantNode fakeTPM">;

// Required by libhydrogen for seeding its internal PRNG. Must be defined in the
// same compartment as hydrogen.c. Signing itself is deterministic, but the
// symbol must resolve and hydro_init() consumes entropy.
extern "C" uint32_t rand_32()
{
	static EntropySource rng;
	uint64_t             r = rng();
	return static_cast<uint32_t>(r);
}

// Domain-separation context (exactly 8 bytes). Every signature this key ever
// produces carries this context, so quotes cannot be confused with signatures
// from any other protocol using the same primitive.
static const char KSignContext[hydro_sign_CONTEXTBYTES] =
  {'P', 'N', '-', 'A', 'T', 'S', 'T', '1'};

// DEMO ONLY. Deterministic seed compiled into the firmware image so the build
// is reproducible and the verifier can derive the matching public key offline
// via hydro_sign_keygen_deterministic(seed). A real TPM would hold a
// hardware-unique secret in tamper-resistant storage, provisioned at
// manufacture and NEVER present in the firmware image. This models that store,
// with CHERIoT compartment isolation (not hardware) as the protection boundary.
static const uint8_t KSeed[hydro_sign_SEEDBYTES] = {
  0x50, 0x4c, 0x41, 0x4e, 0x54, 0x4e, 0x4f, 0x44, 0x45, 0x2d, 0x66,
  0x61, 0x6b, 0x65, 0x2d, 0x74, 0x70, 0x6d, 0x2d, 0x73, 0x65, 0x65,
  0x64, 0x2d, 0x76, 0x31, 0x2d, 0x64, 0x65, 0x6d, 0x6f, 0x21};

static hydro_sign_keypair sKeypair;
static bool               sReady = false;

static void ensure_init()
{
	if (!sReady)
	{
		hydro_init();
		hydro_sign_keygen_deterministic(&sKeypair, KSeed);
		sReady = true;
	}
}

int __cheri_compartment("fake_tpm") fake_tpm_sign(const uint8_t *digest,
                                                  size_t         digestLen,
                                                  uint8_t       *sigOut,
                                                  size_t        *sigLen)
{
	if (!digest || !sigOut || !sigLen)
	{
		return -EINVAL;
	}
	// Fixed-size digest only. See header: prevents use as a general oracle.
	if (digestLen != AttestationDigestLength)
	{
		Debug::log("fake_tpm_sign: bad digest length {}", digestLen);
		return -EINVAL;
	}
	if (!CHERI::check_pointer(digest, digestLen) ||
	    !CHERI::check_pointer(sigOut, AttestationSignatureMaxLength))
	{
		return -EINVAL;
	}

	ensure_init();

	if (hydro_sign_create(
	      sigOut, digest, digestLen, KSignContext, sKeypair.sk) != 0)
	{
		Debug::log("fake_tpm_sign: hydro_sign_create failed");
		return -EIO;
	}
	*sigLen = hydro_sign_BYTES;
	return 0;
}

int __cheri_compartment("fake_tpm")
  fake_tpm_get_public_key(uint8_t *pkOut, size_t *pkLen)
{
	if (!pkOut || !pkLen)
	{
		return -EINVAL;
	}
	if (!CHERI::check_pointer(pkOut, hydro_sign_PUBLICKEYBYTES))
	{
		return -EINVAL;
	}
	ensure_init();
	memcpy(pkOut, sKeypair.pk, hydro_sign_PUBLICKEYBYTES);
	*pkLen = hydro_sign_PUBLICKEYBYTES;
	return 0;
}
