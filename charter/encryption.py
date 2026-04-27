import json
from pathlib import Path

import cydrogen


def load_verifier_keypair() -> cydrogen.KxPair:
    """
    Load the verifier's Noise-N static keypair from keys/verifier.pub and
    keys/verifier.key. The KxPair is reconstructed as pk||sk (64 bytes).
    """
    pk_bytes = Path("keys/verifier.pub").read_bytes()
    sk_bytes = Path("keys/verifier.key").read_bytes()
    return cydrogen.KxPair(pk_bytes + sk_bytes)


def recover_session_key(verifier_kp: cydrogen.KxPair, packet1: bytes) -> bytes:
    """
    Perform Noise-N step 2: recover the session rx-key from the device's
    48-byte packet1 using the verifier's static keypair.
    The device's tx-key == the verifier's rx-key.
    """
    session = cydrogen.kx_n_gen_session_from_packet(verifier_kp, packet1)
    return bytes(session.rx)


def decrypt_telemetry(session_rx: bytes, raw: bytes) -> dict:
    """
    Decrypt a binary telemetry payload produced by attestation_encrypt_payload().

    Wire format: [8 bytes msg_id, little-endian] [hydro_secretbox ciphertext]

    The secretbox context is the cydrogen default (8 space bytes, 0x20),
    which matches the C-side kBoxCtx = "        ".
    """
    if len(raw) < 8:
        raise ValueError(f"Telemetry payload too short: {len(raw)} bytes")

    msg_id = int.from_bytes(raw[:8], "little")
    ciphertext = raw[8:]

    key = cydrogen.SecretBoxKey(session_rx)
    sb = cydrogen.SecretBox(key)
    plaintext = sb.decrypt(ciphertext, msg_id=msg_id)

    return json.loads(plaintext)
