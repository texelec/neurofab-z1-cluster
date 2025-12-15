# Z1 Onyx Cluster - Build Instructions

## Prerequisites

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
   - For running the build script

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

The controller firmware uses a default IP address of **192.168.1.222**. To change this:

### Controller IP Address

**Edit ONE location only:** `controller/w5500_eth.c`

```c
// Network Configuration section (near top of file)
static const uint8_t MAC_ADDRESS[6] = {0x02, 0xA1, 0xB2, 0xC3, 0xD4, 0x01};  // Change if needed
static const uint8_t IP_ADDRESS[4]  = {192, 168, 1, 222};                    // <-- Change here
static const uint8_t SUBNET_MASK[4] = {255, 255, 255, 0};                    // <-- Adjust if needed
static const uint8_t GATEWAY[4]     = {0, 0, 0, 0};                          // <-- Set if needed
```

**That's it!** The OLED display and HTTP responses automatically use the IP address you set above via the `w5500_get_ip_string()` helper function.

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

