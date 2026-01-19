/**
 * Z1 Onyx Cluster - SD Card Interface with FatFS
 * Code by NeuroFab Corp: 2025-2026
 * 
 * Full SD card driver with FAT32 filesystem support for persistent storage
 * of SNN topologies, configuration files, and OTA firmware updates.
 * 
 * Hardware Configuration:
 *   - Interface: SPI1 at 12.5 MHz (4mA drive strength)
 *   - MISO: GPIO 40
 *   - CS:   GPIO 41  
 *   - CLK:  GPIO 42
 *   - MOSI: GPIO 43
 *   - Format: FAT32 (PC-compatible, <32GB cards)
 * 
 * Library: carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico v3.7.0
 * 
 * RP2350B Compatibility Notes:
 *   - FF_FS_NORTC=1 required (RTC incompatibility)
 *   - Stack-allocated DIR/FILINFO required (NOT PSRAM buffers)
 *   - Use f_findfirst/f_findnext pattern (NOT f_opendir/f_readdir)
 *   - HTTP chunked encoding requires w5500_send_response_len() for null-byte safety
 * 
 * Memory Layout (8MB PSRAM):
 *   Zone 1 (0-64KB):      FatFS working buffers (~30KB used)
 *   Zone 2 (64-128KB):    HTTP response buffer (4KB at 0x11010000)
 *   Zone 3 (128KB-8MB):   Reserved for OTA caching (~7.8MB available)
 * 
 * Directory Structure:
 *   /z1.cfg              - Cluster configuration (IP, engine selection)
 *   /engines/            - SNN topology JSON files
 * 
 * Troubleshooting:
 *   - Hangs in directory listing: Check using stack allocation, not PSRAM
 *   - Truncated HTTP responses: Use w5500_send_response_len() for chunk data
 *   - Corrupt file list: Filters applied (alphanumeric names, <100MB, no hidden)
 *   - Mount fails: Verify FAT32 format, check SPI1 pins, try different card
 * 
 * Test Suite: test_sd_card.py (6 tests: status, config r/w, file upload/list/delete)
 */

#ifndef SD_CARD_H
#define SD_CARD_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize SD card hardware and mount filesystem
 * 
 * Returns: true if SD card mounted successfully, false on error
 */
bool sd_card_init(void);

/**
 * Write data to file (creates or overwrites)
 * 
 * @param filename Path to file (e.g., "z1.cfg" or "engines/lif_v1.z1app")
 * @param data Data buffer to write
 * @param size Number of bytes to write
 * @return true on success, false on error
 */
bool sd_card_write_file(const char* filename, const void* data, size_t size);

/**
 * Read entire file into buffer
 * 
 * @param filename Path to file
 * @param buffer Output buffer (caller must free)
 * @param size Output parameter - bytes read
 * @return true on success, false on error (file not found, malloc fail, etc.)
 */
bool sd_card_read_file(const char* filename, uint8_t** buffer, size_t* size);

/**
 * Check if file exists
 * 
 * @param filename Path to file
 * @return true if file exists, false otherwise
 */
bool sd_card_file_exists(const char* filename);

/**
 * Delete file
 * 
 * @param filename Path to file
 * @return true on success, false if file doesn't exist or error
 */
bool sd_card_delete_file(const char* filename);

/**
 * List files in directory
 * 
 * @param dirpath Directory path (e.g., "engines")
 * @param callback Function called for each file (name, size)
 * @return Number of files found, -1 on error
 */
int sd_card_list_directory(const char* dirpath, void (*callback)(const char* name, size_t size));

/**
 * Create directory if it doesn't exist
 * 
 * @param dirpath Directory path
 * @return true on success, false on error
 */
bool sd_card_create_directory(const char* dirpath);

/**
 * Get free space on SD card
 * 
 * @return Free bytes, or 0 on error
 */
uint64_t sd_card_get_free_space(void);

/**
 * Check if SD card is mounted
 * 
 * @return true if mounted, false otherwise
 */
bool sd_card_is_mounted(void);

#ifdef __cplusplus
}
#endif

#endif // SD_CARD_H
