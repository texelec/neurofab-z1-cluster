# GitHub Copilot Instructions for Z1 Onyx Cluster

## Project Overview

This is the **NeuroFab Z1 Onyx** - a distributed spiking neural network (SNN) hardware accelerator cluster based on Raspberry Pi Pico 2 (RP2350) microcontrollers.

### Key Facts
- **Hardware**: RP2350-based controller + compute nodes, Matrix bus interconnect
- **Purpose**: Real-time SNN simulation with hardware acceleration
- **Build System**: CMake + Ninja + ARM GCC + Pico SDK + Python build scripts
- **Note**: Build directory should not contain spaces (ARM GCC compatibility)

---

## Hardware Variants

The project supports **TWO** hardware versions with a single codebase:

### V2 Hardware (16-node, Current/Default)
- 16 compute nodes with auto-detection via GPIO pins (runtime)
- OLED display on controller only (I2C0, SDA=GPIO28, SCL=GPIO29)
- NO OLED on nodes
- Global node reset capability (GPIO 33, software reset used in practice)
- W5500 Ethernet: RST=GPIO34, INT=GPIO35
- Single node firmware binary (`node_dual_16.uf2` - dual-partition with bootloader)
- **Build**: `python build.py` or `python build.py --hw-v2`

### V1 Hardware (12-node, Legacy)
- 12 compute nodes with hardcoded node IDs (0-11, compile-time constant)
- NO OLED display (neither controller nor nodes)
- NO global reset capability
- W5500 Ethernet: RST=GPIO35, INT=GPIO34 (**SWAPPED from V2**)
- Individual node firmware per ID (`node_dual_12_0.uf2` through `node_dual_12_11.uf2`)
- **Build**: `python build.py --hw-v1`
- **NOTE**: V1 firmware builds successfully but has NOT been tested on physical V1 hardware

### Hardware Selection Mechanism
- **Preprocessor defines**: `HW_V1` or `HW_V2` set by CMake
- **Pin headers**: `controller_pins_v1.h` and `controller_pins_v2.h`
- **Selector**: `controller_pins.h` includes correct variant based on define
- **CMake options**: `BUILD_HW_V1` and `BUILD_HW_V2` control target generation

---

## Build System

### Build Targets

**V2 (16-node):**
- `controller_16` - Controller firmware (monolithic)
- `bootloader_16` - Bootloader only (OTA update engine)
- `node_app_16` - Application partition (OTA-capable SNN firmware)
- `node_dual_16.uf2` - Combined bootloader + app (created by build scripts)

**V1 (12-node):**
- `controller_12` - Controller firmware (monolithic)
- `bootloader_12` - Bootloader only (OTA update engine)
- `node_app_12_0` through `node_app_12_11` - Application partitions (one per node)
- `node_dual_12_N.uf2` - Combined bootloader + app for each node (N=0-11)

**Deprecated** (removed January 18, 2026):
- `node_16` - Monolithic V2 node (replaced by dual-partition)
- `node_12_N` - Monolithic V1 nodes (replaced by dual-partition)

### Build Commands

```bash
# V2 hardware (default)
python build.py

# V1 hardware
python build.py --hw-v1

# Manual CMake
cd build
cmake -G Ninja -DBUILD_HW_V2=ON -DBUILD_HW_V1=OFF ..
ninja controller_16 node_16
```

### Output Structure

```
build/                          # All ELF, hex, bin files stay here
FirmwareReleases/
  ├── 16node/                   # V2 firmware
  │   ├── controller_16.uf2     # Controller (root - commonly used)
  │   ├── node_dual_16.uf2      # Node firmware (root - commonly used, OTA-capable)
  │   └── apponly/              # Advanced/recovery files
  │       ├── bootloader_16.uf2 # Bootloader only
  │       └── node_app_16.uf2   # Application only
  └── 12node/                   # V1 firmware
      ├── controller_12.uf2     # Controller (root - commonly used)
      ├── node_dual_12_0.uf2    # Node 0 (root - commonly used, OTA-capable)
      ├── node_dual_12_1.uf2    # Node 1
      ├── ... (12 total)
      └── apponly/              # Advanced/recovery files
          ├── bootloader_12.uf2 # Bootloader only
          ├── node_app_12_0.uf2 # App partition for node 0
          └── ... (12 total)

packages/
  ├── node_app_16.bin        01 (changed from 192.168.1.222)
- **Single point of change**: Edit IP_ADDRESS array in `controller/w5500_eth.c` (line ~25)
- **Runtime config**: Use `python python_tools/bin/zconfig write --ip <new_ip>` (no rebuild!)
- **Automatic propagation**: OLED display and HTTP responses use `w5500_get_ip_string()` helper
- **Python tools**: Default to 192.168.1.201

### UF2 Conversion
- **Tool**: `build_tools/elf2uf2.py` (custom Python converter)
- **Why**: picotool crashes with access violations, so we use Python implementation
- **Process**: ELF → BIN (objcopy) → UF2 (elf2uf2.py)
- **Family ID**: RP2350_ARM_S (0xe48bff59)

### Network Configuration
- **Default IP**: 192.168.1.222 (hardcoded in controller firmware)
- **Single point of change**: Edit IP_ADDRESS array in `controller/w5500_eth.c` (line ~25)
- **Automatic propagation**: OLED display and HTTP responses use `w5500_get_ip_string()` helper
- **Python tools**: Default to 192.168.1.222, overridable with `-c` flag
- **Documentation**: See BUILD_INSTRUCTIONS.md for full details

---

## Important Code Patterns

### Hardware-Specific Code

**Conditional compilation for V2-only features:**
```c
#ifdef HW_V2
    #include "ssd1306.h"  // OLED only on V2
    ssd1306_init();
    gpio_init(GLOBAL_RESET_PIN);  // Reset only on V2
