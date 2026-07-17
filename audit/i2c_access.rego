package i2c_access

import future.keywords

# ---------------------------------------------------------------------------
# CHERIoT PlantNode – I2C access-control audit
# Verifies the capability distribution properties of the i2c_driver subsystem.
# Run with:
#   cheriot-audit \
#     -b build/cheriot/cheriot/release/plantnode.board.json \
#     -j build/cheriot/cheriot/release/plantnode.json \
#     -m audit/i2c_access.rego \
#     -q 'data.i2c_access.all_pass'
# ---------------------------------------------------------------------------

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
# Summary
# ---------------------------------------------------------------------------
all_pass := true if {
  prop1_pass == true
  prop2_pass == true
  prop3_pass == true
  prop4_pass == true
}
