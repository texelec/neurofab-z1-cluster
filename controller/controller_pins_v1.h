/**
 * Z1 Onyx Controller - Hardware Pin Definitions (V1 - 12 Node)
 *
 * Controller-specific peripherals (not present on nodes)
 * NOTE: Controller and nodes have SAME LED pins (GPIO 44/45/46)
 */

#ifndef CONTROLLER_PINS_V1_H
#define CONTROLLER_PINS_V1_H

// LEDs (Same as nodes: GPIO 44/45/46)
#define LED_GREEN_PIN  44
#define LED_BLUE_PIN   45
#define LED_RED_PIN    46

// W5500 Ethernet (Controller only) - SPI0 - V1 PINOUT
#define W5500_SPI_PORT spi0
#define W5500_SPI      spi0
#define W5500_MISO_PIN 36
#define W5500_CS_PIN   37
#define W5500_CLK_PIN  38
#define W5500_MOSI_PIN 39
#define W5500_RST_PIN  35  // V1: Network reset on GPIO 35
#define W5500_INT_PIN  34  // V1: Network IRQ on GPIO 34

// SD Card (Controller only) - SPI1
#define SD_SPI         spi1
#define SD_MISO_PIN    40
#define SD_CS_PIN      41
#define SD_CLK_PIN     42
#define SD_MOSI_PIN    43
#define SD_BAUD_RATE   (12500 * 1000)  // 12.5 MHz

// PSRAM (QSPI - Shares pins with Flash)
// CS: GPIO 47 (dedicated PSRAM CS)
// QSPI Pins: Shared with flash ROM (hardware multiplexed)
// Interface: QMI (Quad SPI Memory Interface)
#define PSRAM_CS_PIN   47

// V1 Hardware: No OLED display
// V1 Hardware: No global node reset

#endif // CONTROLLER_PINS_V1_H
