/* hw_config.c - Hardware configuration for FatFS/SD card
 * Code by NeuroFab Corp: 2025-2026
 * 
 * This file provides the hardware configuration required by the
 * carlk3/no-OS-FatFS library for SD card operation.
 */

#include <string.h>
#include "hw_config.h"
#include "ff.h"
#include "diskio.h"
#include "hardware/gpio.h"  // For GPIO_DRIVE_STRENGTH_*

// Hardware Configuration of SPI for SD card
// Note: SD card uses SPI1 on Z1 Onyx controller
static spi_t spis[] = {
    {
        .hw_inst = spi1,         // SPI1 component
        .miso_gpio = 40,         // GPIO 40 - MISO
        .mosi_gpio = 43,         // GPIO 43 - MOSI
        .sck_gpio = 42,          // GPIO 42 - SCK
        .baud_rate = 12500 * 1000,  // 12.5 MHz (safe for most SD cards)
        .set_drive_strength = true,
        .mosi_gpio_drive_strength = GPIO_DRIVE_STRENGTH_4MA,
        .sck_gpio_drive_strength = GPIO_DRIVE_STRENGTH_4MA
    }
};

// Hardware Configuration of SD Card
static sd_card_t sd_cards[] = {
    {
        .pcName = "0:",          // FatFS drive name
        .spi = &spis[0],        // Use SPI1
        .ss_gpio = 41,          // GPIO 41 - CS (Chip Select)
        .use_card_detect = false,  // No card detect on Z1 Onyx
        .card_detect_gpio = 0,
        .card_detected_true = 0,
        .set_drive_strength = true,
        .ss_gpio_drive_strength = GPIO_DRIVE_STRENGTH_4MA
    }
};

// Required by FatFS library - return number of SPIs
size_t spi_get_num(void) {
    return count_of(spis);
}

// Required by FatFS library - get SPI by index
spi_t *spi_get_by_num(size_t num) {
    if (num < count_of(spis)) {
        return &spis[num];
    }
    return NULL;
}

// Required by FatFS library - return number of SD cards
size_t sd_get_num() {
    return count_of(sd_cards);
}

// Required by FatFS library - get SD card by index
sd_card_t *sd_get_by_num(size_t num) {
    if (num < count_of(sd_cards)) {
        return &sd_cards[num];
    }
    return NULL;
}

// Required by FatFS library - get SD card by name
sd_card_t *sd_get_by_name(const char *const name) {
    for (size_t i = 0; i < count_of(sd_cards); ++i) {
        if (0 == strcmp(sd_cards[i].pcName, name)) {
            return &sd_cards[i];
        }
    }
    return NULL;
}
