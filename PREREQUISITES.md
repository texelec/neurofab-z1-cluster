# Z1 Onyx Cluster - Complete Prerequisites Guide

## Overview

This document provides a comprehensive list of all tools, libraries, and dependencies required to build and deploy the Z1 Onyx firmware.

---

## Build Environment (Windows x64)

### Required Software

| Tool | Version | Purpose | Download |
|------|---------|---------|----------|
| **Raspberry Pi Pico SDK** | Latest | Core SDK for RP2350 | https://github.com/raspberrypi/pico-sdk |
| **ARM GNU Toolchain** | 13.2.1+ | Cross-compiler for ARM Cortex-M33 | https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads |
| **CMake** | 3.13+ | Build system generator | https://cmake.org/download/ |
| **Ninja** | Latest | Fast build system | https://ninja-build.org/ |
| **Python** | 3.7+ | Build scripts and tools | https://www.python.org/downloads/ |
| **Git** | Latest | Version control | https://git-scm.com/downloads |

### Python Packages

```bash
pip install requests  # Required for HTTP API communication
pip install pyusb     # Optional, for USB device communication
```

### Environment Variables

```powershell
# Set Pico SDK path
$env:PICO_SDK_PATH = "C:\Pico\pico-sdk"

# Add tools to PATH
$env:PATH += ";C:\Pico\arm-none-eabi\bin"
$env:PATH += ";C:\tools"  # Ninja location
$env:PATH += ";C:\Program Files\CMake\bin"
```

---

## Included Build Tools (Windows x64 Only)

The `build_tools/` directory contains pre-compiled Windows binaries:

| File | Version | Purpose | Source |
|------|---------|---------|--------|
| **pioasm.exe** | 253 KB | PIO assembler | Pico SDK |
| **picotool.exe** | 1.6 MB | UF2 generator/flasher | Pico SDK |
| **libusb-1.0.dll** | 158 KB | USB library | libusb project |
| **elf2uf2.py** | 1.9 KB | Custom UF2 converter | Z1 Onyx (Python) |
| **merge_dual_partition.py** | 4.8 KB | Bootloader+app merger | Z1 Onyx (Python) |
| **prepend_app_header.py** | 3.9 KB | App header generator | Z1 Onyx (Python) |

**Linux/macOS Users**: You must build pioasm and picotool from source. See Pico SDK documentation:
- https://github.com/raspberrypi/pico-sdk
- https://github.com/raspberrypi/picotool

---

## Third-Party Libraries (Included)

### FatFs Filesystem (common/FatFs_SPI/)
- **Version**: R0.15
- **Author**: ChaN
- **License**: BSD-style (see ff15/source/ff.h)
- **Purpose**: FAT32 filesystem for SD card
- **Website**: http://elm-chan.org/fsw/ff/
- **Adapter**: no-OS-FatFS-SD-SDIO-SPI-RPi-Pico by carlk3 (https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico)

### SSD1306 OLED Driver (common/oled/)
- **Author**: Z1 Onyx team (adapted for RP2350)
- **License**: MIT (matches project license)
- **Purpose**: 128x64 I2C OLED display driver

### PSRAM Driver (common/psram/)
- **Author**: Z1 Onyx team (custom for RP2350B)
- **License**: MIT
- **Purpose**: 8MB QSPI PSRAM interface

---

## Hardware Requirements

### Development Hardware
- **Raspberry Pi Pico 2** (RP2350B) - At least 1 for testing
- **USB cable** - Micro-USB for programming
- **Windows PC** - For using pre-built tools (or Linux/Mac with source builds)

### Z1 Cluster Hardware (Full System)
- **17 Raspberry Pi Pico 2 boards** - 16 nodes + 1 controller
- **Custom PCB backplane** - 4-layer with parallel bus
- **W5500 Ethernet module** - For controller network
- **SD card** - FAT32 formatted, 1GB to 32GB
- **SSD1306 OLED display** - 128x64 I2C (V2 hardware only)
- **Power supply** - 5V, sufficient current for 17 boards

---

## Network Requirements

### For Deployment and OTA Updates
- **Local network** - Controller must be accessible via Ethernet
- **DHCP or static IP** - Default: 192.168.1.201 (configurable)
- **Python with requests** - For HTTP API communication
- **SSH/Serial terminal** - Optional, for debugging

### Python Tools Network Dependencies
All tools in `python_tools/bin/` communicate with controller via HTTP:
- Default IP: 192.168.1.222
- Default Port: 80 (HTTP)
- Change with `-c` flag: `nls -c 192.168.1.100`

---

## Building the Firmware

### Quick Build (V2 - 16 nodes)
```powershell
# Ensure PICO_SDK_PATH is set
python build.py
```

