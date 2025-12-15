/**
 * W5500 Ethernet Library
 * Simplified HTTP server for Z1 Onyx Controller
 */

#include "w5500_eth.h"
#include "z1_http_api.h"
#include "controller_pins.h"
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include <stdio.h>
#include <string.h>

// HTTP response buffer (shared with z1_http_api.c)
char http_response_buffer[4096];

// Network Configuration (192.168.1.222, no gateway)
static const uint8_t MAC_ADDRESS[6] = {0x02, 0xA1, 0xB2, 0xC3, 0xD4, 0x01};
static const uint8_t IP_ADDRESS[4]  = {192, 168, 1, 222};
static const uint8_t SUBNET_MASK[4] = {255, 255, 255, 0};
static const uint8_t GATEWAY[4]     = {0, 0, 0, 0};  // No gateway

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

static const char* HTTP_HELLO_WORLD = 
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
    "<p>IP: 192.168.1.222</p>\n"
    "<p>Multi-socket HTTP server active</p>\n"
    "</body>\n"
    "</html>\n";

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
    
    // Read full request (up to 4096 bytes to handle 1KB+ neuron payloads safely)
    // NOTE: 1024 bytes → 1366 base64 chars + JSON (~1500) + HTTP headers (~500) = ~2000 bytes
    // Using 4KB to have margin for future expansion
    static uint8_t request_buffer[4096];
    uint16_t offset = rx_rd_ptr & 0x07FF;
    uint16_t read_len = (rx_size > 4096) ? 4096 : rx_size;
    
    // Handle wrap-around
    if (offset + read_len > 0x0800) {
        uint16_t first_part = 0x0800 - offset;
        w5500_read_buffer(offset, rx_bsb, request_buffer, first_part);
        w5500_read_buffer(0, rx_bsb, request_buffer + first_part, read_len - first_part);
    } else {
        w5500_read_buffer(offset, rx_bsb, request_buffer, read_len);
    }
    
    // Update RX read pointer (consume all data)
    rx_rd_ptr += rx_size;
    w5500_write_reg(Sn_RX_RD0, reg_bsb, (rx_rd_ptr >> 8) & 0xFF);
    w5500_write_reg(Sn_RX_RD0 + 1, reg_bsb, rx_rd_ptr & 0xFF);
    w5500_write_reg(Sn_CR, reg_bsb, SOCK_RECV);
    
    // Null-terminate request
    request_buffer[read_len] = '\0';
    
    // Extract POST body FIRST (before modifying buffer)
    char* body = NULL;
    char* body_start = strstr((char*)request_buffer, "\r\n\r\n");
    if (body_start) {
        body = body_start + 4;  // Skip \r\n\r\n
        if (*body == '\0') {
            body = NULL;  // Empty body
        }
    }
    
    // Parse HTTP request line (e.g. "GET /api/nodes HTTP/1.1")
    char method[16] = {0};
    char path[128] = {0};
    
    // Extract method and path
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
    }
    
    // Route to API handler
    int status_code = 200;
    char* response_body = http_response_buffer;
    
    if (strlen(method) > 0 && strlen(path) > 0) {
        printf("[HTTP] %s %s\n", method, path);
        if (body) {
            printf("[HTTP] Body: %s\n", body);
        }
        status_code = z1_http_api_route(method, path, body, response_body, sizeof(http_response_buffer));
    } else {
        // Invalid request
        strcpy(response_body, "{\"error\":\"Bad Request\"}");
        status_code = 400;
    }
    
    // Send HTTP response using chunked encoding (reference implementation pattern)
    int body_len = strlen(response_body);
    const char* status_text = (status_code == 200) ? "OK" : 
                              (status_code == 404) ? "Not Found" : "Bad Request";
    
    // Send headers with chunked encoding
    static char headers[512];
    int header_len = snprintf(headers, sizeof(headers),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n",
        status_code, status_text);
    
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
        
        // Send chunk data
        static char chunk_data[1001];
        strncpy(chunk_data, response_body + bytes_sent, chunk_size);
        chunk_data[chunk_size] = '\0';
        w5500_send_response(sock, chunk_data);
        
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
