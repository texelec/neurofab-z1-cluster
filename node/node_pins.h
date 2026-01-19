/**
 * Z1 Onyx Node - Hardware Pin Definitions
 * Code by NeuroFab Corp: 2025-2026
 * 
 * Node-specific peripherals (minimal - most resources on controller)
 * NOTE: Nodes have SAME LED pins as controller (GPIO 44/45/46)
 */

#ifndef NODE_PINS_H
#define NODE_PINS_H

// LEDs (Same as controller: GPIO 44/45/46)
#define LED_GREEN_PIN  44
#define LED_BLUE_PIN   45
#define LED_RED_PIN    46

// Node ID Detection Pins (Hardware strapping with external pull-downs)
// CRITICAL SDK BUG WORKAROUND: Must use gpio_disable_pulls() after gpio_init()
// to avoid internal pull-ups conflicting with external pull-downs
#define NODE_ID_PIN0   40  // ID bit 0 (LSB)
#define NODE_ID_PIN1   41  // ID bit 1
#define NODE_ID_PIN2   42  // ID bit 2
#define NODE_ID_PIN3   43  // ID bit 3
#define NODE_ID_PIN4   44  // ID bit 4 (MSB) - allows 0-31 addressing

// Node does NOT have:
// - OLED display (controller only)
// - SD Card (controller only)
// - Ethernet (controller only)

// PSRAM (QSPI - Shares pins with Flash)
// CS: GPIO 47 (dedicated PSRAM CS)
// QSPI Pins: Shared with flash ROM (hardware multiplexed)
// Interface: QMI (Quad SPI Memory Interface)
#define PSRAM_CS_PIN   47

#endif // NODE_PINS_H
