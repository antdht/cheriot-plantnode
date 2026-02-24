set_project("CHERIoT smart plant node")

-- Has to point to the sdk provided on the `CHERIoT-Platform/cheriot-rtos` repo
sdkdir = "../cheriot-rtos/sdk"
includes(sdkdir)
set_toolchains("cheriot-clang")

option("board")
set_default("sail")

compartment("uart")
add_files("src/uart.cc")

compartment("hello")
add_files("src/hello.cc")

-- Firmware image for the example.
firmware("plantnode")
-- Both compartments need memcpy
add_deps("freestanding", "debug")
add_deps("hello", "uart")
on_load(function(target)
	target:values_set("threads", {
		{
			compartment = "hello",
			priority = 1,
			entry_point = "entry",
			stack_size = 0x200,
			trusted_stack_frames = 1,
		},
	}, { expand = false })
end)
