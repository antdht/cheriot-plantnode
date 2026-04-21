// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#pragma once

#include <compartment.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Sole owner of the I2C0 MMIO capability (qwiic0 connector).
 *
 * Both sensor compartments (moisture_sensor, temperature_sensor) must go
 * through this compartment to reach the bus. This ensures bus access is
 * serialised and that no other compartment holds a raw capability to the I2C
 * peripheral.
 */

/**
 * Write bytes to a device on the I2C bus.
 *
 * @param device_addr  7-bit I2C device address.
 * @param data         Bytes to transmit.
 * @param len          Number of bytes.
 *
 * Returns 0 on success, negative errno on failure.
 */
int __cheri_compartment("i2c_driver") i2c_write(uint8_t        device_addr,
                                                const uint8_t *data,
                                                size_t         len);

/**
 * Read bytes from a device on the I2C bus.
 *
 * @param device_addr  7-bit I2C device address.
 * @param data         Buffer to receive bytes.
 * @param len          Number of bytes to read.
 *
 * Returns 0 on success, negative errno on failure.
 */
int __cheri_compartment("i2c_driver") i2c_read(uint8_t  device_addr,
                                               uint8_t *data,
                                               size_t   len);

/**
 * Combined write-then-read (repeated-start), common for register-based
 * sensors.
 *
 * @param device_addr  7-bit I2C device address.
 * @param wdata        Bytes to write (e.g. register address).
 * @param wlen         Number of bytes to write.
 * @param rdata        Buffer to receive bytes.
 * @param rlen         Number of bytes to read.
 *
 * Returns 0 on success, negative errno on failure.
 */
int __cheri_compartment("i2c_driver")
  i2c_write_read(uint8_t        device_addr,
                 const uint8_t *wdata,
                 size_t         wlen,
                 uint8_t       *rdata,
                 size_t         rlen);
