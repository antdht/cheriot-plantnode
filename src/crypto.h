// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#pragma once

#include "plantnode_types.h"
#include <compartment.h>
#include <stddef.h>
#include <stdint.h>

// hydro_kx_N_PACKET1BYTES = 32 + 16 = 48
constexpr size_t CryptoKxPacketLen = 48;
// 8-byte msg_id prefix + hydro_secretbox_HEADERBYTES (36) + max plaintext (96)
constexpr size_t CryptoEncryptedMaxLen = 8 + 36 + 96;

/**
 * One-time boot: Noise-N KX step 1 using the verifier's compiled-in public key.
 * Stores both TX (device→server) and RX (server→device) session keys internally.
 * packetOut receives the 48-byte packet1 to publish so the verifier can derive
 * its session keys. Must be called before crypto_encrypt or crypto_decrypt.
 */
int __cheri_compartment("crypto")
  crypto_init_session(uint8_t *packetOut, size_t *packetLen);

/**
 * Encrypt a sensor reading using hydro_secretbox with the session TX key.
 * Wire format: [8-byte msg_id LE][hydro_secretbox header + ciphertext].
 * outBuf must be at least CryptoEncryptedMaxLen bytes.
 * Returns 0 on success, negative errno on failure.
 */
int __cheri_compartment("crypto")
  crypto_encrypt(const SensorReading *reading, uint8_t *outBuf, size_t *outLen);

/**
 * Decrypt a server command blob using hydro_secretbox with the session RX key.
 * Expects wire format: [8-byte msg_id LE][hydro_secretbox ciphertext].
 * Returns 0 on success, -EBADMSG if MAC fails, -ENOSYS if session not ready.
 */
int __cheri_compartment("crypto")
  crypto_decrypt(const uint8_t *inBuf,
                 size_t         inLen,
                 uint8_t       *plainOut,
                 size_t        *plainLen);
