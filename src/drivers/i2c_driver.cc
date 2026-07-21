// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#include "i2c_driver.h"
#include <compartment-macros.h>
#include <debug.hh>
#include <errno.h>
#include <futex.h>
#include <locks.hh>
#include <platform-i2c.hh>
#include <thread.h>
#include <token.h>

using Debug = ConditionalDebug<true, "PlantNode I2C">;
using namespace CHERI;

namespace
{
	bool sInitialized = false;

	void init_if_needed()
	{
		if (sInitialized)
		{
			return;
		}
		auto i2c = MMIO_CAPABILITY(OpenTitanI2c, i2c0);
		i2c->reset_fifos();
		i2c->host_mode_set();
		i2c->speed_set(100); // 100 kHz, safe for qwiic
		sInitialized = true;
	}

	// Unseal `dev` and check it grants every bit in `needOps`. Copies the
	// plain-data contents into `out` so the caller never dereferences the
	// sealed pointer directly. Returns 0 on success, -EPERM otherwise.
	int authorize(I2CDevice dev, uint8_t needOps, I2CDeviceCap *out)
	{
		SKey          key = STATIC_SEALING_TYPE(I2CDeviceKey);
		I2CDeviceCap *cap = token_unseal(key, Sealed<I2CDeviceCap>{dev});
		if (cap == nullptr)
		{
			return -EPERM;
		}
		if ((cap->opsMask & needOps) != needOps)
		{
			return -EPERM;
		}
		*out = *cap; // snapshot
		return 0;
	}

	// --- Device reservation table -------------------------------------------
	// One bus, a handful of devices. A reservation = {address, owner,
	// deadline}. Protected by reservationLock (held only for brief scans, never
	// across a transfer or a sleep). `reservationGen` is a futex word bumped on
	// every release/expiry so waiters wake.
	struct Reservation
	{
		bool     active;
		uint8_t  address;
		uint16_t owner;        // thread id
		uint64_t deadlineTick; // always nonzero: every claim (batch or lease) expires
		bool     isLease;      // true only if claimed via i2c_lease_acquire
	};
	constexpr size_t          MaxReservations = 4;
	Reservation               sReservations[MaxReservations];
	FlagLockPriorityInherited sReservationLock;
	uint32_t                  sReservationGen; // futex word

	uint64_t now_tick()
	{
		// Monotonic system tick since boot.
		// SystickReturn has hi/lo fields (no .raw() accessor).
		auto r = thread_systemtick_get();
		return (static_cast<uint64_t>(r.hi) << 32) | r.lo;
	}

	// Caller must hold sReservationLock. Expire stale reservations in place.
	void expire_locked(uint64_t now)
	{
		bool changed = false;
		for (auto &r : sReservations)
		{
			if (r.active && now >= r.deadlineTick)
			{
				r.active = false;
				changed  = true;
			}
		}
		if (changed)
		{
			sReservationGen++;
			futex_wake(&sReservationGen, -1);
		}
	}

	// Returns pointer to an active reservation for `address`, or nullptr.
	Reservation *find_locked(uint8_t address)
	{
		for (auto &r : sReservations)
		{
			if (r.active && r.address == address)
			{
				return &r;
			}
		}
		return nullptr;
	}

	// Reserve `address` for this thread for `holdMs` (always > 0: every fresh
	// claim now expires). `isLease` marks an explicit i2c_lease_acquire claim,
	// as opposed to a bare batch's implicit reservation, so i2c_transact knows
	// whether it must release at the end of its own step loop. Blocks
	// (honouring `t`) while another thread owns it. Returns 0 on success,
	// -ETIMEDOUT / -EBUSY on failure.
	int reserve_device(uint8_t address, uint32_t holdMs, Timeout *t, bool isLease)
	{
		uint16_t me = thread_id_get();
		while (true)
		{
			uint32_t gen;
			{
				LockGuard g{sReservationLock};
				uint64_t  now = now_tick();
				expire_locked(now);
				Reservation *r = find_locked(address);
				if (r == nullptr)
				{
					// Claim a free slot.
					for (auto &slot : sReservations)
					{
						if (!slot.active)
						{
							slot.active       = true;
							slot.address      = address;
							slot.owner        = me;
							slot.deadlineTick = now + MS_TO_TICKS(holdMs);
							slot.isLease      = isLease;
							return 0;
						}
					}
					return -EBUSY; // table full (should not happen)
				}
				if (r->owner == me)
				{
					return 0; // re-entrant: already ours (batch or lease held)
				}
				gen = sReservationGen; // snapshot before sleeping
			}
			// Owned by someone else: wait for a release/expiry, then retry.
			if (futex_timed_wait(t, &sReservationGen, gen, FutexNone) != 0)
			{
				return -ETIMEDOUT;
			}
		}
	}

