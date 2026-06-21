// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#include "pump_driver.h"
#include <debug.hh>
#include <errno.h>

using Debug = ConditionalDebug<true, "PlantNode Pump">;

// Sole owner of the GPIO MMIO capability for the pump control pin.
// TODO: #include <platform-gpio.hh> and MMIO_CAPABILITY(SonataGPIO,
// gpio_arduino)
// TODO: define which GPIO pin drives the pump relay

static bool sPumpState = false;

int __cheri_compartment("pump_driver") pump_on()
{
	Debug::log("Pump ON");
	// TODO: set GPIO pin high
	sPumpState = true;
	return -ENOSYS;
}

int __cheri_compartment("pump_driver") pump_off()
{
	Debug::log("Pump OFF");
	// TODO: set GPIO pin low
	sPumpState = false;
	return -ENOSYS;
}

int __cheri_compartment("pump_driver") pump_get_state(bool *stateOut)
{
	if (!stateOut)
	{
		return -EINVAL;
	}
	*stateOut = sPumpState;
	return 0;
}