#endif
```

**Pin definitions:**
```c
// controller/controller_pins.h (selector)
#ifdef HW_V1
    #include "controller_pins_v1.h"
#elif defined(HW_V2)
    #include "controller_pins_v2.h"
#endif
```

### Node ID Handling

**V1 (compile-time):**
```c
#ifdef NODE_ID_HARDCODED
    const int node_id = NODE_ID_HARDCODED;  // Set by CMake (0-11)
#endif
```

**V2 (runtime detection):**
```c
// Auto-detect from GPIO pins (see node.c)
node_id = read_node_id_from_pins();
```

---

## Critical Build Requirements

### Environment
- **PICO_SDK_PATH**: Must be set to Pico SDK installation
- **Tools in PATH**: arm-none-eabi-gcc, cmake, ninja, python3
- **Python**: 3.7+ required for build scripts and tools
- **No hardcoded paths**: All paths relative to project root or use environment variables

### Common Build Issues

1. **Path with spaces**: Avoid spaces in project path due to ARM GCC compatibility issues
2. **Picotool crashes**: Use `elf2uf2.py` instead (already integrated)
3. **Wrong hardware variant**: Specify `--hw-v1` or `--hw-v2` explicitly
4. **Missing SDK**: Set `PICO_SDK_PATH` environment variable

---

## File Organization

### Firmware Structure
```
embedded_firmware/
├── bootloader/         # Z1 bootloader (future use)
├── controller/         # Controller firmware
│   ├── controller_pins.h       # Hardware variant selector
│   ├── controller_pins_v1.h    # V1 pin definitions
│   ├── controller_pins_v2.h    # V2 pin definitions
│   ├── controller_main.c       # Main controller code
│   └── ...
├── node/              # Compute node firmware
│   ├── node.c         # Main node code
│   └── ...
└── common/            # Shared headers (protocol, Matrix bus)
```

### Python Tools
```
python_tools/
├── bin/               # CLI utilities (nls, nstat, nsnn)
├── lib/               # Shared libraries (z1_client.py)
└── examples/          # Topology JSON files (xor_working.json, mnist_snn.json)
```

### Documentation
```
# Root-level documentation
README.md                    # Project overview and quick start
BUILD_INSTRUCTIONS.md        # Build and flash instructions
TEST_DEPLOYMENT_GUIDE.md     # Testing guide
API_REFERENCE.md             # HTTP API and technical specs
PIN_REFERENCE.md
├── ARCHITECTURE.md
├── USER_GUIDE.md
├── PIN_REFERENCE.md   # Pin assignments for V1/V2
└── ...

# Root-level docs
BUILD_INSTRUCTIONS.md      # How to build both hardware variants
TEST_DEPLOYMENT_GUIDE.md   # How to test deployed cluster
README.md                  # Project overview
```

---

## Testing

### Test Script
- **Location**: `test_deployment.py` (root directory)
- **Purpose**: Comprehensive deployment validation
- **Tests**: Discovery → Deploy → Start → Inject → Monitor → Stop
- **Usage**: `python test_deployment.py -c 192.168.1.222`

### Test Sequence
1. Node discovery (`nls`)
2. Topology deployment (`nsnn deploy`)
3. Node status (`nstat`)
4. Start SNN (`nsnn start`)
5. Spike injection (`nsnn inject`)
6. Statistics (`nstat -s`)
7. Status check (`nsnn status`)
8. Stop SNN (`nsnn stop`)

---

## Network Protocol

### HTTP API (Controller)
- **Port**: 80 (hardware), 8000 (emulator)
- **Endpoints**: `/api/nodes`, `/api/deploy`, `/api/start`, `/api/inject`, etc.
- **Format**: JSON request/response

### Matrix Bus (Inter-node)
- **Physical**: Custom PCB backplane with parallel bus
- **Protocol**: Frame-based with node addressing
- **Purpose**: Spike propagation between compute nodes

---

## Recent Major Changes

1. **Target Rename**: All `_snn` targets renamed to `_16` (e.g., `node_snn` → `node_16`)
2. **Hardware Variants**: Full V1/V2 support with single codebase
3. **UF2 Conversion**: Custom Python converter replaces broken picotool
4. **Build System**: Auto-reconfiguration when switching between hardware variants
5. **Documentation Cleanup**: Removed all hardcoded paths, added hardware variant docs

---

## Coding Guidelines

### When Making Changes
1. **Hardware variants**: Always consider if code affects V1/V2 differently
2. **Pin references**: Use defines from `controller_pins.h`, never hardcode GPIO numbers
3. **Paths**: Use relative paths, never hardcode absolute paths
4. **Documentation**: Update both code comments and relevant .md files
5. **Build testing**: Test both `--hw-v1` and `--hw-v2` builds after changes

### File Naming
- Controller binaries: `controller_12` (V1) or `controller_16` (V2)
- Node binaries: `node_12_N` (V1) or `node_16` (V2)
- Firmware releases: Store in `FirmwareReleases/12node/` or `FirmwareReleases/16node/`

### CMake Patterns
```cmake
if(BUILD_HW_V2)
    add_executable(controller_16 controller_main.c ...)
    target_compile_definitions(controller_16 PRIVATE HW_V2)