	void release_device(uint8_t address)
	{
		uint16_t     me = thread_id_get();
		LockGuard    g{sReservationLock};
		Reservation *r = find_locked(address);
		if (r != nullptr && r->owner == me)
		{
			r->active = false;
			sReservationGen++;
			futex_wake(&sReservationGen, -1);
		}
	}

	// --- Bus lock + single transfer -----------------------------------------
	FlagLockPriorityInherited sBusLock;

	// Execute exactly one transfer step on the wire. Caller has already
	// reserved the device; this takes only the bus lock. Returns 0 / -EIO.
	int do_transfer(const I2CDeviceCap &cap, const I2CStep &step)
	{
		if (step.buffer == nullptr || step.len == 0)
		{
			return -EINVAL;
		}
		auto      i2c = MMIO_CAPABILITY(OpenTitanI2c, i2c0);
		LockGuard g{sBusLock};
		init_if_needed();
		if (step.kind == I2cWrite)
		{
			if (!i2c->blocking_write(cap.address,
			                         static_cast<const uint8_t *>(step.buffer),
			                         step.len,
			                         false))
			{
				Debug::log("write to 0x{:02x} failed", cap.address);
				return -EIO;
			}
		}
		else // I2cRead
		{
			if (!i2c->blocking_read(
			      cap.address, static_cast<uint8_t *>(step.buffer), step.len))
			{
				Debug::log("read from 0x{:02x} failed", cap.address);
				return -EIO;
			}
		}
		return 0;
	}
} // namespace

int __cheri_compartment("i2c_driver")
  i2c_transact(I2CDevice dev, I2CStep *steps, size_t n, Timeout *t)
{
	if (steps == nullptr || n == 0)
	{
		return -EINVAL;
	}
	// Compute the union of ops the batch needs, and validate lengths.
	uint8_t needOps = 0;
	for (size_t i = 0; i < n; i++)
	{
		if (steps[i].kind == I2cWrite)
		{
			needOps |= I2C_OP_WRITE;
		}
		else if (steps[i].kind == I2cRead)
		{
			needOps |= I2C_OP_READ;
		}
	}
	I2CDeviceCap cap{};
	int          ret = authorize(dev, needOps, &cap);
	if (ret != 0)
	{
		return ret;
	}
	for (size_t i = 0; i < n; i++)
	{
		if ((steps[i].kind == I2cWrite || steps[i].kind == I2cRead) &&
		    steps[i].len > cap.maxTransferLen)
		{
			return -EINVAL;
		}
	}

	if (cap.maxBatchMs == 0)
	{
		return -EINVAL;
	}
	// Reserve the device for the batch span, bounded by maxBatchMs. Re-entrant
	// if this thread already holds a lease on the device: reserve_device's
	// owner==me branch returns without touching the existing deadline, so a
	// lease holder's periodic batch polls ride the lease's own deadline
	// instead of being re-bound to maxBatchMs on every call.
	ret = reserve_device(cap.address, cap.maxBatchMs, t, /*isLease=*/false);
	if (ret != 0)
	{
		return ret;
	}
	// Only a lease's release must be left to the caller (i2c_lease_release);
	// a bare batch's own reservation is released at the end of this function.
	bool weHoldLease;
	{
		LockGuard    g{sReservationLock};
		Reservation *r = find_locked(cap.address);
		weHoldLease    = (r != nullptr && r->isLease);
	}

	int result = 0;
	for (size_t i = 0; i < n && result == 0; i++)
	{
		if (steps[i].kind == I2cDelay)
		{
			// Minimum wait, WITHOUT the bus lock: other devices stay usable.
			thread_millisecond_wait(steps[i].delayMs);
		}
		else
		{
			if (steps[i].buffer == nullptr)
			{
				result = -EINVAL;
				break;
			}
			// Verify the reservation is still ours before touching the wire: if
			// a lease expired mid-batch (e.g. during a preceding DELAY), abort
			// so we neither transact post-expiry nor interleave with a caller
			// that claimed the device.
			{
				LockGuard g{sReservationLock};
				expire_locked(now_tick());
				Reservation *r = find_locked(cap.address);
				if (r == nullptr || r->owner != thread_id_get())
				{
					result = -ETIMEDOUT;
					break;
				}
			}
			result = do_transfer(cap, steps[i]);
		}
	}

	if (!weHoldLease)
	{
		release_device(cap.address); // batch-span reservation only
	}
	return result;
}

