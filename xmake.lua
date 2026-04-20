---@diagnostic disable: undefined-global, lowercase-global

set_project("CHERIoT smart plant node")

sdkdir = "../sonata-software/cheriot-rtos/sdk"
netstackdir = "../sonata-software/network-stack/lib"
includes(sdkdir)
includes(netstackdir)
set_toolchains("cheriot-clang")

option("board")
set_default("sail")

compartment("mqtt")
add_files("src/mqtt.cc")
add_includedirs("../sonata-software/network-stack/include")
add_includedirs("../sonata-software/network-stack/examples/04.MQTT")
add_deps("freestanding", "DNS", "TCPIP", "NetAPI", "TLS", "Firewall", "SNTP", "MQTT", "time_helpers", "debug", "stdio")
add_rules("cheriot.network-stack.ipv6")

firmware("plantnode")
add_deps("freestanding", "debug")
add_deps("mqtt")
add_deps("TCPIP", "TLS", "MQTT", "Firewall", "NetAPI", "DNS", "SNTP")
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
			compartment = "mqtt",
			priority = 1,
			entry_point = "mqtt_entry",
			stack_size = 8160,
			trusted_stack_frames = 6,
		},
		{
			compartment = "TCPIP",
			priority = 1,
			entry_point = "ip_thread_entry",
			stack_size = 0xe00,
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
