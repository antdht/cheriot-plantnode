// Copyright CHERIoT Contributors.
// SPDX-License-Identifier: MIT
#pragma once
#include <compartment.h>
#include <stdint.h>

// Highest-priority safety loop entry point: sense -> policy -> pump. Never
// blocks on the network; publishes the latest pump-activation timestamp into
// a local mailbox.
void __cheri_compartment("irrigation") irrigation_loop();

// Copy the most recent pump-activation timestamp into *out (0 = never
// watered). Returns 0 on success, -EINVAL if out is null.
int __cheri_compartment("irrigation")
  irrigation_get_last_watering(uint32_t *out);
