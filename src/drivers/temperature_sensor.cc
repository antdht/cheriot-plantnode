// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

// Adafruit AHT20 Temperature & Humidity Sensor
// I2C address: 0x38
//
// Measurement sequence (from AHT20 datasheet):
//  1. On first use: read 1-byte status. If calibration bit (bit 3) is clear,
//     send initialization command [0xBE, 0x08, 0x00] and wait 10 ms.
//  2. Trigger: send [0xAC, 0x33, 0x00].
//  3. Wait >=80 ms for the measurement to complete.
//  4. Read 6 bytes:
//       byte[0]           = status  (bit 7 = busy, bit 3 = calibrated)
//       bytes[1..2], hi(3) = 20-bit raw humidity   (big-endian, MSB first)
//       lo(byte[3]), [4,5] = 20-bit raw temperature (big-endian)
//  5. Convert:
//       T [°C]  = (rawT / 2^20) * 200 – 50
//       RH [%]  = (rawH / 2^20) * 100

#include "temperature_sensor.h"
#include "i2c_driver.h"
#include <compartment-macros.h>
#include <debug.hh>
#include <errno.h>
#include <thread.h>
#include <timeout.h>

using Debug = ConditionalDebug<true, "PlantNode Temp">;

// Capability for the AHT20 (0x38), read+write, no lease. maxTransferLen 8.
DECLARE_AND_DEFINE_STATIC_SEALED_VALUE(struct I2CDeviceCap,
                                       i2c_driver,
                                       I2CDeviceKey,
                                       tempI2cCap,
                                       /* address        */ 0x38,
                                       /* opsMask         */ I2C_OP_READ |
                                         I2C_OP_WRITE,
                                       /* flags           */ 0,
                                       /* maxTransferLen  */ 8,
                                       /* maxLeaseMs      */ 0);

static constexpr uint8_t KCmdInit[]    = {0xBE, 0x08, 0x00};
static constexpr uint8_t KCmdTrigger[] = {0xAC, 0x33, 0x00};

static constexpr uint8_t KStatusBusy       = 1u << 7;
static constexpr uint8_t KStatusCalibrated = 1u << 3;

static bool sInitialized = false;

static int ensure_calibrated()
{
	uint8_t status = 0;
	int     ret    = i2c_read(STATIC_SEALED_VALUE(tempI2cCap), &status, 1);
	if (ret < 0)
	{
		return ret;
	}
	if (!(status & KStatusCalibrated))
	{
		Debug::log("AHT20 not calibrated, sending init command");
		ret = i2c_write(
		  STATIC_SEALED_VALUE(tempI2cCap), KCmdInit, sizeof(KCmdInit));
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
	I2CStep steps[3] = {
	  {I2cWrite, sizeof(KCmdTrigger), 0, const_cast<uint8_t *>(KCmdTrigger)},
	  {I2cDelay, 0, 85, nullptr}, // datasheet >=80 ms
	  {I2cRead, 6, 0, buf},
	};
	Timeout t{UnlimitedTimeout};
	int     ret = i2c_transact(STATIC_SEALED_VALUE(tempI2cCap), steps, 3, &t);
	if (ret < 0)
	{
		Debug::log("AHT20 measurement transact failed: {}", ret);
		return ret;
	}
	if (buf[0] & KStatusBusy)
	{
		Debug::log("AHT20 still busy after 85 ms");
		return -EBUSY;
	}
	return 0;
}

static void parse_measurement(const uint8_t buf[6],
                              int16_t      *celsiusX10Out,
                              uint16_t     *humidityRx10Out)
{
	// Humidity: bits [39:20] of bytes 1-5
	uint32_t rawH = (static_cast<uint32_t>(buf[1]) << 12) |
	                (static_cast<uint32_t>(buf[2]) << 4) | (buf[3] >> 4);

	// Temperature: bits [19:0] of bytes 3-5
	uint32_t rawT = (static_cast<uint32_t>(buf[3] & 0x0F) << 16) |
	                (static_cast<uint32_t>(buf[4]) << 8) | buf[5];

	// T [°C × 10] = (rawT × 2000 / 2^20) − 500
	if (celsiusX10Out)
	{
		*celsiusX10Out = static_cast<int16_t>(
		  (static_cast<int32_t>(rawT) * 2000 / 1048576) - 500);
	}

	// RH [%RH × 10] = rawH × 1000 / 2^20
	if (humidityRx10Out)
	{
		*humidityRx10Out = static_cast<uint16_t>(rawH * 1000u / 1048576u);
	}
}

int __cheri_compartment("temperature_sensor")
  temperature_read_both(int16_t *celsiusX10Out, uint16_t *humidityRx10Out)
{
	if (!celsiusX10Out && !humidityRx10Out)
	{
		return -EINVAL;
	}

	if (!sInitialized)
	{
		int ret = ensure_calibrated();
		if (ret < 0)
		{
			return ret;
		}
		sInitialized = true;
	}

	uint8_t buf[6] = {};
	int     ret    = do_measurement(buf);
	if (ret < 0)
	{
		return ret;
	}

	parse_measurement(buf, celsiusX10Out, humidityRx10Out);
	return 0;
}

int __cheri_compartment("temperature_sensor")
  temperature_read_celsius_x10(int16_t *celsiusX10Out)
{ return temperature_read_both(celsiusX10Out, nullptr); }
