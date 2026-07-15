// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#pragma once

#include <compartment.h>
#include <stdint.h>

/**
 * Driver for moisture sensor 1 on qwiic0 (I2C0).
 * Communicates with the sensor via the i2c_driver compartment.
 * Only control_loop and core_logic are permitted to call this compartment.
 */

/**
 * Read the raw moisture value from the sensor.
 *
 * @param raw_out  Populated with the raw 16-bit ADC/register value.
 *
 * Returns 0 on success, negative errno on failure.
 */
int __cheri_compartment("moisture_sensor") moisture_read_raw(uint16_t *rawOut);

/**
 * Read the moisture level as a percentage (0–100).
 * Applies the stored calibration curve.
 *
 * @param percent_out  Populated with 0–100 moisture percentage.
 *
 * Returns 0 on success, negative errno on failure.
 */
int __cheri_compartment("moisture_sensor")
  moisture_read_percent(uint8_t *percentOut);
