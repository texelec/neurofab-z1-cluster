# Z1 Onyx Cluster - Pin Definitions Reference

**Last Updated:** December 14, 2025  
**Hardware:** Raspberry Pi Pico 2 (RP2350B) with 8MB PSRAM

---

## Summary

This document provides a comprehensive reference for all GPIO pin assignments in the Z1 Onyx Cluster project. Pin definitions are organized by hardware function and node type.

**IMPORTANT:** Controller and nodes have **SAME LED pins** (GPIO 44/45/46)

---

## Common Bus Signals (All Nodes - GPIO 2-27)

**File:** `common/z1_onyx_bus/z1_onyx_bus_pins.h`

Bus uses 26 pins total via PCIe x4 connector. Original hardware labels preserved.

### Control Signals (GPIO 2-6)
- **BUS_ATTN** = GPIO 2 (Attention - pulled high with 330Ω on backplane)
- **BUS_ACK** = GPIO 3 (Acknowledge - pulled high with 330Ω on backplane)
- **BUS_WR** = GPIO 4 (Write enable)
- **BUS_RD** = GPIO 5 (Read enable)
- **BUS_CLK** = GPIO 6 (Bus clock)

### Address/Select Lines (GPIO 7-11) - 5-bit addressing
- **BUS_SELECT0** = GPIO 7 (LSB)
- **BUS_SELECT1** = GPIO 8
- **BUS_SELECT2** = GPIO 9
- **BUS_SELECT3** = GPIO 10
- **BUS_SELECT4** = GPIO 11 (MSB) - allows 0-31 node addressing

### Data Bus (GPIO 12-27) - 16-bit parallel
- **BUS_DATA[15:0]** = GPIO 12-27
  - BUS_DATA0 = GPIO 12 (LSB)
  - BUS_DATA1 = GPIO 13
  - ...
  - BUS_DATA15 = GPIO 27 (MSB)

---

## Controller Hardware (Node ID: 16)

**Pin Definition Files:** 
- `controller/controller_pins.h` - Hardware variant selector
- `controller/controller_pins_v1.h` - V1 hardware (12-node)
- `controller/controller_pins_v2.h` - V2 hardware (16-node)

### LEDs (Same as nodes - All hardware versions)
- **LED_GREEN_PIN** = GPIO 44
- **LED_BLUE_PIN** = GPIO 45
- **LED_RED_PIN** = GPIO 46

### OLED Display (128x64 I2C SSD1306) - **V2 ONLY**
- **I2C Instance:** I2C0
- **OLED_SDA_PIN** = GPIO 28
- **OLED_SCL_PIN** = GPIO 29
- **I2C Address:** 0x3C (7-bit)
- **V1 Hardware:** No OLED display

### SD Card (SPI1) - All hardware versions
- **SD_MISO_PIN** = GPIO 40
- **SD_CS_PIN** = GPIO 41
- **SD_CLK_PIN** = GPIO 42
- **SD_MOSI_PIN** = GPIO 43
- **Baud Rate:** 12.5 MHz (12500 * 1000)
- **Status:** Hardware present, stub driver (not fully implemented)

### W5500 Ethernet (SPI0) - **PIN DIFFERENCES V1/V2**

**V2 Hardware (16-node):**
- **W5500_MISO_PIN** = GPIO 36
- **W5500_CS_PIN** = GPIO 37
- **W5500_CLK_PIN** = GPIO 38
- **W5500_MOSI_PIN** = GPIO 39
- **W5500_RST_PIN** = GPIO 34 (Network reset)
- **W5500_INT_PIN** = GPIO 35 (Network IRQ)

**V1 Hardware (12-node):**
- **W5500_MISO_PIN** = GPIO 36
- **W5500_CS_PIN** = GPIO 37
- **W5500_CLK_PIN** = GPIO 38
- **W5500_MOSI_PIN** = GPIO 39
- **W5500_RST_PIN** = GPIO 35 (Network reset - SWAPPED)
- **W5500_INT_PIN** = GPIO 34 (Network IRQ - SWAPPED)

### Global Node Reset - **V2 ONLY**
- **GLOBAL_RESET_PIN** = GPIO 33 (Active high - resets all nodes)
- **V1 Hardware:** No global reset capability

### I2C Buses
- **I2C0:** Used by OLED (SDA=28, SCL=29)
- **I2C1:** Exposed on controller hardware but unused (placeholder for future)

