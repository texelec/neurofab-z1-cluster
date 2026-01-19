# Z1 Onyx Comprehensive Tools Reference

**Last Updated:** January 17, 2026  
**Version:** 4.0 (Production Ready)  
**Hardware:** RP2350-based Z1 Onyx 16-node cluster

---

## Table of Contents

1. [Build Scripts](#build-scripts)
2. [Flash Tools](#flash-tools)
3. [Core Management Tools](#core-management-tools)
4. [Firmware Tools](#firmware-tools)
5. [Configuration Tools](#configuration-tools)
6. [Testing Tools](#testing-tools)
7. [Deprecated Tools](#deprecated-tools)
8. [Quick Reference](#quick-reference)
9. [Common Workflows](#common-workflows)

---

## Build Scripts

### **build.py** - Standard Firmware Build
**Location:** Project root  
**Purpose:** Build controller and node firmware for V1 (12-node) or V2 (16-node) hardware  
**Status:** ✅ Production Ready

**Usage:**
```bash
# Build BOTH V1 and V2 hardware (default)
python build.py

# Build ONLY V1 hardware (12 nodes)
python build.py --hw-v1

# Build ONLY V2 hardware (16 nodes)
python build.py --hw-v2
```

**Flags:**
- `--hw-v1` - Build ONLY V1 hardware (12 nodes)
  - Type: boolean flag
  - Default: Builds BOTH if neither flag specified
  - Creates: controller_12, node_12_0 through node_12_11 (12 separate binaries)
  
- `--hw-v2` - Build ONLY V2 hardware (16 nodes)
  - Type: boolean flag
  - Default: Builds BOTH if neither flag specified
  - Creates: controller_16, node_16 (single node binary with auto-detect)

**Outputs:**
- **Build directory:** `build/` (all ELF, BIN, HEX, DIS files)
- **Release directory:** `FirmwareReleases/16node/` and/or `FirmwareReleases/12node/`
  - UF2 files only (ready for flashing)
  - V2: controller_16.uf2, node_16.uf2, bootloader_16.uf2, node_app_16.uf2, node_dual_16.uf2
  - V1: controller_12.uf2, node_12_0.uf2 through node_12_11.uf2
- **Packages directory:** `packages/` (OTA-ready binaries)
  - V2: node_app_16.bin (automatically copied for OTA deployment)

**Requirements:**
- `PICO_SDK_PATH` environment variable must be set
- ARM GCC, CMake, Ninja in PATH
- Run `.\setup_build_env.ps1` first to configure environment

**Build Process:**
1. Check PIO headers (regenerate if pioasm available)
2. Configure CMake with hardware variant defines
3. Build controller firmware
4. Build node firmware (1 binary for V2, 12 binaries for V1)
5. Convert ELF → BIN → UF2 using custom elf2uf2.py
6. Copy UF2 files to FirmwareReleases/
7. **Copy node_app_16.bin to packages/ for OTA deployment** (V2 only)

**Important:** The latest node application binary is **automatically copied to packages/** directory at build time, ready for OTA deployment via `nflash`.

**Note:** For OTA-capable firmware with dual-partition support, see [build_dual.py](#build_dualpy---dual-partition-ota-build) below and the [OTA Application Build Guide](../documentation/OTA_APPLICATION_BUILD_GUIDE.md).

**Flash Tools:** After building, use [flash_node.py](#flash_nodepy---flash-node-firmware) and [flash_controller.py](#flash_controllerpy---flash-controller-firmware) to program devices via USB.

**Example Output:**
```
======================================================================
Z1 Onyx Cluster - Build Script
======================================================================

Building: V1 (12-node) + V2 (16-node)

Step 0: Checking build environment...
   PICO_SDK_PATH: C:\Pico\pico-sdk
   Found cmake: C:\Program Files\CMake\bin\cmake.exe
   Found ninja: C:\tools\ninja.exe

Step 1: Checking for PIO headers...
   Found pioasm: <project_root>\build_tools\pioasm.exe
   PIO headers generated

======================================================================
Building V1 Hardware
======================================================================

Step 2: Configuring build for V1...
   CMake configuration complete
Step 3: Building V1 firmware...
   V1 build complete
Step 4: Copying V1 firmware to releases...
   controller_12.uf2 (119.0 KB)
   node_12_0.uf2 (86.0 KB)
   ...

======================================================================
Building V2 Hardware
======================================================================

Step 2: Configuring build for V2...
   CMake configuration complete
Step 3: Building V2 firmware...
   V2 build complete
Step 4: Copying V2 firmware to releases...
   controller_16.uf2 (343.0 KB)
   node_16.uf2 (93.0 KB)
   bootloader_16.uf2 (89.5 KB)
   node_app_16.uf2 (89.5 KB)
Step 5: Creating dual-partition firmware...
   node_dual_16.uf2 (178.5 KB)
Step 6: Copying node app binary to packages...
   node_app_16.bin → packages/ (44.3 KB)

======================================================================
Build completed successfully!
======================================================================

Firmware ready:

V1 Hardware: FirmwareReleases/12node/
  controller_12.uf2 (119.0 KB)
  node_12_0.uf2 (86.0 KB)
  ...

V2 Hardware: FirmwareReleases/16node/
  bootloader_16.uf2 (89.5 KB)
  controller_16.uf2 (343.0 KB)
  node_16.uf2 (93.0 KB)
  node_app_16.uf2 (89.5 KB)
  node_dual_16.uf2 (178.5 KB)

OTA Package: packages/node_app_16.bin
  node_app_16.bin (44.3 KB) - Ready for OTA deployment
```

---

### **build_dual.py** - Dual-Partition OTA Build
**Location:** Project root  
**Purpose:** Build bootloader + application for OTA-capable firmware (V2 hardware only)  
**Status:** ✅ Production Ready

**Usage:**
### **build_dual.py** - Dual-Partition OTA Build
**Location:** Project root  
**Purpose:** Build bootloader + application for OTA-capable firmware  
**Status:** ✅ Production Ready

**Usage:**
```bash
# Build BOTH V1 and V2 dual-partition firmware (default)
python build_dual.py

# Build ONLY V1 dual-partition firmware (12 nodes)
python build_dual.py --hw-v1

# Build ONLY V2 dual-partition firmware (16 nodes)
python build_dual.py --hw-v2
```

**Flags:**
- `--hw-v1` - Build ONLY V1 hardware (12 nodes)
  - Type: boolean flag
  - Default: Builds BOTH if neither flag specified
  - Creates: bootloader_12.uf2 + node_dual_12_0.uf2 through node_dual_12_11.uf2

- `--hw-v2` - Build ONLY V2 hardware (16 nodes)
  - Type: boolean flag
  - Default: Builds BOTH if neither flag specified
  - Creates: bootloader_16.uf2 + node_dual_16.uf2

**Outputs:**
- **Build directory:** `build/`
  - bootloader_16.elf, bootloader_16.bin, bootloader_16.uf2 (V2)
  - bootloader_12.elf, bootloader_12.bin, bootloader_12.uf2 (V1)
  - node_app_16.elf, node_app_16.bin (V2, with 192-byte app_header_t)
  - node_app_12_N.elf, node_app_12_N.bin (V1, N=0-11, with headers)
  
- **Release directory:** `FirmwareReleases/16node/` and/or `FirmwareReleases/12node/`
  - V2: bootloader_16.uf2, node_app_16.uf2, node_dual_16.uf2
  - V1: bootloader_12.uf2, node_app_12_N.uf2, node_dual_12_N.uf2 (N=0-11)
  
- **Packages directory:** `packages/` (OTA-ready binaries)
  - V2: node_app_16.bin (automatically copied for OTA deployment)
  - V1: node_app_12_0.bin through node_app_12_11.bin (automatically copied)

**Partition Layout:**
```
0x00000000 - 0x0007FFFF  Bootloader (512 KB)
0x00080000 - 0x007FFFFF  Application  (7.5 MB)
```

**App Header Structure (192 bytes):**
```c
typedef struct {
    uint32_t magic;          // 0x5A314150 ("Z1AP")
    uint32_t version_major;
    uint32_t version_minor;
    uint32_t version_patch;
    uint32_t binary_size;
    uint32_t crc32;
    uint32_t entry_offset;   // 0x000000C0 (192 bytes)
    uint32_t reserved[2];
    char name[32];
    char description[64];
    char author[32];
    uint32_t timestamp;
    uint32_t padding[5];
} app_header_t;
```

**Build Process:**
1. Build bootloader firmware (per hardware version)
2. Convert bootloader to UF2
3. Build application firmware (per hardware version)
4. Prepend 192-byte app header to application binary
5. Combine bootloader + app into dual-partition UF2(s)
6. Copy all artifacts to FirmwareReleases/
7. **Copy node_app binaries to packages/ for OTA deployment**

**Use Cases:**
- **First-time flash:** Use `node_dual_16.uf2` (V2) or `node_dual_12_N.uf2` (V1) via USB
- **OTA updates:** Use `node_app_16.bin` (V2) or `node_app_12_N.bin` (V1) via nflash
- **Bootloader recovery:** Use `bootloader_16.uf2` or `bootloader_12.uf2`

**Important Notes:**
- The latest node application binaries are **automatically copied to packages/** directory at build time, ready for OTA deployment via `nflash`.
- See [OTA Troubleshooting Guide](../documentation/OTA_TROUBLESHOOTING_GUIDE.md) for complete details.

**Example Output:**
```
======================================================================
Z1 Onyx Dual Partition Build
======================================================================

Building: V1 (12-node) + V2 (16-node)

Step 0: Checking build environment...
  [OK] PICO_SDK_PATH: C:\Pico\pico-sdk
  [OK] Found cmake: C:\Program Files\CMake\bin\cmake.exe
  [OK] Found ninja: C:\tools\ninja.exe

Step 1: Checking for PIO headers...
  [OK] Found pioasm: <project_root>\build_tools\pioasm.exe

======================================================================
Building V1 Hardware
======================================================================

Step 2: Configuring build for V1...
  [OK] V1 CMake configuration complete
Step 3: Building V1 bootloader and application...
  [OK] V1 build complete
Step 4: Adding V1 app headers...
  [OK] V1 app headers added
Step 5: Creating V1 dual-partition UF2...
  [OK] Created node_dual_12_0.uf2
  [OK] Created node_dual_12_1.uf2
  ...
Step 6: Copying V1 individual binaries...
  [OK] bootloader_12.uf2 (89.5 KB)
  [OK] Copied 12 node_app_12_N.bin files → packages/

======================================================================
Building V2 Hardware
======================================================================

Step 2: Configuring build for V2...
  [OK] V2 CMake configuration complete
Step 3: Building V2 bootloader and application...
  [OK] V2 build complete
Step 4: Adding V2 app headers...
  [OK] V2 app headers added
Step 5: Creating V2 dual-partition UF2...
  [OK] Created node_dual_16.uf2
Step 6: Copying V2 individual binaries...
  [OK] bootloader_16.uf2 (89.5 KB)
  [OK] node_app_16.uf2 (89.5 KB)
  [OK] node_app_16.bin → packages/ (44.3 KB)

======================================================================
Dual Partition Build Complete!
======================================================================

Firmware ready:

V1 Hardware: FirmwareReleases/12node/
Dual-Partition Files (OTA-capable):
  node_dual_12_0.uf2 (178.5 KB) <-- FLASH THIS
  node_dual_12_1.uf2 (178.5 KB) <-- FLASH THIS
  ...

V2 Hardware: FirmwareReleases/16node/
Dual-Partition Files (OTA-capable):
  node_dual_16.uf2 (178.5 KB) <-- FLASH THIS

Individual Components:
  bootloader_16.uf2 (89.5 KB) - Bootloader only
  node_app_16.uf2 (89.5 KB) - App only

OTA Packages: packages/
  node_app_12_0.bin (44.3 KB) - Ready for OTA deployment
  node_app_12_1.bin (44.3 KB) - Ready for OTA deployment
  ...
  node_app_16.bin (44.3 KB) - Ready for OTA deployment

To flash:
  V1: Drag node_dual_12_N.uf2 to BOOTSEL drive (N = node ID 0-11)
  V2: Drag node_dual_16.uf2 to BOOTSEL drive
```

---

## Flash Tools

### **flash_node.py** - Flash Node Firmware
**Location:** Project root  
**Purpose:** Flash node firmware to Pico via USB (BOOTSEL mode)  
**Status:** ✅ Production Ready

**Usage:**
```bash
# Flash V2 node (16-node, default)
python flash_node.py

# Flash V2 node (explicit)
python flash_node.py --hw-v2

# Flash V1 node (12-node, requires node ID)
python flash_node.py --hw-v1 --node 0
python flash_node.py --hw-v1 --node 5
```

**Flags:**
- `--hw-v1` - Use V1 hardware (12-node)
  - Type: boolean flag
  - Default: False
  - Requires: `--node <id>` flag (0-11)
  - Firmware: FirmwareReleases/12node/node_12_N.uf2

- `--hw-v2` - Use V2 hardware (16-node)
  - Type: boolean flag
  - Default: True (if neither flag specified)
  - Firmware: FirmwareReleases/16node/node_dual_16.uf2 (bootloader + app)
  - Node ID auto-detected via GPIO pins

- `--node <id>` - Node ID for V1 hardware (0-11)
  - Type: integer (0-11)
  - Required for: V1 hardware only
  - Example: `--node 3` (flashes node_12_3.uf2)

**Flash Process:**
1. Find picotool (PATH or build_tools/)
2. Locate firmware file (FirmwareReleases/16node/ or 12node/)
3. Reboot device to BOOTSEL mode (`picotool reboot -f -u`)
4. Wait 3 seconds for device enumeration
5. Load firmware and execute (`picotool load <file> -x`)

**Requirements:**
- picotool in PATH or build_tools/picotool.exe
- Pico connected via USB
- Device will auto-reboot to BOOTSEL (or manually: hold BOOTSEL + plug USB)

**Example Output:**
```
============================================================
Z1 Onyx Node Firmware Flash Utility
Hardware: V2 (16-node)
============================================================

[OK] Found picotool: D:\Code\build_tools\picotool.exe
[OK] Firmware: FirmwareReleases\16node\node_dual_16.uf2

Step 1: Rebooting device to BOOTSEL mode...
Waiting for device to enter BOOTSEL mode...

Step 2: Loading firmware...

============================================================
SUCCESS! Node firmware flashed and running.
============================================================
```

---

### **flash_controller.py** - Flash Controller Firmware
**Location:** Project root  
**Purpose:** Flash controller firmware to Pico via USB (BOOTSEL mode)  
**Status:** ✅ Production Ready

**Usage:**
```bash
# Flash V2 controller (16-node, default)
python flash_controller.py

# Flash V2 controller (explicit)
python flash_controller.py --hw-v2

# Flash V1 controller (12-node)
python flash_controller.py --hw-v1
```

**Flags:**
- `--hw-v1` - Use V1 hardware (12-node)
  - Type: boolean flag
  - Default: False
  - Firmware: FirmwareReleases/12node/controller_12.uf2

- `--hw-v2` - Use V2 hardware (16-node)
  - Type: boolean flag
  - Default: True (if neither flag specified)
  - Firmware: FirmwareReleases/16node/controller_16.uf2

**Flash Process:**
1. Find picotool (PATH or build_tools/)
2. Locate firmware file (FirmwareReleases/16node/ or 12node/)
3. Reboot device to BOOTSEL mode (`picotool reboot -f -u`)
4. Wait 3 seconds for device enumeration
5. Load firmware and execute (`picotool load <file> -x`)

**Requirements:**
- picotool in PATH or build_tools/picotool.exe
- Pico connected via USB
- Device will auto-reboot to BOOTSEL (or manually: hold BOOTSEL + plug USB)

**Example Output:**
```
============================================================
Z1 Onyx Controller Firmware Flash Utility
Hardware: V2 (16-node)
============================================================

[OK] Found picotool: D:\Code\build_tools\picotool.exe
[OK] Firmware: FirmwareReleases\16node\controller_16.uf2

Step 1: Rebooting device to BOOTSEL mode...
Waiting for device to enter BOOTSEL mode...

Step 2: Loading firmware...

============================================================
SUCCESS! Controller firmware flashed and running.
============================================================
```

**Note:** For OTA firmware updates over the network (without USB), use [nflash](#nflash---flash-node-firmware-ota) instead.

---

## Core Management Tools

### **nls** - List Cluster Nodes
**Purpose:** Discover and display all nodes in the cluster  
**Status:** ✅ Production Ready

**Usage:**
```bash
# List all nodes from cluster config
python python_tools/bin/nls

# Use custom controller IP (single backplane)
python python_tools/bin/nls -c 192.168.1.201

# Verbose output with detailed info
python python_tools/bin/nls -v

# JSON output for scripting
python python_tools/bin/nls -j

# List nodes from specific backplane
python python_tools/bin/nls --backplane backplane-0

# List all nodes from all backplanes
python python_tools/bin/nls --all
```

**Flags:**
- `-c, --controller <IP>` - Controller IP address (single backplane mode)
  - Type: string
  - Default: None (uses cluster config)
  
- `--config <FILE>` - Cluster configuration file
  - Type: string
  - Default: None
  
- `--backplane <NAME>` - List nodes from specific backplane
  - Type: string
  - Default: None
  
- `--all` - List all nodes from all configured backplanes
  - Type: boolean
  - Default: False
  
- `-v, --verbose` - Show detailed node information
  - Type: boolean
  - Default: False
  
- `-j, --json` - Output in JSON format
  - Type: boolean
  - Default: False
  
- `--parallel` - Query backplanes in parallel (faster for multi-backplane)
  - Type: boolean
  - Default: False

**Output Example:**
```
Z1 Cluster Nodes - 2026-01-17 19:20:45
Backplane: default (192.168.1.201)
====================================================

NODE  STATUS    MEMORY      UPTIME      LED (R/G/B)
----------------------------------------------------
   0  online    8.00 MB        30m 45s    0/255/  0
   1  online    8.00 MB        30m 30s    0/255/  0
   2  online    8.00 MB        58m 22s    0/255/  0
  ...
  15  online    8.00 MB        58m 22s    0/255/  0

Total: 16 nodes (16 online, 0 offline)
```

---

### **nstat** - Cluster Statistics
**Purpose:** Display comprehensive cluster status and SNN statistics  
**Status:** ✅ Production Ready

**Usage:**
```bash
# Basic cluster status
python python_tools/bin/nstat -c 192.168.1.201

# SNN statistics
python python_tools/bin/nstat -s -c 192.168.1.201

# Live monitoring (refresh every 1 second)
python python_tools/bin/nstat -w 1 -c 192.168.1.201

# Live monitoring with SNN stats
python python_tools/bin/nstat -w 2 -s -c 192.168.1.201
```

**Flags:**
- `-c, --controller <IP>` - Controller IP address
  - Type: string
  - Default: None (uses cluster config)
  
- `-w, --watch <SECONDS>` - Refresh every N seconds
  - Type: integer
  - Default: None (single execution)
  
- `-s, --snn` - Show SNN activity statistics
  - Type: boolean
  - Default: False

**Output Example:**
```
Z1 Cluster Status - 2026-01-17 19:16:26
================================================================================

Cluster Overview:
  Total Nodes:     16
  Active Nodes:    16
  Inactive Nodes:  0
  Total Memory:    128.00 MB

SNN Status:
  State:           running
  Neurons:         5
  Active Neurons:  3
  Total Spikes:    800
  Spike Rate:      120.00 Hz
```

---

### **nsnn** - SNN Network Management
**Purpose:** Deploy, start, stop, and manage spiking neural networks  
**Status:** ✅ Production Ready (Enhanced)

**Commands:**
- `deploy <topology.json>` - Deploy SNN topology to cluster
- `status` - Show deployment status
- `start` - Start SNN execution
- `stop` - Stop SNN execution
- `monitor <duration_ms>` - Monitor execution for duration
- `inject <pattern.json>` - Inject input spikes

**Usage:**
```bash
# Deploy topology
python python_tools/bin/nsnn deploy python_tools/examples/xor_working.json -c 192.168.1.201

# Start execution
python python_tools/bin/nsnn start -c 192.168.1.201

# Monitor for 5 seconds
python python_tools/bin/nsnn monitor 5000 -c 192.168.1.201

# Inject spikes
python python_tools/bin/nsnn inject test_spikes.json -c 192.168.1.201

# Check status
python python_tools/bin/nsnn status -c 192.168.1.201

# Stop execution
python python_tools/bin/nsnn stop -c 192.168.1.201
```

**Flags:**
- `command` - Command to execute (positional, required)
  - Type: choice
  - Choices: deploy, status, start, stop, monitor, inject
  
- `argument` - Command argument (topology file, duration, pattern file)
  - Type: string (positional, optional)
  - Required: Yes for deploy, monitor, inject
  
- `-c, --controller <IP>` - Controller IP address
  - Type: string
  - Default: None (uses cluster config)
  
- `--config <FILE>` - Cluster configuration file
  - Type: string
  - Default: None
  
- `--all` - Use all configured backplanes
  - Type: boolean
  - Default: False

**Features:**
- Auto-reset before deployment (clears neuron count)
- Single-backplane fallback (works without cluster_config.json)
- Saves controller IP during deployment
- Multi-backplane deployment support

**Test Results:** Deployed XOR network (5 neurons, 6 synapses) successfully

---

### **nping** - Network Diagnostics
**Purpose:** Test network connectivity and latency to cluster nodes  
**Status:** ✅ Production Ready

**Usage:**
```bash
# Ping node 0 (4 packets default)
python python_tools/bin/nping 0 -c 192.168.1.201

# Ping all nodes
python python_tools/bin/nping all -c 192.168.1.201

# Ping node 15 with verbose output
python python_tools/bin/nping 15 -c 192.168.1.201 -v

# Custom packet count
python python_tools/bin/nping 1 -c 192.168.1.201 -n 10
```

**Flags:**
- `node` - Node ID (0-15) or "all" (positional, required)
  - Type: string/integer
  
- `-c, --controller <IP>` - Controller IP address
  - Type: string
  - Default: None (uses cluster config)
  
- `-n, --count <COUNT>` - Number of pings
  - Type: integer
  - Default: 4
  
- `-v, --verbose` - Verbose output
  - Type: boolean
  - Default: False

**Output Example:**
```
Pinging node 15... OK (time=30.20ms)
Pinging node 15... OK (time=28.81ms)
Pinging node 15... OK (time=28.43ms)
Pinging node 15... OK (time=29.10ms)

4 packets transmitted, 4 received, 0.0% packet loss
rtt min/avg/max = 28.43/29.14/30.20 ms
```

**Test Results (January 17, 2026):**
- Node 1: 0% loss, 29.79ms average RTT
- Node 15: 0% loss, 29.14ms average RTT

---

## Firmware Tools

### **nflash** - OTA Firmware Updates
**Purpose:** Flash firmware to cluster nodes over the network  
**Status:** ✅ Production Ready

**Usage:**
```bash
# Flash single node
python python_tools/bin/nflash -n 0 firmware.bin -c 192.168.1.201

# Flash multiple nodes (comma-separated)
python python_tools/bin/nflash -n 0,1,2,3 firmware.bin -c 192.168.1.201

# Flash node range
python python_tools/bin/nflash -n 0-7 firmware.bin -c 192.168.1.201

# Flash all nodes
python python_tools/bin/nflash -n all firmware.bin -c 192.168.1.201

# Flash firmware that already has app header
python python_tools/bin/nflash -n 0 firmware_with_header.bin -c 192.168.1.201 --no-header

# Custom SD card path
python python_tools/bin/nflash -n 0 firmware.bin -c 192.168.1.201 --sd-path firmware/custom.bin
```

**Flags:**
- `firmware` - Firmware binary file to flash (positional, required)
  - Type: string
  
- `-n, --nodes <SPEC>` - Target nodes
  - Type: string
  - Default: "0"
  - Format: single (0), range (0-7), list (0,1,2), or "all"
  
- `-c, --controller <IP>` - Controller IP address (required)
  - Type: string
  - Required: Yes
  
- `--no-header` - Do not add app header (firmware already has one)
  - Type: boolean
  - Default: False
  
- `--sd-path <PATH>` - Custom SD card path
  - Type: string
  - Default: firmware/<filename>

**Features:**
- OTA firmware updates via bootloader (~70 Kbps throughput)
- Multi-node simultaneous updates
- Automatic node reboot after flash completion
- Progress indication and error handling
- App header prepended automatically (unless --no-header)

**Test Results (January 17, 2026):**
- Successfully flashed nodes 0, 1, 13
- Multi-node OTA working (3 nodes simultaneously)
- Average flash time: ~5 seconds per node

---

### **nreset** - Node Reset Control
**Purpose:** Reset nodes to bootloader or application mode  
**Status:** ✅ Production Ready

**Usage:**
```bash
# Reset all nodes (software reset)
python python_tools/bin/nreset -c 192.168.1.201

# Reset specific node
python python_tools/bin/nreset -c 192.168.1.201 -n 0

# Reset controller itself
python python_tools/bin/nreset -c 192.168.1.201 -n 16

# Force hardware reset (V2 only, resets ALL nodes)
python python_tools/bin/nreset -c 192.168.1.201 --hardware

# Verbose output
python python_tools/bin/nreset -c 192.168.1.201 -n 0 -v
```

**Flags:**
- `-c, --controller <IP>` - Controller IP address
  - Type: string
  - Default: 192.168.1.222
  
- `--software` - Force software reset (watchdog reboot)
  - Type: boolean
  - Default: False
  
- `--hardware` - Force hardware reset (GPIO pin, V2 only)
  - Type: boolean
  - Default: False
  - Note: Resets ALL nodes simultaneously
  
- `-n, --node <ID>` - Reset specific node (0-15) or controller (16)
  - Type: integer
  - Default: None (all nodes)
  
- `-v, --verbose` - Verbose output
  - Type: boolean
  - Default: False

**Features:**
- Triggers watchdog reset to bootloader mode
- Node ID preserved across reset (scratch register persistence)
- Hardware reset available on V2 hardware (GPIO 33)

---

## Configuration Tools

### **nconfig** - Cluster Configuration Manager
**Purpose:** Manage multi-backplane cluster configurations  
**Status:** ✅ Working (Not currently used for single-backplane)

**Commands:**
- `init` - Create default cluster configuration
- `list` - List all configured backplanes
- `add <name> <ip>` - Add backplane to configuration
- `remove <name>` - Remove backplane from configuration
- `show <name>` - Show backplane details

**Usage:**
```bash
# Initialize default configuration
python python_tools/bin/nconfig init

# List all backplanes
python python_tools/bin/nconfig list

# Add backplane
python python_tools/bin/nconfig add bp-0 192.168.1.222

# Add backplane with options
python python_tools/bin/nconfig add bp-1 192.168.1.223 --nodes 16 --port 80

# Remove backplane
python python_tools/bin/nconfig remove bp-0

# Show backplane details
python python_tools/bin/nconfig show bp-0

# JSON output
python python_tools/bin/nconfig list -j
```

**Global Flags:**
- `--config <FILE>` - Cluster configuration file
  - Type: string
  - Default: None
  
- `-j, --json` - Output in JSON format
  - Type: boolean
  - Default: False

**init Subcommand Flags:**
- `-o, --output <FILE>` - Output file
  - Type: string
  - Default: cluster.json
  
- `--force` - Overwrite existing file
  - Type: boolean
  - Default: False

**add Subcommand Flags:**
- `name` - Backplane name (positional, required)
- `ip` - Controller IP address (positional, required)
- `--port <PORT>` - Controller port
  - Type: integer
  - Default: 80
- `--nodes <COUNT>` - Number of nodes
  - Type: integer
  - Default: 16
- `--description <TEXT>` - Description
  - Type: string

**Note:** Keep for future multi-backplane expansion. Currently not needed for single 16-node cluster.

---

### **zconfig** - Network Configuration Manager
**Purpose:** Configure controller IP address and network settings  
**Status:** ✅ Production Ready

**Usage:**
```bash
# Show current configuration
python python_tools/bin/zconfig --show -c 192.168.1.201

# Change IP address and reboot
python python_tools/bin/zconfig --ip 192.168.1.100 --reboot -c 192.168.1.201

# Change MAC suffix and reboot
python python_tools/bin/zconfig --mac C3:D4:05 --reboot -c 192.168.1.201

# Change both IP and MAC
python python_tools/bin/zconfig --ip 192.168.1.100 --mac C3:D4:05 --reboot -c 192.168.1.201

# Set engine type (no reboot)
python python_tools/bin/zconfig --engine iaf -c 192.168.1.201

# Change IP with custom wait time
python python_tools/bin/zconfig --ip 192.168.1.100 --reboot --wait 10 -c 192.168.1.201
```

**Flags:**
- `-c, --controller <IP>` - Controller IP address
  - Type: string
  - Default: 192.168.1.222
  
- `--ip <IP>` - New IP address
  - Type: string
  - Example: 192.168.1.100
  
- `--mac <MAC>` - New MAC suffix (last 3 bytes only)
  - Type: string
  - Format: XX:XX:XX (e.g., C3:D4:05)
  - Note: Prefix 02:5A:31 is fixed (locally administered)
  
- `--engine <TYPE>` - SNN engine type
  - Type: string
  - Choices: iaf, lif, adaptive
  
- `--reboot` - Reboot after config change
  - Type: boolean
  - Default: False
  
- `--show` - Show current config and exit
  - Type: boolean
  - Default: False
  
- `--wait <SECONDS>` - Seconds to wait after reboot
  - Type: integer
  - Default: 5

**Features:**
- Query current network configuration
- Update IP address settings
- Modify MAC address (last 3 bytes only)
- Set SNN engine type
- Optional automatic reboot

**Note:** MAC address prefix 02:5A:31 is fixed. Only the last 3 bytes can be configured.

---

### **zengine** - Topology File Manager
**Purpose:** Validate and manage SNN topology files on SD card  
**Status:** ✅ Production Ready

**Commands:**
- `list` - List all topology files on SD card
- `upload <file>` - Upload topology file to SD card
- `download <file>` - Download topology file from SD card
- `delete <file>` - Delete topology file from SD card

**Usage:**
```bash
# List all topology files
python python_tools/bin/zengine list -c 192.168.1.201

# Upload topology file
python python_tools/bin/zengine upload xor_working.json -c 192.168.1.201

# Upload with custom name
python python_tools/bin/zengine upload xor.json --name xor_network.json -c 192.168.1.201

# Download topology file
python python_tools/bin/zengine download xor_working.json -c 192.168.1.201

# Delete topology file
python python_tools/bin/zengine delete old_topology.json -c 192.168.1.201
```

**Flags:**
- `action` - Action to perform (positional, required)
  - Type: choice
  - Choices: list, upload, download, delete
  
- `file` - Local file path (upload) or remote filename (download/delete)
  - Type: string (positional, optional)
  - Required: Yes for upload, download, delete
  
- `-c, --controller <IP>` - Controller IP address
  - Type: string
  - Default: 192.168.1.222
  
- `--name <NAME>` - Remote filename (for upload)
  - Type: string
  - Default: None (uses local filename)

**Features:**
- Topology file management on controller SD card
- Automatic topologies/ directory usage
- File upload/download/deletion
- List all stored topologies

**Note:** All operations automatically use topologies/ directory on SD card.

---

## Testing Tools

### **test_deployment.py** - Comprehensive Deployment Test
**Purpose:** Full cluster deployment validation  
**Status:** ✅ Production Ready

**Usage:**
```bash
# Basic test with default topology
python python_tools/bin/test_deployment.py -c 192.168.1.201

# Custom topology
python python_tools/bin/test_deployment.py -c 192.168.1.201 -t examples/mnist.json

# More spikes
python python_tools/bin/test_deployment.py -c 192.168.1.201 -s 500

# Quiet mode (hide command output)
python python_tools/bin/test_deployment.py -c 192.168.1.201 -q
```

**Flags:**
- `-c, --controller <IP>` - Controller IP address
  - Type: string
  - Default: 192.168.1.201
  
- `-t, --topology <FILE>` - Topology file (relative to python_tools/)
  - Type: string
  - Default: examples/xor_working.json
  
- `-s, --spikes <COUNT>` - Number of spikes to inject
  - Type: integer
  - Default: 200
  
- `-q, --quiet` - Quiet mode (hide command output)
  - Type: boolean
  - Default: False

**Test Sequence:**
1. Node discovery (nls)
2. Topology deployment (nsnn deploy)
3. Node status (nstat)
4. Start SNN (nsnn start)
5. Inject spikes (nsnn inject)
6. SNN status (nsnn status)
7. Statistics (nstat -s)
8. Stop SNN (nsnn stop)
9. SD card check (optional)

**Test Results (January 17, 2026):** ALL 9 TESTS PASSED ✅

---

### **test_reboot.py** - Reboot API Test
**Purpose:** Test reboot endpoint functionality  
**Status:** ✅ Production Ready

**Usage:**
```bash
python python_tools/bin/test_reboot.py
```

**Arguments:** None (hardcoded to test 192.168.1.201)

**Tests:**
1. GET /api/config (pre-reboot)
2. POST /api/system/reboot

**Test Results:**
- Config read: IP=192.168.1.201, MAC=02:5A:31:03:02:01
- Reboot endpoint responding correctly
- All tests passed (2/2)

**Note:** Does not verify actual reboot behavior, only API responses.

---

### **test_sd_card.py** - SD Card API Test
**Purpose:** Test SD card functionality via HTTP API  
**Status:** ✅ Production Ready

**Usage:**
```bash
# Test default IP (192.168.1.201)
python python_tools/bin/test_sd_card.py

# Test custom IP
python python_tools/bin/test_sd_card.py 192.168.1.222
```

**Arguments:**
- `controller_ip` - Controller IP address (positional, optional)
  - Type: string
  - Default: 192.168.1.201

**Test Sequence:**
1. SD card status
2. Configuration file read
3. Configuration file write
4. File upload
5. File list
6. File delete

**Test Results (January 17, 2026):**
- SD card mounted: 30983 MB free
- All 6 tests passed ✅

---

## Deprecated Tools

**Location:** `python_tools/deprecated/`

### **ncat** - Display Node Memory ❌
**Deprecated:** January 17, 2026  
**Reason:** Controller firmware lacks READ_MEMORY handler  
**Status:** Unusable

**Why Deprecated:**
- Requires GET /nodes/{id}/memory endpoint
- Endpoint not implemented in controller firmware
- Returns 0 bytes on all requests
- No use case for reading raw neuron table memory

**Migration:** Use `nstat -s` for neuron statistics instead

---

### **ncp** - Copy Files to PSRAM ❌
**Deprecated:** January 17, 2026  
**Reason:** Superseded by `nsnn deploy`  
**Status:** Tool works, but workflow obsolete

**Why Deprecated:**
- Originally for manual PSRAM writes
- `nsnn deploy` now handles topology → PSRAM deployment automatically
- No use case outside of topology deployment
- Requires manual address calculation (error-prone)

**Migration:** Use `nsnn deploy topology.json` for all neuron table deployment

---

### **nsnn.bak** - Backup Copy ❌
**Deprecated:** January 17, 2026  
**Reason:** Old backup file from development  
**Status:** Historical artifact

**Migration:** Use `nsnn` (active version)

---

## Quick Reference

### Tool Comparison Matrix

| Tool | Purpose | Key Flags | Default Controller | Status |
|------|---------|-----------|-------------------|--------|
| **build.py** | Build firmware | --hw-v1, --hw-v2 | N/A | ✅ Essential |
| **build_dual.py** | Build OTA firmware | None | N/A | ✅ Essential |
| **nls** | List nodes | -c, -v, -j, --all | cluster config | ✅ Active |
| **nstat** | Cluster status | -c, -w, -s | cluster config | ✅ Active |
| **nsnn** | SNN management | command, -c, --all | cluster config | ✅ Active |
| **nping** | Node ping | node, -c, -n, -v | cluster config | ✅ Active |
| **nflash** | Firmware update | -n, firmware, -c | **required** | ✅ Active |
| **nreset** | Node reset | -c, -n, --hardware | 192.168.1.222 | ✅ Active |
| **nconfig** | Cluster config | command, --config | N/A | ○ Keep |
| **zconfig** | Network config | --ip, --mac, --reboot | 192.168.1.222 | ✅ Active |
| **zengine** | File manager | action, file, -c | 192.168.1.222 | ✅ Active |
| **z1pack** | Package creator | -i, -o, --inspect | N/A | ○ Keep |
| **test_deployment** | Full test | -c, -t, -s, -q | 192.168.1.201 | ✅ Active |
| **test_reboot** | Reboot test | (none) | 192.168.1.201 | ✅ Active |
| **test_sd_card** | SD test | controller_ip | 192.168.1.201 | ✅ Active |

---

## Common Workflows

### Initial Setup

```bash
# 1. Setup build environment
.\setup_build_env.ps1

# 2. Build firmware
python build.py                        # Standard build (V2)
python build_dual.py                   # OTA-capable build (V2)

# 3. Flash bootloader + app (first time)
# Copy FirmwareReleases/16node/node_dual.uf2 to Pico in bootloader mode

# 4. Flash controller
# Copy FirmwareReleases/16node/controller_16.uf2 to controller Pico
```

### Daily Development

```bash
# 1. Discover cluster
python python_tools/bin/nls -c 192.168.1.201

# 2. Check cluster health
python python_tools/bin/nstat -c 192.168.1.201

# 3. Deploy topology
python python_tools/bin/nsnn deploy python_tools/examples/xor_working.json -c 192.168.1.201

# 4. Run test suite
python python_tools/bin/test_deployment.py -c 192.168.1.201
```

### Firmware Update (OTA)

```bash
# 1. Build new firmware
python build_dual.py

# 2. Flash nodes via OTA
python python_tools/bin/nflash -n all FirmwareReleases/16node/node_app.bin -c 192.168.1.201

# 3. Verify nodes came back online
python python_tools/bin/nls -c 192.168.1.201
```

### Network Configuration

```bash
# 1. Show current config
python python_tools/bin/zconfig --show -c 192.168.1.201

# 2. Change IP address
python python_tools/bin/zconfig --ip 192.168.1.100 --reboot -c 192.168.1.201

# 3. Wait for reboot (5 seconds)

# 4. Verify new IP
python python_tools/bin/zconfig --show -c 192.168.1.100
```

### Multi-Node SNN Deployment

```bash
# 1. Deploy topology across cluster
python python_tools/bin/nsnn deploy python_tools/examples/xor_working.json -c 192.168.1.201

# 2. Start execution
python python_tools/bin/nsnn start -c 192.168.1.201

# 3. Inject input pattern
python python_tools/bin/nsnn inject test_spikes.json -c 192.168.1.201

# 4. Monitor execution (5 seconds)
python python_tools/bin/nsnn monitor 5000 -c 192.168.1.201

# 5. Check statistics
python python_tools/bin/nstat -s -c 192.168.1.201

# 6. Stop execution
python python_tools/bin/nsnn stop -c 192.168.1.201
```

### Network Diagnostics

```bash
# 1. Ping specific node
python python_tools/bin/nping 0 -c 192.168.1.201 -v

# 2. Ping all nodes
python python_tools/bin/nping all -c 192.168.1.201

# 3. Extended ping test (10 packets)
python python_tools/bin/nping 15 -c 192.168.1.201 -n 10

# 4. Check cluster status
python python_tools/bin/nstat -c 192.168.1.201
```

### SD Card Management

```bash
# 1. Test SD card functionality
python python_tools/bin/test_sd_card.py 192.168.1.201

# 2. Upload topology to SD card
python python_tools/bin/zengine upload xor_working.json -c 192.168.1.201

# 3. List files on SD card
python python_tools/bin/zengine list -c 192.168.1.201

# 4. Download topology from SD card
python python_tools/bin/zengine download xor_working.json -c 192.168.1.201

# 5. Delete old topology
python python_tools/bin/zengine delete old_topology.json -c 192.168.1.201
```

---

## Environment Variables

Most tools respect these environment variables (via ClusterConfig):

- `Z1_CONTROLLER_IP` - Default controller IP
- `Z1_CONFIG_FILE` - Default cluster config file path

Tools with hardcoded defaults (can be overridden with `-c`):
- nreset: 192.168.1.222
- zconfig: 192.168.1.222
- zengine: 192.168.1.222
- test_deployment: 192.168.1.201
- test_reboot: 192.168.1.201 (hardcoded, no override)
- test_sd_card: 192.168.1.201

---

## Build Requirements

### Environment Setup

**Required Environment Variables:**
- `PICO_SDK_PATH` - Path to Pico SDK installation

**Required Tools in PATH:**
- `arm-none-eabi-gcc` - ARM GCC compiler toolchain
- `cmake` - Build system (version 3.13+)
- `ninja` - Build tool (faster than make)
- `python` - Python 3.7+ for build scripts

**Optional Tools:**
- `pioasm` - PIO assembler (for regenerating PIO headers)
- `picotool` - Pico toolchain utilities

### Quick Setup (Windows)

```powershell
# Run environment setup script
.\setup_build_env.ps1

# Verify environment
cmake --version
arm-none-eabi-gcc --version
ninja --version
python --version
```

### Build Directory Structure

```
build/                          # All build artifacts
├── controller_16.elf
├── controller_16.bin
├── controller_16.hex
├── controller_16.dis
├── node_16.elf
├── node_16.bin
├── ...

FirmwareReleases/
├── 16node/                     # V2 (16-node) releases
│   ├── controller_16.uf2
│   ├── node_16.uf2
│   ├── bootloader.uf2          # (dual-partition build)
│   ├── node_app.bin            # (dual-partition build)
│   └── node_dual.uf2           # (dual-partition build)
└── 12node/                     # V1 (12-node) releases
    ├── controller_12.uf2
    ├── node_12_0.uf2
    ├── node_12_1.uf2
    └── ...
```

---

## Troubleshooting

### Build Issues

**"PICO_SDK_PATH not set"**
- Run `.\setup_build_env.ps1` to configure environment
- Or manually set: `$env:PICO_SDK_PATH = "C:\Pico\pico-sdk"`

**"cmake not found"**
- Install CMake: https://cmake.org/download/
- Add to PATH or run setup script

**"arm-none-eabi-gcc not found"**
- Install ARM GCC toolchain
- Add to PATH or run setup script

**"ninja not found"**
- Install Ninja: https://ninja-build.org/
- Add to PATH or run setup script

### Network Issues

**"Cannot connect to controller"**
- Check controller IP: `ping 192.168.1.201`
- Verify controller power and Ethernet connection
- Use correct IP with `-c` flag

**"No nodes found" (nls)**
- Check node power
- Verify Matrix bus connections
- Wait 5 seconds after power-on for initialization
- Check controller serial output for enumeration

### Deployment Issues

**"Deployment failed" (nsnn deploy)**
- Validate topology JSON: `python python_tools/bin/zengine validate topology.json`
- Check node IDs in topology match physical nodes (0-15)
- Ensure sufficient PSRAM on target nodes
- Check controller serial output for errors

### OTA Issues

**"OTA flash failed" (nflash)**
- Reset node to bootloader: `python python_tools/bin/nreset <node_id> -c 192.168.1.201`
- Verify firmware file exists and is valid UF2/BIN
- Check network stability: `python python_tools/bin/nping <node_id> -c 192.168.1.201`
- Ensure node has dual-partition firmware installed

---

## See Also

- [BUILD_INSTRUCTIONS.md](../BUILD_INSTRUCTIONS.md) - Detailed build guide
- [API_REFERENCE.md](../documentation/API_REFERENCE.md) - HTTP API documentation
- [TEST_DEPLOYMENT_GUIDE.md](../documentation/TEST_DEPLOYMENT_GUIDE.md) - Testing procedures
- [MODIFICATIONS.md](MODIFICATIONS.md) - Change log from original GitHub version
- [python_tools/examples/](examples/) - Example topology files

---

**For Issues/Questions:**
- Check controller serial output for detailed errors
- Review API_REFERENCE.md for HTTP endpoint details
- Use `nstat -s` for SNN execution diagnostics
- Use `nping` for network connectivity tests

---

**Document Version:** 4.0  
**Last Updated:** January 17, 2026  
**Maintainer:** Z1 Onyx Project Team
