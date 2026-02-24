# CHERIoT smart plant node 

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Build](https://img.shields.io/badge/build-xmake-green)](https://xmake.io/)

A secure embedded architecture for plant monitoring systems, implemented as a CHERIoT-RTOS application. Leverages CHERI capabilities for compartmentalized IoT control of sensors (soil moisture, humidity, temperature) and actuators.

## Features
- Capability-based security for sensor isolation.
- Real-time monitoring and control.
- Simulator support for development.

## Prerequisites
Before building, follow the official [CHERIoT Getting Started Guide](https://github.com/CHERIoT-Platform/cheriot-rtos/blob/main/docs/GettingStarted.md) to set up the toolchain and simulator.

You should add the simulator to your path. 

Verify with:

```bash
cheriot-simulator

# Should normally produce 
No elf file provided.
Usage: cheriot_sim [options] <elf_file> [<elf_file> ...]
```

## Build and Run
Quick summary of what is explained in the getting started guide, if you are familiar with it, you can skip this part.

But, as a quick reminder: as explained in the getting started guide, the build system requires a configure step and a build step:

### Configure
Use `xmake` to configure the project: 

```bash
# For cheriot simulator (defaults for the sail board)
xmake config --sdk=../builds/cheriot-llvm -m debug

# For the sonata board
xmake config --sdk=../builds/cheriot-llvm -m release --board=sonata
```

In the getting started guide, the `--sdk` in more commonly referred to as `/cheriot-tools/`

### Build
```bash
xmake build 
```

The final step of the build should look like this 

`[ 96%]: linking firmware build/cheriot/cheriot/<release|debug>/plantnode`

### Run 
Whether you configured your project the board `sail` (the default one) or the `sonata` one, `xmake run` will execute accordingly. In the first case it will run you project with the simulator and in the latest, it will compile the source code and try to push it directly to the plugged sonata board.

Typically, you'd simply type:

```bash
xmake run 
```

## Project Structure
```
cheriot-plantnode
├── src/              # Application source 
├── xmake.lua         # Build configuration
├── README.md
└── LICENSE
```
