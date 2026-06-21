// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#include "data_processing.h"
#include "display/display_data.h"
#include "drivers/temperature_sensor.h"
#include <debug.hh>
#include <errno.h>
#include <sntp.h>

using Debug = ConditionalDebug<true, "PlantNode DataProc">;

int __cheri_compartment("data_processing") data_read_sensors(SensorReading *out)
{
	if (!out)
	{
		return -EINVAL;
	}

	out->valid = false;

	// WARN: Temporary, waiting for the moisture sensor (fake reading of 900)
	// int ret = moisture_read_raw(&out->moistureRaw);
	// if (ret < 0)
	// {
	// 	Debug::log("Moisture read failed: {}", ret);
	// 	return ret;
	// }
	out->moistureRaw = 900;

	int ret = temperature_read_both(&out->temperatureCx10, &out->humidityRx10);
	if (ret < 0)
	{
		Debug::log("Temperature/humidity read failed: {}", ret);
		return ret;
	}

	struct timeval tv;
	out->timestamp =
	  (gettimeofday(&tv, nullptr) == 0) ? static_cast<uint32_t>(tv.tv_sec) : 0;

	out->valid = true;

	Debug::log("moisture={} temp={} hum={}",
	           out->moistureRaw,
	           out->temperatureCx10,
	           out->humidityRx10);

	display_sensor_readings(out);
	return 0;
}
