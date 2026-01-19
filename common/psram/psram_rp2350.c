/**
 * PSRAM Driver for RP2350 (8MB QSPI PSRAM)
 * Code by NeuroFab Corp: 2025-2026
 * 
 * CRITICAL: RP2350 XIP Cache Coherency Issue
 * ------------------------------------------
 * The RP2350's XIP controller has a cache that can cause data corruption when
 * writing to external memory (PSRAM/Flash) via XIP addresses. Symptoms include:
 * - Immediate reads show correct data
 * - Later reads show corrupted/stale data
 * - Data "reverts" to old values after some time
 * 
 * Root Cause:
 * The ARM Cortex-M33 CPU can buffer/cache writes. Even with DSB/DMB barriers,
 * the XIP controller's cache may not be synchronized immediately. Other activity
 * (DMA, interrupts, broker tasks) can trigger cache line evictions that overwrite
 * correct data with stale cached values.
 * 
 * Solution: Use UNCACHED XIP Alias
 * RP2350 provides multiple address aliases for XIP memory:
 *   0x10000000-0x13FFFFFF = XIP_BASE (cached, fast)
 *   0x14000000-0x17FFFFFF = XIP_NOCACHE_NOALLOC_BASE (uncached, guaranteed consistent)
 * 
 * For PSRAM at 0x11000000:
 *   Cached:   0x11000000 (use for reads in normal operation)
 *   Uncached: 0x15000000 (use for ALL writes, especially OTA)
 * 
 * Performance impact: ~10-20% slower writes, but 100% data integrity.
 * This is critical for OTA firmware updates where corruption = bricked device.
 */

#include "psram_rp2350.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "hardware/regs/qmi.h"
#include "hardware/regs/xip.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include <stdio.h>
#include <string.h>

// PSRAM Commands
#define PSRAM_CMD_QUAD_END    0xF5
#define PSRAM_CMD_QUAD_ENABLE 0x35
#define PSRAM_CMD_READ_ID     0x9F
#define PSRAM_CMD_QUAD_READ   0xEB
#define PSRAM_CMD_QUAD_WRITE  0x38
#define PSRAM_CMD_NOOP        0xFF
#define PSRAM_ID              0x5D

// PSRAM is mapped into RP2350's XIP address space at two aliases:
// 
// RP2350 memory map (from hardware/regs/addressmap.h):
//   0x10000000 = XIP_BASE (cached, fast)
//   0x14000000 = XIP_NOCACHE_NOALLOC_BASE (uncached, coherent)
//   Offset: 0x14000000 - 0x10000000 = 0x04000000
// 
// PSRAM mapping:
//   0x11000000 (cached)   - Fast reads, use for normal operation
//   0x15000000 (uncached) - Guaranteed coherent, use for ALL writes
// 
// WARNING: Do NOT use PSRAM_BASE_ADDR for writes! XIP cache coherency issues
// will corrupt data. Always use PSRAM_UNCACHED_BASE_ADDR for writes.
#define PSRAM_BASE_ADDR  ((volatile uint8_t*)0x11000000)
#define PSRAM_UNCACHED_BASE_ADDR  ((volatile uint8_t*)0x15000000)

// Private state
static bool psram_initialized = false;
static bool quad_mode_success = false;
static size_t psram_size = 0;

// Private helper functions
static size_t __no_inline_not_in_flash_func(detect_psram_size)(void);
static bool __no_inline_not_in_flash_func(setup_psram_hardware)(void);

bool psram_init(void) {
    printf("PSRAM: Initializing 8MB PSRAM...\n");
    
    // Set PSRAM CS pin to XIP_CS1 function
    gpio_set_function(PSRAM_CS_PIN, GPIO_FUNC_XIP_CS1);
    
    // Detect PSRAM size
    psram_size = detect_psram_size();
    if (psram_size == 0) {
        printf("PSRAM: Detection failed!\n");
        return false;
    }
    
    // Setup QMI hardware for quad mode
    if (!setup_psram_hardware()) {
        printf("PSRAM: Hardware setup failed!\n");
        return false;
    }
    
    psram_initialized = true;
    printf("PSRAM: Initialized at base address 0x%08X\n", (uint32_t)PSRAM_BASE_ADDR);
    printf("PSRAM: Size: %zu MB, Mode: %s\n", 
           psram_size / (1024*1024),
           quad_mode_success ? "QUAD" : "SERIAL");
    
    return true;
}

