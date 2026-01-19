/**
 * Z1 Onyx Cluster - SD Card Implementation with FatFS
 * Code by NeuroFab Corp: 2025-2026
 * 
 * Full driver using carlk3/no-OS-FatFS library for RP2350B.
 * Follows the library's recommended usage pattern:
 * - Get SD card object via sd_get_by_num(0)
 * - Mount using f_mount(&pSD->fatfs, pSD->pcName, 1)
 * - Library handles SPI init and low-level operations automatically
 */

#include "hw_config.h"  // SD card hardware configuration
#include "../FatFs_SPI/sd_driver/sd_card.h"  // Library's SD card driver
#include "sd_card.h"    // Our wrapper API

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pico/stdlib.h"
#include "hardware/spi.h"

// FatFS includes
#include "ff.h"       // FatFS main header
#include "diskio.h"   // FatFS disk I/O layer

// PSRAM base address for RP2350B (memory-mapped)
#define PSRAM_BASE 0x11000000

// PSRAM Memory Layout (8MB total):
// Zone 1 (0-64KB):    FatFS working buffers (current ~30KB, room to grow)
// Zone 2 (64-128KB):  HTTP buffers (current 4KB, can grow to 64KB)
// Zone 3 (128KB+):    Future use (OTA, caching, temp files) - ~7.8MB available
//
// FatFS file/directory handles in PSRAM (memory-mapped, no linker support needed)
// Note: FATFS filesystem object is in sd_card_t struct, NOT in PSRAM
#define FILE_BUFFER_PSRAM  ((uint8_t*)(PSRAM_BASE + 0x00001000)) // 4KB buffer at 4KB
#define DIR_BUFFER_PSRAM   ((DIR*)(PSRAM_BASE + 0x00002000))     // 32 bytes at 8KB
#define FIL_PSRAM_1        ((FIL*)(PSRAM_BASE + 0x00004000))     // 560 bytes at 16KB
#define FIL_PSRAM_2        ((FIL*)(PSRAM_BASE + 0x00005000))     // 560 bytes at 20KB
#define DIR_PSRAM_LIST     ((DIR*)(PSRAM_BASE + 0x00006000))     // 32 bytes at 24KB
#define FILINFO_PSRAM_1    ((FILINFO*)(PSRAM_BASE + 0x00007000)) // 320 bytes at 28KB
#define FILINFO_PSRAM_2    ((FILINFO*)(PSRAM_BASE + 0x00007200)) // 320 bytes at 28.5KB
#define FILINFO_PSRAM_3    ((FILINFO*)(PSRAM_BASE + 0x00007400)) // 320 bytes at 29KB

static bool mounted = false;

/**
 * Initialize SD card hardware and mount filesystem
 * 
 * Note: sd_init_driver() is called automatically by the library when needed.
 * We just call f_mount() directly as shown in the library examples.
 */
bool sd_card_init(void) {
    printf("[SD Card] Initializing SD card...\n");
    
    // Get the SD card object (defined in hw_config.c)
    sd_card_t *pSD = sd_get_by_num(0);
    if (!pSD) {
        printf("[SD Card] ERROR: Failed to get SD card object\n");
        return false;
    }
    
    // Mount filesystem using the FATFS member of sd_card_t
    // The library will automatically call sd_init_driver() when needed
    FRESULT fr = f_mount(&pSD->fatfs, pSD->pcName, 1);
    if (fr != FR_OK) {
        printf("[SD Card] ERROR: Mount failed (FRESULT=%d)\n", fr);
        return false;
    }
    
    printf("[SD Card] Filesystem mounted\n");
    
    // Try to access the card to verify it's present (with quick fail if not)
    FATFS *fs_ptr;
    DWORD free_clusters;
    fr = f_getfree(pSD->pcName, &free_clusters, &fs_ptr);
    if (fr != FR_OK) {
        printf("[SD Card] No card detected or card not formatted (FRESULT=%d) - skipping\n", fr);
        return false;  // Not an error - just no card present
    }
    
    printf("[SD Card] Card detected and mounted successfully\n");
    pSD->mounted = true;
    mounted = true;
    
    // Get volume information
    uint64_t total_bytes = (uint64_t)(fs_ptr->n_fatent - 2) * fs_ptr->csize * 512;
    uint64_t free_bytes = (uint64_t)free_clusters * fs_ptr->csize * 512;
    printf("[SD Card] Capacity: %llu MB, Free: %llu MB\n",
           total_bytes / (1024*1024), free_bytes / (1024*1024));
    
    return true;
}

