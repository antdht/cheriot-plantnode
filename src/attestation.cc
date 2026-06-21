// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#include "attestation.h"
#include "fake_tpm.h"
#include <cheri.hh>
#include <compartment-macros.h>
#include <debug.hh>
#include <errno.h>
#include <platform-entropy.hh>
#include <platform-gpio.hh>
#include <platform-spi.hh>
#include <string.h>

#include "hydrogen.h"

using Debug = ConditionalDebug<true, "PlantNode Attest">;

// Required by libhydrogen (hydrogen.c is compiled into this compartment for
// hydro_hash). Signing lives in fake_tpm; here we only hash, but the symbol
// must still resolve.
extern "C" uint32_t rand_32()
{
	static EntropySource rng;
	uint64_t             r = rng();
	return static_cast<uint32_t>(r);
}

static const char KDeviceId[] = "plantnode-001";

// 8-byte hydro_hash contexts (domain separation). The verifier MUST use the
// identical contexts when recomputing the image hash and the quote digest.
static const char KImageHashContext[hydro_hash_CONTEXTBYTES] =
  {'P', 'N', '-', 'I', 'M', 'A', 'G', 'E'};
static const char KQuoteContext[hydro_hash_CONTEXTBYTES] =
  {'P', 'N', '-', 'Q', 'U', 'O', 'T', 'E'};

// Software-slot flash layout
//
// Base offsets of the three software slots in SPI flash, mirroring the Sonata
// bootloader's SoftwareSlots[] (sonata-system
// .../sw/cheri/boot/boot_loader.cc): each slot is 10 MiB and the bootloader
// reads the *selected* slot over SPI via SpiFlash::read() to load the ELF. The
// attestation compartment re-reads the SAME slot over the SAME SPI device
// (spi2) to measure exactly what booted.
//
//   index 0 -> "slot 1" -> 0x0000000   (0  MiB)
//   index 1 -> "slot 2" -> 0x0A00000   (10 MiB)
//   index 2 -> "slot 3" -> 0x1400000   (20 MiB)
//
// NOTE: the UF2 packer in xmake.lua uses different -b base addresses
//   (0x00000000 / 0x10000000 / 0x20000000). Those are flashing-tool addresses,
//   NOT raw SPI byte offsets, do not confuse them with the values here.
static constexpr uint32_t SoftwareSlotFlashOffset[3] = {0x0000000u,
                                                        0x0A00000u,
                                                        0x1400000u};

// The booted slot is strapped on these GPIO board input pins, exactly as the
// bootloader's read_selected_software_slot() reads them: the first asserted pin
// wins, and none asserted defaults to slot 0. We read the same strap so the
// measurement always covers the slot the device actually booted from, rather
// than a hard-coded guess.
static constexpr uint8_t SlotSelectGpioPins[3] = {13, 14, 15};

static uint8_t booted_slot()
{
	auto           gpio  = MMIO_CAPABILITY(SonataGpioBoard, gpio_board);
	const uint32_t Input = gpio->input;
	for (uint8_t i = 0; i < 3; i++)
	{
		if (Input & (1u << SlotSelectGpioPins[i]))
		{
			return i;
		}
	}
	return 0;
}

// Safety cap: refuse to hash more than this many bytes even if the ELF header
// claims a larger image. Comfortably above a real PlantNode build, well under
// the 10 MiB slot size.
static constexpr uint32_t MaxImageBytes = 4u * 1024u * 1024u;

// SPI flash access. Minimal subset of the bootloader's SpiFlash, kept
// in-compartment so attestation depends on nothing but the SPI MMIO device.
//
// IMPORTANT device-name mapping: the bootloader reaches the flash via
// spi_ptr(root, 2) == SPI_ADDRESS + 2*SPI_RANGE == 0x80300000 + 0x2000 ==
// 0x80302000. In the RTOS board.json that address is named "spi0" (the
// bootloader's SPI *index* 2 is NOT board.json "spi2", which is 0x80304000).
// So the flash device here is spi0.
using FlashSpi = SonataSpi::Generic<>;

static const uint8_t      CmdEnableReset   = 0x66;
static const uint8_t      CmdReset         = 0x99;
static const uint8_t      CmdReadData4Addr = 0x13;
static constexpr uint16_t FlashChunkBytes  = 256;

