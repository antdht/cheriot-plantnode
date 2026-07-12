// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT
#pragma once
#include "plantnode_types.h"
#include <compartment.h>

// Highest-priority safety loop entry point: sense -> policy -> pump. Never
// blocks on the network; publishes the latest reading into a local mailbox.
void __cheri_compartment("control_loop") control_entry();

// Copy the most recent SensorReading into *out. Returns 0 on success,
// -EAGAIN if no reading has been produced yet.
int __cheri_compartment("control_loop")
  control_get_latest_reading(SensorReading *out);
