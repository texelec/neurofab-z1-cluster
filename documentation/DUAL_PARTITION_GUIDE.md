# Z1 Onyx Dual-Partition Firmware Guide

## Overview

The Z1 Onyx cluster uses a **dual-partition flash architecture** to enable Over-The-Air (OTA) firmware updates for compute nodes. This guide explains how to build applications that run in the application partition.

## Status

**✅ FULLY OPERATIONAL** (January 11, 2026)
- Bootloader validates and launches app successfully
- App runs in separate memory space with proper initialization
- Bus, broker, and PSRAM operational in app partition
- SNN engine working (topology deployment, spike injection, monitoring)
- All 16 nodes communicating reliably with controller
- Test suite: 9/9 tests passing
- Ready for OTA Phase 2 (controller-commanded updates)

## Flash Layout

```
0x00000000 - 0x0007FFFF (512 KB)  : Bootloader Partition
0x00080000 - 0x007FFFFF (7.5 MB) : Application Partition
```

The **bootloader** (at 0x00000000) initializes critical hardware, validates the app partition, then performs a direct jump to the **application** (at 0x00080000). The app must re-initialize certain subsystems to point to its own memory space.

## Architecture Benefits

- **OTA Updates**: Controller can update node firmware over the network without physical access
- **Rollback Safety**: If new firmware fails validation (bad CRC), bootloader won't boot it
- **Consistent Environment**: Bootloader guarantees hardware is initialized before app runs
- **Reduced App Size**: Apps don't need bus/PSRAM/broker initialization code

## Building Applications

### Quick Start

```bash
# Build dual-partition firmware (bootloader + app)
python build_dual.py

# Output: FirmwareReleases/16node/node_dual_16.uf2 (contains both partitions)
```

### Manual CMake Build

If you need custom build configurations:

```bash
cd build
cmake -G Ninja -DBUILD_HW_V2=ON ..
ninja bootloader_16 node_app_16
```

## Application Code Requirements

### 1. Use APP_PARTITION_MODE Flag

This flag tells your code it's running from the bootloader:

```c
#ifdef APP_PARTITION_MODE
    // Running from bootloader - re-enable interrupts first
    __asm__ volatile ("cpsie i" : : : "memory");
    
    // Enable FPU if using floating point
    volatile uint32_t *cpacr = (uint32_t *)0xE000ED88;
    *cpacr |= (0xF << 20);
    __asm__ volatile ("dsb" : : : "memory");
    __asm__ volatile ("isb" : : : "memory");
    
    // Re-init stdio for app's USB stack
    stdio_usb_init();
    sleep_ms(2000);
    
    my_node_id = read_node_id();
    
    // LED init (safe to re-init GPIO)
    gpio_init(LED_GREEN_PIN);
    gpio_set_dir(LED_GREEN_PIN, GPIO_OUT);
    
    // CRITICAL: Re-initialize bus/broker to point DMA at APP memory
    // Bootloader's DMA points to bootloader's buffers at different addresses
    // App has separate copies of rx_buffer/tx_buffer at 0x00080000+ addresses
    z1_bus_init_node();
    z1_bus_set_node_id(my_node_id);
    z1_bus_set_speed_mhz(BUS_CLOCK_MHZ);
    z1_broker_init();
    
    // PSRAM doesn't need re-init (hardware state preserved)
    
    // Your app-specific init
    z1_snn_init(my_node_id);
#else
    // Monolithic mode - full initialization
    stdio_init_all();
    my_node_id = read_node_id();
    z1_bus_init_node();
    z1_broker_init();
    psram_init();
    z1_snn_init(my_node_id);
#endif
```

**Why bus/broker re-init is required:**
- Bootloader and app have **separate copies** of global variables (rx_buffer, tx_buffer, etc.)
- Bootloader at 0x00000000, app at 0x00080000 → different memory addresses
- DMA channels must be reconfigured to point to app's buffer addresses
- Without re-init, app reads from wrong memory → infinite loop in receive function

### 2. CMakeLists.txt Configuration

Create a separate target for the app partition:

```cmake
# Application partition target
add_executable(node_app_16
    node_main.c
    z1_snn_engine.c
)

set_target_properties(node_app_16 PROPERTIES PICO_BOARD pico2)

# CRITICAL: Relocate .text section to app partition address
# App starts at 0x10080000 + 192-byte header = 0x100800C0
target_link_options(node_app_16 PRIVATE
    "LINKER:--defsym=__flash_binary_start=0x100800C0"
    "LINKER:--section-start=.text=0x100800C0"
)

# Link libraries (same as monolithic)
target_link_libraries(node_app_16
    pico_stdlib
    hardware_pio
    hardware_dma
    hardware_gpio
    z1_onyx_bus
    z1_broker
    z1_commands
    psram
)

# Set APP_PARTITION_MODE flag
target_compile_definitions(node_app_16 PRIVATE
    HW_V2
    APP_PARTITION_MODE  # Critical flag!
    PICO_STACK_SIZE=0x800  # 2KB stack sufficient for app
)

# USB output (optional - bootloader also has USB active)
pico_enable_stdio_usb(node_app_16 1)
pico_enable_stdio_uart(node_app_16 0)

# Generate binary outputs
pico_add_extra_outputs(node_app_16)
```

### 3. Hardware Initialization Rules

**What the Bootloader Initializes:**
- ✅ System clocks (266 MHz CPU, 133 MHz bus)
- ✅ Matrix bus (8.0 MHz, DMA channels 0/1)
- ✅ Z1 Broker (command queue, statistics)
- ✅ PSRAM (8MB @ 0x11000000, QUAD mode)
- ✅ OTA engine (buffer @ 0x11010000)
- ✅ USB serial (for bootloader output)

**What Your App Must Do:**
- ✅ Re-enable interrupts (`cpsie i`) immediately after entry
- ✅ Initialize app-specific peripherals (LEDs, sensors, etc.)
- ✅ Call `z1_bus_set_node_id()` to register with bus
- ✅ Initialize app-specific libraries (SNN engine, etc.)

**What Your App Must NOT Do:**
- ❌ Call `stdio_init_all()` (reconfigures clocks, breaks USB)
- ❌ Call `z1_bus_init()` (already done)
- ❌ Call `z1_broker_init()` (already done)
- ❌ Call `psram_init()` (already done)
- ❌ Reconfigure system clocks
- ❌ Reinitialize DMA channels 0/1 (used by Matrix bus)

### 4. USB Serial Output (Optional)

If you need serial debugging in the app:

```c
#ifdef APP_PARTITION_MODE
    // Re-enable interrupts FIRST
    __asm__ volatile ("cpsie i" : : : "memory");
    
    // Reinitialize USB after app's vector table is active
    stdio_usb_init();
    sleep_ms(1000);  // Wait for USB enumeration
    
    printf("[APP] Node %d ready\n", my_node_id);
#else
    stdio_init_all();
    printf("[MONO] Node %d ready\n", my_node_id);
#endif
```

**Note**: USB output is optional. LED blink patterns work well for status indication without USB complexity.

## Build System Details

### File Structure

```
embedded_firmware/
├── bootloader/
│   ├── CMakeLists.txt        # Bootloader build config
│   ├── bootloader_main.c     # Bootloader entry point
│   └── ota_engine.c          # OTA update handler
├── node/
│   ├── CMakeLists.txt        # Monolithic + app targets
│   ├── node_main.c           # App entry point
│   └── z1_snn_engine.c       # SNN algorithm
└── common/                   # Shared libraries (bus, broker, PSRAM)
```

### Binary Sizes

**Typical Sizes:**
- Bootloader: ~42KB (includes OTA + flash programming)
- Node App: ~41KB (includes SNN engine)
- Combined UF2: ~227KB (bootloader + app + erase blocks)

**Why Bootloader is Larger:**
OTA engine (27KB) is similar in size to SNN engine (21KB). Bootloader needs:
- Network buffer management
- Flash sector erasing
- Flash page programming
- CRC32 validation
- Binary header parsing

### App Header

Every application binary includes a 192-byte header:

