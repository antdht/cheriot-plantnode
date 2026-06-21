// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#include "i2c_driver.h"
#include <debug.hh>
#include <errno.h>
#include <platform-i2c.hh>

using Debug = ConditionalDebug<true, "PlantNode I2C">;

static bool sInitialized = false;

static void init_if_needed()
{
	if (sInitialized)
	{
		return;
	}
	auto i2c = MMIO_CAPABILITY(OpenTitanI2c, i2c0);
	i2c->reset_fifos();
	i2c->host_mode_set();
	i2c->speed_set(100); // 100 kHz safe for qwiic
	sInitialized = true;
}

int __cheri_compartment("i2c_driver")
  i2c_write(uint8_t deviceAddr, const uint8_t *data, size_t len)
{
	if (!data || len == 0)
	{
		return -EINVAL;
	}
	init_if_needed();
	auto i2c = MMIO_CAPABILITY(OpenTitanI2c, i2c0);
	if (!i2c->blocking_write(
	      deviceAddr, data, static_cast<uint32_t>(len), false))
	{
		Debug::log("i2c_write to 0x{:02x} failed", deviceAddr);
		return -EIO;
	}
	return 0;
}

int __cheri_compartment("i2c_driver")
  i2c_read(uint8_t deviceAddr, uint8_t *data, size_t len)
{
	if (!data || len == 0)
	{
		return -EINVAL;
	}
	init_if_needed();
	auto i2c = MMIO_CAPABILITY(OpenTitanI2c, i2c0);
	if (!i2c->blocking_read(deviceAddr, data, static_cast<uint32_t>(len)))
	{
		Debug::log("i2c_read from 0x{:02x} failed", deviceAddr);
		return -EIO;
	}
	return 0;
}

int __cheri_compartment("i2c_driver") i2c_write_read(uint8_t        deviceAddr,
                                                     const uint8_t *wdata,
                                                     size_t         wlen,
                                                     uint8_t       *rdata,
                                                     size_t         rlen)
{
	if (!wdata || !rdata || wlen == 0 || rlen == 0)
	{
		return -EINVAL;
	}
	init_if_needed();
	auto i2c = MMIO_CAPABILITY(OpenTitanI2c, i2c0);
	// Write register address with STOP, then read with a fresh START.
	// Sensors that need a delay between write and read should use separate
	// i2c_write / i2c_read calls with thread_millisecond_wait in between.
	if (!i2c->blocking_write(
	      deviceAddr, wdata, static_cast<uint32_t>(wlen), false))
	{
		Debug::log("i2c_write_read write to 0x{:02x} failed", deviceAddr);
		return -EIO;
	}
	if (!i2c->blocking_read(deviceAddr, rdata, static_cast<uint32_t>(rlen)))
	{
		Debug::log("i2c_write_read read from 0x{:02x} failed", deviceAddr);
		return -EIO;
	}
	return 0;
}
