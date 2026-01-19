/**
 * Z1 Onyx Cluster - FatFS Hardware Configuration
 * 
 * Defines SPI and SD card configuration for Z1 controller.
 * Both V1 and V2 hardware use identical SD card wiring (SPI1, pins 40-43).
 */

#include "hw_config.h"
#include "pico/stdlib.h"

// Single SPI controller configuration (SPI1)
static spi_t spi = {
    .hw_inst = spi1,
    .miso_gpio = 40,
    .mosi_gpio = 43,
    .sck_gpio = 42,
    .baud_rate = 12500 * 1000,  // 12.5 MHz
    .set_drive_strength = true,
    .mosi_gpio_drive_strength = GPIO_DRIVE_STRENGTH_4MA,
    .sck_gpio_drive_strength = GPIO_DRIVE_STRENGTH_4MA
};

// Single SD card configuration
static sd_card_t sd_card = {
    .pcName = "0:",  // FatFS logical drive number
    .spi = &spi,
    .ss_gpio = 41,   // Chip Select (CS) pin
    .use_card_detect = false,  // No Card Detect on Z1 hardware
    .card_detect_gpio = 0,
    .card_detected_true = 0,
    .set_drive_strength = true,
    .ss_gpio_drive_strength = GPIO_DRIVE_STRENGTH_4MA
};

// Required by FatFS library
size_t spi_get_num() { 
    return 1; 
}

spi_t *spi_get_by_num(size_t num) {
    if (num == 0) return &spi;
    return NULL;
}

size_t sd_get_num() { 
    return 1; 
}

sd_card_t *sd_get_by_num(size_t num) {
    if (num == 0) return &sd_card;
    return NULL;
}
