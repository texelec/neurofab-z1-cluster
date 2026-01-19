# NeuroFab Z1 Onyx Cluster

**A distributed spiking neural network (SNN) hardware accelerator based on Raspberry Pi Pico 2 (RP2350) microcontrollers.**

![Hardware Version](https://img.shields.io/badge/Hardware-V1%20%2B%20V2-blue)
![Firmware](https://img.shields.io/badge/Firmware-RP2350-green)
![Build](https://img.shields.io/badge/Build-Passing-brightgreen)
![Tests](https://img.shields.io/badge/Tests-9%2F9%20Passing-brightgreen)
![Status](https://img.shields.io/badge/Status-Development%20Ready-yellow)
![SNN](https://img.shields.io/badge/SNN-Operational-brightgreen)

---

## Overview

The Z1 Onyx is a real-time neuromorphic computing cluster designed for hardware-accelerated spiking neural network simulation. It features 16 compute nodes plus a controller, all interconnected via a custom high-speed parallel bus (Matrix bus).

### Key Features

- **17 RP2350 Nodes**: 16 compute nodes + 1 controller
- **Custom Parallel Bus**: 16-bit data @ 10 MHz (10.31 MB/s TX, 8.07 MB/s RX, 100% reliability)
- **Distributed SNN Processing**: Multi-node spike propagation and routing (fully operational)
- **8MB PSRAM per Node**: For large-scale neural network storage
- **Dual-Partition Bootloader**: OTA firmware updates via HTTP API (Phase 2 complete)
- **OTA Flash Tools**: Remote node firmware updates without physical access (`nflash` tool)
- **DMA Recovery**: Automatic recovery from bus corruption (100ms cooldown)
- **SD Card Storage**: FAT32 for config, topologies, and OTA updates (30GB tested)
- **Runtime Network Config**: Change IP/MAC without firmware rebuild (`zconfig` tool)
- **Streaming File System**: Upload/download topologies up to 1MB+ via HTTP (`zengine` tool)
- **Async Spike Injection**: Background job queue for non-blocking spike processing
- **RESTful HTTP API**: JSON-based cluster management with reboot support
- **Auto-Configuration**: V2 hardware automatically detects node IDs (0-15)
- **Flash Utilities**: USB-based firmware flashing tools (`flash_node.py`, `flash_controller.py`)
- **Comprehensive Python Tools**: 13+ CLI utilities for deployment, monitoring, and OTA updates

### Hardware Specifications

- **MCU**: Raspberry Pi Pico 2 (RP2350B, ARM Cortex-M33)
- **PSRAM**: 8MB per node (RP2350 QSPI)
- **Clock**: 266 MHz (overclocked from 150 MHz)
- **Flash Layout**: Dual-partition architecture
  - **Bootloader**: 512KB @ 0x00000000 (OTA update engine)
  - **Application**: 7.5MB @ 0x00080000 (SNN firmware + 192-byte header)
- **Interconnect**: Custom PCB backplane with parallel bus
- **Network**: W5500 Ethernet (controller only)
- **Storage**: SD card (FAT32, up to 30GB tested)
- **Display**: SSD1306 OLED (controller only, V2 hardware)

---

## Quick Start

### 1. Hardware Setup

**Dual-Partition Firmware (OTA-Capable)**
- Flash node firmware: `FirmwareReleases/16node/node_dual_16.uf2` (bootloader + app)
- Flash controller firmware: `FirmwareReleases/16node/controller_16.uf2` (monolithic)
- Alternative: Use `flash_node.py` and `flash_controller.py` for automated USB flashing
- Controller network default: 192.168.1.222 (configurable via `zconfig`)
- Power on cluster

**Directory Structure**:
```
FirmwareReleases/16node/
├── controller_16.uf2           # Controller (main build, drag to BOOTSEL)
├── node_dual_16.uf2            # Node firmware (bootloader + app, OTA-capable)
└── apponly/                    # Advanced/recovery files
    ├── bootloader_16.uf2       # Bootloader only (recovery)
    └── node_app_16.uf2         # App partition only (advanced)
```

**V1 Hardware** (12-node):
```
FirmwareReleases/12node/
├── controller_12.uf2           # Controller
├── node_dual_12_0.uf2          # Node 0 (bootloader + app)
├── node_dual_12_1.uf2          # Node 1 (bootloader + app)
├── ... (12 total node files)
└── apponly/                    # Advanced/recovery files
```

### 2. Test Deployment

```bash
# Run comprehensive test suite
python test_deployment.py

# Or specify custom controller IP
python test_deployment.py -c 192.168.1.100
```

### 3. Deploy Your Own Network

```bash
# List available nodes
python python_tools/bin/nls

# Deploy SNN topology
python python_tools/bin/nsnn deploy python_tools/examples/xor_working.json

# Start SNN processing
python python_tools/bin/nsnn start

# Inject spikes
python python_tools/bin/nsnn inject spike_pattern.json

# Monitor status
python python_tools/bin/nstat -s
```

---

## Documentation

| Document | Description |
|----------|-------------|
| [BUILD_INSTRUCTIONS.md](BUILD_INSTRUCTIONS.md) | How to build firmware from source |
| [TEST_DEPLOYMENT_GUIDE.md](TEST_DEPLOYMENT_GUIDE.md) | Comprehensive testing guide |
| [API_REFERENCE.md](API_REFERENCE.md) | HTTP API and technical reference |
| [PIN_REFERENCE.md](PIN_REFERENCE.md) | Hardware pin assignments (V1 and V2) |

---

## Building from Source

### Prerequisites

- Raspberry Pi Pico SDK
- ARM GCC Toolchain (arm-none-eabi-gcc 13.2+)
- CMake 3.13+
- Ninja build system
- Python 3.7+

**Platform Note**: Pre-built tools in `build_tools/` (pioasm.exe, picotool.exe, libusb-1.0.dll) are for **Windows x64 only**. Linux and macOS users must build these tools from source - see [PREREQUISITES.md](PREREQUISITES.md) for instructions.

### Build Commands

```bash
# Set Pico SDK path
export PICO_SDK_PATH=/path/to/pico-sdk  # Linux/Mac
$env:PICO_SDK_PATH = "C:\Pico\pico-sdk" # Windows

# Build V2 hardware (16-node, current)
python build.py

# Build V1 hardware (12-node, legacy)
python build.py --hw-v1
```

Firmware outputs: `FirmwareReleases/16node/` or `FirmwareReleases/12node/`

See [BUILD_INSTRUCTIONS.md](BUILD_INSTRUCTIONS.md) for detailed setup.

---

## Hardware Variants

### V2 (16-node) - Current Production
- 16 compute nodes with automatic ID detection
- OLED display on controller
- Global node reset capability
- W5500 pins: RST=GPIO34, INT=GPIO35

### V1 (12-node) - Legacy
- 12 compute nodes with hardcoded IDs
- No OLED display
- No global reset
- W5500 pins: RST=GPIO35, INT=GPIO34 (swapped from V2)
- **Status**: Fully validated, OTA operational

Both variants supported by a single codebase with compile-time selection.

---

## Network Configuration

**Default:** 192.168.1.222

**Method 1: Runtime (Recommended)** - No rebuild required:
```bash
python python_tools/bin/zconfig write --ip 192.168.1.100 -c 192.168.1.222
```

**Method 2: Build-time** - Edit `controller/w5500_eth.c`:
```c
static const uint8_t IP_ADDRESS[4] = {192, 168, 1, 222};  // Change here
```

The OLED display and HTTP responses automatically update via `w5500_get_ip_string()`.

See [BUILD_INSTRUCTIONS.md](BUILD_INSTRUCTIONS.md#customizing-network-configuration) for details.

---

## Python Tools

Located in `python_tools/bin/`:

| Tool | Purpose |
|------|---------|
| `nls` | List all nodes with status and memory info |
| `nstat` | Cluster statistics and monitoring |
| `nsnn` | Deploy, start, stop, and manage SNN topologies |
| `nping` | Network latency testing |
| `ncp` | Copy files to node PSRAM |
| `nflash` | **OTA firmware updates** - Flash nodes remotely via HTTP API |
| `zconfig` | Manage controller network configuration and reboot |
| `zengine` | Upload/download topology files to controller SD card (supports 1MB+) |
| `flash_node.py` | **USB flashing** - Flash node firmware via BOOTSEL (root directory) |
| `flash_controller.py` | **USB flashing** - Flash controller firmware via BOOTSEL (root directory) |

All tools support `-c <IP>` to specify controller address (default: 192.168.1.222).

### OTA Firmware Updates (nflash)

Flash node firmware remotely without physical access:

```bash
# Flash single node
python python_tools/bin/nflash 0 packages/node_app_16.bin -c 192.168.1.222

# Flash multiple nodes
python python_tools/bin/nflash 0,1,2,3 packages/node_app_16.bin -c 192.168.1.222

# Flash all nodes
python python_tools/bin/nflash all packages/node_app_16.bin -c 192.168.1.222
```

**Features**:
- **No physical access** - Update via HTTP API
- **Automatic validation** - CRC32 checksums on upload
- **Bootloader integration** - Seamless reboot into new firmware
- **Progress tracking** - Real-time upload status

**Note**: Requires dual-partition firmware (bootloader + app). See [BUILD_INSTRUCTIONS.md](BUILD_INSTRUCTIONS.md) for details.

### USB Flashing Tools

Located in project root for convenience:

```bash
# Flash V2 node (auto-detects ID)
python flash_node.py --hw-v2

# Flash V1 node (requires node ID)
python flash_node.py --hw-v1 --node 0

# Flash controller
python flash_controller.py --hw-v2
```

**Features**:
- **Auto-reboot** - Automatically enters BOOTSEL mode
- **Auto-detect** - Finds picotool in PATH or build_tools/
- **Cross-platform** - Works on Windows, Linux, macOS

### Network Configuration (zconfig)

Manage controller configuration stored in `z1.cfg` on SD card:

```bash
# View current configuration
python python_tools/bin/zconfig read -c 192.168.1.222

# Change IP address (updates config and reboots)
python python_tools/bin/zconfig write --ip 192.168.1.100 -c 192.168.1.222

# Change MAC address
python python_tools/bin/zconfig write --mac 02:00:00:00:00:01 -c 192.168.1.222

# Reboot controller
python python_tools/bin/zconfig reboot -c 192.168.1.222
```

### Topology Management (zengine)

Upload/download SNN topology files using streaming (tested up to 1MB):

```bash
# List all topology files
python python_tools/bin/zengine list -c 192.168.1.222

# Upload topology to SD card
python python_tools/bin/zengine upload python_tools/examples/xor_working.json -c 192.168.1.222

# Download topology from SD card
python python_tools/bin/zengine download xor_working.json -c 192.168.1.222

# Delete topology
python python_tools/bin/zengine delete xor_working.json -c 192.168.1.222
```

**Features**:
- **Streaming upload/download** - No memory limits, handles files up to 1MB+
- **Directory listing** - View all files with sizes
- **Automatic directory creation** - Creates `topologies/` if it doesn't exist
- **File deletion** - Remove old topology files
- **Integrity verification** - SHA256 checksums for upload/download validation
- **Tested file sizes**: 2KB to 1MB with perfect integrity

See [STREAMING_FILE_SYSTEM.md](STREAMING_FILE_SYSTEM.md) for technical details.

---

## SD Card Directory Structure

The controller's SD card uses FAT32 filesystem with the following layout:

```
/
├── z1.cfg              # Network configuration (IP, MAC)
└── topologies/         # SNN network topology files (JSON)
    ├── xor_working.json
    ├── mnist_snn.json
    └── custom.json
```

**Note**: Topology files are network configurations deployed via PSRAM commands.

---

## HTTP API

Controller exposes REST API on port 80 (default IP: 192.168.1.222).

### Key Endpoints

- `GET /api/nodes` - List all nodes
- `POST /api/snn/deploy` - Deploy topology
- `POST /api/snn/start` - Start SNN processing
- `POST /api/snn/input` - Inject spikes
- `GET /api/snn/status` - Get network status

See [API_REFERENCE.md](API_REFERENCE.md#http-api) for complete API reference.

---

## Testing

Run the comprehensive test suite to validate deployment:

```bash
python test_deployment.py
```

This tests:
1. Node discovery
2. Topology deployment
3. SNN start/stop
4. Spike injection (**async - queued and processed in background**)
5. Statistics retrieval

**Spike Injection Efficiency:** The HTTP API now uses an **asynchronous job queue** architecture:
- **HTTP returns immediately** (< 1ms) after queueing spikes
- **Background task** injects spikes at controlled rate (100 spikes/sec)
- **Controller stays responsive** to other HTTP requests during injection
- **Example:** 2500 spikes = 25 seconds processing time, but HTTP returns in < 1ms
- **Monitoring:** Poll `/api/nodes` or check serial console for progress

See [ASYNC_SPIKE_INJECTION.md](ASYNC_SPIKE_INJECTION.md) for architecture details and [TEST_DEPLOYMENT_GUIDE.md](TEST_DEPLOYMENT_GUIDE.md) for testing documentation.

---

## Project Structure

```
├── controller/              # Controller firmware (bus master, HTTP server)
├── node/                    # Compute node firmware (SNN engine)
├── common/                  # Shared libraries
│   ├── z1_onyx_bus/         # Matrix bus protocol
│   ├── z1_broker/           # Message broker
│   ├── z1_commands/         # Command definitions
│   ├── oled/                # SSD1306 OLED driver (V2 only)
│   └── psram/               # RP2350 PSRAM driver
├── python_tools/            # Python CLI utilities
│   ├── bin/                 # Command-line tools
│   ├── lib/                 # Python libraries
│   └── examples/            # Sample topology files
├── build_tools/             # Build utilities (elf2uf2.py)
├── FirmwareReleases/        # Pre-built UF2 files
│   ├── 16node/              # V2 hardware binaries
│   └── 12node/              # V1 hardware binaries
├── build.py                 # Automated build script
├── test_deployment.py       # Comprehensive test suite
├── API_REFERENCE.md         # HTTP API and technical specs
└── PIN_REFERENCE.md         # Hardware pin assignments
```

---

## System Architecture

```
┌─────────────┐
│  Controller │  ← W5500 Ethernet (HTTP API)
│   (Node 16) │  ← SSD1306 OLED Display (V2)
└──────┬──────┘
       │
   Matrix Bus (16-bit parallel, 10 MHz)
       │
  ┌────┴────┬────┬────┬─────────┐
  │         │    │    │         │
┌─┴─┐     ┌─┴─┐ ┌┴──┐ ┌┴──┐   ┌─┴─┐
│N0 │ ... │N1 │ │N2 │ │N3 │...│N15│
└───┘     └───┘ └───┘ └───┘   └───┘
 8MB       8MB   8MB   8MB     8MB
PSRAM     PSRAM PSRAM PSRAM   PSRAM
```roduction Ready** ✅ (as of January 18, 2026)

- ✅ Multi-node spike propagation working
- ✅ HTTP API fully functional
- ✅ **OTA firmware updates** via HTTP (`nflash` tool)
- ✅ **Dual-partition bootloader** (512KB + 7.5MB app)
- ✅ **Streaming file system** (1MB+ topology upload/download)
- ✅ **Async spike injection** (background job queue)
- ✅ **Runtime network config** (no rebuild for IP/MAC changes)
- ✅ 13+ Python CLI tools operational
- ✅ Hardware variants (V1/V2) supported
- ✅ Automated testing suite (9/9 passing)
- ✅ Build system validated
- ✅ Zero CRC errors over 200000+ frames
- ✅ 100% bus reliability
- ✅ **USB flash utilities** (`flash_node.py`, `flash_controller.py`)
- ✅ Monolithic node firmware deprecated (OTA only)
- ✅ Multi-node spike propagation working
- ✅ HTTP API fully functional
- ✅ Python tools operational
- ✅ Hardware variants (V1/V2) supported
- ✅ Automated testing suite
- ✅ Build system validated
- ✅ Zero CRC errors over 200000+ frames
- ✅ 100% bus reliability

See [API_REFERENCE.md](API_REFERENCE.md) for technical details.

---

## Performance Metrics

| Metric | Value |
|--------|-------|
| Bus TX Throughput | 10.31 MB/s |
| Bus RX Throughput | 8.07 MB/s |
| ACK Round-Trip Time | ~200 μs |
| Frame Reliability | 100% (0 CRC errors) |
| Max Frame Size | 384 bytes payload |
| Bus Clock | 10 MHz |

---

## Example: XOR Network

Deploy a simple XOR network across 2 nodes:

```bash
# Deploy topology
python python_tools/bin/nsnn deploy python_tools/examples/xor_working.json

# Start processing
python python_tools/bin/nsnn start

# Inject test spikes
python python_tools/bin/nsnn inject spike_pattern.json

# Monitor results
python python_tools/bin/nstat -s
```

---

## Contributing

This is a hardware research project. Key areas for contribution:
- SNN algorithms and topologies
- STDP (spike-timing-dependent plasticity) implementation
- Performance optimization
- Multi-backplane cluster support
- Visualization tools

---

## Authors

NeuroFab Z1 Onyx Cluster
Hardware accelerated neuromorphic computing platform

---

## Third-Party Components

- **FatFs** - Generic FAT Filesystem Module R0.15 by ChaN (http://elm-chan.org/fsw/ff/)
- **SSD1306 OLED Driver** - Adapted for RP2350 I2C
- **no-OS-FatFS-SD-SDIO-SPI-RPi-Pico** - SD card SPI driver by carlk3 (https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico)

---

## Acknowledgments

- Raspberry Pi Foundation for the Pico 2 (RP2350) platform and Pico SDK
- ChaN for FatFs filesystem library
- carlk3 for SD card SPI driver
- ARM for GNU Toolchain
- Open source community for development tools

**Note**: Build tools in `build_tools/` directory (pioasm.exe, picotool.exe, libusb-1.0.dll) are pre-compiled for **Windows x64** only. Linux/Mac users must build these tools from source.