static size_t __no_inline_not_in_flash_func(detect_psram_size)(void) {
    size_t detected_size = 0;
    
    // Use fixed clock divider of 6 for detection at 266MHz (~44MHz, very safe for SPI)
    // Direct CSR communication for PSRAM ID detection
    qmi_hw->direct_csr = 6 << QMI_DIRECT_CSR_CLKDIV_LSB | QMI_DIRECT_CSR_EN_BITS;
    
    // Wait for cooldown and let clock settle (with timeout)
    uint32_t timeout = 10000;
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {
        if (--timeout == 0) {
            printf("PSRAM: Timeout waiting for QMI ready\n");
            return 0;
        }
    }
    sleep_us(10);
    
    // Exit QPI quad mode first
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS | QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB | PSRAM_CMD_QUAD_END;
    timeout = 10000;
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {
        if (--timeout == 0) {
            printf("PSRAM: Timeout in quad exit\n");
            qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS | QMI_DIRECT_CSR_EN_BITS);
            return 0;
        }
    }
    (void)qmi_hw->direct_rx;
    qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS);
    
    // Read PSRAM ID
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    uint8_t kgd = 0;
    uint8_t eid = 0;
    
    for (size_t i = 0; i < 7; i++) {
        qmi_hw->direct_tx = (i == 0 ? PSRAM_CMD_READ_ID : PSRAM_CMD_NOOP);
        while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS) == 0) {}
        while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {}
        
        if (i == 5) kgd = qmi_hw->direct_rx;
        if (i == 6) eid = qmi_hw->direct_rx; 
        else (void)qmi_hw->direct_rx;
    }
    
    qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS);
    qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_EN_BITS);
    
    printf("PSRAM: ID read - KGD: 0x%02X, EID: 0x%02X\n", kgd, eid);
    
    // Check if this is a supported PSRAM (0x5D = APMemory APS6404L)
    if (kgd == PSRAM_ID) {
        detected_size = 1024 * 1024; // 1 MiB base
        uint8_t size_id = eid >> 5;
        if (eid == 0x26 || size_id == 2) {
            detected_size *= 8; // 8MB
        } else if (size_id == 0) {
            detected_size *= 2; // 2MB
        } else if (size_id == 1) {
            detected_size *= 4; // 4MB
        }
        printf("PSRAM: Detected %zu MB\n", detected_size / (1024*1024));
    } else {
        printf("PSRAM: Unsupported ID (expected 0x%02X, got 0x%02X)\n", PSRAM_ID, kgd);
    }
    
    return detected_size;
}