```c
struct app_header {
    uint32_t magic;           // 0x5A314150 ('ZA1P')
    uint32_t version;         // App version
    uint32_t binary_size;     // Code size in bytes
    uint32_t crc32;           // Checksum
    uint32_t entry_point;     // Offset to vector table (0xC0)
    char name[64];            // "Z1 Node App"
    // ... padding to 192 bytes
};
```

The build system automatically adds this header via `build_tools/add_app_header.py`.

## Flashing Firmware

### First-Time Flash

1. **Hold BOOTSEL**, connect USB
2. Copy `FirmwareReleases/16node/node_dual_16.uf2` to Pico drive
3. Pico reboots with bootloader + app

### Erasing Old Bootloader

If you previously flashed different firmware, old bootloaders may remain in flash:

**Option 1: Use picotool (if you have USB support):**
```bash
# Put Pico in BOOTSEL mode
picotool erase -a

# Then flash new firmware
# Copy node_dual_16.uf2 to Pico drive
```

### OTA Update (Over Network)

```bash
# Controller must be running and connected to node
python python_tools/bin/nsnn ota node_app_16.bin --node 0
```

The controller streams the binary to the node's OTA engine, which:
1. Writes to app partition
2. Validates CRC32
3. Resets node to reboot into new app

## Debugging Tips

### LED Diagnostics

```c
// Bootloader stage
// 5 red blinks = app reached main()
// 3 green blinks = init_system() complete
// Solid green = idle loop running

// Use different patterns for app errors
gpio_put(LED_RED_PIN, 1);  // Solid red = fatal error
while (1) { tight_loop_contents(); }
```

### Check Binary Addresses

```bash
# Verify app binary is linked correctly
arm-none-eabi-objdump -h build/node/node_app_16.elf | Select-String ".text"

# Should show: .text at 0x100800C0 (not 0x10000000)
```

### Check Symbol Sizes

```bash
# Compare bootloader vs app code size
arm-none-eabi-size build/bootloader/bootloader_16.elf
arm-none-eabi-size build/node/node_app_16.elf

# App should be similar size or slightly smaller
```

## Common Issues

### App Hangs at Startup

**Symptom**: Bootloader runs, jumps to app, then nothing
**Cause**: App trying to reinitialize hardware bootloader already configured
**Fix**: Wrap all hardware init in `#ifndef APP_PARTITION_MODE`

### USB Serial Not Working

**Symptom**: printf() output doesn't appear
**Cause**: USB interrupts using wrong vector table or stdio reconfiguring clocks
**Fix**: Either skip USB in app, or use LED diagnostics only

### Watchdog Resets

**Symptom**: System reboots every few seconds
**Cause**: App not servicing watchdog or crash loop
**Fix**: Check app logic, add LED blinks to see where it stops

### Double Bootloader Messages

**Symptom**: See two bootloader banners on serial output
**Cause**: Old firmware still present at 0x200000 in flash
**Fix**: Use `picotool erase -a` before flashing

## Verification Checklist

✅ App CMakeLists.txt has `APP_PARTITION_MODE` compile definition
✅ App CMakeLists.txt has `LINKER:--section-start=.text=0x100800C0`
✅ App code wraps hardware init in `#ifndef APP_PARTITION_MODE`
✅ App re-enables interrupts (`cpsie i`) at start
✅ App binary linked to 0x100800C0 (verify with objdump)
✅ Flash erased before first dual-partition firmware load
✅ node_dual_16.uf2 contains both bootloader and app

## Reference Files

- **Bootloader**: `bootloader/bootloader_main.c`
- **App Example**: `node/node_main.c`
- **Build Script**: `build_dual.py`
- **Header Script**: `build_tools/add_app_header.py`
- **UF2 Merger**: `build_tools/merge_dual_partition.py`
- **CMake Config**: `node/CMakeLists.txt` (search for `node_app_16`)

## Additional Resources

- RP2350 Datasheet: Section 2.8 (Boot sequence)
- Pico SDK Docs: [https://rptl.io/pico-c-sdk](https://rptl.io/pico-c-sdk)
- Z1 Onyx API: `API_REFERENCE.md`
- Build Instructions: `BUILD_INSTRUCTIONS.md`
