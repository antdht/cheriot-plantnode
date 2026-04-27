// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#include "data_processing.h"
#include "display/display_data.h"
#include "drivers/moisture_sensor.h"
#include "drivers/temperature_sensor.h"
#include <debug.hh>
#include <errno.h>
#include <sntp.h>

using Debug = ConditionalDebug<true, "PlantNode DataProc">;

// int __cheri_compartment("data_processing") data_read_sensors(SensorReading
// *out)
// {
// 	if (!out)
// 	{
// 		return -EINVAL;
// 	}
//
// 	out->valid = false;
//
// 	int ret = moisture_read_raw(&out->moisture_raw);
// 	if (ret < 0)
// 	{
// 		Debug::log("Moisture read failed: {}", ret);
// 		return ret;
// 	}
//
// 	ret = temperature_read_both(&out->temperature_cx10, &out->humidity_rx10);
// 	if (ret < 0)
// 	{
// 		Debug::log("Temperature/humidity read failed: {}", ret);
// 		return ret;
// 	}
//
// 	struct timeval tv;
// 	out->timestamp =
// 	  (gettimeofday(&tv, nullptr) == 0) ? (uint32_t)tv.tv_sec : 0;
//
// 	out->valid = true;
//
// 	Debug::log("moisture={} temp={} hum={}",
// 	           out->moisture_raw,
// 	           out->temperature_cx10,
// 	           out->humidity_rx10);
//
// 	display_sensor_readings(out);
// 	return 0;
// }

// Debugging version
int __cheri_compartment("data_processing") data_read_sensors(SensorReading *out)
{
	if (!out)
	{
		return -EINVAL;
	}

	out->moisture_raw     = 900;
	out->temperature_cx10 = 235;
	out->humidity_rx10    = 455;

	struct timeval tv;
	out->timestamp =
	  (gettimeofday(&tv, nullptr) == 0) ? (uint32_t)tv.tv_sec : 0;

	out->valid = true;

	Debug::log("moisture={} temp={} hum={}",
	           out->moisture_raw,
	           out->temperature_cx10,
	           out->humidity_rx10);

	display_sensor_readings(out);
	return 0;
}
