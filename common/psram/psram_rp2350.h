/**
 * PSRAM Driver for RP2350 (8MB QSPI PSRAM)
 * Code by NeuroFab Corp: 2025-2026
 * 
 * Hardware Configuration:
 * - CS: GPIO 47 (dedicated PSRAM chip select)
 * - QSPI Data/Clock: Shared with Flash ROM (hardware multiplexed by RP2350)
 * - Interface: QMI (Quad SPI Memory Interface)
 * - Base Address: 0x11000000 (fixed in RP2350 memory map)
 * 
 * Tested and working with RP2350B
 * CRITICAL: Apply RP2350-E5 DMA errata workaround when using DMA transfers
 */

#ifndef PSRAM_RP2350_H
#define PSRAM_RP2350_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// PSRAM Hardware Configuration
#define PSRAM_CS_PIN      47  // GPIO 47 - dedicated PSRAM chip select
#define PSRAM_SIZE_BYTES  (8 * 1024 * 1024)  // 8MB

// Initialize PSRAM (QSPI mode)
bool psram_init(void);

// Basic read/write operations
void psram_write(uint32_t addr, const uint8_t* data, uint32_t len);
void psram_read(uint32_t addr, uint8_t* data, uint32_t len);

// Word operations for efficiency
void psram_write_word(uint32_t addr, uint32_t value);
uint32_t psram_read_word(uint32_t addr);

// DMA transfers (apply RP2350-E5 workaround internally)
void psram_dma_write(uint32_t addr, const void* data, uint32_t len);
void psram_dma_read(uint32_t addr, void* data, uint32_t len);

// Memory test
bool psram_test(void);

// Mark PSRAM as initialized (for app partition after bootloader init)
void psram_mark_initialized(size_t size_bytes);

#endif // PSRAM_RP2350_H
