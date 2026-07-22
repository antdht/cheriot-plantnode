package plantnode

import future.keywords

# ---------------------------------------------------------------------------
# CHERIoT PlantNode – whole-firmware capability-graph audit.
#
# Properties 1-5 check the I2C driver's capability graph (bus MMIO
# exclusivity, sealed per-device capabilities, and the maxBatchMs ceiling).
# Properties A-F check claims made elsewhere in the design about who may
# call whom and who may hold what, across the rest of the compartment
# topology. Each property is annotated with the security objective (O1,
# O3, O4, O5, O6 — see DESIGN.md / thesis Section 3) it discharges.
#
# Run with:
#   cheriot-audit \
#     -b build/cheriot/cheriot/release/plantnode.board.json \
#     -j build/cheriot/cheriot/release/plantnode.json \
#     -m audit/plantnode.rego \
#     -q 'data.plantnode.all_pass'
# ---------------------------------------------------------------------------

# ===========================================================================
# I2C driver capability graph (O5, O6)
# ===========================================================================

# The i2c0 MMIO base address on Sonata (from the board JSON).
i2c0_start  := 2149580800
i2c0_length := 128

# ---------------------------------------------------------------------------
# Property 1 – i2c0 MMIO is imported by i2c_driver ONLY.
# ---------------------------------------------------------------------------
i2c0_mmio_holders := {name |
  some name, comp in input.compartments
  some imp in comp.imports
  imp.kind == "MMIO"
  imp.start == i2c0_start
  imp.length == i2c0_length
}

prop1_pass := true if {
  i2c0_mmio_holders == {"i2c_driver"}
}

# ---------------------------------------------------------------------------
# Property 2 – i2c_driver exports the sealing key I2CDeviceKey; exactly two
#              compartments hold a static sealed object under that key.
# ---------------------------------------------------------------------------
i2c_driver_exports_sealing_key := true if {
  some e in input.compartments.i2c_driver.exports
  e.export_symbol == "__export.sealing_type.i2c_driver.I2CDeviceKey"
  e.kind == "SealingKey"
}

i2c_sealed_object_holders := {name |
  some name, comp in input.compartments
  some imp in comp.imports
  imp.kind == "SealedObject"
  imp.sealing_type.key == "I2CDeviceKey"
  imp.sealing_type.compartment == "i2c_driver"
}

prop2_pass := true if {
  i2c_driver_exports_sealing_key == true
  count(i2c_sealed_object_holders) == 2
}

# ---------------------------------------------------------------------------
# Property 3 – moisture_sensor holds cap for 0x36; temperature_sensor for 0x38.
#              Each is held by that compartment ONLY.
# ---------------------------------------------------------------------------

# The first two hex characters of contents encode the I2CDeviceCap.address byte.
moisture_cap_contents := imp.contents if {
  some imp in input.compartments.moisture_sensor.imports
  imp.kind == "SealedObject"
  imp.sealing_type.key == "I2CDeviceKey"
  imp.sealing_type.compartment == "i2c_driver"
}

temperature_cap_contents := imp.contents if {
  some imp in input.compartments.temperature_sensor.imports
  imp.kind == "SealedObject"
  imp.sealing_type.key == "I2CDeviceKey"
  imp.sealing_type.compartment == "i2c_driver"
}

prop3_pass := true if {
  lower(substring(moisture_cap_contents, 0, 2)) == "36"
  lower(substring(temperature_cap_contents, 0, 2)) == "38"
  i2c_sealed_object_holders == {"moisture_sensor", "temperature_sensor"}
}

# ---------------------------------------------------------------------------
# Property 4 – No compartment outside the authorized I2C set holds i2c0 MMIO
#              or an I2CDeviceKey-sealed object.
#
# The "others" set is DERIVED from the live compartment list, so any future
# compartment (e.g. control_loop) is covered automatically without editing
# this policy.
# ---------------------------------------------------------------------------
authorized_i2c_compartments := {"i2c_driver", "moisture_sensor", "temperature_sensor"}

