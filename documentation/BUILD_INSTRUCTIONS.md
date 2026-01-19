# Z1 Onyx Cluster - Build Instructions

## Prerequisites

**Platform Note**: This project includes pre-built tools for **Windows x64** only (`build_tools/` directory contains pioasm.exe, picotool.exe, libusb-1.0.dll). Linux and macOS users must build these tools from source - see Pico SDK documentation.

The build system uses standard Pico SDK tools that must be available in your system PATH:

### Required Tools

1. **Raspberry Pi Pico SDK**
   - Download from: https://github.com/raspberrypi/pico-sdk
   - Set environment variable: `PICO_SDK_PATH=C:\Pico\pico-sdk` (or your installation path)

2. **ARM GCC Toolchain**
   - Download from: https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads
   - Add to PATH: `C:\Pico\arm-none-eabi\bin`
   - Version: arm-none-eabi-gcc 13.2.1 or later

3. **CMake**
   - Download from: https://cmake.org/download/
   - Version: 3.13 or later
   - Add to PATH

4. **Ninja Build System**
   - Download from: https://ninja-build.org/
   - Add to PATH
   - Or use `make` instead (modify build.py)

5. **Python 3**
   - Version: 3.7 or later
   - Required packages:
     - `requests` - For HTTP API communication (install: `pip install requests`)
     - `pyusb` - For USB device communication (optional, for picotool)
   - For running the build scripts and Python tools

6. **Picotool**
   - Required by Pico SDK for UF2 generation
   - Must be built and installed once before building firmware
   - See "Installing Picotool" section below

### Optional Tools

- **pioasm** - For regenerating PIO headers (pre-generated headers are included)

---

## Installing Picotool

Picotool is required by the Pico SDK to generate UF2 files. You must build and install it once before building the Z1 firmware.

### Windows with Visual Studio

```powershell
# Clone picotool
cd C:\Pico
git clone https://github.com/raspberrypi/picotool.git
cd picotool
git submodule update --init

# Build with Visual Studio
mkdir build
cd build
cmake -G "Visual Studio 17 2022" -A x64 ..

# Create placeholder for missing OTP file (build system bug workaround)
New-Item -Path "rp2350_otp_contents.json" -ItemType File -Value "{}" -Force

# Build picotool
cmake --build . --config Release --target picotool

# Install to system PATH
Copy-Item "Release\picotool.exe" "C:\Pico\picotool.exe" -Force

# Add to PATH (run as admin or add via System Properties)
$env:Path += ";C:\Pico"
[System.Environment]::SetEnvironmentVariable("Path", $env:Path + ";C:\Pico", [System.EnvironmentVariableTarget]::User)
```

### Verify Installation

```powershell
picotool version
# Should show: picotool v2.x.x
```

**Note**: Picotool only needs to be built once. After installation, the Pico SDK will use it automatically for all subsequent builds.

## Quick Start

### Windows

```powershell
# 1. Set environment variable
$env:PICO_SDK_PATH = "C:\Pico\pico-sdk"

# 2. Ensure tools are in PATH
$env:PATH += ";C:\Pico\arm-none-eabi\bin;C:\Pico\ninja;C:\Program Files\CMake\bin"

# 3. Build V2 hardware (16 nodes - default)
python build.py

# Or build V1 hardware (12 nodes - legacy)
python build.py --hw-v1
```

### Linux/Mac

```bash
# 1. Set environment variable
export PICO_SDK_PATH=/path/to/pico-sdk

# 2. Ensure tools are in PATH
export PATH=$PATH:/path/to/arm-none-eabi/bin:/path/to/ninja

# 3. Build V2 hardware (16 nodes - default)
python build.py

# Or build V1 hardware (12 nodes - legacy)
python build.py --hw-v1
```

## Hardware Versions

### V2 (16-node, Current)
- **Features**: 16 nodes with auto-detection, OLED display, global node reset
- **W5500 Pins**: MISO=36, CS=37, CLK=38, MOSI=39, RST=34, INT=35
- **Output**: `controller_16` + `node_16` (single binary for all nodes)
- **Location**: `FirmwareReleases/16node/`

