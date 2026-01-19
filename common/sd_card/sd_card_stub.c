/**
 * Z1 Onyx Cluster - SD Card Stub Implementation
 * Code by NeuroFab Corp: 2025-2026
 * 
 * Placeholder stub - hardware present but not initialized yet.
 * 
 * Controller Hardware Configuration:
 * - SPI Instance: SPI1
 * - MISO: GPIO 40
 * - CS:   GPIO 41
 * - CLK:  GPIO 42
 * - MOSI: GPIO 43
 * - Baud: 12.5 MHz (12500 * 1000)
 * 
 * To integrate full FatFS library from old project:
 * Copy from: old/Z1_BUS_TEST/src/lib/FatFS/FatFs_SPI/
 * Required files:
 * - sd_driver/* (sd_card.c, sd_spi.c, spi.c, crc.c)
 * - ff15/source/* (ff.c, diskio.c, ffconf.h)
 * - include/* (ff_stdio.c, f_util.c)
 */

#include "sd_card_stub.h"
#include <stdio.h>

bool sd_card_init(void) {
    printf("[SD-STUB] SD card disabled (compile with USE_SD_CARD to enable)\n");
    return false;  // Stub always fails init
}