/**
 * Write data to file (creates or overwrites)
 */
bool sd_card_write_file(const char* filename, const void* data, size_t size) {
    if (!mounted) {
        printf("[SD Card] ERROR: Not mounted\n");
        return false;
    }
    
    // Extract directory path and ensure it exists
    char dirpath[256];
    strncpy(dirpath, filename, sizeof(dirpath) - 1);
    dirpath[sizeof(dirpath) - 1] = '\0';
    
    // Find last slash to get directory path
    char* last_slash = strrchr(dirpath, '/');
    if (last_slash != NULL && last_slash != dirpath) {
        *last_slash = '\0';  // Terminate at last slash to get directory
        // Try to ensure directory exists (ignore failure - might already exist)
        // Skip if dirpath is empty (root directory)
        if (strlen(dirpath) > 0) {
            sd_card_create_directory(dirpath);
        }
    }
    
    FIL* file = FIL_PSRAM_1;  // Use PSRAM buffer
    FRESULT fr = f_open(file, filename, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        printf("[SD Card] ERROR: Failed to open %s for writing (FRESULT=%d)\n", filename, fr);
        return false;
    }
    
    UINT bytes_written;
    fr = f_write(file, data, size, &bytes_written);
    f_close(file);  // f_close() automatically calls f_sync()
    
    if (fr != FR_OK || bytes_written != size) {
        printf("[SD Card] ERROR: Write failed (FRESULT=%d, wrote %u/%zu bytes)\n",
               fr, bytes_written, size);
        return false;
    }
    
    printf("[SD Card] Wrote %zu bytes to %s\n", size, filename);
    sleep_ms(10);  // Give SD card time to complete write
    return true;
}

/**
 * Read entire file into buffer
 */
bool sd_card_read_file(const char* filename, uint8_t** buffer, size_t* size) {
    if (!mounted) {
        printf("[SD Card] ERROR: Not mounted\n");
        return false;
    }
    
    FIL* file = FIL_PSRAM_2;  // Use PSRAM buffer (different from write)
    FRESULT fr = f_open(file, filename, FA_READ);
    if (fr != FR_OK) {
        printf("[SD Card] ERROR: Failed to open %s for reading (FRESULT=%d)\n", filename, fr);
        return false;
    }
    
    // Get file size
    FSIZE_t file_size = f_size(file);
    
    // Allocate buffer
    *buffer = (uint8_t*)malloc(file_size);
    if (*buffer == NULL) {
        printf("[SD Card] ERROR: Failed to allocate %llu bytes for %s\n", file_size, filename);
        f_close(file);
        return false;
    }
    
    // Read entire file
    UINT bytes_read;
    fr = f_read(file, *buffer, file_size, &bytes_read);
    f_close(file);
    
    if (fr != FR_OK || bytes_read != file_size) {
        printf("[SD Card] ERROR: Read failed (FRESULT=%d, read %u/%llu bytes)\n",
               fr, bytes_read, file_size);
        free(*buffer);
        *buffer = NULL;
        return false;
    }
    
    *size = file_size;
    printf("[SD Card] Read %zu bytes from %s\n", *size, filename);
    sleep_ms(10);  // Give SD card time to settle
    return true;
}

/**
 * Check if file exists
 */
bool sd_card_file_exists(const char* filename) {
    if (!mounted) return false;
    
    FILINFO* fno = FILINFO_PSRAM_1;
    FRESULT fr = f_stat(filename, fno);
    return (fr == FR_OK);
}

/**
 * Delete file
 */
bool sd_card_delete_file(const char* filename) {
    if (!mounted) {
        printf("[SD Card] ERROR: Not mounted\n");
        return false;
    }
    
    FRESULT fr = f_unlink(filename);
    if (fr != FR_OK) {
        printf("[SD Card] ERROR: Failed to delete %s (FRESULT=%d)\n", filename, fr);
        return false;
    }
    
    printf("[SD Card] Deleted %s\n", filename);
    return true;
}