### V1 (12-node, Legacy)
- **Features**: 12 nodes with hardcoded IDs, no OLED, no global reset
- **W5500 Pins**: MISO=36, CS=37, CLK=38, MOSI=39, RST=35, INT=34
- **Output**: `controller_12` + `node_12_0` through `node_12_11` (individual binaries per node)
- **Location**: `FirmwareReleases/12node/`

## Manual Build

If you prefer to build manually:

### V2 Hardware (16 nodes)
```bash
mkdir build
cd build
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_HW_V1=OFF -DBUILD_HW_V2=ON ..
ninja controller_16 node_16
```

### V1 Hardware (12 nodes)
```bash
mkdir build
cd build
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_HW_V1=ON -DBUILD_HW_V2=OFF ..
ninja controller_12 node_12_0 node_12_1 node_12_2 ... node_12_11
```

Firmware files are generated in:
- Build outputs: `build/controller/` and `build/node/`
- Release ready: `FirmwareReleases/16node/` or `FirmwareReleases/12node/`

## Flashing Firmware

### V2 Hardware (16 nodes)

**UF2 Method (Recommended):**
1. Hold BOOTSEL button while plugging in the Pico
2. Drag and drop from `FirmwareReleases/16node/`:
   - `z1_controller_16.uf2` → Controller board
   - `z1_node_16.uf2` → All node boards (auto-detects ID)

**Hex Method:**
```bash
picotool load build/controller/controller_16.hex
picotool load build/node/node_16.hex
```

### V1 Hardware (12 nodes)

**UF2 Method (Recommended):**
1. Hold BOOTSEL button while plugging in the Pico
2. Flash from `FirmwareReleases/12node/`:
   - `z1_controller_12.uf2` → Controller board
   - `z1_node_12_0.uf2` → Node 0
   - `z1_node_12_1.uf2` → Node 1
   - ... (flash correct numbered binary to each node)
   - `z1_node_12_11.uf2` → Node 11

**Hex Method:**
```bash
picotool load build/controller/controller_12.hex
picotool load build/node/node_12_0.hex  # For node 0
picotool load build/node/node_12_1.hex  # For node 1
# etc.
```

## Customizing Network Configuration

The controller firmware uses a default IP address of **192.168.1.222** and MAC **02:5A:31:C3:D4:01**.

### Runtime Configuration (Recommended)

**For deployed systems**, use the `zconfig` tool to modify network settings without rebuilding firmware:

```bash
# Change IP address and reboot
python python_tools/bin/zconfig --ip 192.168.1.100 --reboot

# Change MAC address
python python_tools/bin/zconfig --mac C3:D4:05 --reboot

# View current configuration
python python_tools/bin/zconfig --show
```

**How it works:**
1. Controller reads `z1.cfg` from SD card at startup
2. If config file exists, IP/MAC are applied before W5500 initialization
3. If config file missing, defaults to hardcoded values below
4. Changes take effect immediately after reboot

**Benefits:**
- No firmware rebuild required
- Multiple controllers can share same firmware
- Easy to recover from misconfiguration (delete z1.cfg, reboot to defaults)

