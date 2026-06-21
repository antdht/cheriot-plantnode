import ctypes
import struct
import subprocess
from pathlib import Path

# Relative to the charter/ working directory: sonata-software is a sibling of
# the cheriot-plantnode project (two levels up from here).
DEFAULT_FIRMWARE_ELF = (
    "../../sonata-software/build/cheriot/cheriot/release/plantnode.strip"
)

# Must match attestation.cc exactly: hydro_hash, 8-byte context "PN-IMAGE",
# empty key, 32-byte digest.
IMAGE_HASH_CONTEXT = "PN-IMAGE"
IMAGE_HASH_LENGTH = 32

# Quote-digest hash context (attestation.cc kQuoteContext) and signing context
# (fake_tpm.cc kSignContext). Must match the firmware byte-for-byte.
QUOTE_DIGEST_CONTEXT = "PN-QUOTE"
SIGN_CONTEXT = "PN-ATST1"
# Nonce-combination context (crypto.cc kCombineCtx / AttestationCombineContext).
COMBINE_CONTEXT = "PN-COMB1"

NONCE_LENGTH = 32
SIGNATURE_LENGTH = 64  # hydro_sign_BYTES
PUBLIC_KEY_LENGTH = 32  # hydro_sign_PUBLICKEYBYTES
SEED_LENGTH = 32  # hydro_sign_SEEDBYTES
SECRET_KEY_LENGTH = 64  # hydro_sign_SECRETKEYBYTES

# DEMO seed compiled into fake_tpm.cc (kSeed). The verifier derives the device's
# signing public key offline from this via hydro_sign_keygen_deterministic. A
# real deployment would provision a hardware-unique key never present in the image.
FAKE_TPM_SEED = b"PLANTNODE-fake-tpm-seed-v1-demo!"
assert len(FAKE_TPM_SEED) == SEED_LENGTH

# Remote-attestation handshake message-type tags (first plaintext byte).
# Verifier -> device:
RA_CHALLENGE1 = 0x01  # payload: nonce_V[32]
RA_CHALLENGE2 = 0x02  # payload: combined[32]
RA_APPROVED = 0x03  # payload: combined[32] (quote accepted; device may publish)
# Device -> verifier:
RA_NONCE_REPLY = 0x01  # payload: nonce_V[32] || nonce_D[32]
RA_QUOTE = 0x02  # payload: serialised AttestationQuote

# We hash with the *same* libhydrogen the firmware is built against, NOT cydrogen
# (whose bundled libhydrogen is a different version and produces different
# digests for the same input). cheriot-demos is a sibling of cheriot-plantnode.
_LIBHYDROGEN_SRC = (
    Path(__file__).resolve().parent.parent.parent
    / "cheriot-demos/third_party/crypto/libhydrogen"
)
_LIBHYDROGEN_SO = Path(__file__).resolve().parent / "_libhydrogen.so"


def _load_libhydrogen() -> ctypes.CDLL:
    """Compile (once, cached) and load the firmware's libhydrogen as a shared
    library so the verifier hashes byte-for-byte like the device."""
    src = _LIBHYDROGEN_SRC / "hydrogen.c"
    if not src.exists():
        raise FileNotFoundError(f"libhydrogen source not found at {src}")
    if not _LIBHYDROGEN_SO.exists() or (
        _LIBHYDROGEN_SO.stat().st_mtime < src.stat().st_mtime
    ):
        subprocess.run(
            [
                "cc",
                "-O2",
                "-fPIC",
                "-shared",
                "-I",
                str(_LIBHYDROGEN_SRC),
                str(src),
                "-o",
                str(_LIBHYDROGEN_SO),
            ],
            check=True,
        )
    lib = ctypes.CDLL(str(_LIBHYDROGEN_SO))
    lib.hydro_init.restype = ctypes.c_int
    lib.hydro_hash_hash.restype = ctypes.c_int
    lib.hydro_hash_hash.argtypes = [
        ctypes.c_char_p,
        ctypes.c_size_t,  # out, out_len
        ctypes.c_char_p,
        ctypes.c_size_t,  # in, in_len
        ctypes.c_char_p,  # ctx[8]
        ctypes.c_char_p,  # key[32] or NULL
    ]
    # Ed25519-style signing (hydro_sign), to derive the device key and verify
    # quotes exactly as fake_tpm.cc produces them.
    lib.hydro_sign_keygen_deterministic.restype = None
    lib.hydro_sign_keygen_deterministic.argtypes = [
        ctypes.c_char_p,  # hydro_sign_keypair* (pk[32] || sk[64])
        ctypes.c_char_p,  # seed[32]
    ]
    lib.hydro_sign_verify.restype = ctypes.c_int
    lib.hydro_sign_verify.argtypes = [
        ctypes.c_char_p,  # csig[64]
        ctypes.c_char_p,
        ctypes.c_size_t,  # m, m_len
        ctypes.c_char_p,  # ctx[8]
        ctypes.c_char_p,  # pk[32]
    ]
    lib.hydro_init()
    return lib


