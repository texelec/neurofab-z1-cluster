/**
 * PSRAM Driver for RP2350 (8MB QSPI PSRAM)
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

// PSRAM is mapped into RP2350's XIP address space
#define PSRAM_BASE_ADDR  ((volatile uint8_t*)0x11000000)

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
    
    // Wait for cooldown and let clock settle
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {}
    sleep_us(10);
    
    // Exit QPI quad mode first
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS | QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB | PSRAM_CMD_QUAD_END;
    while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) != 0) {}
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

void psram_write(uint32_t addr, const uint8_t* data, uint32_t len) {
    if (!psram_initialized) {
        printf("[PSRAM] ERROR: psram_write() called but PSRAM not initialized!\n");
        return;
    }
    if (addr + len > PSRAM_SIZE_BYTES) {
        printf("[PSRAM] ERROR: psram_write() addr=0x%08lX len=%lu exceeds size %lu\n", 
               addr, len, PSRAM_SIZE_BYTES);
        return;
    }
    
    volatile uint8_t* dest = PSRAM_BASE_ADDR + addr;
    for (uint32_t i = 0; i < len; i++) {
        dest[i] = data[i];
    }
}

void psram_read(uint32_t addr, uint8_t* data, uint32_t len) {
    if (!psram_initialized || addr + len > PSRAM_SIZE_BYTES) return;
    volatile uint8_t* src = PSRAM_BASE_ADDR + addr;
    for (uint32_t i = 0; i < len; i++) {
        data[i] = src[i];
    }
}

void psram_write_word(uint32_t addr, uint32_t value) {
    if (!psram_initialized || addr + sizeof(uint32_t) > PSRAM_SIZE_BYTES) return;
    *((volatile uint32_t*)(PSRAM_BASE_ADDR + addr)) = value;
}

uint32_t psram_read_word(uint32_t addr) {
    if (!psram_initialized || addr + sizeof(uint32_t) > PSRAM_SIZE_BYTES) return 0;
    return *((volatile uint32_t*)(PSRAM_BASE_ADDR + addr));
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