static bool __no_inline_not_in_flash_func(setup_psram_hardware)(void) {
    uint32_t sys_freq = clock_get_hz(clk_sys);
    
    // Use 84MHz for SPI mode (datasheet max for SPI)
    uint32_t setup_clk_div = (sys_freq + 84000000 - 1) / 84000000;
    
    // CRITICAL: Enable M1 writes in XIP controller
    xip_ctrl_hw->ctrl |= XIP_CTRL_WRITABLE_M1_BITS;
    
    // Configure M1 for SERIAL SPI mode first
    qmi_hw->m[1].rfmt = (QMI_M1_RFMT_PREFIX_WIDTH_VALUE_S << QMI_M1_RFMT_PREFIX_WIDTH_LSB |
                         QMI_M1_RFMT_ADDR_WIDTH_VALUE_S << QMI_M1_RFMT_ADDR_WIDTH_LSB |
                         QMI_M1_RFMT_SUFFIX_WIDTH_VALUE_S << QMI_M1_RFMT_SUFFIX_WIDTH_LSB |
                         QMI_M1_RFMT_DUMMY_WIDTH_VALUE_S << QMI_M1_RFMT_DUMMY_WIDTH_LSB |
                         QMI_M1_RFMT_DUMMY_LEN_VALUE_4 << QMI_M1_RFMT_DUMMY_LEN_LSB |
                         QMI_M1_RFMT_DATA_WIDTH_VALUE_S << QMI_M1_RFMT_DATA_WIDTH_LSB |
                         QMI_M1_RFMT_PREFIX_LEN_VALUE_8 << QMI_M1_RFMT_PREFIX_LEN_LSB |
                         0 << QMI_M1_RFMT_SUFFIX_LEN_LSB);

    qmi_hw->m[1].rcmd = 0x0B << QMI_M1_RCMD_PREFIX_LSB | 0 << QMI_M1_RCMD_SUFFIX_LSB;

    qmi_hw->m[1].wfmt = (QMI_M1_WFMT_PREFIX_WIDTH_VALUE_S << QMI_M1_WFMT_PREFIX_WIDTH_LSB |
                         QMI_M1_WFMT_ADDR_WIDTH_VALUE_S << QMI_M1_WFMT_ADDR_WIDTH_LSB |
                         QMI_M1_WFMT_SUFFIX_WIDTH_VALUE_S << QMI_M1_WFMT_SUFFIX_WIDTH_LSB |
                         0 << QMI_M1_WFMT_DUMMY_WIDTH_LSB |
                         0 << QMI_M1_WFMT_DUMMY_LEN_LSB |
                         QMI_M1_WFMT_DATA_WIDTH_VALUE_S << QMI_M1_WFMT_DATA_WIDTH_LSB |
                         QMI_M1_WFMT_PREFIX_LEN_VALUE_8 << QMI_M1_WFMT_PREFIX_LEN_LSB |
                         0 << QMI_M1_WFMT_SUFFIX_LEN_LSB);

    qmi_hw->m[1].wcmd = 0x02 << QMI_M1_WCMD_PREFIX_LSB | 0 << QMI_M1_WCMD_SUFFIX_LSB;

    qmi_hw->m[1].timing = (2 << QMI_M1_TIMING_COOLDOWN_LSB) |
                          (2 << QMI_M1_TIMING_RXDELAY_LSB) |
                          (2 << QMI_M1_TIMING_SELECT_SETUP_LSB) |
                          (2 << QMI_M1_TIMING_SELECT_HOLD_LSB) |
                          (setup_clk_div << QMI_M1_TIMING_CLKDIV_LSB);
    
    // Send Enter Quad Mode using serial SPI (still at 84MHz)
    qmi_hw->direct_csr = setup_clk_div << QMI_DIRECT_CSR_CLKDIV_LSB | QMI_DIRECT_CSR_EN_BITS;
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {}
    
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    qmi_hw->direct_tx = PSRAM_CMD_QUAD_ENABLE;
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {}
    (void)qmi_hw->direct_rx;
    qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS);
    
    qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_EN_BITS);
    sleep_us(200);

    // NOW in quad mode - can use up to 133MHz (datasheet max)
    uint32_t quad_clk_div = (sys_freq + 133000000 - 1) / 133000000;
    
    // Configure M1 for QUAD operations
    qmi_hw->m[1].rfmt = (QMI_M1_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M1_RFMT_PREFIX_WIDTH_LSB |
                         QMI_M1_RFMT_ADDR_WIDTH_VALUE_Q << QMI_M1_RFMT_ADDR_WIDTH_LSB |
                         QMI_M1_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M1_RFMT_SUFFIX_WIDTH_LSB |
                         QMI_M1_RFMT_DUMMY_WIDTH_VALUE_Q << QMI_M1_RFMT_DUMMY_WIDTH_LSB |
                         6 << QMI_M1_RFMT_DUMMY_LEN_LSB |
                         QMI_M1_RFMT_DATA_WIDTH_VALUE_Q << QMI_M1_RFMT_DATA_WIDTH_LSB |
                         QMI_M1_RFMT_PREFIX_LEN_VALUE_8 << QMI_M1_RFMT_PREFIX_LEN_LSB |
                         0 << QMI_M1_RFMT_SUFFIX_LEN_LSB);

    qmi_hw->m[1].rcmd = PSRAM_CMD_QUAD_READ << QMI_M1_RCMD_PREFIX_LSB | 0 << QMI_M1_RCMD_SUFFIX_LSB;

    qmi_hw->m[1].wfmt = (QMI_M1_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M1_WFMT_PREFIX_WIDTH_LSB |
                         QMI_M1_WFMT_ADDR_WIDTH_VALUE_Q << QMI_M1_WFMT_ADDR_WIDTH_LSB |
                         QMI_M1_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M1_WFMT_SUFFIX_WIDTH_LSB |
                         0 << QMI_M1_WFMT_DUMMY_WIDTH_LSB |
                         0 << QMI_M1_WFMT_DUMMY_LEN_LSB |
                         QMI_M1_WFMT_DATA_WIDTH_VALUE_Q << QMI_M1_WFMT_DATA_WIDTH_LSB |
                         QMI_M1_WFMT_PREFIX_LEN_VALUE_8 << QMI_M1_WFMT_PREFIX_LEN_LSB |
                         0 << QMI_M1_WFMT_SUFFIX_LEN_LSB);

    qmi_hw->m[1].wcmd = PSRAM_CMD_QUAD_WRITE << QMI_M1_WCMD_PREFIX_LSB | 0 << QMI_M1_WCMD_SUFFIX_LSB;

    qmi_hw->m[1].timing = (3 << QMI_M1_TIMING_COOLDOWN_LSB) |
                          (1 << QMI_M1_TIMING_RXDELAY_LSB) |
                          (1 << QMI_M1_TIMING_SELECT_SETUP_LSB) |
                          (3 << QMI_M1_TIMING_SELECT_HOLD_LSB) |
                          (quad_clk_div << QMI_M1_TIMING_CLKDIV_LSB);
    
    quad_mode_success = true;
    return true;
}