endif()

if(BUILD_HW_V1)
    add_executable(controller_12 controller_main.c ...)
    target_compile_definitions(controller_12 PRIVATE HW_V1)
endif()
```

---

## Important Context for AI Assistants

- **This is embedded firmware**: Memory-constrained, real-time requirements, no OS
- **Hardware abstraction**: Same codebase runs on two different PCB designs
- **Build complexity**: Multi-target builds (1 controller + 1 node for V2, 1 controller + 12 nodes for V1)
- **Active development**: SNN engine, STDP learning, multi-backplane support in progress
- **Production ready**: V2 hardware is current production, V1 is legacy support

### When Suggesting Changes
- Respect memory constraints (PSRAM available but limited)
- Consider real-time implications (neuron updates happen in tight loops)
- Maintain hardware variant abstraction (don't break V1 support)
- Keep build process simple and reliable
- Document hardware-specific assumptions

---

## Quick Reference Commands

```bash
# Build V2 (default)
python build.py

# Build V1
python build.py --hw-v1

# Flash controller (V2)
# Copy FirmwareReleases/16node/controller_16.uf2 to Pico in bootloader mode

# Flash nodes (V2)
# Copy FirmwareReleases/16node/node_16.uf2 to each Pico in bootloader mode

# Flash nodes (V1)
# Copy FirmwareReleases/12node/node_12_0.uf2 to first Pico
# Copy FirmwareReleases/12node/node_12_1.uf2 to second Pico, etc.

# Test deployment
python test_deployment.py -c <controller_ip>

# List nodes
python python_tools/bin/nls -c <controller_ip>

# Deploy topology
python python_tools/bin/nsnn deploy python_tools/examples/xor_working.json -c <controller_ip>
```

---

## Project Status
## Project Status

**Current State (January 18, 2026)**: 
- ✅ **PRODUCTION READY**: https://github.com/texelec/neurofab-z1-cluster
- ✅ V2 (16-node) firmware tested and validated on hardware
- ✅ V1 (12-node) firmware builds successfully but untested on physical V1 hardware
- ✅ All documentation current and accurate
- ✅ Build system uses environment variables only (PICO_SDK_PATH)
- ✅ Zero hardcoded paths - fully portable
- ✅ Comprehensive test suite passing (9/9 tests)
- ✅ **OTA firmware updates** operational (`nflash` tool)
- ✅ **Dual-partition bootloader** (512KB + 7.5MB app)
- ✅ **Streaming file system** (1MB+ topology upload/download via `zengine`)
- ✅ **Async spike injection** (background job queue, non-blocking)
- ✅ **Runtime network config** (`zconfig` - no rebuild for IP/MAC changes)
- ✅ **USB flash utilities** (`flash_node.py`, `flash_controller.py` in root)
- ✅ **Monolithic node firmware deprecated** (removed, OTA dual-partition only)
- ✅ **app_main.c deprecated** (replaced with node_main.c for V1 consistency)

**Build Artifacts:**
- Controller V2: 343.0 KB (with OLED)
- Controller V1: 337.5 KB (without OLED) 
- Node V2 app: 44.3 KB (auto-detect ID, OTA-ready)
- Node V1 app: 44.3 KB × 12 files (hardcoded IDs, OTA-ready)
- Dual-partition V2: 178.5 KB (bootloader + app)
- Dual-partition V1: 180.0 KB × 12 files (bootloader + app per node)

**Critical Build Note**: `pico_set_binary_type(copy_to_ram)` is REQUIRED in controller CMakeLists.txt for RP2350B to boot correctly. Without this, firmware will lock on startup. NOTE: This is NOT used in node app partitions (they run from flash with bootloader).
**Critical Build Note**: `pico_set_binary_type(copy_to_ram)` is REQUIRED in all CMakeLists.txt for RP2350B to boot correctly. Without this, firmware will lock on startup.

**Next Steps**: SNN algorithm refinement, STDP implementation, multi-backplane cluster support, performance optimization.
