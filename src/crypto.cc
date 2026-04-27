// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#include "crypto.h"
#include <debug.hh>
#include <errno.h>
#include <platform-entropy.hh>
#include <string.h>

#include "hydrogen.h"

using Debug = ConditionalDebug<true, "PlantNode Crypto">;

// Required by libhydrogen for seeding its internal PRNG.
// Must be defined in the same compartment as hydrogen.c.
extern "C" uint32_t rand_32()
{
	static EntropySource rng;
	uint64_t             r = rng();
	return static_cast<uint32_t>(r);
}

// Verifier's static Noise-N public key (compiled in).
// Generated once: cydrogen.KxPair.gen(); verifier holds the sk.
static const uint8_t kVerifierPublicKey[hydro_kx_PUBLICKEYBYTES] = {
  0x8d, 0xae, 0x2d, 0x4d, 0xdc, 0x9b, 0xe6, 0x5b, 0x4e, 0x89, 0xaf,
  0x65, 0x9c, 0xbc, 0xc1, 0xff, 0x4e, 0xd1, 0xe9, 0xb7, 0x90, 0xd7,
  0x7e, 0xd0, 0xd9, 0x9b, 0x80, 0x20, 0xa9, 0x67, 0x2a, 0x16};

// Context strings (must be exactly 8 bytes).
// kBoxCtx matches cydrogen's SecretBox default (8 space bytes 0x20).
static const char kBoxCtx[hydro_secretbox_CONTEXTBYTES] =
  {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};

// Per-boot session keys (both directions).
static uint8_t  s_sessionTxKey[hydro_kx_SESSIONKEYBYTES];
static uint8_t  s_sessionRxKey[hydro_kx_SESSIONKEYBYTES];
static bool     s_sessionReady = false;
static uint64_t s_msgId        = 0;

static void ensure_init()
{
	static bool sInitDone = false;
	if (!sInitDone)
	{
		hydro_init();
		sInitDone = true;
	}
}

int __cheri_compartment("crypto")
  crypto_init_session(uint8_t *packetOut, size_t *packetLen)
{
	if (!packetOut || !packetLen)
	{
		return -EINVAL;
	}

	ensure_init();

	hydro_kx_session_keypair sessionKp;
	uint8_t                  packet1[hydro_kx_N_PACKET1BYTES];

	if (hydro_kx_n_1(&sessionKp, packet1, nullptr, kVerifierPublicKey) != 0)
	{
		Debug::log("Noise-N kx step 1 failed");
		return -EIO;
	}

	memcpy(s_sessionTxKey, sessionKp.tx, hydro_kx_SESSIONKEYBYTES);
	memcpy(s_sessionRxKey, sessionKp.rx, hydro_kx_SESSIONKEYBYTES);
	s_sessionReady = true;
	s_msgId        = 0;

	memcpy(packetOut, packet1, hydro_kx_N_PACKET1BYTES);
	*packetLen = hydro_kx_N_PACKET1BYTES;

	Debug::log("Noise-N packet1 generated ({} bytes)", hydro_kx_N_PACKET1BYTES);
	return 0;
}

