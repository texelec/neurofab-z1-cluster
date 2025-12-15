# NeuroFab Z1 Onyx Cluster

**A distributed spiking neural network (SNN) hardware accelerator based on Raspberry Pi Pico 2 (RP2350) microcontrollers.**

![Hardware Version](https://img.shields.io/badge/Hardware-V2%20(16--node)-blue)
![Firmware](https://img.shields.io/badge/Firmware-RP2350-green)
![Build](https://img.shields.io/badge/Build-Passing-brightgreen)

---

## Overview

The Z1 Onyx is a real-time neuromorphic computing cluster designed for hardware-accelerated spiking neural network simulation. It features 16 compute nodes plus a controller, all interconnected via a custom high-speed parallel bus (Matrix bus).

### Key Features

- **17 RP2350 Nodes**: 16 compute nodes + 1 controller
- **Custom Parallel Bus**: 16-bit data @ 10 MHz (10.31 MB/s TX, 8.07 MB/s RX)
- **Distributed SNN Processing**: Multi-node spike propagation and routing
- **8MB PSRAM per Node**: For large-scale neural network storage
- **RESTful HTTP API**: JSON-based cluster management
- **Auto-Configuration**: V2 hardware automatically detects node IDs
- **Python Tools**: CLI utilities for deployment, monitoring, and testing

### Hardware Specifications

- **MCU**: Raspberry Pi Pico 2 (RP2350B, ARM Cortex-M33)
- **PSRAM**: 8MB per node (RP2350 QSPI)
- **Clock**: 266 MHz (overclocked from 150 MHz)
- **Interconnect**: Custom PCB backplane with parallel bus
- **Network**: W5500 Ethernet (controller only)
- **Display**: SSD1306 OLED (controller only, V2 hardware)

---

## Quick Start

### 1. Hardware Setup

- Flash controller firmware: `FirmwareReleases/16node/z1_controller_16.uf2`
- Flash node firmware: `FirmwareReleases/16node/z1_node_16.uf2` (same file for all nodes)
- Connect controller to network (default IP: 192.168.1.222)
- Power on cluster

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
- **Note**: Firmware builds successfully but untested on V1 hardware

Both variants supported by a single codebase with compile-time selection.

---

## Network Configuration

**Default IP**: 192.168.1.222

To customize, edit `controller/w5500_eth.c`:
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

All tools support `-c <IP>` to specify controller address.

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
4. Spike injection
5. Statistics retrieval

See [TEST_DEPLOYMENT_GUIDE.md](TEST_DEPLOYMENT_GUIDE.md) for detailed testing documentation.

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
```

Each node runs an SNN engine that processes neurons and propagates spikes. The controller manages the cluster via the Matrix bus and provides external access via HTTP.

---

## Current Status

**Phase 4 Complete** ✅ (as of December 14, 2025)

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

## Acknowledgments

- Raspberry Pi Foundation for the Pico 2 (RP2350) platform
- Pico SDK and community tools
