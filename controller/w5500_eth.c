/**
 * W5500 Ethernet Library
 * Code by NeuroFab Corp: 2025-2026
 * 
 * Simplified HTTP server for Z1 Onyx Controller
 */

#include "w5500_eth.h"
#include "z1_http_api.h"
#include "controller_pins.h"
#include "../common/z1_broker/z1_broker.h"  // For z1_broker_send_command
#include "../common/z1_commands/z1_commands.h"  // For OPCODE_STOP_SNN/START_SNN
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "hardware/watchdog.h"
#include "ff.h"  // FatFS for streaming file uploads
#include <stdio.h>
#include <string.h>
#include <stdlib.h>  // For atoi()

// HTTP response buffer in PSRAM (shared with z1_http_api.c)
// HTTP zone: 64KB-128KB (allows growth to 64KB if needed)
// FatFS zone: 0-64KB (reserved for filesystem structures)
#define HTTP_BUFFER_PSRAM ((char*)(0x11010000))
#define HTTP_BUFFER_SIZE 16384
char* http_response_buffer = HTTP_BUFFER_PSRAM;

// ============================================================================
// Network Configuration - Loaded from z1.cfg at startup via zconfig tool
// ============================================================================
static uint8_t MAC_ADDRESS[6] = {0x02, 0x5A, 0x31, 0xC3, 0xD4, 0x01};  // Default MAC (locally administered)
static uint8_t IP_ADDRESS[4]  = {192, 168, 1, 222};                    // Default IP
static const uint8_t SUBNET_MASK[4] = {255, 255, 255, 0};              // /24 network (255.255.255.0)
static const uint8_t GATEWAY[4]     = {0, 0, 0, 0};                    // No gateway (local network only)

// Set network configuration from config file (must be called before w5500_eth_init)
void w5500_set_network_config(const uint8_t* ip, const uint8_t* mac) {
    if (ip) {
        memcpy(IP_ADDRESS, ip, 4);
        printf("[W5500] IP address set to %d.%d.%d.%d\n",
               IP_ADDRESS[0], IP_ADDRESS[1], IP_ADDRESS[2], IP_ADDRESS[3]);
    }
    if (mac) {
        memcpy(MAC_ADDRESS, mac, 6);
        printf("[W5500] MAC address set to %02X:%02X:%02X:%02X:%02X:%02X\n",
               MAC_ADDRESS[0], MAC_ADDRESS[1], MAC_ADDRESS[2],
               MAC_ADDRESS[3], MAC_ADDRESS[4], MAC_ADDRESS[5]);
    }
}

// Helper function to get IP address as string (used by display and HTTP)
const char* w5500_get_ip_string(void) {
    static char ip_str[16];
    snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d", 
             IP_ADDRESS[0], IP_ADDRESS[1], IP_ADDRESS[2], IP_ADDRESS[3]);
    return ip_str;
}

// Helper function to get MAC address (used by display and HTTP)
const uint8_t* w5500_get_mac_address(void) {
    return MAC_ADDRESS;
}

// W5500 Register Addresses
#define W5500_MR         0x0000
#define W5500_GAR0       0x0001
#define W5500_SUBR0      0x0005
#define W5500_SHAR0      0x0009
#define W5500_SIPR0      0x000F
#define W5500_VERSIONR   0x0039
#define W5500_PHYCFGR    0x002E

// Socket Registers (offset within socket block)
#define Sn_MR            0x0000
#define Sn_CR            0x0001
#define Sn_SR            0x0003
#define Sn_PORT0         0x0004
#define Sn_TX_FSR0       0x0020
#define Sn_TX_WR0        0x0024
#define Sn_RX_RSR0       0x0026
#define Sn_RX_RD0        0x0028

// Block Select Bits (BSB)
#define COMMON_REG_BSB   0x00
#define SOCKET0_REG_BSB  0x08
#define SOCKET1_REG_BSB  0x28
#define SOCKET2_REG_BSB  0x48
#define SOCKET3_REG_BSB  0x68
#define SOCKET0_TX_BSB   0x10
#define SOCKET1_TX_BSB   0x30
#define SOCKET2_TX_BSB   0x50
#define SOCKET3_TX_BSB   0x70
#define SOCKET0_RX_BSB   0x18
#define SOCKET1_RX_BSB   0x38
#define SOCKET2_RX_BSB   0x58
#define SOCKET3_RX_BSB   0x78

// Socket Commands
#define SOCK_OPEN        0x01
#define SOCK_LISTEN      0x02
#define SOCK_DISCON      0x08
#define SOCK_CLOSE       0x10
#define SOCK_SEND        0x20
#define SOCK_RECV        0x40

// Socket Status
#define SOCK_STAT_CLOSED      0x00
#define SOCK_STAT_INIT        0x13
#define SOCK_STAT_LISTEN      0x14
#define SOCK_STAT_ESTABLISHED 0x17
#define SOCK_STAT_CLOSE_WAIT  0x1C

// Socket Mode
#define SOCK_TCP         0x01

#define MAX_SOCKETS      4
#define HTTP_PORT        80

// Socket BSB arrays
static const uint8_t SOCKET_REG_BSB[] = {SOCKET0_REG_BSB, SOCKET1_REG_BSB, SOCKET2_REG_BSB, SOCKET3_REG_BSB};
static const uint8_t SOCKET_TX_BSB[] = {SOCKET0_TX_BSB, SOCKET1_TX_BSB, SOCKET2_TX_BSB, SOCKET3_TX_BSB};
static const uint8_t SOCKET_RX_BSB[] = {SOCKET0_RX_BSB, SOCKET1_RX_BSB, SOCKET2_RX_BSB, SOCKET3_RX_BSB};

// ============================================================================
// W5500 Low-Level SPI Functions
// ============================================================================

static void w5500_select(void) {
    gpio_put(W5500_CS_PIN, 0);
    sleep_us(1);
}

static void w5500_deselect(void) {
    sleep_us(1);
    gpio_put(W5500_CS_PIN, 1);
    sleep_us(1);
}

static uint8_t w5500_read_reg(uint16_t addr, uint8_t bsb) {
    uint8_t data;
    w5500_select();
    spi_write_blocking(W5500_SPI_PORT, (uint8_t[]){addr >> 8}, 1);
    spi_write_blocking(W5500_SPI_PORT, (uint8_t[]){addr & 0xFF}, 1);
    spi_write_blocking(W5500_SPI_PORT, &bsb, 1);
    spi_read_blocking(W5500_SPI_PORT, 0x00, &data, 1);
    w5500_deselect();
    return data;
}

