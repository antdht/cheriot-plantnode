// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#pragma once

// Adafruit AHT20 Temperature & Humidity sensor  (I2C address 0x38)
// Both measurements come from the same 6-byte I2C transaction

#include <compartment.h>
#include <stdint.h>

/**
 * Read temperature (°C × 10) and relative humidity (%RH × 10) in one
 * transaction. Preferred over the individual functions.
 *
 * @param celsius_x10_out   e.g. 235 = 23.5 °C
 * @param humidity_rx10_out e.g. 455 = 45.5 %RH
 *
 * Returns 0 on success, negative errno on failure.
 */
int __cheri_compartment("temperature_sensor")
  temperature_read_both(int16_t *celsiusX10Out, uint16_t *humidityRx10Out);

/**
 * Read temperature only (°C × 10). Triggers a full AHT20 measurement but
 * discards the humidity result.
 *
 * Returns 0 on success, negative errno on failure.
 */
int __cheri_compartment("temperature_sensor")
  temperature_read_celsius_x10(int16_t *celsiusX10Out);
