// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

// Adafruit STEMMA Soil Sensor - I2C Capacitive Moisture Sensor (#4026)
// Chip: Adafruit Seesaw (ATSAMD10)   I2C address: 0x36
//
// Protocol: write [SEESAW_TOUCH_BASE, SEESAW_TOUCH_CHANNEL_OFFSET] to select
// the touch ADC channel, wait ≥1 ms for the measurement to settle, then read
// 2 bytes (big-endian uint16_t) containing the raw capacitance value.
//
// Typical raw values (may vary by sensor and soil composition):
//   Dry air / empty:  ~200
//   Dry soil:         ~400
//   Wet soil:         ~1500
//   Submerged:        ~2000

#include "moisture_sensor.h"
#include "i2c_driver.h"
#include <debug.hh>
#include <errno.h>
#include <thread.h>

using Debug = ConditionalDebug<true, "PlantNode Moisture">;

static constexpr uint8_t kAddr            = 0x36;
static constexpr uint8_t kSeesawTouchBase = 0x0F;
static constexpr uint8_t kSeesawTouchCh0  = 0x10;

// Calibration end-points — adjust after measuring your sensor in dry/wet soil.
static constexpr uint16_t kCalDry = 400;
static constexpr uint16_t kCalWet = 1800;

int __cheri_compartment("moisture_sensor") moisture_read_raw(uint16_t *raw_out)
{
	if (!raw_out)
	{
		return -EINVAL;
	}

	// Select the touch ADC channel
	uint8_t cmd[2] = {kSeesawTouchBase, kSeesawTouchCh0};
	int     ret    = i2c_write(kAddr, cmd, sizeof(cmd));
	if (ret < 0)
	{
		Debug::log("Failed to write Seesaw touch register: {}", ret);
		return ret;
	}

	// Seesaw needs ≥1 ms to complete the capacitance measurement
	thread_millisecond_wait(2);

	// Read 2-byte big-endian result
	uint8_t buf[2] = {};
	ret            = i2c_read(kAddr, buf, sizeof(buf));
	if (ret < 0)
	{
		Debug::log("Failed to read Seesaw touch value: {}", ret);
		return ret;
	}

	*raw_out = static_cast<uint16_t>((buf[0] << 8) | buf[1]);
	return 0;
}

int __cheri_compartment("moisture_sensor")
  moisture_read_percent(uint8_t *percent_out)
{
	if (!percent_out)
	{
		return -EINVAL;
	}

	uint16_t raw = 0;
	int      ret = moisture_read_raw(&raw);
	if (ret < 0)
	{
		return ret;
	}

	if (raw <= kCalDry)
	{
		*percent_out = 0;
	}
	else if (raw >= kCalWet)
	{
		*percent_out = 100;
	}
	else
	{
		*percent_out = static_cast<uint8_t>(
		  (static_cast<uint32_t>(raw - kCalDry) * 100u) / (kCalWet - kCalDry));
	}
	return 0;
}
