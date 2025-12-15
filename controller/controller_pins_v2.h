/**
 * Z1 Onyx Controller - Hardware Pin Definitions (V2 - 16 Node)
 *
 * Controller-specific peripherals (not present on nodes)
 * NOTE: Controller and nodes have SAME LED pins (GPIO 44/45/46)
 */

#ifndef CONTROLLER_PINS_V2_H
#define CONTROLLER_PINS_V2_H// LEDs (Same as nodes: GPIO 44/45/46)
#define LED_GREEN_PIN  44
#define LED_BLUE_PIN   45
#define LED_RED_PIN    46

// SSD1306 OLED Display (Controller only) - I2C0
#define OLED_I2C       i2c0
#define OLED_SDA_PIN   28
#define OLED_SCL_PIN   29
#define OLED_I2C_ADDR  0x3C  // 7-bit address

// SD Card (Controller only) - SPI1
#define SD_SPI         spi1
#define SD_MISO_PIN    40
#define SD_CS_PIN      41
#define SD_CLK_PIN     42
#define SD_MOSI_PIN    43
#define SD_BAUD_RATE   (12500 * 1000)  // 12.5 MHz

// W5500 Ethernet (Controller only) - SPI0
#define W5500_SPI_PORT spi0
#define W5500_SPI      spi0
#define W5500_MISO_PIN 36
#define W5500_CS_PIN   37
#define W5500_CLK_PIN  38
#define W5500_MOSI_PIN 39
#define W5500_RST_PIN  34  // Network reset
#define W5500_INT_PIN  35  // Network IRQ

// I2C Buses (Both exposed on controller hardware)
// I2C0: Used by OLED (SDA=28, SCL=29)
// I2C1: Available but unused - placeholder for future use
// #define I2C1_SDA_PIN  ??
// #define I2C1_SCL_PIN  ??

// Global Node Reset (Controller only)
#define GLOBAL_RESET_PIN 33  // Active high, resets all nodes simultaneously

// SSD1306 OLED Display (Controller only) - I2C0 - V2 ONLY
#define OLED_I2C       i2c0
#define OLED_SDA_PIN   28
#define OLED_SCL_PIN   29
#define OLED_I2C_ADDR  0x3C  // 7-bit address

// Global Node Reset (Controller only) - V2 ONLY
#define GLOBAL_RESET_PIN 33  // Active high, resets all nodes simultaneously

// PSRAM (QSPI - Shares pins with Flash)
// CS: GPIO 47 (dedicated PSRAM CS)
// QSPI Pins: Shared with flash ROM (hardware multiplexed)
// Interface: QMI (Quad SPI Memory Interface)
#define PSRAM_CS_PIN   47

#endif // CONTROLLER_PINS_V2_H