See [API_REFERENCE.md](API_REFERENCE.md#zconfig---configuration-management) for full `zconfig` documentation.

### Compile-Time Defaults

**Edit ONE location only:** `controller/w5500_eth.c`

These values are used when `z1.cfg` is missing or SD card is not present:

```c
// Network Configuration section (near top of file)
static uint8_t MAC_ADDRESS[6] = {0x02, 0x5A, 0x31, 0xC3, 0xD4, 0x01};  // Default MAC
static uint8_t IP_ADDRESS[4]  = {192, 168, 1, 222};                    // <-- Default IP
static const uint8_t SUBNET_MASK[4] = {255, 255, 255, 0};              // <-- Adjust if needed
static const uint8_t GATEWAY[4]     = {0, 0, 0, 0};                    // <-- Set if needed
```

**That's it!** The OLED display and HTTP responses automatically use the IP address via the `w5500_get_ip_string()` helper function.

**Note**: `MAC_ADDRESS` and `IP_ADDRESS` arrays are **not** `const` because they can be updated at runtime from `z1.cfg`.

### Python Tools Default IP

**Edit:** Command-line scripts to match your controller IP

1. **test_deployment.py** - Change default on line ~190:
   ```python
   parser.add_argument('-c', '--controller', default='192.168.1.222', ...)
   ```

2. **python_tools/lib/z1_client.py** - Change default on line ~56:
   ```python
   def __init__(self, controller_ip: str = "192.168.1.222", ...):
   ```

3. **python_tools/bin/nsnn** - Change default on line ~31:
   ```python
   controller_ip = args.controller if args.controller else '192.168.1.222'
   ```

**Note:** You can override these defaults with the `-c` or `--controller` command-line argument without editing the files.

After changing the controller IP, rebuild the firmware with `python build.py` and reflash the controller.

---

## Troubleshooting

### "PICO_SDK_PATH not found"
- Make sure the environment variable is set
- Point it to the root of your pico-sdk installation

### "ninja not found" / "cmake not found"
- Ensure all tools are in your system PATH
- On Windows, you may need to restart your terminal after installing tools

### "arm-none-eabi-gcc not found"
- Download and install the ARM GCC toolchain
- Add the `bin` directory to your PATH

### Build fails with picotool errors
- The stub picotool in `build/_deps/` works around path issues
- You can still flash using .hex or .bin files without picotool

## Project Structure

- `controller/` - Controller firmware (manages the cluster)
  - `controller_pins_v1.h` - V1 hardware pin definitions (12-node)
  - `controller_pins_v2.h` - V2 hardware pin definitions (16-node)
- `node/` - Node firmware (SNN processing units)
- `common/` - Shared libraries (bus protocol, OLED, etc.)
- `build.py` - Automated build script with hardware variant support
- `build_tools/` - Build tools (pioasm, picotool stub, elf2uf2.py)
- `FirmwareReleases/` - Ready-to-flash firmware files (UF2 only)
  - `16node/` - V2 hardware firmware
  - `12node/` - V1 hardware firmware
- `build/` - Build artifacts (ELF, hex, bin files)

## Hardware Targets

- **Board**: Raspberry Pi Pico 2 (RP2350)
- **Architecture**: ARM Cortex-M33
- **Platform**: rp2350-arm-s

---

## Dual-Partition Builds (OTA-Capable Firmware)

The Z1 Onyx cluster supports Over-The-Air (OTA) firmware updates via a dual-partition architecture. This separates the fixed bootloader from the updateable application code.

### Architecture Overview

**Flash Memory Layout:**
```
0x00000000 - 0x001FFFFF (2MB):  Bootloader Partition (NEVER updated)
  - Matrix Bus driver
  - Z1 Broker
  - PSRAM driver
  - OTA update engine
  - App validation & jump logic

0x00200000 - 0x007FFFFF (6MB):  Application Partition (OTA updateable)
  - 192-byte header (magic, version, CRC32)
  - Node firmware (bus, broker, PSRAM, SNN)
```

**XIP (Execute-In-Place) Addresses:**
```
Bootloader runs at: 0x10000000 (flash 0x00000000)
Application runs at: 0x10200000 (flash 0x00200000)
```

### Building Dual-Partition Firmware

**Use the dedicated build script:**

```bash
# Build V2 (16-node) dual-partition firmware
python build_dual.py

# Build V1 (12-node) dual-partition firmware
python build_dual.py --hw-v1
```

**Output files (in FirmwareReleases/16node/ or /12node/):**
- `node_dual_16.uf2` (V2) - **FLASH THIS** - Single file with bootloader + app
- `bootloader_16.uf2` - Bootloader only (reference)
- `node_app_16.uf2` - App only (for OTA updates)

**For V1 hardware:** 12 separate files (`node_dual_12_0.uf2` through `node_dual_12_11.uf2`) since node IDs are hardcoded.

### Application Header Requirement

**CRITICAL:** All application binaries require a 192-byte header for bootloader validation.

The build system **automatically** prepends this header using `build_tools/prepend_app_header.py`:

**Header Structure (192 bytes):**
```c
typedef struct __attribute__((packed)) {
    uint32_t magic;              // 0x5A314150 ("Z1AP")
    uint32_t version_major;      // Version numbers
    uint32_t version_minor;
    uint32_t version_patch;
    uint32_t flags;              // Reserved flags
    uint32_t binary_size;        // App size (excludes header)
    uint32_t crc32;              // CRC32 of binary only
    uint32_t entry_point;        // Should be 0xC0 (192 bytes)
    char     name[32];           // App name
    char     description[64];    // Description
    uint8_t  reserved[64];       // Reserved for future use
} app_header_t;  // Total: 192 bytes
```

**The header is added automatically during:**
1. Dual-partition builds (`build_dual.py`) - for initial deployment
2. OTA package creation (`z1pack`) - for OTA updates

**You do NOT need to add the header manually in your code.**

### Bootloader Validation Process

When a node boots with dual-partition firmware:

1. **Bootloader starts** at 0x10000000
2. **Reads header** at 0x10200000 (192 bytes)
3. **Validates:**
   - Magic number = 0x5A314150
   - Binary size < 6MB
   - Entry point = 0xC0
   - CRC32 matches binary content
4. **If valid:** Sets VTOR, loads stack pointer, jumps to app at 0x102000C0
5. **If invalid:** Enters safe mode (red LED blinking 1Hz), waits for OTA update

### Creating OTA Update Packages

When you want to update deployed nodes:

**CRITICAL:** The 192-byte header MUST be in the .z1app package BEFORE uploading to the controller. The OTA engine writes the package byte-for-byte to flash - it does NOT add the header during transfer. The header must be present so the bootloader can validate it on reboot.

**1. Build the new app firmware:**
```bash
python build_dual.py  # Creates node_app_16.bin (without header)
```

**2. Package for OTA using z1pack** (adds header automatically):
```bash
# From raw binary (header will be added)
python python_tools/bin/z1pack \
    -i build/node/node_app_16.bin \
    -o packages/node_v1.1.0.z1app \
    --name "Z1 Node App" \
    --version "1.1.0"

# OR use the version with header already added by build_dual.py
python python_tools/bin/z1pack \
    -i build/node/node_app_16_header.bin \
    -o packages/node_v1.1.0.z1app \
    --name "Z1 Node App" \
    --version "1.1.0"
```

**Note:** `z1pack` detects if the header is already present (checks for magic 0x5A314150) and won't duplicate it.

**3. Deploy via OTA:**
```bash
# Upload to controller SD card
python python_tools/bin/zengine upload packages/node_v1.1.0.z1app -c 192.168.1.222

# Deploy to all nodes (controller distributes to each node)
curl -X POST http://192.168.1.222/api/firmware/deploy \
  -H "Content-Type: application/json" \
  -d '{"filename":"engines/node_v1.1.0.z1app","nodes":[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15]}'
```

**What the OTA engine does:**
1. Receives .z1app package in 2KB chunks → stores in PSRAM
2. Validates header (magic, size, CRC32)
3. Writes ENTIRE package (header + binary) to flash at 0x00200000
4. Does NOT modify or add anything during transfer

**What happens on reboot:**
1. Bootloader reads header at flash 0x00200000
2. Validates magic, CRC32, size
3. Jumps to code at 0x002000C0 (192 bytes after header start)

See [OTA_UPDATE_SPEC.md](OTA_UPDATE_SPEC.md) for complete OTA protocol details.

### XIP Base Address Configuration

**Why it matters:** RP2350 executes code directly from flash (XIP). Code linked for 0x10000000 will crash if jumped to at 0x10200000.

**Solution:** The build system sets `PICO_FLASH_XIP_BASE=0x102000C0` for the app partition:

```cmake
# node/CMakeLists.txt - automatically configured
target_compile_definitions(node_app_16 PRIVATE
    PICO_FLASH_XIP_BASE=0x102000C0  # App runs here, not 0x10000000
)
```

This ensures all code addresses, vector tables, and function pointers are correct for execution at the app partition address.

### Flashing Dual-Partition Firmware

**Initial deployment:**
1. Hold BOOTSEL on node board
2. Drag `node_dual_16.uf2` to Pico drive
3. Node boots into bootloader → validates app → runs app

**The single UF2 file programs both partitions:**
- Bootloader at flash 0x00000000
- App (with header) at flash 0x00200000

**After OTA update:**
- Only app partition (0x00200000) is rewritten
- Bootloader (0x00000000) remains unchanged
- Node reboots, bootloader validates new app, jumps to it

### Build System Details

**Dual-partition build steps:**
1. Configure CMake with `BUILD_HW_V2=ON` or `BUILD_HW_V1=ON`
2. Build bootloader_16 and node_app_16 targets
3. Run `prepend_app_header.py` to add 192-byte header to app binary
4. Run `merge_dual_partition.py` to create single UF2 with both partitions
5. Copy UF2 files to `FirmwareReleases/16node/`

**Key scripts:**
- `build_dual.py` - Orchestrates dual-partition build
- `build_tools/prepend_app_header.py` - Adds 192-byte header to app binary
- `build_tools/merge_dual_partition.py` - Merges bootloader + app into single UF2
- `build_tools/elf2uf2.py` - Converts ELF/BIN to UF2 format

**All scripts are portable** - no hardcoded paths, use environment variables only.

### Dual-Partition vs Monolithic

**Use dual-partition when:**
- You need OTA updates in production
- Nodes are deployed in hard-to-reach locations
- You want safe recovery from bad firmware

**Use monolithic when:**
- Prototyping or development
- Easy physical access for reflashing
- Slightly smaller binary size needed (no bootloader overhead)

**Binary sizes:**
- Monolithic: ~87KB (node_16.uf2)
- Dual-partition: ~167KB (bootloader 45KB + app 87KB + header 0.2KB)
- Bootloader overhead: ~80KB (one-time, never updated)

### Optimizing Partition Sizes (Optional)

**Current allocation (default):**
- Bootloader: 2MB (flash 0x00000000-0x001FFFFF) - **only 45KB used (2.2%)**
- App: 6MB (flash 0x00200000-0x007FFFFF) - **only 87.5KB used (1.4%)**

**Recommended optimization: 256KB bootloader**
- Bootloader: 256KB (flash 0x00000000-0x0003FFFF) - **45KB = 17.6% used**
- App: 15.75MB (flash 0x00040000-0x00FFFFFF) - **2.6x more space**

This gives maximum space for app firmware while keeping plenty of headroom for bootloader growth.

**Changes required (4 files):**

**1. Update bootloader partition address:**
```bash
# File: bootloader/bootloader_main.c (line ~42)
# Change:
#define APP_PARTITION_START 0x10200000  // Old: 6MB app at 2MB offset

# To:
#define APP_PARTITION_START 0x10040000  // New: 15.75MB app at 256KB offset
```

**2. Update app XIP base address:**
```bash
# File: node/CMakeLists.txt (line ~55)
# Change:
PICO_FLASH_XIP_BASE=0x102000C0  # Old: App at 2MB + 192 bytes

# To:
PICO_FLASH_XIP_BASE=0x100400C0  # New: App at 256KB + 192 bytes
```

**3. Update merge script:**
```bash
# File: build_tools/merge_dual_partition.py (line ~84)
# Change:
app_blocks = bin_to_uf2_blocks(app_data, 0x10200000, 0)  # Old: 2MB

# To:
app_blocks = bin_to_uf2_blocks(app_data, 0x10040000, 0)  # New: 256KB
```

**4. Update build script output:**
```bash
# File: build_dual.py (line ~95 in print statements)
# Change:
print("  0x00000000-0x001FFFFF (2MB):  Bootloader")
print("  0x00200000-0x007FFFFF (6MB):  Application")

# To:
print("  0x00000000-0x0003FFFF (256KB): Bootloader")
print("  0x00040000-0x00FFFFFF (15.75MB): Application")
```

**After making changes:**
```bash
# Clean rebuild
rm -rf build
python build_dual.py

# Test deployment
python test_deployment.py -c 192.168.1.222
```

**Validation:**
- Bootloader should print correct app partition address on startup
- App should execute normally
- OTA updates should write to correct flash offset

**Note:** This is a one-time change that doesn't break existing deployments. Nodes with old partition layout continue working. New builds use optimized layout.

