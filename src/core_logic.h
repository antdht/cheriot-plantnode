// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT

#pragma once

#include <compartment.h>

/**
 * Main entry point for the plant node.
 *
 * Owns the primary application thread. Orchestrates the sense → evaluate →
 * attest → publish loop by calling into the other compartments. Holds no
 * hardware capabilities itself.
 */
void __cheri_compartment("core_logic") core_entry();