static void w5500_write_reg(uint16_t addr, uint8_t bsb, uint8_t data) {
    w5500_select();
    spi_write_blocking(W5500_SPI_PORT, (uint8_t[]){addr >> 8}, 1);
    spi_write_blocking(W5500_SPI_PORT, (uint8_t[]){addr & 0xFF}, 1);
    uint8_t control = bsb | 0x04;  // Write mode
    spi_write_blocking(W5500_SPI_PORT, &control, 1);
    spi_write_blocking(W5500_SPI_PORT, &data, 1);
    w5500_deselect();
}

static void w5500_read_buffer(uint16_t addr, uint8_t bsb, uint8_t* buf, uint16_t len) {
    w5500_select();
    spi_write_blocking(W5500_SPI_PORT, (uint8_t[]){addr >> 8}, 1);
    spi_write_blocking(W5500_SPI_PORT, (uint8_t[]){addr & 0xFF}, 1);
    spi_write_blocking(W5500_SPI_PORT, &bsb, 1);
    spi_read_blocking(W5500_SPI_PORT, 0x00, buf, len);
    w5500_deselect();
}

static void w5500_write_buffer(uint16_t addr, uint8_t bsb, const uint8_t* buf, uint16_t len) {
    w5500_select();
    spi_write_blocking(W5500_SPI_PORT, (uint8_t[]){addr >> 8}, 1);
    spi_write_blocking(W5500_SPI_PORT, (uint8_t[]){addr & 0xFF}, 1);
    uint8_t control = bsb | 0x04;  // Write mode
    spi_write_blocking(W5500_SPI_PORT, &control, 1);
    spi_write_blocking(W5500_SPI_PORT, buf, len);
    w5500_deselect();
}

static void w5500_hardware_reset(void) {
    printf("[W5500] Hardware reset\n");
    gpio_put(W5500_RST_PIN, 0);
    sleep_ms(10);
    gpio_put(W5500_RST_PIN, 1);
    sleep_ms(200);
}

// ============================================================================
// W5500 Initialization
// ============================================================================

