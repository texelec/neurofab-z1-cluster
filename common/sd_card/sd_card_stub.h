/**
 * Z1 Onyx Cluster - SD Card Stub Interface
 * 
 * Placeholder for SD card initialization.
 * Hardware: SPI1, MISO=40, CS=41, CLK=42, MOSI=43
 * 
 * Full FatFS library can be integrated later.
 * For now, just provides init function that does nothing.
 */

#ifndef SD_CARD_STUB_H
#define SD_CARD_STUB_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize SD card hardware (stub - does nothing yet)
 * 
 * Returns: true always (not yet implemented)
 */
bool sd_card_init(void);

#ifdef __cplusplus
}
#endif

#endif // SD_CARD_STUB_H
