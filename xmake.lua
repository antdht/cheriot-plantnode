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
add_includedirs("src", "../sonata-software/network-stack/include")
add_deps("freestanding", "debug", "SNTP")

compartment("comms")
add_files("src/comms.cc")
add_includedirs("src", "../sonata-software/network-stack/include", "../sonata-software/network-stack/examples/04.MQTT")
add_deps("freestanding", "debug", "DNS", "TCPIP", "NetAPI", "TLS", "Firewall", "SNTP", "MQTT", "time_helpers", "stdio")
add_rules("cheriot.network-stack.ipv6")

compartment("policy_engine")
add_files("src/policy_engine.cc")
add_includedirs("src")
add_deps("freestanding", "debug")

-- attestation: MOCK measurer. Returns a canned firmware measurement and builds
-- a mock quote, calling tpm to "sign" it. No longer reads flash or hashes, so
-- it needs no SPI capability and no libhydrogen.
compartment("attestation")
add_files("src/attestation.cc")
add_includedirs("src")
add_deps("freestanding", "debug")

-- tpm: MOCK signing oracle (NOT a real TPM). Returns canned bytes; no real key,
-- no libhydrogen.
compartment("tpm")
add_files("src/tpm.cc")
add_includedirs("src")
add_deps("freestanding", "debug")

compartment("crypto")
add_files("src/crypto.cc")
add_files("../cheriot-demos/third_party/crypto/libhydrogen/hydrogen.c")
add_includedirs("src", "../cheriot-demos/third_party/crypto/libhydrogen")
add_defines("CHERIOT_NO_AMBIENT_MALLOC")
add_deps("freestanding", "debug")

compartment("control_loop")
add_files("src/control_loop.cc")
add_includedirs("src", "../sonata-software/network-stack/include")
add_deps("freestanding", "debug", "locks", "time_helpers", "SNTP")

-- Driver compartments

compartment("i2c_driver")
add_files("src/drivers/i2c_driver.cc")
add_includedirs("src")
add_deps("freestanding", "debug", "locks")

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
add_deps("core_logic", "comms", "policy_engine", "attestation", "tpm", "crypto", "display", "control_loop")
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
			compartment = "control_loop",
			priority = 3,
			entry_point = "control_entry",
			stack_size = 4096,
			trusted_stack_frames = 6,
		},
		{
			-- Telemetry thread. Stack must cover the full
			-- cross-compartment call chain incl. TLS in comms.
			compartment = "core_logic",
			priority = 1,
			entry_point = "telemetry_entry",
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
