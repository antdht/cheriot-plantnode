// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

// Adafruit AHT20 Temperature & Humidity Sensor
// I2C address: 0x38
//
// Measurement sequence (from AHT20 datasheet):
//  1. On first use: read 1-byte status. If calibration bit (bit 3) is clear,
//     send initialization command [0xBE, 0x08, 0x00] and wait 10 ms.
//  2. Trigger: send [0xAC, 0x33, 0x00].
//  3. Wait ≥80 ms for the measurement to complete.
//  4. Read 6 bytes:
//       byte[0]           = status  (bit 7 = busy, bit 3 = calibrated)
//       bytes[1..2], hi(3) = 20-bit raw humidity   (big-endian, MSB first)
//       lo(byte[3]), [4,5] = 20-bit raw temperature (big-endian)
//  5. Convert:
//       T [°C]  = (raw_t / 2^20) * 200 – 50
//       RH [%]  = (raw_h / 2^20) * 100

#include "temperature_sensor.h"
#include "i2c_driver.h"
#include <debug.hh>
#include <errno.h>
#include <thread.h>

using Debug = ConditionalDebug<true, "PlantNode Temp">;

static constexpr uint8_t kAddr = 0x38;

static constexpr uint8_t kCmdInit[]    = {0xBE, 0x08, 0x00};
static constexpr uint8_t kCmdTrigger[] = {0xAC, 0x33, 0x00};

static constexpr uint8_t kStatusBusy       = 1u << 7;
static constexpr uint8_t kStatusCalibrated = 1u << 3;

static bool s_initialized = false;

static int ensure_calibrated()
{
	uint8_t status = 0;
	int     ret    = i2c_read(kAddr, &status, 1);
	if (ret < 0)
	{
		return ret;
	}
	if (!(status & kStatusCalibrated))
	{
		Debug::log("AHT20 not calibrated, sending init command");
		ret = i2c_write(kAddr, kCmdInit, sizeof(kCmdInit));
		if (ret < 0)
		{
			return ret;
		}
		thread_millisecond_wait(10);
	}
	return 0;
}

static int do_measurement(uint8_t buf[6])
{
	// Trigger measurement
	int ret = i2c_write(kAddr, kCmdTrigger, sizeof(kCmdTrigger));
	if (ret < 0)
	{
		Debug::log("AHT20 trigger failed: {}", ret);
		return ret;
	}

	// Wait for measurement to complete (datasheet: ≥80 ms)
	thread_millisecond_wait(85);

	// Read 6 bytes
	ret = i2c_read(kAddr, buf, 6);
	if (ret < 0)
	{
		Debug::log("AHT20 read failed: {}", ret);
		return ret;
	}

	if (buf[0] & kStatusBusy)
	{
		Debug::log("AHT20 still busy after 85 ms");
		return -EBUSY;
	}
	return 0;
}

static void parse_measurement(const uint8_t buf[6],
                              int16_t      *celsius_x10_out,
                              uint16_t     *humidity_rx10_out)
{
	// Humidity: bits [39:20] of bytes 1-5
	uint32_t raw_h =
	  ((uint32_t)buf[1] << 12) | ((uint32_t)buf[2] << 4) | (buf[3] >> 4);

	// Temperature: bits [19:0] of bytes 3-5
	uint32_t raw_t =
	  ((uint32_t)(buf[3] & 0x0F) << 16) | ((uint32_t)buf[4] << 8) | buf[5];

	// T [°C × 10] = (raw_t × 2000 / 2^20) − 500
	if (celsius_x10_out)
	{
		*celsius_x10_out = (int16_t)(((int32_t)raw_t * 2000 / 1048576) - 500);
	}

	// RH [%RH × 10] = raw_h × 1000 / 2^20
	if (humidity_rx10_out)
	{
		*humidity_rx10_out = (uint16_t)((uint32_t)raw_h * 1000u / 1048576u);
	}
}

int __cheri_compartment("temperature_sensor")
  temperature_read_both(int16_t *celsius_x10_out, uint16_t *humidity_rx10_out)
{
	if (!celsius_x10_out && !humidity_rx10_out)
	{
		return -EINVAL;
	}

	if (!s_initialized)
	{
		int ret = ensure_calibrated();
		if (ret < 0)
		{
			return ret;
		}
		s_initialized = true;
	}

	uint8_t buf[6] = {};
	int     ret    = do_measurement(buf);
	if (ret < 0)
	{
		return ret;
	}

	parse_measurement(buf, celsius_x10_out, humidity_rx10_out);
	return 0;
}

int __cheri_compartment("temperature_sensor")
  temperature_read_celsius_x10(int16_t *celsius_x10_out)
{ return temperature_read_both(celsius_x10_out, nullptr); }
