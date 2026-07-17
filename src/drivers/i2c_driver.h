// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#pragma once

#include <cheri.hh>
#include <compartment.h>
#include <stddef.h>
#include <stdint.h>
#include <timeout.h>

/**
 * Sole owner of the I2C0 MMIO capability (qwiic0 connector). Every device is
 * reached through a sealed I2CDeviceCap minted under the i2c_driver sealing key
 * "I2CDeviceKey" and granted (statically) to exactly one compartment. The
 * driver serialises the bus with a priority-inheriting bus lock and a
 * per-device reservation table.
 */

// Operation bits for I2CDeviceCap.opsMask.
static constexpr uint8_t I2C_OP_READ  = 1u << 0;
static constexpr uint8_t I2C_OP_WRITE = 1u << 1;
// Flag bits for I2CDeviceCap.flags.
static constexpr uint8_t I2C_MAY_LEASE = 1u << 0;

// Plain-data contents of a sealed per-device capability.
struct I2CDeviceCap
{
	uint8_t  address;        // 7-bit I2C device address
	uint8_t  opsMask;        // I2C_OP_READ | I2C_OP_WRITE
	uint8_t  flags;          // I2C_MAY_LEASE
	uint16_t maxTransferLen; // per-step byte cap (bounds head-of-line blocking)
	uint32_t maxLeaseMs;     // hard auto-expiry ceiling for any lease
};

// Sealed handle type passed across the compartment boundary.
using I2CDevice = CHERI_SEALED(struct I2CDeviceCap *);

enum I2CStepKind
{
	I2cWrite = 0,
	I2cRead  = 1,
	I2cDelay = 2,
};

// One step of an atomic batch. For I2cWrite/I2cRead, `buffer`/`len` apply;
// for I2cDelay, `delayMs` applies (a *minimum* wait).
struct I2CStep
{
	uint8_t  kind;
	uint16_t len;
	uint16_t delayMs;
	void    *buffer;
};

// Run `steps` as a unit on `dev` (an implicit reservation spans the batch).
// Returns 0 on success, negative errno on failure.
int __cheri_compartment("i2c_driver")
  i2c_transact(I2CDevice dev, I2CStep *steps, size_t n, Timeout *t);

// Reserve `dev` for up to min(durationMs, cap.maxLeaseMs); requires
// I2C_MAY_LEASE.
int __cheri_compartment("i2c_driver")
  i2c_lease_acquire(I2CDevice dev, uint32_t durationMs, Timeout *t);
int __cheri_compartment("i2c_driver") i2c_lease_release(I2CDevice dev);

// Back-compat single/double-step convenience wrappers (now capability-gated).
int __cheri_compartment("i2c_driver")
  i2c_write(I2CDevice dev, const uint8_t *d, size_t len);
int __cheri_compartment("i2c_driver")
  i2c_read(I2CDevice dev, uint8_t *d, size_t len);
int __cheri_compartment("i2c_driver") i2c_write_read(I2CDevice      dev,
                                                     const uint8_t *w,
                                                     size_t         wl,
                                                     uint8_t       *r,
                                                     size_t         rl);