[[nodiscard, gnu::always_inline]] static CHERI::Capability<volatile FlashSpi>
flash_spi()
{ return MMIO_CAPABILITY(FlashSpi, spi0); }

// CS line 0, active low (bit 0 == 0 asserts).
static void flash_set_cs(volatile FlashSpi *spi, bool assertCs)
{
	spi->chipSelects =
	  assertCs ? (spi->chipSelects & ~1u) : (spi->chipSelects | 1u);
}

static void flash_init_reset(volatile FlashSpi *spi)
{
	spi->init(false, false, true, 0);

	flash_set_cs(spi, true);
	spi->blocking_write(&CmdEnableReset, 1);
	spi->wait_idle();
	flash_set_cs(spi, false);

	flash_set_cs(spi, true);
	spi->blocking_write(&CmdReset, 1);
	spi->wait_idle();
	flash_set_cs(spi, false);

	// Datasheet requires ~30us for the reset to settle; crude spin is fine
	// here.
	for (uint32_t i = 0; i < 4000; i++)
	{
		asm volatile("" ::: "memory");
	}
}

static void
flash_read(volatile FlashSpi *spi, uint32_t address, uint8_t *out, uint32_t len)
{
	uint32_t done = 0;
	while (done < len)
	{
		uint32_t       remaining = len - done;
		const uint16_t Chunk     = remaining > FlashChunkBytes
		                             ? FlashChunkBytes
		                             : static_cast<uint16_t>(remaining);
		const uint32_t A         = address + done;
		const uint8_t  Cmd[5]    = {CmdReadData4Addr,
		                            static_cast<uint8_t>((A >> 24) & 0xff),
		                            static_cast<uint8_t>((A >> 16) & 0xff),
		                            static_cast<uint8_t>((A >> 8) & 0xff),
		                            static_cast<uint8_t>(A & 0xff)};
		flash_set_cs(spi, true);
		spi->blocking_write(Cmd, 5);
		spi->blocking_read(out + done, Chunk);
		flash_set_cs(spi, false);
		done += Chunk;
	}
}

// Minimal little-endian ELF32 header (RISC-V is LE; struct overlay is safe on
// the naturally-aligned stack buffer below).
struct Elf32Ehdr
{
	uint8_t  eIdent[16];
	uint16_t eType;
	uint16_t eMachine;
	uint32_t eVersion;
	uint32_t eEntry;
	uint32_t ePhoff;
	uint32_t eShoff;
	uint32_t eFlags;
	uint16_t eEhsize;
	uint16_t ePhentsize;
	uint16_t ePhnum;
	uint16_t eShentsize;
	uint16_t eShnum;
	uint16_t eShstrndx;
};

