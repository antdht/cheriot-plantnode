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
                "cc", "-O2", "-fPIC", "-shared",
                "-I", str(_LIBHYDROGEN_SRC),
                str(src), "-o", str(_LIBHYDROGEN_SO),
            ],
            check=True,
        )
    lib = ctypes.CDLL(str(_LIBHYDROGEN_SO))
    lib.hydro_init.restype = ctypes.c_int
    lib.hydro_hash_hash.restype = ctypes.c_int
    lib.hydro_hash_hash.argtypes = [
        ctypes.c_char_p, ctypes.c_size_t,  # out, out_len
        ctypes.c_char_p, ctypes.c_size_t,  # in, in_len
        ctypes.c_char_p,                   # ctx[8]
        ctypes.c_char_p,                   # key[32] or NULL
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


def verify_image_hash(
    received: bytes, elf_path: str = DEFAULT_FIRMWARE_ELF
) -> bool:
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