int __cheri_compartment("i2c_driver")
  i2c_write(I2CDevice dev, const uint8_t *d, size_t len)
{
	if (len > UINT16_MAX)
	{
		return -EINVAL;
	}
	I2CStep s{
	  I2cWrite, static_cast<uint16_t>(len), 0, const_cast<uint8_t *>(d)};
	Timeout t{UnlimitedTimeout};
	return i2c_transact(dev, &s, 1, &t);
}

int __cheri_compartment("i2c_driver")
  i2c_read(I2CDevice dev, uint8_t *d, size_t len)
{
	if (len > UINT16_MAX)
	{
		return -EINVAL;
	}
	I2CStep s{I2cRead, static_cast<uint16_t>(len), 0, d};
	Timeout t{UnlimitedTimeout};
	return i2c_transact(dev, &s, 1, &t);
}

int __cheri_compartment("i2c_driver") i2c_write_read(I2CDevice      dev,
                                                     const uint8_t *w,
                                                     size_t         wl,
                                                     uint8_t       *r,
                                                     size_t         rl)
{
	if (wl > UINT16_MAX || rl > UINT16_MAX)
	{
		return -EINVAL;
	}
	I2CStep s[2] = {
	  {I2cWrite, static_cast<uint16_t>(wl), 0, const_cast<uint8_t *>(w)},
	  {I2cRead, static_cast<uint16_t>(rl), 0, r}};
	Timeout t{UnlimitedTimeout};
	return i2c_transact(dev, s, 2, &t);
}

int __cheri_compartment("i2c_driver")
  i2c_lease_acquire(I2CDevice dev, uint32_t durationMs, Timeout *t)
{
	I2CDeviceCap cap{};
	// A lease needs neither read nor write by itself; authorise with 0 ops then
	// check the lease flag.
	int ret = authorize(dev, 0, &cap);
	if (ret != 0)
	{
		return ret;
	}
	if ((cap.flags & I2C_MAY_LEASE) == 0)
	{
		return -EPERM;
	}
	uint32_t holdMs = durationMs;
	if (cap.maxLeaseMs != 0 && holdMs > cap.maxLeaseMs)
	{
		holdMs = cap.maxLeaseMs; // clamp to ceiling
	}
	if (holdMs == 0)
	{
		holdMs = cap.maxLeaseMs; // 0 means "as long as allowed"
	}
	// A leasable device must declare a finite ceiling: never grant an
	// unexpirable lease (would violate the bounded-lockout guarantee).
	if (holdMs == 0)
	{
		return -EINVAL;
	}
	return reserve_device(cap.address, holdMs, t, /*isLease=*/true);
}

int __cheri_compartment("i2c_driver") i2c_lease_release(I2CDevice dev)
{
	I2CDeviceCap cap{};
	int          ret = authorize(dev, 0, &cap);
	if (ret != 0)
	{
		return ret;
	}
	release_device(cap.address);
	return 0;
}