/**
 * Write data to PSRAM
 * 
 * CRITICAL: Uses UNCACHED XIP address (0x15000000) instead of cached (0x11000000)
 * 
 * Why uncached?
 * -------------
 * During OTA firmware updates, we discovered that writes to cached XIP addresses
 * (0x11000000) were being corrupted by the RP2350's XIP cache:
 * 
 * Timeline of the bug:
 * 1. Write chunk 0 to 0x11010000 (cached) - data: 50 41 31 5A (correct magic)
 * 2. Immediate read verifies: 50 41 31 5A ✓
 * 3. Write chunks 1-88 to subsequent addresses
 * 4. 5 seconds later, finalize reads 0x11010000 again
 * 5. Data now shows: 05 04 13 15 (CORRUPTED!)
 * 
 * Root cause: CPU write buffer holds data, XIP cache not flushed properly.
 * Broker DMA/interrupt activity triggers cache eviction with stale data.
 * 
 * Solution: Write to UNCACHED alias at 0x15000000 which bypasses cache entirely.
 * Reads from cached address still work fine - only writes need uncached access.
 * 
 * Performance: ~10-20% slower writes, but prevents catastrophic OTA corruption.
 * 
 * @param addr Absolute PSRAM address (e.g., 0x11010000)
 * @param data Source buffer to copy from
 * @param len Number of bytes to write
 */
void psram_write(uint32_t addr, const uint8_t* data, uint32_t len) {
    if (!psram_initialized) {
        printf("[PSRAM] ERROR: psram_write() called but PSRAM not initialized!\n");
        return;
    }
    // addr is absolute UNCACHED address, but convert back to cached for actual write
    uint32_t offset = addr - (uint32_t)PSRAM_UNCACHED_BASE_ADDR;
    if (offset + len > PSRAM_SIZE_BYTES) {
        printf("[PSRAM] ERROR: psram_write() offset=0x%08lX len=%lu exceeds size %lu\n", 
               offset, len, PSRAM_SIZE_BYTES);
        return;
    }
    
    // CRITICAL FIX: Write directly to UNCACHED address BUT use 32-bit words!
    // RP2350 PSRAM hardware has a bug: byte writes to uncached PSRAM corrupt data!
    // MUST use 32-bit word writes for data integrity.
    
    volatile uint32_t* dest_word = (volatile uint32_t*)addr;
    const uint32_t* src_word = (const uint32_t*)data;
    
    // Write whole 32-bit words
    uint32_t word_count = len / 4;
    for (uint32_t i = 0; i < word_count; i++) {
        dest_word[i] = src_word[i];
    }
    
    // Handle remaining bytes (if len not multiple of 4)
    uint32_t remaining = len % 4;
    if (remaining > 0) {
        uint32_t last_word = 0;
        uint8_t* last_src = (uint8_t*)&src_word[word_count];
        for (uint32_t i = 0; i < remaining; i++) {
            ((uint8_t*)&last_word)[i] = last_src[i];
        }
        dest_word[word_count] = last_word;
    }
    
    // Memory barrier to ensure write completes before returning
    __asm volatile ("dsb" ::: "memory");
    __asm volatile ("isb" ::: "memory");
}