_LIB = _load_libhydrogen()


def hydro_hash(data: bytes, ctx: str, digest_size: int = IMAGE_HASH_LENGTH) -> bytes:
    """hydro_hash_hash(out, out_len, data, len, ctx, NULL) — matches the device's
    hydro_hash over a NULL (empty) key."""
    ctx_bytes = ctx.encode()
    if len(ctx_bytes) != 8:
        raise ValueError("hydro_hash context must be exactly 8 bytes")
    out = ctypes.create_string_buffer(digest_size)
    rc = _LIB.hydro_hash_hash(out, digest_size, data, len(data), ctx_bytes, None)
    if rc != 0:
        raise RuntimeError(f"hydro_hash_hash failed (rc={rc})")
    return out.raw[:digest_size]


def hydro_sign_verify(sig: bytes, msg: bytes, ctx: str, pk: bytes) -> bool:
    """hydro_sign_verify(sig, m, m_len, ctx, pk) == 0 — matches fake_tpm.cc's
    hydro_sign_create over the same message (the 32-byte quote digest)."""
    ctx_bytes = ctx.encode()
    if len(ctx_bytes) != 8:
        raise ValueError("sign context must be exactly 8 bytes")
    if len(sig) != SIGNATURE_LENGTH:
        raise ValueError(f"signature must be {SIGNATURE_LENGTH} bytes")
    if len(pk) != PUBLIC_KEY_LENGTH:
        raise ValueError(f"public key must be {PUBLIC_KEY_LENGTH} bytes")
    rc = _LIB.hydro_sign_verify(sig, msg, len(msg), ctx_bytes, pk)
    return rc == 0


_DEVICE_PUBKEY: bytes | None = None


def device_signing_pubkey() -> bytes:
    """Derive (and cache) the device's Ed25519 signing public key from the demo
    seed compiled into fake_tpm.cc, via hydro_sign_keygen_deterministic."""
    global _DEVICE_PUBKEY
    if _DEVICE_PUBKEY is None:
        keypair = ctypes.create_string_buffer(PUBLIC_KEY_LENGTH + SECRET_KEY_LENGTH)
        _LIB.hydro_sign_keygen_deterministic(keypair, FAKE_TPM_SEED)
        _DEVICE_PUBKEY = keypair.raw[:PUBLIC_KEY_LENGTH]
    return _DEVICE_PUBKEY


def combine_nonce(nonce_v: bytes, nonce_d: bytes) -> bytes:
    """combined = hydro_hash(ctx="PN-COMB1", nonce_V || nonce_D, 32). Mirrors
    crypto_combine_nonce() on the device."""
    if len(nonce_v) != NONCE_LENGTH or len(nonce_d) != NONCE_LENGTH:
        raise ValueError(f"each nonce must be {NONCE_LENGTH} bytes")
    return hydro_hash(nonce_v + nonce_d, COMBINE_CONTEXT, NONCE_LENGTH)


def deserialize_quote(buf: bytes) -> dict:
    """Parse the wire form produced by attestation_quote_serialize():
    slot(1) | device_id_len(1) | device_id | image_hash(32) | nonce(32) |
    signature(64)."""
    if len(buf) < 2:
        raise ValueError("quote too short")
    slot = buf[0]
    dev_id_len = buf[1]
    off = 2
    device_id = buf[off : off + dev_id_len]
    off += dev_id_len
    image_hash = buf[off : off + IMAGE_HASH_LENGTH]
    off += IMAGE_HASH_LENGTH
    nonce = buf[off : off + NONCE_LENGTH]
    off += NONCE_LENGTH
    signature = buf[off : off + SIGNATURE_LENGTH]
    off += SIGNATURE_LENGTH
    if off != len(buf):
        raise ValueError(f"quote length mismatch (parsed {off}, got {len(buf)} bytes)")
    if len(image_hash) != IMAGE_HASH_LENGTH or len(nonce) != NONCE_LENGTH:
        raise ValueError("quote truncated")
    if len(signature) != SIGNATURE_LENGTH:
        raise ValueError("quote signature truncated")
    return {
        "slot": slot,
        "device_id": device_id,
        "image_hash": image_hash,
        "nonce": nonce,
        "signature": signature,
    }