### PSRAM (QSPI)
- **Size:** 8MB
- **CS Pin:** GPIO 47 (dedicated PSRAM chip select)
- **QSPI Data/Clock:** Shared with Flash ROM (hardware multiplexed by RP2350)
- **Interface:** QMI (Quad SPI Memory Interface)
- **Base Address:** 0x11000000 (fixed in RP2350 memory map)

---

## Node Hardware (Node ID: 0-15)

**Pin Definition File:** `node/node_pins.h`

### LEDs (Same as controller!)
- **LED_GREEN_PIN** = GPIO 44
- **LED_BLUE_PIN** = GPIO 45
- **LED_RED_PIN** = GPIO 46

### Node ID Detection (Hardware Strapping)
- **NODE_ID_PIN0** = GPIO 40 (LSB)
- **NODE_ID_PIN1** = GPIO 41
- **NODE_ID_PIN2** = GPIO 42
- **NODE_ID_PIN3** = GPIO 43
- **NODE_ID_PIN4** = GPIO 44 (MSB)
- **Configuration:** External pull-downs, read at startup
- **ID Range:** 0-15 (nodes), 16=controller (hardcoded)
- **SDK BUG WORKAROUND:** Must call `gpio_disable_pulls()` after `gpio_init()` to prevent internal pull-ups from conflicting with external pull-downs

### PSRAM (QSPI)
- **Size:** 8MB
- **CS Pin:** GPIO 47 (dedicated PSRAM chip select)
- **QSPI Data/Clock:** Shared with Flash ROM (hardware multiplexed by RP2350)
- **Interface:** QMI (Quad SPI Memory Interface)
- **Base Address:** 0x11000000 (fixed in RP2350 memory map)

### NOT Present on Nodes
- ❌ NO OLED Display (controller only)
- ❌ NO SD Card (controller only)
- ❌ NO Ethernet (controller only)

---

## Pin Conflicts & Notes

### GPIO 40-44 Conflict (Intentional)
- **Controller:** SD Card uses GPIO 40-43
- **Nodes:** Node ID detection uses GPIO 40-44
- **Resolution:** These peripherals are mutually exclusive by hardware design
  - SD card only exists on controller
  - Node ID pins only exist on nodes

### Hardware Pull-ups on Bus (Backplane)
- **BUS_ATTN** (GPIO 2): 330Ω pull-up on backplane
- **BUS_ACK** (GPIO 3): 330Ω pull-up on backplane

---

## File Locations

### Pin Definition Headers
```
common/z1_onyx_bus/z1_onyx_bus_pins.h   - Bus signals (all nodes)
controller/controller_pins.h             - Controller peripherals
node/node_pins.h                         - Node peripherals + ID detection
```

### Implementation Files
```
controller/controller_main.c             - Controller firmware
node/node_main.c                         - Node firmware (with SDK bug workaround)
common/psram/psram_rp2350.c             - PSRAM driver
common/oled/ssd1306.c                    - OLED driver (controller only)
common/sd_card/sd_card_stub.c           - SD card stub (controller only)
```

---

## Hardware Differences Summary

| Peripheral | Controller | Node |
|------------|-----------|------|
| LEDs | **3 (Green/Blue/Red @ 44/45/46)** | **3 (Green/Blue/Red @ 44/45/46)** |
| OLED | ✅ Yes (I2C0, GPIO 28/29) | ❌ No |
| SD Card | ✅ Yes (SPI1, GPIO 40-43) | ❌ No |
| Ethernet | ✅ Yes (SPI0, GPIO 36-39, RST=34, IRQ=35) | ❌ No |
| PSRAM | ✅ 8MB (GPIO 47 CS, QSPI shared) | ✅ 8MB (GPIO 47 CS, QSPI shared) |
| Node ID | Hardcoded (16) | Detected (GPIO 40-44) |
| I2C Buses | I2C0 (OLED), I2C1 (unused) | ❌ None |

---

## Notes

1. **Bus pins (GPIO 2-27)** are common to all nodes and defined in `z1_onyx_bus_pins.h`
2. **Peripheral pins** are node-type specific and defined in separate headers
3. **PSRAM uses GPIO 47 for CS** and shares QSPI data/clock pins with Flash ROM (hardware multiplexed)
4. **SD card is stubbed** - full FatFS library available in `old/Z1_BUS_TEST/src/lib/FatFS/`
5. **All pin definitions preserved** from original working hardware design
6. **LED pins are IDENTICAL** on both controller and nodes (GPIO 44/45/46)
7. **Node ID detection requires SDK bug workaround** - see `node_main.c` for implementation

---