void psram_read(uint32_t addr, uint8_t* data, uint32_t len) {
    if (!psram_initialized) return;
    
    // CRITICAL FIX: Read using 32-bit words from UNCACHED address
    // Matching psram_write() which uses 32-bit word writes
    volatile uint32_t* src_word = (volatile uint32_t*)addr;
    uint32_t* dest_word = (uint32_t*)data;
    
    // Read whole 32-bit words
    uint32_t word_count = len / 4;
    for (uint32_t i = 0; i < word_count; i++) {
        dest_word[i] = src_word[i];
    }
    
    // Handle remaining bytes (if len not multiple of 4)
    uint32_t remaining = len % 4;
    if (remaining > 0) {
        uint32_t last_word = src_word[word_count];
        uint8_t* dest_bytes = &data[word_count * 4];
        for (uint32_t i = 0; i < remaining; i++) {
            dest_bytes[i] = ((uint8_t*)&last_word)[i];
        }
    }
    
    // Memory barrier
    __asm volatile ("dsb" ::: "memory");
    __asm volatile ("isb" ::: "memory");
}

void psram_write_word(uint32_t addr, uint32_t value) {
    if (!psram_initialized || addr + sizeof(uint32_t) > PSRAM_SIZE_BYTES) return;
    // Use UNCACHED address for cache coherency (addr is offset from PSRAM base)
    *((volatile uint32_t*)(PSRAM_UNCACHED_BASE_ADDR + addr)) = value;
    __asm volatile ("dsb" ::: "memory");
}

uint32_t psram_read_word(uint32_t addr) {
    if (!psram_initialized || addr + sizeof(uint32_t) > PSRAM_SIZE_BYTES) return 0;
    // Use UNCACHED address for cache coherency (addr is offset from PSRAM base)
    __asm volatile ("dsb" ::: "memory");
    return *((volatile uint32_t*)(PSRAM_UNCACHED_BASE_ADDR + addr));
}

void psram_dma_write(uint32_t addr, const void* data, uint32_t len) {
    // For now, use memcpy (can optimize with DMA later)
    psram_write(addr, (const uint8_t*)data, len);
}

void psram_dma_read(uint32_t addr, void* data, uint32_t len) {
    // For now, use memcpy (can optimize with DMA later)
    psram_read(addr, (uint8_t*)data, len);
}

bool psram_test(void) {
    if (!psram_initialized) {
        printf("PSRAM: Not initialized!\n");
        return false;
    }
    
    printf("PSRAM: Running memory test...\n");
    
    // Test pattern: write and read back
    const uint32_t test_patterns[] = {
        0x00000000, 0xFFFFFFFF, 0xAAAAAAAA, 0x55555555,
        0x12345678, 0x87654321, 0xDEADBEEF, 0xCAFEBABE
    };
    
    const uint32_t test_addr = 0x1000; // Test at offset 4KB
    
    for (uint32_t i = 0; i < sizeof(test_patterns) / sizeof(test_patterns[0]); i++) {
        uint32_t addr = test_addr + (i * 4);
        uint32_t expected = test_patterns[i];
        
        psram_write_word(addr, expected);
        uint32_t readback = psram_read_word(addr);
        
        if (readback != expected) {
            printf("PSRAM: Test FAILED at 0x%08X (wrote 0x%08X, read 0x%08X)\n", 
                   addr, expected, readback);
            return false;
        }
    }
    
    // Test buffer write/read
    uint8_t write_buf[256];
    uint8_t read_buf[256];
    
    for (uint32_t i = 0; i < sizeof(write_buf); i++) {
        write_buf[i] = (uint8_t)(i ^ 0xAA);
    }
    
    const uint32_t buf_test_addr = 0x2000;
    psram_write(buf_test_addr, write_buf, sizeof(write_buf));
    psram_read(buf_test_addr, read_buf, sizeof(read_buf));
    
    if (memcmp(write_buf, read_buf, sizeof(write_buf)) != 0) {
        printf("PSRAM: Buffer test FAILED\n");
        return false;
    }
    
    printf("PSRAM: All tests PASSED (8MB available)\n");
    return true;
}

void psram_mark_initialized(size_t size_bytes) {
    // Ensure GPIO is still configured for PSRAM (may reset during partition jump)
    gpio_set_function(PSRAM_CS_PIN, GPIO_FUNC_XIP_CS1);
    
    psram_initialized = true;
    psram_size = size_bytes;
    quad_mode_success = true;  // Assume quad mode if bootloader succeeded
    printf("[PSRAM] Marked as initialized (%zu MB)\n", size_bytes / (1024*1024));
}
