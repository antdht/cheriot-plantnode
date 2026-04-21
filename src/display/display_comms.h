// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#pragma once

// Display functions callable ONLY by the comms compartment.
//
// CHERIoT enforcement: including only this header means the comms compartment
// receives a sealed cross-compartment call capability solely for
// display_connecting_network(). It cannot call functions declared in
// display_data.h or display_policy.h — the sealed capabilities for those
// are never provisioned to it.

#include <compartment.h>

/**
 * Clear the screen and show a "Connecting to network…" message.
 * Called by comms before network_start() / mqtt_connect().
 */
void __cheri_compartment("display") display_connecting_network();

/**
 * Replace the "Connecting…" screen with a "Connected!" confirmation.
 * Called by comms after mqtt_connect() succeeds.
 */
void __cheri_compartment("display") display_connected_network();
