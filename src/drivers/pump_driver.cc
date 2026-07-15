// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#include "pump_driver.h"
#include <debug.hh>
#include <errno.h>
#include <platform-gpio.hh>

using Debug = ConditionalDebug<true, "PlantNode Pump">;

// Sole owner of the GPIO MMIO capability for the pump control pin.
// TODO: define which GPIO pin drives the pump relay

// POC stand-in: no relay pin is wired yet, so an onboard LED lights up
// instead whenever the pump would activate, to make the call chain visible.
static constexpr uint32_t KPumpLedIndex = 0;

static bool sPumpState = false;

int __cheri_compartment("pump_driver") pump_on()
{
	Debug::log("Pump ON");
	auto gpio = MMIO_CAPABILITY(SonataGpioBoard, gpio_board);
	gpio->led_on(KPumpLedIndex);
	// TODO: set GPIO pin high
	sPumpState = true;
	return -ENOSYS;
}

int __cheri_compartment("pump_driver") pump_off()
{
	Debug::log("Pump OFF");
	auto gpio = MMIO_CAPABILITY(SonataGpioBoard, gpio_board);
	gpio->led_off(KPumpLedIndex);
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