bool w5500_eth_init(void) {
    printf("[W5500] Initializing Ethernet...\n");
    
    // Initialize SPI at 40MHz
    spi_init(W5500_SPI_PORT, 40000000);
    gpio_set_function(W5500_CLK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(W5500_MOSI_PIN, GPIO_FUNC_SPI);
    gpio_set_function(W5500_MISO_PIN, GPIO_FUNC_SPI);
    
    // Initialize CS pin
    gpio_init(W5500_CS_PIN);
    gpio_set_dir(W5500_CS_PIN, GPIO_OUT);
    gpio_put(W5500_CS_PIN, 1);
    
    // Initialize RST pin
    gpio_init(W5500_RST_PIN);
    gpio_set_dir(W5500_RST_PIN, GPIO_OUT);
    gpio_put(W5500_RST_PIN, 1);
    
    // Note: INT pin not used - using polling like WIZnet reference examples
    // Reference implementations poll socket status instead of using interrupts
    
    // Hardware reset
    w5500_hardware_reset();
    
    // Check chip version
    uint8_t version = w5500_read_reg(W5500_VERSIONR, COMMON_REG_BSB);
    printf("[W5500] Chip version: 0x%02X\n", version);
    
    if (version != 0x04) {
        printf("[W5500] ERROR: Invalid version (expected 0x04)\n");
        return false;
    }
    
    // Configure network
    printf("[W5500] Configuring network...\n");
    
    // Set MAC address
    for (int i = 0; i < 6; i++) {
        w5500_write_reg(W5500_SHAR0 + i, COMMON_REG_BSB, MAC_ADDRESS[i]);
    }
    
    // Set Gateway (all zeros = no gateway)
    for (int i = 0; i < 4; i++) {
        w5500_write_reg(W5500_GAR0 + i, COMMON_REG_BSB, GATEWAY[i]);
    }
    
    // Set Subnet Mask
    for (int i = 0; i < 4; i++) {
        w5500_write_reg(W5500_SUBR0 + i, COMMON_REG_BSB, SUBNET_MASK[i]);
    }
    
    // Set IP Address
    for (int i = 0; i < 4; i++) {
        w5500_write_reg(W5500_SIPR0 + i, COMMON_REG_BSB, IP_ADDRESS[i]);
    }
    
    printf("[W5500] Network Configuration:\n");
    printf("        MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
           MAC_ADDRESS[0], MAC_ADDRESS[1], MAC_ADDRESS[2],
           MAC_ADDRESS[3], MAC_ADDRESS[4], MAC_ADDRESS[5]);
    printf("        IP:  %d.%d.%d.%d\n",
           IP_ADDRESS[0], IP_ADDRESS[1], IP_ADDRESS[2], IP_ADDRESS[3]);
    printf("        Mask: %d.%d.%d.%d\n",
           SUBNET_MASK[0], SUBNET_MASK[1], SUBNET_MASK[2], SUBNET_MASK[3]);
    
    printf("[W5500] Initialization complete - using polling mode\n");
    return true;
}

// ============================================================================
// HTTP Server Setup
// ============================================================================

bool w5500_eth_start_server(uint16_t port) {
    printf("[W5500] Starting HTTP server on port %d with %d sockets\n", port, MAX_SOCKETS);
    
    // Setup all 4 sockets
    for (int sock = 0; sock < MAX_SOCKETS; sock++) {
        uint8_t reg_bsb = SOCKET_REG_BSB[sock];
        
        // Close socket if open
        w5500_write_reg(Sn_CR, reg_bsb, SOCK_CLOSE);
        sleep_ms(10);
        
        // Set socket mode to TCP
        w5500_write_reg(Sn_MR, reg_bsb, SOCK_TCP);
        
        // Set source port
        w5500_write_reg(Sn_PORT0, reg_bsb, (port >> 8) & 0xFF);
        w5500_write_reg(Sn_PORT0 + 1, reg_bsb, port & 0xFF);
        
        // Open socket
        w5500_write_reg(Sn_CR, reg_bsb, SOCK_OPEN);
        sleep_ms(10);
        
        // Check if opened
        uint8_t status = w5500_read_reg(Sn_SR, reg_bsb);
        if (status != SOCK_STAT_INIT) {
            printf("[W5500] ERROR: Socket %d failed to open (status: 0x%02X)\n", sock, status);
            return false;
        }
        
        // Start listening
        w5500_write_reg(Sn_CR, reg_bsb, SOCK_LISTEN);
        sleep_ms(10);
        
        status = w5500_read_reg(Sn_SR, reg_bsb);
        if (status != SOCK_STAT_LISTEN) {
            printf("[W5500] ERROR: Socket %d failed to listen (status: 0x%02X)\n", sock, status);
            return false;
        }
        
        printf("[W5500] Socket %d listening\n", sock);
    }
    
    printf("[W5500] HTTP server ready on http://%d.%d.%d.%d:%d\n",
           IP_ADDRESS[0], IP_ADDRESS[1], IP_ADDRESS[2], IP_ADDRESS[3], port);
    return true;
}

// ============================================================================
// HTTP Request Processing
// ============================================================================

// Generate HTTP hello world response dynamically with current IP
static void w5500_build_hello_response(char* buffer, size_t size) {
    snprintf(buffer, size,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Connection: close\r\n"
        "\r\n"
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head><title>Z1 Onyx Cluster</title></head>\n"
        "<body>\n"
        "<h1>Hello from Z1 Onyx Controller!</h1>\n"
        "<p>W5500 Ethernet is working</p>\n"
        "<p>IP: %s</p>\n"
        "<p>Multi-socket HTTP server active</p>\n"
        "</body>\n"
        "</html>\n",
        w5500_get_ip_string());
}

static void w5500_send_response(uint8_t sock, const char* response) {
    uint8_t reg_bsb = SOCKET_REG_BSB[sock];
    uint8_t tx_bsb = SOCKET_TX_BSB[sock];
    uint16_t length = strlen(response);
    
    // Wait for sufficient TX buffer space (reference implementation pattern)
    uint32_t timeout_count = 100;
    while (timeout_count-- > 0) {
        uint8_t tx_fsr_high = w5500_read_reg(Sn_TX_FSR0, reg_bsb);
        uint8_t tx_fsr_low = w5500_read_reg(Sn_TX_FSR0 + 1, reg_bsb);
        uint16_t tx_free_space = (tx_fsr_high << 8) | tx_fsr_low;
        
        if (tx_free_space >= length) {
            break;
        }
        
        sleep_ms(10);
    }
    
    if (timeout_count == 0) {
        printf("[W5500] ERROR: Timeout waiting for TX buffer space (need %d bytes)\n", length);
        return;
    }
    
    // Get TX write pointer
    uint8_t tx_wr_high = w5500_read_reg(Sn_TX_WR0, reg_bsb);
    uint8_t tx_wr_low = w5500_read_reg(Sn_TX_WR0 + 1, reg_bsb);
    uint16_t tx_wr_ptr = (tx_wr_high << 8) | tx_wr_low;
    
    // Write data byte-by-byte (reference implementation pattern)
    for (uint16_t i = 0; i < length; i++) {
        uint16_t addr = (tx_wr_ptr + i) & 0x07FF;  // 2KB buffer mask
        w5500_write_reg(addr, tx_bsb, response[i]);
    }
    
    // Update TX write pointer
    tx_wr_ptr += length;
    w5500_write_reg(Sn_TX_WR0, reg_bsb, (tx_wr_ptr >> 8) & 0xFF);
    w5500_write_reg(Sn_TX_WR0 + 1, reg_bsb, tx_wr_ptr & 0xFF);
    
    // Send command
    w5500_write_reg(Sn_CR, reg_bsb, SOCK_SEND);
    
    // Wait for send command to complete (reference implementation pattern)
    timeout_count = 100;
    while (timeout_count-- > 0) {
        uint8_t cmd = w5500_read_reg(Sn_CR, reg_bsb);
        if (cmd == 0) {
            break;
        }
        sleep_ms(5);
    }
    
    if (timeout_count == 0) {
        printf("[W5500] ERROR: Send command timeout\n");
    }
}

// Send response with explicit length (for binary data that may contain null bytes)
// CRITICAL: This function is required for HTTP chunked encoding with PSRAM buffers.
// 
// Problem: PSRAM buffers may contain stale null bytes from previous use. If we use
// strlen() to determine length, transmission stops at first null byte, truncating
// the response and causing JSON parse errors on the client side.
//
// Solution: Accept explicit length parameter and write exactly that many bytes,
// regardless of null bytes in the data. This is binary-safe and required for
// reliable chunked transfer encoding from PSRAM.
//
// Use Cases:
// - HTTP chunked encoding chunk data (response may contain null bytes)
// - Binary file transfers
// - Any PSRAM buffer transmission where strlen() would be incorrect
static void w5500_send_response_len(uint8_t sock, const char* response, uint16_t length) {
    uint8_t reg_bsb = SOCKET_REG_BSB[sock];
    uint8_t tx_bsb = SOCKET_TX_BSB[sock];
    
    // Wait for sufficient TX buffer space (reference implementation pattern)
    uint32_t timeout_count = 100;
    while (timeout_count-- > 0) {
        uint8_t tx_fsr_high = w5500_read_reg(Sn_TX_FSR0, reg_bsb);
        uint8_t tx_fsr_low = w5500_read_reg(Sn_TX_FSR0 + 1, reg_bsb);
        uint16_t tx_free_space = (tx_fsr_high << 8) | tx_fsr_low;
        
        if (tx_free_space >= length) {
            break;
        }
        
        sleep_ms(10);
    }
    
    if (timeout_count == 0) {
        printf("[W5500] ERROR: Timeout waiting for TX buffer space (need %d bytes)\n", length);
        return;
    }
    
    // Get TX write pointer
    uint8_t tx_wr_high = w5500_read_reg(Sn_TX_WR0, reg_bsb);
    uint8_t tx_wr_low = w5500_read_reg(Sn_TX_WR0 + 1, reg_bsb);
    uint16_t tx_wr_ptr = (tx_wr_high << 8) | tx_wr_low;
    
    // Write data byte-by-byte (reference implementation pattern)
    for (uint16_t i = 0; i < length; i++) {
        uint16_t addr = (tx_wr_ptr + i) & 0x07FF;  // 2KB buffer mask
        w5500_write_reg(addr, tx_bsb, response[i]);
    }
    
    // Update TX write pointer
    tx_wr_ptr += length;
    w5500_write_reg(Sn_TX_WR0, reg_bsb, (tx_wr_ptr >> 8) & 0xFF);
    w5500_write_reg(Sn_TX_WR0 + 1, reg_bsb, tx_wr_ptr & 0xFF);
    
    // Send command
    w5500_write_reg(Sn_CR, reg_bsb, SOCK_SEND);
    
    // Wait for send command to complete (reference implementation pattern)
    timeout_count = 100;
    while (timeout_count-- > 0) {
        uint8_t cmd = w5500_read_reg(Sn_CR, reg_bsb);
        if (cmd == 0) {
            break;
        }
        sleep_ms(5);
    }
    
    if (timeout_count == 0) {
        printf("[W5500] ERROR: Send command timeout\n");
    }
}

/**
 * Handle large file upload with streaming to SD card
 * 
 * For PUT requests with large bodies, read body in chunks and write directly to SD card.
 * Uses PSRAM buffer at offset 0x00008000 (32KB) for temporary storage.
 * 
 * @param sock Socket number
 * @param filepath Destination path on SD card
 * @param content_length Total body bytes to read
 * @return true if upload succeeded
 */
static bool w5500_stream_upload_to_sd(uint8_t sock, const char* filepath, size_t content_length) {
    // PSRAM buffer for chunked reading (32KB offset, 8KB buffer)
    #define UPLOAD_CHUNK_SIZE 2048  // W5500 RX buffer is 2KB, read in chunks
    uint8_t* chunk_buffer = (uint8_t*)(0x11000000 + 0x00008000);
    
    printf("[HTTP] Streaming upload: %s (%zu bytes)\n", filepath, content_length);
    
    // Open file for writing
    FIL fil;
    FRESULT fr = f_open(&fil, filepath, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        printf("[HTTP] ERROR: Failed to open %s (FRESULT=%d)\n", filepath, fr);
        return false;
    }
    
    size_t total_written = 0;
    uint8_t reg_bsb = (sock << 5) | 0x08;
    uint8_t rx_bsb = (sock << 5) | 0x18;
    
    while (total_written < content_length) {
        // Check available data in RX buffer
        uint16_t rx_size = (w5500_read_reg(Sn_RX_RSR0, reg_bsb) << 8) | 
                           w5500_read_reg(Sn_RX_RSR0 + 1, reg_bsb);
        
        if (rx_size == 0) {
            // Wait for data
            uint32_t timeout = time_us_32() + 5000000;  // 5s timeout
            while (rx_size == 0 && time_us_32() < timeout) {
                sleep_ms(10);
                rx_size = (w5500_read_reg(Sn_RX_RSR0, reg_bsb) << 8) | 
                          w5500_read_reg(Sn_RX_RSR0 + 1, reg_bsb);
            }
            if (rx_size == 0) {
                printf("[HTTP] ERROR: Upload timeout\n");
                f_close(&fil);
                return false;
            }
        }
        
        // Calculate chunk size
        size_t remaining = content_length - total_written;
        size_t chunk_size = (rx_size > UPLOAD_CHUNK_SIZE) ? UPLOAD_CHUNK_SIZE : rx_size;
        if (chunk_size > remaining) chunk_size = remaining;
        
        // Read from W5500
        uint16_t rx_rd_ptr = (w5500_read_reg(Sn_RX_RD0, reg_bsb) << 8) | 
                             w5500_read_reg(Sn_RX_RD0 + 1, reg_bsb);
        uint16_t offset = rx_rd_ptr & 0x07FF;
        
        if (offset + chunk_size > 0x0800) {
            uint16_t first = 0x0800 - offset;
            w5500_read_buffer(offset, rx_bsb, chunk_buffer, first);
            w5500_read_buffer(0, rx_bsb, chunk_buffer + first, chunk_size - first);
        } else {
            w5500_read_buffer(offset, rx_bsb, chunk_buffer, chunk_size);
        }
        
        // Update RX pointer
        rx_rd_ptr += chunk_size;
        w5500_write_reg(Sn_RX_RD0, reg_bsb, (rx_rd_ptr >> 8) & 0xFF);
        w5500_write_reg(Sn_RX_RD0 + 1, reg_bsb, rx_rd_ptr & 0xFF);
        w5500_write_reg(Sn_CR, reg_bsb, SOCK_RECV);
        
        // Write to SD card
        UINT bw;
        fr = f_write(&fil, chunk_buffer, chunk_size, &bw);
        if (fr != FR_OK || bw != chunk_size) {
            printf("[HTTP] ERROR: SD write failed (FRESULT=%d)\n", fr);
            f_close(&fil);
            return false;
        }
        
        total_written += chunk_size;
    }
    
    f_close(&fil);
    printf("[HTTP] Upload complete: %zu bytes\n", total_written);
    return true;
}

static void w5500_handle_request(uint8_t sock) {
    uint8_t reg_bsb = SOCKET_REG_BSB[sock];
    uint8_t rx_bsb = SOCKET_RX_BSB[sock];
    
    // Get RX receive size
    uint8_t rx_size_high = w5500_read_reg(Sn_RX_RSR0, reg_bsb);
    uint8_t rx_size_low = w5500_read_reg(Sn_RX_RSR0 + 1, reg_bsb);
    uint16_t rx_size = (rx_size_high << 8) | rx_size_low;
    
    if (rx_size == 0) return;
    
    // Get RX read pointer
    uint8_t rx_rd_high = w5500_read_reg(Sn_RX_RD0, reg_bsb);
    uint8_t rx_rd_low = w5500_read_reg(Sn_RX_RD0 + 1, reg_bsb);
    uint16_t rx_rd_ptr = (rx_rd_high << 8) | rx_rd_low;
    
    // Read full request (up to 2500 bytes to handle 1KB+ neuron payloads safely)
    // Need room for: HTTP headers (~200B) + JSON structure (~50B) + base64 data (1368 chars for 1024B payload)
    // Total: ~1620B minimum, using 2500B for safety margin
    static uint8_t request_buffer[2500];
    uint16_t offset = rx_rd_ptr & 0x07FF;
    uint16_t read_len = (rx_size > 2500) ? 2500 : rx_size;
    
    // Handle wrap-around
    if (offset + read_len > 0x0800) {
        uint16_t first_part = 0x0800 - offset;
        w5500_read_buffer(offset, rx_bsb, request_buffer, first_part);
        w5500_read_buffer(0, rx_bsb, request_buffer + first_part, read_len - first_part);
    } else {
        w5500_read_buffer(offset, rx_bsb, request_buffer, read_len);
    }
    
    // DON'T consume data yet - let the file upload handler do it properly
    // (For normal requests, we'll consume at the end after processing)
    
    // Null-terminate request
    request_buffer[read_len] = '\0';
    
    // Parse HTTP request line FIRST
    char method[16] = {0};
    char path[128] = {0};
    char* line_end = strstr((char*)request_buffer, "\r\n");
    if (line_end) {
        *line_end = '\0';  // Null-terminate request line
        
        // Parse "METHOD /path HTTP/1.x"
        char* space1 = strchr((char*)request_buffer, ' ');
        if (space1) {
            size_t method_len = space1 - (char*)request_buffer;
            if (method_len < sizeof(method)) {
                strncpy(method, (char*)request_buffer, method_len);
                method[method_len] = '\0';
                
                char* space2 = strchr(space1 + 1, ' ');
                if (space2) {
                    size_t path_len = space2 - (space1 + 1);
                    if (path_len < sizeof(path)) {
                        strncpy(path, space1 + 1, path_len);
                        path[path_len] = '\0';
                    }
                }
            }
        }
        *line_end = '\r';  // Restore for header parsing
    }
    
    // Check for Content-Length header (for file uploads)
    int content_length = -1;
    char* cl_header = strstr((char*)request_buffer, "Content-Length: ");
    if (cl_header) {
        content_length = atoi(cl_header + 16);
    }
    
    // Initialize response
    int status_code = 200;
    char* response_body = http_response_buffer;
    
    // ========================================================================
    // SPECIAL CASE: Streaming File Download (GET /api/files/*)
    // ========================================================================
    // This bypasses the 4KB response buffer limit by streaming files directly
    // from SD card to W5500 TX buffer using PSRAM as intermediary.
    // 
    // Key Features:
    // - Handles files up to 1MB+ (tested: 2KB to 1MB)
    // - Uses 1KB chunks with 5ms delay between sends
    // - Automatic Content-Length header
    // - Directory detection: skips directories for proper listing
    // 
    // Why directory detection is needed:
    // - Prevents trying to "download" directories as files
    // - Allows API handler to generate JSON directory listings
    // - f_stat() with AM_DIR flag distinguishes files from directories
    // ========================================================================
    if (strcmp(method, "GET") == 0 && strncmp(path, "/api/files/", 11) == 0) {
        const char* filepath = path + 11;  // Skip "/api/files/"
        
        // Check if it's a directory using f_stat
        FILINFO fno;
        FRESULT fr_stat = f_stat(filepath, &fno);
        
        // If it's a directory, skip streaming and let API handler do directory listing
        // This allows GET /api/files/topologies to return JSON list of files
        if (fr_stat == FR_OK && (fno.fattrib & AM_DIR)) {
            // Fall through to normal API routing for directory listing
            goto normal_routing;
        }
        
        printf("[HTTP] GET %s (streaming download)\n", path);
        
        // Open file for reading
        FIL fil;
        FRESULT fr = f_open(&fil, filepath, FA_READ);
        if (fr != FR_OK) {
            printf("[HTTP] ERROR: File not found: %s (FRESULT=%d)\n", filepath, fr);
            strcpy(http_response_buffer, "{\"error\":\"File not found\"}");
            status_code = 404;
            
            // Reset metadata
            http_response_metadata_t* metadata = z1_http_api_get_response_metadata();
            metadata->is_binary = false;
            metadata->content_length = 0;
            
            goto send_response;
        }
        
        // Get file size
        FSIZE_t file_size = f_size(&fil);
        printf("[HTTP] Streaming file: %lu bytes\n", (unsigned long)file_size);
        
        // Send HTTP headers manually
        static char headers[256];
        int header_len = snprintf(headers, sizeof(headers),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/octet-stream\r\n"
            "Content-Length: %lu\r\n"
            "Connection: close\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "\r\n",
            (unsigned long)file_size);
        
        w5500_send_response(sock, headers);
        sleep_ms(10);
        
        // Stream file in 1KB chunks
        uint8_t* chunk_buf = (uint8_t*)(0x11000000 + 0x00020000);  // PSRAM buffer
        const size_t CHUNK_SIZE = 1024;
        FSIZE_t bytes_sent = 0;
        
        while (bytes_sent < file_size) {
            UINT bytes_to_read = (file_size - bytes_sent > CHUNK_SIZE) ? CHUNK_SIZE : (file_size - bytes_sent);
            UINT bytes_read = 0;
            
            fr = f_read(&fil, chunk_buf, bytes_to_read, &bytes_read);
            if (fr != FR_OK || bytes_read == 0) {
                printf("[HTTP] ERROR: Read failed at byte %lu\n", (unsigned long)bytes_sent);
                break;
            }
            
            // Send chunk
            w5500_send_response_len(sock, (char*)chunk_buf, bytes_read);
            bytes_sent += bytes_read;
            
            if (bytes_sent < file_size) {
                sleep_ms(5);  // Small delay between chunks
            }
        }
        
        f_close(&fil);
        printf("[HTTP] Download complete: %lu bytes\n", (unsigned long)bytes_sent);
        
        // Disconnect
        w5500_write_reg(Sn_CR, reg_bsb, SOCK_DISCON);
        
        // Consume RX data
        rx_rd_ptr += read_len;
        w5500_write_reg(Sn_RX_RD0, reg_bsb, (rx_rd_ptr >> 8) & 0xFF);
        w5500_write_reg(Sn_RX_RD0 + 1, reg_bsb, rx_rd_ptr & 0xFF);
        w5500_write_reg(Sn_CR, reg_bsb, SOCK_RECV);
        
        return;  // Exit early - download handled
    }
    
    // ========================================================================
    // SPECIAL CASE: Streaming File Upload (PUT /api/files/*)
    // ========================================================================
    // Bypasses 4KB buffer limit by streaming data directly from W5500 RX buffer
    // to SD card using PSRAM as intermediary.
    // 
    // Key Features:
    // - Handles unlimited file sizes (tested: 2KB to 1MB+)
    // - Uses 2KB chunks from W5500 RX buffer (circular wraparound handling)
    // - Automatic parent directory creation (e.g., topologies/, engines/)
    // - CRC32 integrity checking via FatFS
    // - Tracks local stream_rd_ptr to avoid double-consumption bug
    // 
    // Implementation Details:
    // - Parses Content-Length header to know total bytes expected
    // - Writes initial chunk already in request buffer
    // - Streams remaining data in chunks until total reached
    // - Calls f_sync() before close to ensure data integrity
    // 
    // Directory Creation:
    // - Extracts directory from filepath (e.g., "topologies/file.json")
    // - Calls f_mkdir() before f_open() (ignores FR_EXIST error)
    // - Ensures directories exist without manual creation
    // ========================================================================
    if (strcmp(method, "PUT") == 0 && 
        strncmp(path, "/api/files/", 11) == 0 && 
        content_length > 0) {
        
        printf("[HTTP] PUT %s (Content-Length: %d)\n", path, content_length);
        
        // Calculate header size to skip
        char* body_start = strstr((char*)request_buffer, "\r\n\r\n");
        if (!body_start) {
            printf("[HTTP] ERROR: No header end found\n");
            strcpy(http_response_buffer, "{\"error\":\"Bad Request - No headers\"}");
            status_code = 400;
            goto send_response;  // Jump to response sending
        }
        
        size_t header_size = (body_start + 4) - (char*)request_buffer;
        size_t body_in_buffer = read_len - header_size;
        
        printf("[HTTP] Header: %zu bytes, Body in buffer: %zu\n", header_size, body_in_buffer);
        
        // Extract filepath
        const char* filepath = path + 11;  // Skip "/api/files/"
        
        // Create parent directory if filepath contains a slash
        const char* slash = strchr(filepath, '/');
        if (slash != NULL) {
            // Extract directory path
            size_t dir_len = slash - filepath;
            char dirpath[256];
            if (dir_len < sizeof(dirpath)) {
                memcpy(dirpath, filepath, dir_len);
                dirpath[dir_len] = '\0';
                
                // Create directory (f_mkdir fails if exists, that's OK)
                FRESULT fr_dir = f_mkdir(dirpath);
                if (fr_dir != FR_OK && fr_dir != FR_EXIST) {
                    printf("[HTTP] WARNING: Failed to create dir %s (FRESULT=%d)\n", dirpath, fr_dir);
                }
            }
        }
        
        // Open file for writing
        bool success = false;
        FIL fil;
        FRESULT fr = f_open(&fil, filepath, FA_WRITE | FA_CREATE_ALWAYS);
        if (fr != FR_OK) {
            printf("[HTTP] ERROR: Failed to open %s (FRESULT=%d)\n", filepath, fr);
            strcpy(http_response_buffer, "{\"error\":\"Failed to open file\"}");
            status_code = 500;
            goto send_response;
        }
        
        // Write first chunk if any body data is already in buffer
        if (body_in_buffer > 0) {
            UINT bw;
            fr = f_write(&fil, body_start + 4, body_in_buffer, &bw);
            if (fr != FR_OK || bw != body_in_buffer) {
                printf("[HTTP] ERROR: Initial write failed (FRESULT=%d)\n", fr);
                f_close(&fil);
                strcpy(http_response_buffer, "{\"error\":\"Write failed\"}");
                status_code = 500;
                goto send_response;
            }
            printf("[HTTP] Wrote initial %zu bytes\n", body_in_buffer);
        }
        
        // If there's more data to read, consume what we've read and stream the rest
        size_t remaining = content_length - body_in_buffer;
        if (remaining > 0) {
            // Consume only the data we've processed (headers + body written)
            rx_rd_ptr += header_size + body_in_buffer;
            w5500_write_reg(Sn_RX_RD0, reg_bsb, (rx_rd_ptr >> 8) & 0xFF);
            w5500_write_reg(Sn_RX_RD0 + 1, reg_bsb, rx_rd_ptr & 0xFF);
            w5500_write_reg(Sn_CR, reg_bsb, SOCK_RECV);
            
            // Check how much data is already available
            uint16_t avail_now = (w5500_read_reg(Sn_RX_RSR0, reg_bsb) << 8) | 
                                  w5500_read_reg(Sn_RX_RSR0 + 1, reg_bsb);
            printf("[HTTP] Streaming %zu more bytes (RX buffer has %u bytes)...\n", remaining, avail_now);
            
            // Stream remaining data
            success = true;
            uint8_t* chunk_buf = (uint8_t*)(0x11000000 + 0x00020000);  // Use 128KB offset to avoid HTTP buffer
            size_t total_read = 0;
            uint16_t stream_rd_ptr = rx_rd_ptr;  // Use the pointer we just updated
            
            while (total_read < remaining && success) {
                // Wait for data
                uint16_t avail = 0;
                uint32_t timeout = time_us_32() + 5000000;
                while (avail == 0 && time_us_32() < timeout) {
                    sleep_ms(10);
                    avail = (w5500_read_reg(Sn_RX_RSR0, reg_bsb) << 8) | 
                            w5500_read_reg(Sn_RX_RSR0 + 1, reg_bsb);
                }
                if (avail == 0) {
                    printf("[HTTP] ERROR: Stream timeout\n");
                    success = false;
                    break;
                }
                
                // Read chunk
                size_t chunk_size = (avail > 2048) ? 2048 : avail;
                if (chunk_size > remaining - total_read) {
                    chunk_size = remaining - total_read;
                }
                
                uint16_t offs = stream_rd_ptr & 0x07FF;
                
                if (offs + chunk_size > 0x0800) {
                    uint16_t first = 0x0800 - offs;
                    w5500_read_buffer(offs, rx_bsb, chunk_buf, first);
                    w5500_read_buffer(0, rx_bsb, chunk_buf + first, chunk_size - first);
                } else {
                    w5500_read_buffer(offs, rx_bsb, chunk_buf, chunk_size);
                }
                
                stream_rd_ptr += chunk_size;
                w5500_write_reg(Sn_RX_RD0, reg_bsb, (stream_rd_ptr >> 8) & 0xFF);
                w5500_write_reg(Sn_RX_RD0 + 1, reg_bsb, stream_rd_ptr & 0xFF);
                w5500_write_reg(Sn_CR, reg_bsb, SOCK_RECV);
                
                // Write to SD
                UINT bw;
                fr = f_write(&fil, chunk_buf, chunk_size, &bw);
                if (fr != FR_OK || bw != chunk_size) {
                    printf("[HTTP] ERROR: SD write failed (FRESULT=%d)\n", fr);
                    success = false;
                    break;
                }
                printf("[HTTP] Wrote chunk: %zu bytes (total: %zu/%zu)\n", chunk_size, total_read + chunk_size, remaining);
                
                total_read += chunk_size;
            }
            
            // Sync and close file
            if (success) {
                fr = f_sync(&fil);
                if (fr != FR_OK) {
                    printf("[HTTP] ERROR: f_sync failed (FRESULT=%d)\n", fr);
                    success = false;
                }
            }
            f_close(&fil);
            printf("[HTTP] Upload %s: %zu bytes total\n", 
                   success ? "SUCCESS" : "FAILED", body_in_buffer + total_read);
        } else {
            // No remaining data
            success = true;
            f_sync(&fil);
            f_close(&fil);
            printf("[HTTP] Upload SUCCESS: %zu bytes\n", body_in_buffer);
        }
        
        // Format response
        if (success) {
            snprintf(http_response_buffer, HTTP_BUFFER_SIZE, 
                    "{\"success\":true,\"size\":%d}", content_length);
            status_code = 200;
        } else {
            strcpy(http_response_buffer, "{\"error\":\"Upload failed\"}");
            status_code = 500;
        }
        
        // Reset metadata to indicate JSON response (not binary)
        http_response_metadata_t* metadata = z1_http_api_get_response_metadata();
        metadata->is_binary = false;
        metadata->content_length = 0;
        
        goto send_response;
    }
    
normal_routing:
    // NORMAL REQUEST HANDLING (non-file-upload)
    char* body = NULL;  // Declare outside if block so it's accessible later
    bool ota_body_streamed = false;  // Track if we streamed OTA chunk body
    if (status_code == 200 && (strcmp(method, "PUT") != 0 || strncmp(path, "/api/files/", 11) != 0)) {
        // Extract POST/PUT body
        char* body_start = strstr((char*)request_buffer, "\r\n\r\n");
        if (body_start) {
            body = body_start + 4;  // Skip \r\n\r\n
            
            // For OTA chunk uploads with large bodies, we need to stream the rest
            if (content_length > 0 && strstr(path, "/api/ota/update_chunk") != NULL) {
                size_t header_size = body - (char*)request_buffer;
                size_t body_in_buffer = read_len - header_size;
                size_t remaining = content_length - body_in_buffer;
                
                if (remaining > 0) {
                    printf("[HTTP] OTA chunk body incomplete: have %zu, need %zu more\n", 
                           body_in_buffer, remaining);
                    
                    // Allocate PSRAM buffer for complete body
                    char* complete_body = (char*)(0x11000000 + 0x00020000);  // PSRAM temp buffer
                    
                    // Copy initial chunk
                    memcpy(complete_body, body, body_in_buffer);
                    
                    // CONSUME headers + initial body before streaming (same as file upload!)
                    rx_rd_ptr += header_size + body_in_buffer;
                    w5500_write_reg(Sn_RX_RD0, reg_bsb, (rx_rd_ptr >> 8) & 0xFF);
                    w5500_write_reg(Sn_RX_RD0 + 1, reg_bsb, rx_rd_ptr & 0xFF);
                    w5500_write_reg(Sn_CR, reg_bsb, SOCK_RECV);
                    
                    // Stream remaining data from socket
                    size_t total_read = body_in_buffer;
                    uint16_t stream_rd_ptr = rx_rd_ptr;  // Start from updated position
                    
                    while (total_read < content_length) {
                        // Wait for data with timeout
                        uint16_t avail = 0;
                        uint32_t timeout = time_us_32() + 2000000;  // 2s timeout
                        while (avail == 0 && time_us_32() < timeout) {
                            sleep_ms(5);
                            avail = (w5500_read_reg(Sn_RX_RSR0, reg_bsb) << 8) | 
                                    w5500_read_reg(Sn_RX_RSR0 + 1, reg_bsb);
                        }
                        if (avail == 0) {
                            printf("[HTTP] ERROR: OTA chunk timeout\n");
                            strcpy(response_body, "{\"error\":\"Body timeout\"}");
                            status_code = 408;
                            goto send_response;
                        }
                        
                        // Read chunk from socket
                        size_t chunk_size = (avail > 512) ? 512 : avail;
                        if (chunk_size > content_length - total_read) {
                            chunk_size = content_length - total_read;
                        }
                        
                        uint16_t offs = stream_rd_ptr & 0x07FF;
                        if (offs + chunk_size > 0x0800) {
                            uint16_t first = 0x0800 - offs;
                            w5500_read_buffer(offs, rx_bsb, 
                                            (uint8_t*)(complete_body + total_read), first);
                            w5500_read_buffer(0, rx_bsb, 
                                            (uint8_t*)(complete_body + total_read + first), 
                                            chunk_size - first);
                        } else {
                            w5500_read_buffer(offs, rx_bsb, 
                                            (uint8_t*)(complete_body + total_read), chunk_size);
                        }
                        
                        stream_rd_ptr += chunk_size;
                        w5500_write_reg(Sn_RX_RD0, reg_bsb, (stream_rd_ptr >> 8) & 0xFF);
                        w5500_write_reg(Sn_RX_RD0 + 1, reg_bsb, stream_rd_ptr & 0xFF);
                        w5500_write_reg(Sn_CR, reg_bsb, SOCK_RECV);
                        
                        total_read += chunk_size;
                    }
                    
                    complete_body[content_length] = '\0';
                    body = complete_body;
                    ota_body_streamed = true;  // Mark that we streamed the body
                    printf("[HTTP] OTA chunk body complete: %zu bytes\n", content_length);
                    
                    // NOTE: Don't update rx_rd_ptr here - the initial read already
                    // updated it, and stream_rd_ptr is relative to that update
                }
            }
            
            if (*body == '\0') {
                body = NULL;  // Empty body
            }
        }
    }
    
    // Route to API handler if not already handled
    if (strlen(method) > 0 && strlen(path) > 0) {
        printf("[HTTP] %s %s\n", method, path);
        if (body) {
            size_t body_len = strlen(body);
            printf("[HTTP] Body: %zu bytes\n", body_len);
            if (body_len < 200) {
                printf("[HTTP] Body content: %s\n", body);
            }
        } else {
            printf("[HTTP] Body: NULL\n");
        }
        status_code = z1_http_api_route(method, path, body, response_body, HTTP_BUFFER_SIZE);
    } else {
        // Invalid request
        strcpy(response_body, "{\"error\":\"Bad Request\"}");
        status_code = 400;
    }
    
send_response:
    // Consume RX data for normal requests (file uploads and OTA body streaming already consumed)
    if ((strcmp(method, "PUT") != 0 || strncmp(path, "/api/files/", 11) != 0) && !ota_body_streamed) {
        rx_rd_ptr += read_len;
        w5500_write_reg(Sn_RX_RD0, reg_bsb, (rx_rd_ptr >> 8) & 0xFF);
        w5500_write_reg(Sn_RX_RD0 + 1, reg_bsb, rx_rd_ptr & 0xFF);
        w5500_write_reg(Sn_CR, reg_bsb, SOCK_RECV);
    }
    
    // Check if response is binary file download
    http_response_metadata_t* metadata = z1_http_api_get_response_metadata();
    
    // Determine body length
    int body_len = metadata->is_binary ? metadata->content_length : strlen(response_body);
    
    const char* status_text = (status_code == 200) ? "OK" :
                              (status_code == 299) ? "OK" :  // Reboot request - return OK to client
                              (status_code == 404) ? "Not Found" : "Bad Request";
    
    // Map 299 to 200 for HTTP response (internal code for reboot)
    int http_status = (status_code == 299) ? 200 : status_code;
    
    // Determine Content-Type based on response type
    const char* content_type = metadata->content_type ? metadata->content_type :
                              (metadata->is_binary ? "application/octet-stream" : "application/json");
    
    // Send headers with chunked encoding
    static char headers[256];
    int header_len = snprintf(headers, sizeof(headers),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n",
        http_status, status_text, content_type);
    
    if (header_len < 0 || header_len >= sizeof(headers)) {
        printf("[HTTP] ERROR: Header too large\n");
        return;
    }
    
    w5500_send_response(sock, headers);
    sleep_ms(10);  // Let headers drain
    
    // Send body in chunks (reference: 1000 byte chunks)
    const int CHUNK_DATA_SIZE = 1000;
    int bytes_sent = 0;
    
    while (bytes_sent < body_len) {
        int remaining = body_len - bytes_sent;
        int chunk_size = (remaining > CHUNK_DATA_SIZE) ? CHUNK_DATA_SIZE : remaining;
        
        // Send chunk size header (hex format + CRLF)
        static char chunk_header[16];
        snprintf(chunk_header, sizeof(chunk_header), "%X\r\n", chunk_size);
        w5500_send_response(sock, chunk_header);
        
        // Send chunk data using length-based function (handles null bytes)
        w5500_send_response_len(sock, response_body + bytes_sent, chunk_size);
        
        // Send chunk trailer (CRLF)
        w5500_send_response(sock, "\r\n");
        
        bytes_sent += chunk_size;
        
        // Wait for buffer to drain before next chunk
        if (bytes_sent < body_len) {
            sleep_ms(10);
        }
    }
    
    // Send terminating chunk ("0\r\n\r\n")
    w5500_send_response(sock, "0\r\n\r\n");
    
    printf("[HTTP] Sent %d bytes in chunked encoding\n", body_len);
    
    // Disconnect after response
    w5500_write_reg(Sn_CR, reg_bsb, SOCK_DISCON);
    
    // Handle reboot request (status code 299 = success + reboot pending)
    if (status_code == 299) {
        printf("[HTTP] Reboot requested - rebooting in 1 second...\n");
        sleep_ms(1000);  // Allow response to reach client
        watchdog_reboot(0, 0, 0);  // Trigger watchdog reset
    }
}

// State tracking for non-blocking socket operations
static struct {
    absolute_time_t reopen_time;
    bool pending_reopen;
} socket_state[MAX_SOCKETS] = {0};

// Timer for 1ms polling interval
static absolute_time_t next_poll_time = {0};

void w5500_eth_process(void) {
    // Non-blocking timer: only process every 1000 microseconds (1ms)
    absolute_time_t now = get_absolute_time();
    
    // Check if it's time to process (returns negative if not yet time)
    if (absolute_time_diff_us(next_poll_time, now) < 0) {
        return;  // Not yet time, return immediately without blocking
    }
    
    // Schedule next poll for 1ms from now
    next_poll_time = make_timeout_time_us(1000);
    
    // Process all sockets
    for (int sock = 0; sock < MAX_SOCKETS; sock++) {
        uint8_t reg_bsb = SOCKET_REG_BSB[sock];
        
        // Handle pending socket reopen (non-blocking delay)
        if (socket_state[sock].pending_reopen) {
            if (absolute_time_diff_us(socket_state[sock].reopen_time, get_absolute_time()) >= 0) {
                // 5ms has elapsed, complete the reopen
                w5500_write_reg(Sn_CR, reg_bsb, SOCK_LISTEN);
                socket_state[sock].pending_reopen = false;
            }
            continue; // Skip to next socket
        }
        
        uint8_t status = w5500_read_reg(Sn_SR, reg_bsb);
        
        switch (status) {
            case SOCK_STAT_ESTABLISHED:
                // Handle incoming data
                w5500_handle_request(sock);
                break;
                
            case SOCK_STAT_CLOSE_WAIT:
                // Peer closed connection
                w5500_write_reg(Sn_CR, reg_bsb, SOCK_DISCON);
                break;
                
            case SOCK_STAT_CLOSED:
                // Start non-blocking reopen sequence
                w5500_write_reg(Sn_MR, reg_bsb, SOCK_TCP);
                w5500_write_reg(Sn_PORT0, reg_bsb, (HTTP_PORT >> 8) & 0xFF);
                w5500_write_reg(Sn_PORT0 + 1, reg_bsb, HTTP_PORT & 0xFF);
                w5500_write_reg(Sn_CR, reg_bsb, SOCK_OPEN);
                
                // Schedule LISTEN command for 5ms later (non-blocking)
                socket_state[sock].reopen_time = make_timeout_time_ms(5);
                socket_state[sock].pending_reopen = true;
                break;
        }
    }
}