### V1 Hardware (12 nodes - legacy)
```powershell
python build.py --hw-v1
```

### Output Locations
- **V2 Firmware**: `FirmwareReleases/16node/`
- **V1 Firmware**: `FirmwareReleases/12node/`
- **Build artifacts**: `build/` (ignored by git)

---

## Python Tools

### Command-Line Utilities (python_tools/bin/)

| Tool | Purpose | Requirements |
|------|---------|--------------|
| `nls` | List cluster nodes | requests |
| `nstat` | Node statistics | requests |
| `nflash` | OTA firmware updates | requests |
| `nsnn` | SNN deployment | requests |
| `nreset` | Reset nodes | requests |
| `zconfig` | Network configuration | requests |
| `zengine` | File management | requests |
| `z1pack` | Package creation | None |
| `test_deployment.py` | Full cluster test | requests |

### Installation (Optional)
```powershell
# Add python_tools/bin to PATH for global access
$env:PATH += ";d:\ACODE\Z1Onyx\Code\python_tools\bin"
```

---

## Testing Requirements

### Minimal Test Setup
- 1 controller board (flashed with controller firmware)
- 2 compute nodes (flashed with node firmware)
- Network cable (for controller Ethernet)
- SD card (in controller)

### Full Test Setup
- All 17 boards powered and connected
- Run: `python test_deployment.py`

---

## Optional Tools

### For Development
- **VS Code** - Recommended IDE with C/C++ extensions
- **Serial terminal** - PuTTY, Tera Term, or VS Code Serial Monitor
- **Logic analyzer** - For debugging bus communication (optional)
- **Oscilloscope** - For signal integrity analysis (optional)

### For Advanced OTA Development
- **SD card reader** - For manual firmware file management
- **JTAG/SWD debugger** - For low-level debugging (optional)

---

## Platform-Specific Notes

### Windows (Fully Supported)
- All tools pre-compiled in `build_tools/`
- setup_build_env.ps1 automates environment setup
- No additional compilation needed

### Linux (Manual Build Required)
```bash
# Build picotool
cd ~/pico
git clone https://github.com/raspberrypi/picotool.git
cd picotool
mkdir build && cd build
cmake ..
make
sudo make install

# Build pioasm (included in Pico SDK build)
cd ~/pico/pico-sdk
mkdir build && cd build
cmake ..
make
```

### macOS (Manual Build Required)
```bash
# Install Homebrew dependencies
brew install cmake
brew install libusb
brew install arm-none-eabi-gcc

# Build picotool (same as Linux)
cd ~/pico
git clone https://github.com/raspberrypi/picotool.git
cd picotool
mkdir build && cd build
cmake ..
make
sudo make install
```

---

## Troubleshooting

### Common Issues

**"PICO_SDK_PATH not set"**
- Solution: Set environment variable: `$env:PICO_SDK_PATH = "C:\Pico\pico-sdk"`

**"arm-none-eabi-gcc not found"**
- Solution: Add to PATH: `$env:PATH += ";C:\Pico\arm-none-eabi\bin"`

**"picotool.exe not found"**
- Solution: Verify `build_tools/picotool.exe` exists or build from source

**"Module 'requests' not found"**
- Solution: `pip install requests`

**Build fails with "spaces in path" error**
- Solution: Move project to path without spaces (ARM GCC limitation)

---

## Quick Reference

### Verify Environment
```powershell
# Check all tools
arm-none-eabi-gcc --version
cmake --version
ninja --version
python --version
$env:PICO_SDK_PATH

# Test Python packages
python -c "import requests; print('requests OK')"
```

### Build and Flash
```bash
# Build firmware
python build.py

# Flash via USB (automated)
python flash_controller.py
python flash_node.py

# Or manually: drag UF2 files to Pico in BOOTSEL mode
```

### Quick Deploy
```bash
# List nodes
python python_tools/bin/nls

# Deploy topology
python python_tools/bin/nsnn deploy python_tools/examples/xor_working.json

# Start SNN
python python_tools/bin/nsnn start
```

---

## Version Information

- **Project**: NeuroFab Z1 Onyx Cluster
- **Document Version**: 1.0 (January 18, 2026)
- **Firmware Version**: See build outputs for individual component versions
- **Pico SDK**: Compatible with latest Pico SDK (tested with 2.0.0)

---

## Support and Documentation

- **Build Instructions**: See BUILD_INSTRUCTIONS.md
- **API Reference**: See documentation/API_REFERENCE.md
- **Test Guide**: See TEST_DEPLOYMENT_GUIDE.md
- **Documentation Index**: See documentation/DOCUMENTATION_INDEX.md