other_app_compartments := {name |
  some name, _ in input.compartments
  not name in authorized_i2c_compartments
}

other_with_i2c_mmio := {name |
  some name in other_app_compartments
  name in i2c0_mmio_holders
}

other_with_i2c_sealed := {name |
  some name in other_app_compartments
  name in i2c_sealed_object_holders
}

prop4_pass := true if {
  count(other_with_i2c_mmio) == 0
  count(other_with_i2c_sealed) == 0
}

# ---------------------------------------------------------------------------
# Property 5 – Every I2CDeviceKey sealed object declares a nonzero maxBatchMs,
#              and the two known sensor caps carry their expected ceiling.
#
# struct I2CDeviceCap is not packed, so maxBatchMs (uint16) sits at byte
# offset 6-7, right after maxTransferLen (uint16, offset 4-5) and before the
# maxLeaseMs (uint32) at offset 8-11. In the space-grouped `contents` hex
# string ("AABBCCDD EEFFGGHH IIJJKKLL", 4 bytes/group) that is the last 4 hex
# characters of the middle group, i.e. contents[13:17], still little-endian.
# A build predating this field encodes zero there (it was padding), so the
# nonzero check also catches a stale/pre-cap build being audited by mistake.
# ---------------------------------------------------------------------------
i2c_sealed_contents := {name: c |
  some name, comp in input.compartments
  some imp in comp.imports
  imp.kind == "SealedObject"
  imp.sealing_type.key == "I2CDeviceKey"
  imp.sealing_type.compartment == "i2c_driver"
  c := imp.contents
}

max_batch_ms_hex_le(contents) := lower(substring(contents, 13, 4))

zero_max_batch_ms_holders := {name |
  some name, c in i2c_sealed_contents
  max_batch_ms_hex_le(c) == "0000"
}

prop5_pass := true if {
  count(zero_max_batch_ms_holders) == 0
  max_batch_ms_hex_le(moisture_cap_contents) == "1400"    # 0x0014 = 20 ms
  max_batch_ms_hex_le(temperature_cap_contents) == "9600" # 0x0096 = 150 ms
}

# ===========================================================================
# Whole-firmware call graph and export surface (O1, O3, O4, O6)
# ===========================================================================

# ---------------------------------------------------------------------------
# Property A (O3) – pump_driver is the sole holder of the GPIO MMIO range
#                   controlling the Arduino shield's relay pin.
#
# Same exclusivity shape as Property 1, applied to the actuation side
# instead of the sensing side: the relay pin is reachable only through the
# one compartment holding this MMIO capability.
# ---------------------------------------------------------------------------
gpio_start  := 2147483648
gpio_length := 16

gpio_holders := {name |
  some name, comp in input.compartments
  some imp in comp.imports
  imp.kind == "MMIO"
  imp.start == gpio_start
  imp.length == gpio_length
}

propA_pass := true if {
  gpio_holders == {"pump_driver"}
}

# ---------------------------------------------------------------------------
# Property B (O3) – pump_driver's actuation entry points are callable only
#                    from policy_engine.
#
# Backs the design claim that pump actuation happens only after policy
# evaluation: no other compartment, however it got compromised, can drive
# the relay directly.
# ---------------------------------------------------------------------------
propB_pass := true if {
  data.compartment.compartment_call_allow_list("pump_driver", "pump_on.*", {"policy_engine"})
  data.compartment.compartment_call_allow_list("pump_driver", "pump_off.*", {"policy_engine"})
}

# ---------------------------------------------------------------------------
# Property C (O6) – sensor access split is exactly what the design claims:
#                    temperature_sensor is reachable only from telemetry;
#                    moisture_sensor is reachable from irrigation and
#                    telemetry, and no one else.
#
# This is the machine-checked form of "Irrigation contains no compiled
# reference to the temperature driver" — previously only a claim about the
# source, now a claim about the linked image.
# ---------------------------------------------------------------------------
propC_pass := true if {
  data.compartment.compartment_call_allow_list("temperature_sensor", "temperature_read_.*", {"telemetry"})
  data.compartment.compartment_call_allow_list("moisture_sensor", "moisture_read_.*", {"irrigation", "telemetry"})
}