int __cheri_compartment("crypto")
  crypto_encrypt(const SensorReading *reading, uint8_t *outBuf, size_t *outLen)
{
	if (!reading || !outBuf || !outLen)
	{
		return -EINVAL;
	}
	ensure_init();
	if (!s_sessionReady)
	{
		Debug::log("crypto_encrypt: session not ready");
		return -ENOSYS;
	}

	// Serialise reading to compact JSON without snprintf (no stdio dependency).
	// Format: {"timestamp":U,"humidity":U,"temperature":I,"moisture":U}
	char   plain[96];
	size_t pos = 0;

	auto write_char = [&](char c) -> bool {
		if (pos >= sizeof(plain))
			return false;
		plain[pos++] = c;
		return true;
	};
	auto write_literal = [&](const char *s, size_t n) -> bool {
		if (pos + n > sizeof(plain))
			return false;
		memcpy(plain + pos, s, n);
		pos += n;
		return true;
	};
	auto write_uint = [&](uint32_t v) -> bool {
		char tmp[11];
		int  len = 0;
		if (v == 0)
		{
			tmp[len++] = '0';
		}
		else
		{
			while (v)
			{
				tmp[len++] = '0' + (v % 10);
				v /= 10;
			}
			for (int i = 0, j = len - 1; i < j; i++, j--)
			{
				char t = tmp[i];
				tmp[i] = tmp[j];
				tmp[j] = t;
			}
		}
		if (pos + (size_t)len > sizeof(plain))
			return false;
		memcpy(plain + pos, tmp, len);
		pos += len;
		return true;
	};
	auto write_int = [&](int32_t v) -> bool {
		if (v < 0)
		{
			if (!write_char('-'))
				return false;
			v = -v;
		}
		return write_uint((uint32_t)v);
	};

#define WL(s) write_literal(s, sizeof(s) - 1)
	bool ok = WL("{\"timestamp\":") && write_uint(reading->timestamp) &&
	          WL(",\"humidity\":") && write_uint(reading->humidity_rx10) &&
	          WL(",\"temperature\":") && write_int(reading->temperature_cx10) &&
	          WL(",\"moisture\":") && write_uint(reading->moisture_raw) &&
	          WL("}");
#undef WL

	if (!ok)
	{
		Debug::log("crypto_encrypt: JSON buffer overflow");
		return -ENOMEM;
	}
	size_t plainLen = pos;

	// Write msg_id as 8 little-endian bytes.
	uint64_t msgId = s_msgId;
	for (int i = 0; i < 8; i++)
	{
		outBuf[i] = (uint8_t)(msgId >> (8 * i));
	}

	if (hydro_secretbox_encrypt(
	      outBuf + 8, plain, plainLen, msgId, kBoxCtx, s_sessionTxKey) != 0)
	{
		Debug::log("crypto_encrypt: secretbox encrypt failed");
		return -EIO;
	}

	s_msgId++;
	*outLen = 8 + hydro_secretbox_HEADERBYTES + plainLen;

	Debug::log("Encrypted payload: msg_id={}, {} bytes", msgId, *outLen);
	return 0;
}

int __cheri_compartment("crypto") crypto_decrypt(const uint8_t *inBuf,
                                                 size_t         inLen,
                                                 uint8_t       *plainOut,
                                                 size_t        *plainLen)
{
	if (!inBuf || !plainOut || !plainLen)
	{
		return -EINVAL;
	}
	ensure_init();
	if (!s_sessionReady)
	{
		Debug::log("crypto_decrypt: session not ready");
		return -ENOSYS;
	}
	if (inLen < 8 + hydro_secretbox_HEADERBYTES)
	{
		Debug::log("crypto_decrypt: buffer too short ({} bytes)", inLen);
		return -EINVAL;
	}

	// msg_id is passed to hydro_secretbox_decrypt as an AEAD nonce tweak.
	// libhydrogen uses it for domain separation, not replay prevention.
	// Replay protection (e.g. monotonic window) is a future concern.
	// Extract msg_id from first 8 bytes (little-endian).
	uint64_t msgId = 0;
	for (int i = 0; i < 8; i++)
	{
		msgId |= ((uint64_t)inBuf[i]) << (8 * i);
	}

	size_t cipherLen    = inLen - 8;
	size_t decryptedLen = cipherLen - hydro_secretbox_HEADERBYTES;

	if (hydro_secretbox_decrypt(
	      plainOut, inBuf + 8, cipherLen, msgId, kBoxCtx, s_sessionRxKey) != 0)
	{
		Debug::log("crypto_decrypt: MAC verification failed");
		return -EBADMSG;
	}

	*plainLen = decryptedLen;
	Debug::log("Decrypted command: msg_id={}, {} bytes", msgId, *plainLen);
	return 0;
}