// Read the ELF header, derive the total file length, then stream the whole
// image through hydro_hash. Writes AttestationImageHashLength bytes to hashOut.
static int measure_image(uint8_t *hashOut)
{
	volatile FlashSpi *spi  = flash_spi();
	const uint8_t      Slot = booted_slot();
	const uint32_t     Base = SoftwareSlotFlashOffset[Slot];

	flash_init_reset(spi);

	Elf32Ehdr ehdr;
	flash_read(spi, Base, reinterpret_cast<uint8_t *>(&ehdr), sizeof(ehdr));

	if (ehdr.eIdent[0] != 0x7f || ehdr.eIdent[1] != 'E' ||
	    ehdr.eIdent[2] != 'L' || ehdr.eIdent[3] != 'F')
	{
		Debug::log("measure_image: no ELF magic at slot offset {} "
		           "(first bytes {} {} {} {})",
		           Base,
		           ehdr.eIdent[0],
		           ehdr.eIdent[1],
		           ehdr.eIdent[2],
		           ehdr.eIdent[3]);
		return -EIO;
	}

	// Whole-file hash: section headers sit at the end of the file, so the
	// total length is eShoff + eShnum * eShentsize.
	const uint32_t Total =
	  ehdr.eShoff + static_cast<uint32_t>(ehdr.eShnum) *
	                  static_cast<uint32_t>(ehdr.eShentsize);
	if (Total < sizeof(Elf32Ehdr) || Total > MaxImageBytes)
	{
		Debug::log("measure_image: implausible image length {}", Total);
		return -EIO;
	}

	// Diagnostics: which slot/offset, the image length, and the entry point.
	// (Debug::log prints integers in hex with a 0x prefix; no format spec.)
	Debug::log("measure_image: slot {} base {} total {} eEntry {}",
	           Slot,
	           Base,
	           Total,
	           ehdr.eEntry);

	hydro_hash_state state;
	hydro_hash_init(&state, KImageHashContext, nullptr);

	uint8_t  buf[FlashChunkBytes];
	uint32_t off = 0;
	while (off < Total)
	{
		const uint32_t Remaining = Total - off;
		const uint32_t Chunk =
		  Remaining > FlashChunkBytes ? FlashChunkBytes : Remaining;
		flash_read(spi, Base + off, buf, Chunk);
		hydro_hash_update(&state, buf, Chunk);
		off += Chunk;
	}

	hydro_hash_final(&state, hashOut, AttestationImageHashLength);
	Debug::log("measure_image: hashed {} bytes of slot {}", Total, Slot);
	return 0;
}

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
  attestation_measure_image(uint8_t *hashOut, size_t *hashLen)
{
	if (!hashOut || !hashLen)
	{
		return -EINVAL;
	}
	if (!CHERI::check_pointer(hashOut, AttestationImageHashLength))
	{
		return -EINVAL;
	}
	int ret = measure_image(hashOut);
	if (ret != 0)
	{
		return ret;
	}

	// DIAGNOSTIC: measure a second time and compare. If two reads of the same
	// flash disagree, the bulk SPI read is non-deterministic (FIFO overflow
	// under preemption) rather than the slot holding a different image.
	uint8_t again[AttestationImageHashLength];
	if (measure_image(again) == 0)
	{
		Debug::log("measure_image: re-read {}",
		           memcmp(hashOut, again, AttestationImageHashLength) == 0
		             ? "STABLE"
		             : "UNSTABLE");
	}

	*hashLen = AttestationImageHashLength;
	return 0;
}

int __cheri_compartment("attestation")
  attestation_quote(const uint8_t    *nonce,
                    size_t            nonceLen,
                    AttestationQuote *quoteOut)
{
	if (!nonce || !quoteOut)
	{
		return -EINVAL;
	}
	if (nonceLen != AttestationNonceLength)
	{
		Debug::log("attestation_quote: bad nonce length {}", nonceLen);
		return -EINVAL;
	}
	if (!CHERI::check_pointer(nonce, nonceLen) ||
	    !CHERI::check_pointer(quoteOut, sizeof(AttestationQuote)))
	{
		return -EINVAL;
	}

	memset(quoteOut, 0, sizeof(*quoteOut));
	quoteOut->slot = booted_slot();

	const size_t IdLen = sizeof(KDeviceId) - 1;
	memcpy(quoteOut->deviceId, KDeviceId, IdLen);
	quoteOut->deviceIdLen = static_cast<uint8_t>(IdLen);

	int ret = measure_image(quoteOut->imageHash);
	if (ret != 0)
	{
		return ret;
	}
	memcpy(quoteOut->nonce, nonce, nonceLen);

	// Build the signed message: deviceId | slot | imageHash | nonce, then
	// hash it to a 32-byte digest. The verifier reconstructs this identically.
	uint8_t msg[DeviceIdMaxLength + 1 + AttestationImageHashLength +
	            AttestationNonceLength];
	size_t  p = 0;
	memcpy(msg + p, quoteOut->deviceId, IdLen);
	p += IdLen;
	msg[p++] = quoteOut->slot;
	memcpy(msg + p, quoteOut->imageHash, AttestationImageHashLength);
	p += AttestationImageHashLength;
	memcpy(msg + p, quoteOut->nonce, nonceLen);
	p += nonceLen;

	uint8_t digest[AttestationDigestLength];
	hydro_hash_hash(digest, sizeof(digest), msg, p, KQuoteContext, nullptr);

	size_t sigLen = 0;
	ret = fake_tpm_sign(digest, sizeof(digest), quoteOut->signature, &sigLen);
	if (ret != 0)
	{
		Debug::log("attestation_quote: fake_tpm_sign failed {}", ret);
		return ret;
	}

	Debug::log("attestation_quote: signed quote for slot {} ready",
	           quoteOut->slot);
	return 0;
}