def quote_digest(quote: dict) -> bytes:
    """Rebuild the signed digest exactly as attestation.cc does:
    hydro_hash(ctx="PN-QUOTE", device_id || slot || image_hash || nonce, 32).
    Note device_id is the actual device_id_len bytes, not a padded field."""
    msg = (
        quote["device_id"]
        + bytes([quote["slot"]])
        + quote["image_hash"]
        + quote["nonce"]
    )
    return hydro_hash(msg, QUOTE_DIGEST_CONTEXT, IMAGE_HASH_LENGTH)


def verify_quote(
    quote: dict, expected_combined: bytes, elf_path: str = DEFAULT_FIRMWARE_ELF
) -> tuple[bool, list[str]]:
    """Verify a deserialised quote: signature over the rebuilt digest, the
    combined nonce, and the image hash. Returns (ok, reasons)."""
    reasons: list[str] = []

    digest = quote_digest(quote)
    sig_ok = hydro_sign_verify(
        quote["signature"], digest, SIGN_CONTEXT, device_signing_pubkey()
    )
    reasons.append("signature OK" if sig_ok else "signature INVALID")

    nonce_ok = quote["nonce"] == expected_combined
    reasons.append("combined nonce OK" if nonce_ok else "combined nonce MISMATCH")

    try:
        expected_img = expected_image_hash(elf_path)
        image_ok = quote["image_hash"] == expected_img
        reasons.append(
            "image hash OK"
            if image_ok
            else f"image hash MISMATCH (got {quote['image_hash'].hex()}, "
            f"expected {expected_img.hex()})"
        )
    except Exception as e:
        image_ok = False
        reasons.append(f"image hash UNVERIFIABLE ({e})")

    return (sig_ok and nonce_ok and image_ok), reasons


def _measured_length(elf: bytes) -> int:
    """
    Number of leading bytes of the ELF the device hashes.

    Mirrors measure_image() in attestation.cc: the device reads the ELF32 header
    and hashes bytes [0, e_shoff + e_shnum * e_shentsize), i.e. up to the end of
    the section-header table. For a normal ELF this equals the file size.

    ELF32 little-endian field offsets: e_shoff @ 32 (u32), e_shentsize @ 46
    (u16), e_shnum @ 48 (u16).
    """
    if len(elf) < 52 or elf[:4] != b"\x7fELF":
        raise ValueError("Not a valid ELF32 image")
    e_shoff = struct.unpack_from("<I", elf, 32)[0]
    e_shentsize = struct.unpack_from("<H", elf, 46)[0]
    e_shnum = struct.unpack_from("<H", elf, 48)[0]
    total = e_shoff + e_shnum * e_shentsize
    if not 52 <= total <= len(elf):
        raise ValueError(
            f"Implausible measured length {total} (file is {len(elf)} bytes)"
        )
    return total


def expected_image_hash(elf_path: str = DEFAULT_FIRMWARE_ELF) -> bytes:
    """
    Recompute the 32-byte firmware measurement the device publishes on
    plantnode/attestation: the firmware's hydro_hash over the booted slot's ELF,
    context "PN-IMAGE", empty key, 32-byte digest.
    """
    elf = Path(elf_path).read_bytes()
    total = _measured_length(elf)
    return hydro_hash(elf[:total], IMAGE_HASH_CONTEXT, IMAGE_HASH_LENGTH)


def verify_image_hash(received: bytes, elf_path: str = DEFAULT_FIRMWARE_ELF) -> bool:
    """
    Check a raw attestation payload (the 32 bytes published on
    plantnode/attestation) against the expected hash of elf_path.
    """
    if len(received) != IMAGE_HASH_LENGTH:
        raise ValueError(
            f"Attestation payload has wrong length "
            f"({len(received)} bytes, expected {IMAGE_HASH_LENGTH})"
        )
    return received == expected_image_hash(elf_path)


if __name__ == "__main__":
    import sys

    path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_FIRMWARE_ELF
    print(f"Firmware artifact: {path}")
    print(f"Expected image hash: {expected_image_hash(path).hex()}")
