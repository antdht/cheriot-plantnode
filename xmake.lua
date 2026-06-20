---@diagnostic disable: undefined-global, lowercase-global

set_project("CHERIoT smart plant node")

sdkdir = "../sonata-software/cheriot-rtos/sdk"
netstackdir = "../sonata-software/network-stack/lib"
libsdir = "../sonata-software/libraries"
includes(sdkdir)
includes(netstackdir)
includes(libsdir)
set_toolchains("cheriot-clang")

option("board")
set_default("sail")

-- Display compartment

compartment("display")
add_files("src/display/display.cc")
add_includedirs("src", "../sonata-software/libraries")
add_deps("freestanding", "debug", "lcd")

-- Application compartments

compartment("core_logic")
add_files("src/core_logic.cc")
add_includedirs("src")
add_deps("freestanding", "debug")

compartment("comms")
add_files("src/comms.cc")
add_includedirs("src", "../sonata-software/network-stack/include", "../sonata-software/network-stack/examples/04.MQTT")
add_deps("freestanding", "debug", "DNS", "TCPIP", "NetAPI", "TLS", "Firewall", "SNTP", "MQTT", "time_helpers", "stdio")
add_rules("cheriot.network-stack.ipv6")

compartment("data_processing")
add_files("src/data_processing.cc")
add_includedirs("src", "../sonata-software/network-stack/include")
add_deps("freestanding", "debug", "time_helpers", "SNTP")

compartment("policy_engine")
add_files("src/policy_engine.cc")
add_includedirs("src")
add_deps("freestanding", "debug")

-- attestation: measures the firmware image (reads spi2 flash, hydro_hash) and
-- builds quotes. Holds the SPI-flash capability and is the only caller of
-- fake_tpm_sign. Compiles its own copy of libhydrogen for hashing.
compartment("attestation")
add_files("src/attestation.cc")
add_files("../cheriot-demos/third_party/crypto/libhydrogen/hydrogen.c")
add_includedirs("src", "../cheriot-demos/third_party/crypto/libhydrogen")
add_defines("CHERIOT_NO_AMBIENT_MALLOC")
add_deps("freestanding", "debug")

-- fake_tpm: pure signing oracle. Holds the device signing key and nothing
-- else; compiles its own copy of libhydrogen for hydro_sign.
compartment("fake_tpm")
add_files("src/fake_tpm.cc")
add_files("../cheriot-demos/third_party/crypto/libhydrogen/hydrogen.c")
add_includedirs("src", "../cheriot-demos/third_party/crypto/libhydrogen")
add_defines("CHERIOT_NO_AMBIENT_MALLOC")
add_deps("freestanding", "debug")

compartment("crypto")
add_files("src/crypto.cc")
add_files("../cheriot-demos/third_party/crypto/libhydrogen/hydrogen.c")
add_includedirs("src", "../cheriot-demos/third_party/crypto/libhydrogen")
add_defines("CHERIOT_NO_AMBIENT_MALLOC")
add_deps("freestanding", "debug")

-- Driver compartments

compartment("i2c_driver")
add_files("src/drivers/i2c_driver.cc")
add_includedirs("src")
add_deps("freestanding", "debug")

compartment("moisture_sensor")
add_files("src/drivers/moisture_sensor.cc")
add_includedirs("src")
add_deps("freestanding", "debug")

compartment("temperature_sensor")
add_files("src/drivers/temperature_sensor.cc")
add_includedirs("src")
add_deps("freestanding", "debug")

compartment("pump_driver")
add_files("src/drivers/pump_driver.cc")
add_includedirs("src")
add_deps("freestanding", "debug")

-- Firmware image

firmware("plantnode")
add_deps("freestanding", "debug")
-- Application
add_deps("core_logic", "comms", "data_processing", "policy_engine", "attestation", "fake_tpm", "crypto", "display")
-- Drivers
add_deps("i2c_driver", "moisture_sensor", "temperature_sensor", "pump_driver")
-- Network stack
add_deps("TCPIP", "TLS", "MQTT", "Firewall", "NetAPI", "DNS", "SNTP")
-- Required for RSA certificate validation)
add_options("tls-rsa")
after_link(function(target)
	local fw = target:targetfile()
	local outdir = path.join(os.projectdir(), "build/uf2")
	os.mkdir(outdir)
	os.execv("llvm-strip", { fw, "-o", fw .. ".strip" })
	os.execv(
		"uf2conv",
		{ fw .. ".strip", "-b0x00000000", "-f0x6CE29E60", "-co", path.join(outdir, "plantnode.slot1.uf2") }
	)
	os.execv(
		"uf2conv",
		{ fw .. ".strip", "-b0x10000000", "-f0x6CE29E60", "-co", path.join(outdir, "plantnode.slot2.uf2") }
	)
	os.execv(
		"uf2conv",
		{ fw .. ".strip", "-b0x20000000", "-f0x6CE29E60", "-co", path.join(outdir, "plantnode.slot3.uf2") }
	)
end)
on_load(function(target)
	target:values_set("board", "sonata-1.1")
	target:values_set("threads", {
		{
			-- Main application thread. Stack must cover the full
			-- cross-compartment call chain incl. TLS in comms.
			compartment = "core_logic",
			priority = 1,
			entry_point = "core_entry",
			stack_size = 8160,
			trusted_stack_frames = 8,
		},
		{
			compartment = "TCPIP",
			priority = 1,
			entry_point = "ip_thread_entry",
			stack_size = 0x1000,
			trusted_stack_frames = 5,
		},
		{
			compartment = "Firewall",
			priority = 2,
			entry_point = "ethernet_run_driver",
			stack_size = 0x1000,
			trusted_stack_frames = 5,
		},
	}, { expand = false })
end)
