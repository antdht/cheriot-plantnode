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

// String formatting helpers

static char *append_str(char *p, const char *s)
{
	while (*s)
	{
		*p++ = *s++;
	}
	return p;
}

static char *append_uint(char *p, unsigned v)
{
	char tmp[10];
	int  n = 0;
	do
	{
		tmp[n++] = '0' + (v % 10);
		v /= 10;
	} while (v);
	for (int i = n - 1; i >= 0; i--)
	{
		*p++ = tmp[i];
	}
	return p;
}

// Formats val/divisor with one decimal place (e.g. 235/10 -> "23.5").
static char *append_decimal(char *p, int val, unsigned divisor)
{
	if (val < 0)
	{
		*p++ = '-';
		val  = -val;
	}
	p    = append_uint(p, (unsigned)val / divisor);
	*p++ = '.';
	p    = append_uint(p, (unsigned)val % divisor);
	return p;
}

// Shared layout helpers

static void draw_title_bar(SonataLcd &l, Size res, Color bg = Color::Blue)
{
	l.fill_rect({0, 0, res.width, 18}, bg);
	l.draw_str({4, 3}, "PlantNode", bg, Color::White, Font::M5x7_16pt);
}

// comms

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

// telemetry

void __cheri_compartment("display")
  display_sensor_readings(const SensorReading *reading)
{
	auto &l   = lcd();
	auto  res = l.resolution();

	l.clean(Color::Black);
	draw_title_bar(l, res);

	if (!reading || !reading->valid)
	{
		l.draw_str({4, 30},
		           "No sensor data",
		           Color::Black,
		           Color::White,
		           Font::LucidaConsole_10pt);
		return;
	}

	char  line[24];
	char *p;

	// "Temp: 23.5 C"
	p  = line;
	p  = append_str(p, "Temp: ");
	p  = append_decimal(p, reading->temperatureCx10, 10);
	p  = append_str(p, " C");
	*p = '\0';
	l.draw_str(
	  {4, 30}, line, Color::Black, Color::White, Font::LucidaConsole_10pt);

	// "Hum:  45.5 %"
	p  = line;
	p  = append_str(p, "Hum:  ");
	p  = append_decimal(p, (int)reading->humidityRx10, 10);
	p  = append_str(p, " %");
	*p = '\0';
	l.draw_str(
	  {4, 46}, line, Color::Black, Color::White, Font::LucidaConsole_10pt);

	// "Moist: 1234"
	p  = line;
	p  = append_str(p, "Moist: ");
	p  = append_uint(p, reading->moistureRaw);
	*p = '\0';
	l.draw_str(
	  {4, 62}, line, Color::Black, Color::White, Font::LucidaConsole_10pt);
}

// policy_engine

void __cheri_compartment("display") display_pump_activation(bool activating)
{
	auto &l   = lcd();
	auto  res = l.resolution();

	l.clean(Color::Black);
	draw_title_bar(l, res, activating ? Color::Red : Color::Green);

	if (activating)
	{
		l.draw_str({4, 48},
		           "Currently",
		           Color::Black,
		           Color::White,
		           Font::LucidaConsole_10pt);
		l.draw_str({4, 64},
		           "watering...",
		           Color::Black,
		           Color::White,
		           Font::LucidaConsole_10pt);
	}
	else
	{
		l.draw_str({4, 48},
		           "Watering",
		           Color::Black,
		           Color::White,
		           Font::LucidaConsole_10pt);
		l.draw_str({4, 64},
		           "finished!",
		           Color::Black,
		           Color::White,
		           Font::LucidaConsole_10pt);
	}
}
