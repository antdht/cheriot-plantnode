# CHERIoT smart plant node 

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Build](https://img.shields.io/badge/build-xmake-green)](https://xmake.io/)

A secure embedded architecture for plant monitoring systems, implemented as a CHERIoT-RTOS application targeting the [Sonata board](https://github.com/lowRISC/sonata-system). Leverages CHERI capabilities for compartmentalized IoT communication (MQTT over TLS).

## Prerequisites

This project depends on [`sonata-software`](https://github.com/lowRISC/sonata-software) for the CHERIoT SDK, network stack, and build toolchain. Clone it as a sibling directory:

```
root/
├── sonata-software/   # provides SDK, network stack, nix dev environment
└── cheriot-plantnode/ # this project
```

Enter the `sonata-software` nix development environment, which provides `cheriot-clang`, `xmake`, `llvm-strip`, `uf2conv`, and all other required tools:

```bash
cd sonata-software
nix develop
```

## Build

From inside the nix shell, run xmake **pointing at this project**:

```bash
# from the sonata-software/ directory (while inside nix develop)
xmake -P ../cheriot-plantnode
```

> **IPv4-only networks:** disable IPv6 before building to avoid connection issues:
> ```bash
> xmake config -P ../cheriot-plantnode --IPv6=n
> ```

This compiles the firmware and automatically produces three UF2 images (one per flash slot) under `cheriot-plantnode/build/uf2/`:

```
build/uf2/
├── plantnode.slot1.uf2   # base address 0x00000000
├── plantnode.slot2.uf2   # base address 0x10000000
└── plantnode.slot3.uf2   # base address 0x20000000
```

## Flash to the Sonata board

Plug in the Sonata board over USB. It will appear as a USB mass-storage drive. Copy the UF2 for the desired slot to the drive:

```bash
cp ./build/uf2/plantnode.slot2.uf2 /run/media/$USER/SONATA/firmware.uf2
```

The board reboots automatically and boots from the selected slot. Use the slot-select switch on the board to choose which slot to run at startup.

## Serial output

Debug output is printed over UART at 921600 baud. Connect with any serial terminal:

```bash
picocom -b 921600 /dev/ttyUSB1   # adjust device as needed
# or
screen /dev/ttyUSB1 921600
```

On a successful start you should see:

```
PlantNode: === PlantNode firmware starting ===
PlantNode MQTT: Starting network stack...
PlantNode MQTT: Network stack started.
PlantNode MQTT: Synchronising time via SNTP...
...
```

## Project Structure

```
cheriot-plantnode/
├── src/               # Application source
├── build/
│   └── uf2/           # generated UF2 images (after build)
├── xmake.lua          # build configuration
├── README.md
└── LICENSE
```
