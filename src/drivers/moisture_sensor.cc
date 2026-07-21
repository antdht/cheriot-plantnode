// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

// Adafruit STEMMA Soil Sensor - I2C Capacitive Moisture Sensor (#4026)
// Chip: Adafruit Seesaw (ATSAMD10)   I2C address: 0x36
//
// Protocol: write [SEESAW_TOUCH_BASE, SEESAW_TOUCH_CHANNEL_OFFSET] to select
// the touch ADC channel, wait >=1 ms for the measurement to settle, then read
// 2 bytes (big-endian uint16_t) containing the raw capacitance value.
//
// Typical raw values (may vary by sensor and soil composition):
//   Dry air / empty:  ~200
//   Dry soil:         ~400
//   Wet soil:         ~1500
//   Submerged:        ~2000

#include "moisture_sensor.h"
#include "i2c_driver.h"
#include <compartment-macros.h>
#include <debug.hh>
#include <errno.h>
#include <timeout.h>

using Debug = ConditionalDebug<true, "PlantNode Moisture">;

static constexpr uint8_t KSeesawTouchBase = 0x0F;
static constexpr uint8_t KSeesawTouchCh0  = 0x10;

// Capability granting THIS compartment access to the Seesaw soil sensor (0x36),
// read+write, no lease. maxTransferLen 8 covers all its transfers.
// maxBatchMs 20 covers the write+2ms-delay+read batch (~4ms worst case) with
// headroom, while still bounding any hang to a small, fixed ceiling.
DECLARE_AND_DEFINE_STATIC_SEALED_VALUE(struct I2CDeviceCap,
                                       i2c_driver,
                                       I2CDeviceKey,
                                       moistureI2cCap,
                                       /* address        */ 0x36,
                                       /* opsMask         */ I2C_OP_READ |
                                         I2C_OP_WRITE,
                                       /* flags           */ 0,
                                       /* maxTransferLen  */ 8,
                                       /* maxBatchMs      */ 20,
                                       /* maxLeaseMs      */ 0);

// Calibration end-points, adjust after measuring your sensor in dry/wet soil.
static constexpr uint16_t KCalDry = 400;
static constexpr uint16_t KCalWet = 1800;

int __cheri_compartment("moisture_sensor") moisture_read_raw(uint16_t *rawOut)
{
	if (!rawOut)
	{
		return -EINVAL;
	}
	uint8_t cmd[2]   = {KSeesawTouchBase, KSeesawTouchCh0};
	uint8_t buf[2]   = {};
	I2CStep steps[3] = {
	  {I2cWrite, sizeof(cmd), 0, cmd},
	  {I2cDelay, 0, 2, nullptr}, // Seesaw needs >=1 ms; ask for 2
	  {I2cRead, sizeof(buf), 0, buf},
	};
	Timeout t{UnlimitedTimeout};
	int ret = i2c_transact(STATIC_SEALED_VALUE(moistureI2cCap), steps, 3, &t);
	if (ret < 0)
	{
		Debug::log("Seesaw transact failed: {}", ret);
		return ret;
	}
	*rawOut = static_cast<uint16_t>((buf[0] << 8) | buf[1]);
	return 0;
}

// POC stand-in for moisture_read_raw(): no real sensor involved. Starts at
// KFakeMoistureReset (~900), steps down by KFakeMoistureStep each call, and
// resets once it crosses policy_engine's low threshold (kept in sync here as
// KFakeMoistureLow) so the watering pulse (and its LED) can be observed
// repeatedly without real hardware.
static constexpr uint16_t KFakeMoistureReset = 900;
static constexpr uint16_t KFakeMoistureStep  = 60;
static constexpr uint16_t KFakeMoistureLow   = 300;

static uint16_t sFakeMoisture = 310;

int __cheri_compartment("moisture_sensor")
  moisture_read_raw_mock(uint16_t *rawOut)
{
	if (!rawOut)
	{
		return -EINVAL;
	}
	*rawOut = sFakeMoisture;
	Debug::log("Fake moisture reading: {}", sFakeMoisture);
	// Reset only AFTER a reading has actually gone below the policy engine's
	// threshold (strict "<"): resetting as soon as we reach the threshold
	// value itself would never emit a value that trips policy_evaluate's
	// `moistureRaw < sMoistureLow` check, so the pump/LED would never fire.
	if (sFakeMoisture < KFakeMoistureLow)
	{
		sFakeMoisture = KFakeMoistureReset;
	}
	else
	{
		sFakeMoisture -= KFakeMoistureStep;
	}
	return 0;
}

int __cheri_compartment("moisture_sensor")
  moisture_read_percent(uint8_t *percentOut)
{
	if (!percentOut)
	{
		return -EINVAL;
	}

	uint16_t raw = 0;
	int      ret = moisture_read_raw(&raw);
	if (ret < 0)
	{
		return ret;
	}

	if (raw <= KCalDry)
	{
		*percentOut = 0;
	}
	else if (raw >= KCalWet)
	{
		*percentOut = 100;
	}
	else
	{
		*percentOut = static_cast<uint8_t>(
		  (static_cast<uint32_t>(raw - KCalDry) * 100u) / (KCalWet - KCalDry));
	}
	return 0;
}