/**
 * List files in directory
 * 
 * CRITICAL: Uses stack-allocated DIR/FILINFO with f_findfirst/f_findnext pattern.
 * This is the ONLY reliable pattern for RP2350B + PSRAM + FatFS.
 * 
 * DO NOT use PSRAM buffers for DIR/FILINFO - causes timing issues and hangs.
 * See carlk3/no-OS-FatFS f_util.c ls() function for reference implementation.
 * 
 * Filtering Strategy:
 * - Skips directories (AM_DIR attribute)
 * - Skips hidden/system files (AM_HID, AM_SYS attributes)  
 * - Skips dot files (., .., .hidden)
 * - Validates first character is alphanumeric/underscore
 * - Rejects files >100MB (likely corrupt FAT entries)
 * 
 * These filters are essential for handling SD cards with filesystem corruption.
 */
int sd_card_list_directory(const char* dirpath, void (*callback)(const char* name, size_t size)) {
    if (!mounted) {
        printf("[SD Card] ERROR: Not mounted\n");
        return -1;
    }
    
    // Use stack allocation with proper initialization (per reference implementation)
    // The = {} initializer is critical - ensures all fields start at zero
    DIR dir = {};
    FILINFO fno = {};
    FRESULT fr;
    int count = 0;
    int max_entries = 1000;  // Safety limit to prevent infinite loops on corrupt cards
    
    // Use f_findfirst for proper initialization (reference implementation pattern)
    // This is MORE reliable than f_opendir + f_readdir for directory iteration
    fr = f_findfirst(&dir, &fno, dirpath, "*");
    if (fr != FR_OK) {
        printf("[SD Card] ERROR: Failed to open directory %s (FRESULT=%d)\n", dirpath, fr);
        return -1;
    }
    
    // Loop until no more entries or safety limit reached
    // Check both fr==FR_OK and fno.fname[0] (empty name = end of directory)
    while (fr == FR_OK && fno.fname[0] && max_entries-- > 0) {
        
        // Skip directories
        if (fno.fattrib & AM_DIR) {
            fr = f_findnext(&dir, &fno);
            continue;
        }
        
        // Skip hidden/system files and dot files
        if (fno.fattrib & (AM_HID | AM_SYS)) {
            fr = f_findnext(&dir, &fno);
            continue;
        }
        if (fno.fname[0] == '.') {
            fr = f_findnext(&dir, &fno);
            continue;
        }
        
        // Validate filename: skip if first char is not alphanumeric/underscore
        char first = fno.fname[0];
        if (!((first >= 'A' && first <= 'Z') || 
              (first >= 'a' && first <= 'z') || 
              (first >= '0' && first <= '9') ||
              first == '_')) {
            fr = f_findnext(&dir, &fno);
            continue;  // Skip invalid filenames
        }
        
        // Skip unreasonably large file sizes (>100MB suggests corruption)
        if (fno.fsize > 100*1024*1024) {
            fr = f_findnext(&dir, &fno);
            continue;
        }
        
        // Valid file entry
        if (callback) {
            callback(fno.fname, fno.fsize);
        }
        count++;
        
        // Get next entry
        fr = f_findnext(&dir, &fno);
    }
    
    f_closedir(&dir);
    return count;
}

/**
 * Create directory if it doesn't exist
 */
bool sd_card_create_directory(const char* dirpath) {
    if (!mounted) {
        printf("[SD Card] ERROR: Not mounted\n");
        return false;
    }
    
    // Check if directory already exists
    FILINFO* fno = FILINFO_PSRAM_3;
    FRESULT fr = f_stat(dirpath, fno);
    if (fr == FR_OK) {
        if (fno->fattrib & AM_DIR) {
            // Directory already exists - success
            return true;
        } else {
            // Path exists but is a file
            printf("[SD Card] ERROR: %s exists but is not a directory\n", dirpath);
            return false;
        }
    }
    
    // Create directory
    fr = f_mkdir(dirpath);
    if (fr == FR_OK || fr == FR_EXIST) {
        // Success or already exists
        return true;
    }
    
    printf("[SD Card] ERROR: Failed to create directory %s (FRESULT=%d)\n", dirpath, fr);
    return false;
}

/**
 * Get free space on SD card
 */
uint64_t sd_card_get_free_space(void) {
    if (!mounted) return 0;
    
    FATFS *fs_ptr;
    DWORD free_clusters;
    FRESULT fr = f_getfree("", &free_clusters, &fs_ptr);
    
    if (fr != FR_OK) {
        printf("[SD Card] WARNING: f_getfree failed (FRESULT=%d)\n", fr);
        return 0;
    }
    
    return (uint64_t)free_clusters * fs_ptr->csize * 512;
}

/**
 * Check if SD card is mounted
 */
bool sd_card_is_mounted(void) {
    return mounted;
}