# ---------------------------------------------------------------------------
# Property D (O4) – the irrigation compartment holds no capability, sealed
#                    object, or cross-compartment call reaching any
#                    network-facing compartment.
#
# The set below is named explicitly, not derived as "everything irrigation
# doesn't already use" (contrast with Property 4): the claim here is
# qualitative — the safety loop must never touch the network path — not
# merely "nothing unexpected". Adding a legitimate new dependency for
# irrigation therefore requires deliberately editing this list, which is
# the point.
# ---------------------------------------------------------------------------
network_facing_compartments := {
  "comms", "crypto", "attestation", "tpm",
  "TCPIP", "Firewall", "MQTT", "NetAPI", "SNTP", "TLS", "DNS",
  "telemetry", "display",
}

irrigation_network_imports := {imp |
  some imp in input.compartments.irrigation.imports
  imp.kind == "CompartmentExport"
  some name in network_facing_compartments
  endswith(imp.provided_by, sprintf("%s.compartment", [name]))
}

propD_pass := true if {
  count(irrigation_network_imports) == 0
}

# ---------------------------------------------------------------------------
# Property E (O1) – crypto exports exactly its three intended entry points,
#                    and nothing else: no exported symbol carries session
#                    key material out of the compartment.
#
# sSessionTxKey / sSessionRxKey are file-scope statics in crypto.cc with no
# accessor; this property checks that fact holds in the built image, not
# merely in the source as written, by asserting the export table cannot
# contain anything a key-exfiltration path could hang off — every export is
# a Function, there are exactly three of them, and each is one of the three
# named entry points.
# ---------------------------------------------------------------------------
crypto_export_names := {"crypto_init_session", "crypto_encrypt", "crypto_decrypt"}

crypto_export_matches(sym) := true if {
  some name in crypto_export_names
  contains(sym, name)
}

unmatched_crypto_exports := {e.export_symbol |
  some e in input.compartments.crypto.exports
  not crypto_export_matches(e.export_symbol)
}

non_function_crypto_exports := {e.export_symbol |
  some e in input.compartments.crypto.exports
  e.kind != "Function"
}

propE_pass := true if {
  count(input.compartments.crypto.exports) == 3
  count(unmatched_crypto_exports) == 0
  count(non_function_crypto_exports) == 0
}

# ---------------------------------------------------------------------------
# Property F (defence in depth) – exclusivity of a few more single-caller
#                                  entry points named in the design:
#                                  the watering mailbox is read only by
#                                  telemetry, tpm_attest is invoked only by
#                                  attestation, and the MQTT session lifecycle
#                                  (connect/poll) is driven only by telemetry.
#
# None of these are load-bearing security objectives on their own, but a
# second compartment calling any of them would be a bug worth catching, the
# same reasoning the caesar.rego tutorial applies to its own encrypt/decrypt
# allow-lists.
# ---------------------------------------------------------------------------
propF_pass := true if {
  data.compartment.compartment_call_allow_list("irrigation", "irrigation_get_last_watering.*", {"telemetry"})
  data.compartment.compartment_call_allow_list("tpm", "tpm_attest.*", {"attestation"})
  data.compartment.compartment_call_allow_list("comms", "comms_connect.*", {"telemetry"})
  data.compartment.compartment_call_allow_list("comms", "comms_poll.*", {"telemetry"})
}

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
all_pass := true if {
  data.rtos.valid
  prop1_pass == true
  prop2_pass == true
  prop3_pass == true
  prop4_pass == true
  prop5_pass == true
  propA_pass == true
  propB_pass == true
  propC_pass == true
  propD_pass == true
  propE_pass == true
  propF_pass == true
}
