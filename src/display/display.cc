// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#include "display_comms.h"
#include "display_data.h"
#include "display_policy.h"
#include <debug.hh>

#include <lcd.hh>

using namespace sonata::lcd;
using Debug = ConditionalDebug<true, "PlantNode Display">;

// Lazily initialised on first use; lives for the lifetime of the compartment.
static SonataLcd &lcd()
{
	static SonataLcd instance;
	return instance;
}

// ── Shared layout helpers ─────────────────────────────────────────────────

static void draw_title_bar(SonataLcd &l, Size res)
{
	l.fill_rect({0, 0, res.width, 18}, Color::Blue);
	l.draw_str({4, 3}, "PlantNode", Color::Blue, Color::White, Font::M5x7_16pt);
}

// ── comms ─────────────────────────────────────────────────────────────────

void __cheri_compartment("display") display_connecting_network()
{
	auto &l   = lcd();
	auto  res = l.resolution();

	l.clean(Color::Black);
	draw_title_bar(l, res);

	l.draw_str({4, 30},
	           "Connecting to",
	           Color::Black,
	           Color::White,
	           Font::LucidaConsole_10pt);
	l.draw_str({4, 46},
	           "network...",
	           Color::Black,
	           Color::White,
	           Font::LucidaConsole_10pt);
}

void __cheri_compartment("display") display_connected_network()
{
	auto &l   = lcd();
	auto  res = l.resolution();

	l.clean(Color::Black);
	draw_title_bar(l, res);

	l.draw_str({4, 30},
	           "Connected to",
	           Color::Black,
	           Color::White,
	           Font::LucidaConsole_10pt);
	l.draw_str({4, 46},
	           "network !",
	           Color::Black,
	           Color::White,
	           Font::LucidaConsole_10pt);
}

// ── data_processing ───────────────────────────────────────────────────────

void __cheri_compartment("display")
  display_sensor_readings(const SensorReading *reading)
{
	// TODO: implement sensor readings display
	// Layout sketch:
	//   Title bar
	//   "Temp:  XX.X C"
	//   "Moisture: XXX%"
	(void)reading;
}

// ── policy_engine ─────────────────────────────────────────────────────────

void __cheri_compartment("display") display_pump_activation(bool activating)
{
	// TODO: implement pump status display
	// Layout sketch:
	//   Title bar
	//   Red/Green banner: "Watering..." / "Watering done"
	(void)activating;
}
