/**
 * Z1 Onyx Bus - Core Management Layer
 * Code by NeuroFab Corp: 2025-2026
 * 
 * Source-synchronous multi-master bus implementation
 * Phase 1: Basic TX/RX without arbitration
 */

#include "z1_bus.h"
#include "z1_onyx_bus_pins.h"
#include "z1_bus_tx.pio.h"
#include "z1_bus_rx.pio.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/structs/dma.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>

// ============================================================================
// Bus Timing Statistics
// ============================================================================

static z1_bus_stats_t bus_stats = {0};

// ============================================================================
// Topology Management (Phase 3b)
// ============================================================================

static z1_topology_t cluster_topology = {0};  // Library-owned topology table

// Forward declaration
static void z1_bus_process_topology_broadcast(const uint16_t *payload, uint16_t length);

// ============================================================================
// CRC16-CCITT (Polynomial 0x1021) - Phase 2 Error Detection
// ============================================================================
// Lookup table for fast CRC16 calculation (XModem variant)
static const uint16_t crc16_table[256] = {
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7,
    0x8108, 0x9129, 0xa14a, 0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef,
    0x1231, 0x0210, 0x3273, 0x2252, 0x52b5, 0x4294, 0x72f7, 0x62d6,
    0x9339, 0x8318, 0xb37b, 0xa35a, 0xd3bd, 0xc39c, 0xf3ff, 0xe3de,
    0x2462, 0x3443, 0x0420, 0x1401, 0x64e6, 0x74c7, 0x44a4, 0x5485,
    0xa56a, 0xb54b, 0x8528, 0x9509, 0xe5ee, 0xf5cf, 0xc5ac, 0xd58d,
    0x3653, 0x2672, 0x1611, 0x0630, 0x76d7, 0x66f6, 0x5695, 0x46b4,
    0xb75b, 0xa77a, 0x9719, 0x8738, 0xf7df, 0xe7fe, 0xd79d, 0xc7bc,
    0x48c4, 0x58e5, 0x6886, 0x78a7, 0x0840, 0x1861, 0x2802, 0x3823,
    0xc9cc, 0xd9ed, 0xe98e, 0xf9af, 0x8948, 0x9969, 0xa90a, 0xb92b,
    0x5af5, 0x4ad4, 0x7ab7, 0x6a96, 0x1a71, 0x0a50, 0x3a33, 0x2a12,
    0xdbfd, 0xcbdc, 0xfbbf, 0xeb9e, 0x9b79, 0x8b58, 0xbb3b, 0xab1a,
    0x6ca6, 0x7c87, 0x4ce4, 0x5cc5, 0x2c22, 0x3c03, 0x0c60, 0x1c41,
    0xedae, 0xfd8f, 0xcdec, 0xddcd, 0xad2a, 0xbd0b, 0x8d68, 0x9d49,
    0x7e97, 0x6eb6, 0x5ed5, 0x4ef4, 0x3e13, 0x2e32, 0x1e51, 0x0e70,
    0xff9f, 0xefbe, 0xdfdd, 0xcffc, 0xbf1b, 0xaf3a, 0x9f59, 0x8f78,
    0x9188, 0x81a9, 0xb1ca, 0xa1eb, 0xd10c, 0xc12d, 0xf14e, 0xe16f,
    0x1080, 0x00a1, 0x30c2, 0x20e3, 0x5004, 0x4025, 0x7046, 0x6067,
    0x83b9, 0x9398, 0xa3fb, 0xb3da, 0xc33d, 0xd31c, 0xe37f, 0xf35e,
    0x02b1, 0x1290, 0x22f3, 0x32d2, 0x4235, 0x5214, 0x6277, 0x7256,
    0xb5ea, 0xa5cb, 0x95a8, 0x8589, 0xf56e, 0xe54f, 0xd52c, 0xc50d,
    0x34e2, 0x24c3, 0x14a0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
    0xa7db, 0xb7fa, 0x8799, 0x97b8, 0xe75f, 0xf77e, 0xc71d, 0xd73c,
    0x26d3, 0x36f2, 0x0691, 0x16b0, 0x6657, 0x7676, 0x4615, 0x5634,
    0xd94c, 0xc96d, 0xf90e, 0xe92f, 0x99c8, 0x89e9, 0xb98a, 0xa9ab,
    0x5844, 0x4865, 0x7806, 0x6827, 0x18c0, 0x08e1, 0x3882, 0x28a3,
    0xcb7d, 0xdb5c, 0xeb3f, 0xfb1e, 0x8bf9, 0x9bd8, 0xabbb, 0xbb9a,
    0x4a75, 0x5a54, 0x6a37, 0x7a16, 0x0af1, 0x1ad0, 0x2ab3, 0x3a92,
    0xfd2e, 0xed0f, 0xdd6c, 0xcd4d, 0xbdaa, 0xad8b, 0x9de8, 0x8dc9,
    0x7c26, 0x6c07, 0x5c64, 0x4c45, 0x3ca2, 0x2c83, 0x1ce0, 0x0cc1,
    0xef1f, 0xff3e, 0xcf5d, 0xdf7c, 0xaf9b, 0xbfba, 0x8fd9, 0x9ff8,
    0x6e17, 0x7e36, 0x4e55, 0x5e74, 0x2e93, 0x3eb2, 0x0ed1, 0x1ef0
};

/**
 * Calculate CRC16-CCITT over 16-bit word array
 * 
 * @param data Pointer to 16-bit word array
 * @param word_count Number of 16-bit words to process
 * @return CRC16 value
 * 
 * NOTE: Processes data as byte stream (little-endian word order)
 * Each 16-bit word contributes LSB first, then MSB
 */
/**
 * Hardware-accelerated CRC16-CCITT using RP2350's DMA sniffer
 * ~10X faster than software table lookup (10μs vs 100μs per frame)
 * This is CRITICAL for keeping RX processing under 25.9μs budget
 * 
 * DMA sniffer mode 0x2 = CRC-16-CCITT calculation in hardware
 * Note: DMA sniffer processes data during DMA transfer, so we use
 * a dummy DMA channel to feed data to the sniffer
 */
static uint16_t z1_bus_crc16(const uint16_t *data, uint32_t word_count) {
    // Seed with 0xFFFF for CRC-16-CCITT (standard initialization)
    dma_sniffer_set_data_accumulator(0xFFFF);
    
    // Configure sniffer for CRC-16-CCITT (mode 0x2)
    dma_sniffer_enable(0, 0x2, false);  // channel 0, CRC16-CCITT mode, no force
    
    // Configure dummy DMA transfer to feed data to sniffer
    dma_channel_config c = dma_channel_get_default_config(0);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_16);  // 16-bit transfers
    channel_config_set_read_increment(&c, true);   // Increment read address
    channel_config_set_write_increment(&c, false); // Don't increment write
    channel_config_set_sniff_enable(&c, true);     // Enable sniffer
    
    // Point write address to dummy location (sniffer captures data, we don't care about write)
    static volatile uint16_t dummy_sink;
    
    dma_channel_configure(0, &c,
                         (void*)&dummy_sink,  // Write to dummy location
                         data,                 // Read from input data
                         word_count,           // Transfer count
                         true);                // Start immediately
    
    // Wait for DMA to finish
    dma_channel_wait_for_finish_blocking(0);
    
    // Get CRC result from sniffer
    uint32_t crc_result = dma_sniffer_get_data_accumulator();
    
    // Disable sniffer
    dma_sniffer_disable();
    
    return (uint16_t)(crc_result & 0xFFFF);
}

// PIO and DMA resources
static PIO bus_pio = pio0;
static uint tx_sm = 0;
static uint rx_sm = 1;
static int tx_dma_chan = -1;
static int rx_dma_chan = -1;

// Node ID for this device (set during init)
// VOLATILE: Shared between cores, must not be cached
static volatile uint8_t sender_node_id = 0xFF;  // Invalid until set

// Buffers (sized for efficient DMA)
#define RX_BUFFER_SIZE 8192  // 8192 words = 16KB, power of 2 for ring buffer
static uint16_t rx_buffer[RX_BUFFER_SIZE] __attribute__((aligned(16384)));  // 16KB aligned for DMA ring wrap
static volatile uint32_t rx_write_index = 0;  // DMA writes here
static volatile uint32_t rx_read_index = 0;   // Application reads here

// RX Frame State Machine (moved here for visibility in init function)
typedef enum {
    RX_STATE_WAIT_HEADER,
    RX_STATE_WAIT_LENGTH,
    RX_STATE_WAIT_PAYLOAD,
    RX_STATE_WAIT_CRC,
    RX_STATE_DISCARD_WAIT_LENGTH,  // Frame rejected, need to read length to know how much to skip
    RX_STATE_DISCARD_SKIP           // Skipping payload+CRC of rejected frame
} rx_frame_state_t;

static rx_frame_state_t rx_state = RX_STATE_WAIT_HEADER;
static uint16_t rx_header = 0;
static uint16_t rx_length = 0;
static uint16_t rx_payload[600];
static uint16_t rx_payload_idx = 0;
static uint16_t rx_payload_words = 0;
static uint16_t rx_discard_remaining = 0;  // Beats remaining to skip for rejected frames
static absolute_time_t rx_frame_start = {0};  // Track RX frame timing

// Forward declarations
uint32_t z1_bus_rx_depth(void);

/**
 * Initialize bus as controller (ID 16)
 * CRITICAL: Controller enables pullups on CLK and DATA lines
 * Bidirectional: TX + RX for multi-master support
 */
void z1_bus_init_controller(void) {
    // Set sender node ID for controller
    sender_node_id = 16;
    
    // SELECT0 used for carrier sense (bus busy/idle indicator)
    // Controller maintains pull-down when idle, drives HIGH when transmitting
    gpio_init(BUS_SELECT0_PIN);
    gpio_set_dir(BUS_SELECT0_PIN, GPIO_IN);
    gpio_pull_down(BUS_SELECT0_PIN);  // Pull-down when idle
    
    // SELECT1: Reserved for future use
    gpio_init(BUS_SELECT1_PIN);
    gpio_set_dir(BUS_SELECT1_PIN, GPIO_IN);
    gpio_pull_down(BUS_SELECT1_PIN);
    
    // SELECT2-4: Reserved for Phase 3 arbitration, set as inputs with pull-downs
    for (int i = 2; i < 5; i++) {
        gpio_init(BUS_SELECT0_PIN + i);
        gpio_set_dir(BUS_SELECT0_PIN + i, GPIO_IN);
        gpio_set_pulls(BUS_SELECT0_PIN + i, false, true);  // pull-down
    }
    
    // Load PIO programs
    uint tx_offset = pio_add_program(bus_pio, &z1_bus_tx_program);
    uint rx_offset = pio_add_program(bus_pio, &z1_bus_rx_program);
    
    // Initialize TX state machine (SM0) - load program but DON'T configure pins yet
    // Controller will configure TX pins only when transmitting (same as nodes)
    pio_sm_config c = z1_bus_tx_program_get_default_config(tx_offset);
    sm_config_set_out_pins(&c, BUS_DATA0_PIN, 16);
    sm_config_set_sideset_pins(&c, BUS_CLK_PIN);
    sm_config_set_out_shift(&c, false, true, 16);
    sm_config_set_clkdiv(&c, 13.3f);  // 266MHz / (13.3 * 4) = 5 MHz (default)
    pio_sm_init(bus_pio, tx_sm, tx_offset, &c);
    // NOTE: Pins NOT configured for PIO yet - will be done in send_frame()
    
    // Initialize RX state machine (SM1)
    z1_bus_rx_program_init(bus_pio, rx_sm, rx_offset,
                           BUS_CLK_PIN, BUS_SELECT0_PIN, BUS_DATA0_PIN);
    
    // Clear RX FIFO before enabling
    pio_sm_clear_fifos(bus_pio, rx_sm);
    pio_sm_restart(bus_pio, rx_sm);
    
    // Claim DMA channels
    tx_dma_chan = dma_claim_unused_channel(true);
    rx_dma_chan = dma_claim_unused_channel(true);
    
    // Configure RX DMA for continuous capture
    dma_channel_config rx_config = dma_channel_get_default_config(rx_dma_chan);
    channel_config_set_transfer_data_size(&rx_config, DMA_SIZE_16);
    channel_config_set_read_increment(&rx_config, false);
    channel_config_set_write_increment(&rx_config, true);
    channel_config_set_dreq(&rx_config, pio_get_dreq(bus_pio, rx_sm, false));
    channel_config_set_ring(&rx_config, true, 14);  // 2^14 = 16384 bytes = 8192 words wraparound

    dma_channel_configure(
        rx_dma_chan,
        &rx_config,
        rx_buffer,
        &bus_pio->rxf[rx_sm],
        0xFFFFFFFF,
        true  // Start immediately
    );
    
    // Enable RX state machine
    pio_sm_set_enabled(bus_pio, rx_sm, true);
    
    printf("[BUS] Controller initialized (TX+RX, DMA chan %d/%d) - BUILD 2025-12-06-v3\n", tx_dma_chan, rx_dma_chan);
}

/**
 * Initialize bus as node (ID 0-15)
 * CRITICAL: Nodes NEVER enable pullups (16 nodes on bus)
 * Bidirectional: TX + RX for multi-master support
 */
void z1_bus_init_node(void) {
    // CRITICAL: Reset RX state machine variables (important for APP_PARTITION_MODE)
    // Bootloader may have left these in mid-frame state
    rx_state = RX_STATE_WAIT_HEADER;
    rx_header = 0;
    rx_length = 0;
    rx_payload_idx = 0;
    rx_payload_words = 0;
    rx_discard_remaining = 0;
    rx_read_index = 0;
    rx_write_index = 0;
    memset(rx_payload, 0, sizeof(rx_payload));
    
    // SELECT0 used for carrier sense (bus busy/idle indicator)
    // Nodes monitor SELECT0, drive HIGH when transmitting
    gpio_init(BUS_SELECT0_PIN);
    gpio_set_dir(BUS_SELECT0_PIN, GPIO_IN);
    gpio_disable_pulls(BUS_SELECT0_PIN);  // No pulls (controller has pull-down)
    
    // SELECT1: Reserved for future use
    gpio_init(BUS_SELECT1_PIN);
    gpio_set_dir(BUS_SELECT1_PIN, GPIO_IN);
    gpio_disable_pulls(BUS_SELECT1_PIN);  // No pulls (controller has pull-down)
    
    // SELECT2-4: Reserved for Phase 3 arbitration, set as inputs with pull-downs
    for (int i = 2; i < 5; i++) {
        gpio_init(BUS_SELECT0_PIN + i);
        gpio_set_dir(BUS_SELECT0_PIN + i, GPIO_IN);
        gpio_set_pulls(BUS_SELECT0_PIN + i, false, true);  // pull-down
    }
    
    // Load PIO programs
    uint tx_offset = pio_add_program(bus_pio, &z1_bus_tx_program);
    uint rx_offset = pio_add_program(bus_pio, &z1_bus_rx_program);
    
    // Initialize TX state machine (SM0) - load program but DON'T configure pins yet
    // Node will configure TX pins only when transmitting (to avoid interfering with bus)
    pio_sm_config c = z1_bus_tx_program_get_default_config(tx_offset);
    sm_config_set_out_pins(&c, BUS_DATA0_PIN, 16);
    sm_config_set_sideset_pins(&c, BUS_CLK_PIN);
    sm_config_set_out_shift(&c, false, true, 16);
    sm_config_set_clkdiv(&c, 13.3f);  // 266MHz / (13.3 * 4) = 5 MHz (default)
    pio_sm_init(bus_pio, tx_sm, tx_offset, &c);
    // NOTE: Pins NOT configured for PIO yet - will be done in send_frame()
    
    // Initialize RX state machine (SM1)
    z1_bus_rx_program_init(bus_pio, rx_sm, rx_offset,
                           BUS_CLK_PIN, BUS_SELECT0_PIN, BUS_DATA0_PIN);
    
    // CRITICAL: Clear FIFO before enabling (SDK pattern from logic_analyser)
    pio_sm_clear_fifos(bus_pio, rx_sm);
    pio_sm_restart(bus_pio, rx_sm);  // Clear input shift counter
    
    // Claim DMA channels
    tx_dma_chan = dma_claim_unused_channel(true);
    rx_dma_chan = dma_claim_unused_channel(true);
    
    // Configure RX DMA for continuous capture
    dma_channel_config rx_config = dma_channel_get_default_config(rx_dma_chan);
    channel_config_set_transfer_data_size(&rx_config, DMA_SIZE_16);
    channel_config_set_read_increment(&rx_config, false);            // Read from same FIFO
    channel_config_set_write_increment(&rx_config, true);            // Increment through buffer
    channel_config_set_dreq(&rx_config, pio_get_dreq(bus_pio, rx_sm, false));  // Paced by RX FIFO
    // Enable ring buffer on write (wraps at buffer boundary)
    channel_config_set_ring(&rx_config, true, 14);  // 2^14 = 16384 bytes = 8192 words wraparound

    // Configure circular DMA (automatically wraps)
    dma_channel_configure(
        rx_dma_chan,
        &rx_config,
        rx_buffer,                          // Destination: our buffer
        &bus_pio->rxf[rx_sm],              // Source: PIO RX FIFO
        0xFFFFFFFF,                         // Transfer count (continuous)
        true                                // Start immediately
    );
    
    // Enable RX state machine (runs continuously)
    pio_sm_set_enabled(bus_pio, rx_sm, true);
    
    printf("[BUS] Node initialized (TX+RX, DMA chan %d/%d)\n", tx_dma_chan, rx_dma_chan);
}

/**
 * Send a frame using DMA (zero-CPU overhead)
 * 
 * Frame format per protocol spec (Phase 2 with CRC):
 *   Beat 0 (SOP): BUS_DATA[15:12]=Type, [11:8]=Dest, [7:0]=StreamID
 *                 BUS_SELECT[0]=1 (SOP flag asserted for 1µs)
 *   Beat 1: Length in bytes (payload size)
 *   Beat 2..N: Payload data (max 256 words)
 *   Beat N+1: CRC16-CCITT checksum over beats 0..N
 *   Final beat: BUS_SELECT[1]=1 (EOP flag, Phase 2)
 * 
 * Total beats for 256-word payload: 259 (1 header + 1 length + 256 payload + 1 CRC)
 * 
 * @param frame_type Frame type (UNICAST=0, BROADCAST=1, ACK=2, CTRL=3)
 * @param dest_id Destination node ID (0-15=workers, 16=controller, 31=broadcast)
 * @param stream_id Stream ID (0-7) with optional NO_ACK flag in bit 3
 * @param data Pointer to payload buffer (16-bit beats)
 * @param num_beats Number of payload beats to send (max 256)
 * @return true if sent successfully
 */
bool z1_bus_send_frame(uint8_t frame_type, uint8_t dest_id, uint8_t stream_id, const uint16_t *data, uint32_t num_beats) {
    // Validation
    // CRITICAL LIMIT: Max frame size affects ALL layers (HTTP, broker, bus RX buffers)
    // Current: 600 words (1200 bytes payload) for large neuron table deployments
    // Larger frames = better throughput but higher latency for small spike messages
    // NOTE: Spikes remain small (<16 words), this limit is for deployment commands only
    // WARNING: Increasing this requires updating ALL buffer sizes in chain
    if (num_beats == 0 || num_beats > 600 || tx_dma_chan < 0) {
        return false;
    }
    
    // Start timing
    absolute_time_t tx_start = get_absolute_time();
    
    // Build frame in temporary buffer (header + length + payload)
    // Static to avoid stack overflow - placed in BSS section
    // Size: 2 header beats + 600 payload words + 1 CRC + margin = 1203 words
    static uint16_t tx_buffer[1203] __attribute__((aligned(4)));
    
    // Beat 0: Build header per Phase 3a spec
    // Format: Type[15:14] | Src[13:9] | Dest[8:4] | NoACK[3] | Stream[2:0]
    uint16_t header = ((frame_type & 0x03) << 14) | ((sender_node_id & 0x1F) << 9) | 
                      ((dest_id & 0x1F) << 4) | (stream_id & 0x0F);
    tx_buffer[0] = header;
    
    // Beat 1: Length in bytes
    tx_buffer[1] = num_beats * 2;
    
    // DEBUG: Print every frame being transmitted
    // DEBUG: Disabled - printf blocks for 100s of microseconds
    // printf("[TX] type=%d, src=%d, dest=%d, stream=0x%02X, header=0x%04X, len=%d\n",
    //        frame_type, sender_node_id, dest_id, stream_id, header, num_beats * 2);
    
    // Beat 2..N: Copy payload
    for (uint32_t i = 0; i < num_beats; i++) {
        tx_buffer[2 + i] = data[i];
    }
    
    uint32_t total_beats = num_beats + 2;  // header + length + payload
    
    // Phase 2: Calculate CRC16 over header + length + payload
    uint16_t crc = z1_bus_crc16(tx_buffer, total_beats);
    tx_buffer[total_beats] = crc;  // Append CRC as final beat
    total_beats++;  // Now includes CRC (259 beats for 256-word payload)
    
    // Initialize TX pins for PIO control (first time only for nodes)
    static bool tx_pins_initialized = false;
    if (!tx_pins_initialized) {
        pio_gpio_init(bus_pio, BUS_CLK_PIN);
        for (int i = 0; i < 16; i++) {
            pio_gpio_init(bus_pio, BUS_DATA0_PIN + i);
        }
        tx_pins_initialized = true;
    }
    
    // Clear TX FIFO and restart SM before each transmission
    pio_sm_set_enabled(bus_pio, tx_sm, false);
    pio_sm_clear_fifos(bus_pio, tx_sm);
    pio_sm_restart(bus_pio, tx_sm);
    
    // Apply RP2350-E5 DMA errata workaround
    hw_clear_bits(&dma_hw->ch[tx_dma_chan].ctrl_trig, DMA_CH0_CTRL_TRIG_EN_BITS);
    dma_channel_abort(tx_dma_chan);
    uint32_t pre_abort_timeout = 0;
    while (dma_channel_is_busy(tx_dma_chan)) {
        if (++pre_abort_timeout > 10000) {
            printf("[BUS] PRE-TX DMA abort timeout - DMA hardware stuck!\n");
            return false;  // Cannot transmit - DMA is stuck
        }
        tight_loop_contents();
    }
    
    // Grab bus - set pins as outputs
    pio_sm_set_consecutive_pindirs(bus_pio, tx_sm, BUS_DATA0_PIN, 16, true);
    pio_sm_set_consecutive_pindirs(bus_pio, tx_sm, BUS_CLK_PIN, 1, true);
    
    // Assert SELECT0 for carrier sense (bus busy indicator)
    gpio_set_dir(BUS_SELECT0_PIN, GPIO_OUT);
    gpio_put(BUS_SELECT0_PIN, 1);  // HIGH = bus busy
    
    // Configure DMA
    dma_channel_config tx_config = dma_channel_get_default_config(tx_dma_chan);
    channel_config_set_transfer_data_size(&tx_config, DMA_SIZE_16);
    channel_config_set_read_increment(&tx_config, true);
    channel_config_set_write_increment(&tx_config, false);
    channel_config_set_dreq(&tx_config, pio_get_dreq(bus_pio, tx_sm, true));
    
    dma_channel_configure(
        tx_dma_chan,
        &tx_config,
        &bus_pio->txf[tx_sm],
        tx_buffer,
        total_beats,
        true
    );
    
    // Wait for FIFO to have data, then enable PIO (WITH TIMEOUT)
    uint32_t fifo_wait = 0;
    while (pio_sm_get_tx_fifo_level(bus_pio, tx_sm) == 0) {
        if (++fifo_wait > 10000) {
            printf("[BUS] FIFO fill timeout - aborting TX\n");
            goto cleanup_and_return;
        }
        tight_loop_contents();
    }
    
    pio_sm_set_enabled(bus_pio, tx_sm, true);
    
    // Wait for completion (WITH TIMEOUT - SDK call blocks forever if DMA hangs)
    uint32_t dma_wait = 0;
    while (dma_channel_is_busy(tx_dma_chan)) {
        if (++dma_wait > 100000) {
            printf("[BUS] DMA completion timeout - forcing abort\n");
            break;
        }
        tight_loop_contents();
    }
    
cleanup_and_return:
    // Clean up DMA channel state
    hw_clear_bits(&dma_hw->ch[tx_dma_chan].ctrl_trig, DMA_CH0_CTRL_TRIG_EN_BITS);
    dma_channel_abort(tx_dma_chan);
    uint32_t abort_timeout_2 = 0;
    while (dma_channel_is_busy(tx_dma_chan)) {
        if (++abort_timeout_2 > 100000) {
            break;
        }
        tight_loop_contents();
    }
    
    // Wait for PIO FIFO to drain
    uint32_t drain_wait_count = 0;
    while (!pio_sm_is_tx_fifo_empty(bus_pio, tx_sm)) {
        drain_wait_count++;
        if (drain_wait_count > 100000) {
            break;
        }
        tight_loop_contents();
    }
    
    sleep_us(1);
    
    // Disable TX
    pio_sm_set_enabled(bus_pio, tx_sm, false);
    
    // CRITICAL FIX: Release data/clock pins BEFORE releasing SELECT0
    // This prevents corruption if another node sees SELECT0 deassert while we still drive data
    
    // 1. Clear outputs to known state (all LOW)
    gpio_put_masked((0xFFFF << BUS_DATA0_PIN) | (1 << BUS_CLK_PIN), 0);
    
    // 2. Release data/clock pins to high-impedance INPUT (float on bus)
    pio_sm_set_consecutive_pindirs(bus_pio, tx_sm, BUS_DATA0_PIN, 16, false);
    pio_sm_set_consecutive_pindirs(bus_pio, tx_sm, BUS_CLK_PIN, 1, false);
    
    // 3. NOW safe to release SELECT0 (other nodes can transmit without seeing our stale data)
    // Drive LOW briefly to discharge bus capacitance before releasing
    gpio_put(BUS_SELECT0_PIN, 0);  // LOW = bus idle
    sleep_us(5);  // 5μs discharge (optimized from 20μs - verified safe via scope)
    gpio_set_dir(BUS_SELECT0_PIN, GPIO_IN);  // Release to input (controller pulldown holds)
    
    // Only controller maintains pull-down (nodes must float to avoid parallel resistance)
    if (sender_node_id == 16) {
        gpio_pull_down(BUS_SELECT0_PIN);  // Controller maintains idle state
    }
    
    // Update TX statistics
    absolute_time_t tx_end = get_absolute_time();
    uint64_t elapsed_us = absolute_time_diff_us(tx_start, tx_end);
    bus_stats.last_tx_us = elapsed_us;
    bus_stats.total_tx_us += elapsed_us;
    bus_stats.tx_count++;
    
    return true;
}

/**
 * Check if RX buffer has data available
 * DMA continuously fills buffer, we track position via write address
 */
bool z1_bus_rx_available(void) {
    // Calculate DMA write position from write address
    uintptr_t write_addr = (uintptr_t)dma_channel_hw_addr(rx_dma_chan)->write_addr;
    uintptr_t buffer_start = (uintptr_t)rx_buffer;
    uint32_t dma_write = (write_addr - buffer_start) / sizeof(uint16_t);
    dma_write = dma_write % RX_BUFFER_SIZE;  // Handle ring wrap
    
    return (rx_read_index != dma_write);
}

/**
 * Read one beat from RX buffer (non-blocking)
 * 
 * @param data Pointer to receive 16-bit beat
 * @return true if data read, false if buffer empty
 */
bool z1_bus_rx_read(uint16_t *data) {
    // Calculate DMA write position from write address
    uintptr_t write_addr = (uintptr_t)dma_channel_hw_addr(rx_dma_chan)->write_addr;
    uintptr_t buffer_start = (uintptr_t)rx_buffer;
    
    // CRITICAL: Validate write_addr is within buffer range
    // Prevents infinite loop if DMA pointer becomes invalid after continuous operation
    uintptr_t buffer_end = buffer_start + (RX_BUFFER_SIZE * sizeof(uint16_t));
    if (write_addr < buffer_start || write_addr >= buffer_end) {
        // DMA write pointer out of range - HARDWARE FAILURE DETECTED
        // This happens after heavy bus traffic corrupts the DMA channel state
        static uint32_t last_recovery_time = 0;
        uint32_t now = time_us_32();
        
        // Rate-limit recovery attempts to once per 100ms (10 Hz max recovery rate)
        if (now - last_recovery_time > 100000) {
            printf("[BUS-RX CRITICAL] DMA corruption detected! addr=0x%08X (valid: 0x%08X-0x%08X)\n",
                   write_addr, buffer_start, buffer_end);
            printf("[BUS-RX RECOVERY] Resetting RX DMA and state machine...\n");
            
            // Perform full hardware reset
            z1_bus_rx_flush();
            
            printf("[BUS-RX RECOVERY] Reset complete - RX operational\n");
            last_recovery_time = now;
        }
        return false;
    }
    
    uint32_t dma_write = (write_addr - buffer_start) / sizeof(uint16_t);
    dma_write = dma_write % RX_BUFFER_SIZE;  // Handle ring buffer wrap
    
    if (rx_read_index == dma_write) {
        return false;  // Buffer empty
    }
    
    *data = rx_buffer[rx_read_index];
    rx_read_index = (rx_read_index + 1) % RX_BUFFER_SIZE;
    
    return true;
}

/**
 * Get current RX buffer depth
 */
uint32_t z1_bus_rx_depth(void) {
    // Calculate DMA write position from write address
    uintptr_t write_addr = (uintptr_t)dma_channel_hw_addr(rx_dma_chan)->write_addr;
    uintptr_t buffer_start = (uintptr_t)rx_buffer;
    uint32_t dma_write = (write_addr - buffer_start) / sizeof(uint16_t);
    dma_write = dma_write % RX_BUFFER_SIZE;  // Handle ring wrap
    
    if (dma_write >= rx_read_index) {
        return dma_write - rx_read_index;
    } else {
        return RX_BUFFER_SIZE - rx_read_index + dma_write;
    }
}

/**
 * Public CRC16 validation function (for test code)
 */
uint16_t z1_bus_crc16_validate(const uint16_t *data, uint32_t word_count) {
    return z1_bus_crc16(data, word_count);
}

/**
 * Flush RX buffer (discard all data)
 * CRITICAL FIX: Atomic flush prevents race conditions during multi-step operation
 */
void z1_bus_rx_flush(void) {
    // STEP 1: Disable RX state machine to prevent new data arriving during flush
    // This makes the flush atomic and prevents desync
    pio_sm_set_enabled(bus_pio, rx_sm, false);
    
    // STEP 2: Wait for RX FIFO to drain (let in-flight data settle)
    // This ensures the last frame is fully captured before we flush
    // Worst case: 259 beats @ 10MHz = 25.9μs + 4-word FIFO drain
    uint32_t timeout = 0;
    while (!pio_sm_is_rx_fifo_empty(bus_pio, rx_sm)) {
        if (++timeout > 1000) break;  // Safety: ~100μs max wait
        tight_loop_contents();
    }
    
    // STEP 3: Abort DMA and clear state
    dma_channel_abort(rx_dma_chan);
    pio_sm_clear_fifos(bus_pio, rx_sm);
    pio_sm_restart(bus_pio, rx_sm);
    
    // STEP 4: Reconfigure and restart DMA
    dma_channel_config rx_config = dma_channel_get_default_config(rx_dma_chan);
    channel_config_set_transfer_data_size(&rx_config, DMA_SIZE_16);
    channel_config_set_read_increment(&rx_config, false);
    channel_config_set_write_increment(&rx_config, true);
    channel_config_set_dreq(&rx_config, pio_get_dreq(bus_pio, rx_sm, false));
    channel_config_set_ring(&rx_config, true, 14);  // 2^14 = 16384 bytes = 8192 words wraparound
    
    dma_channel_configure(
        rx_dma_chan,
        &rx_config,
        rx_buffer,
        &bus_pio->rxf[rx_sm],
        0xFFFFFFFF,  // Continuous transfer
        false  // Don't start yet
    );
    
    // STEP 5: Reset software indices
    rx_read_index = 0;
    rx_write_index = 0;
    
    // STEP 5.5: CRITICAL - Reset RX state machine to WAIT_HEADER
    // Without this, state machine stays in mid-frame state with no data!
    rx_state = RX_STATE_WAIT_HEADER;
    rx_payload_idx = 0;
    rx_payload_words = 0;
    rx_discard_remaining = 0;
    
    // STEP 6: Re-enable RX state machine (atomic flush complete)
    pio_sm_set_enabled(bus_pio, rx_sm, true);
    
    // STEP 7: Start DMA
    dma_channel_start(rx_dma_chan);
}

/**
 * Set sender node ID (must be called by nodes after init)
 */
void z1_bus_set_node_id(uint8_t node_id) {
    sender_node_id = node_id;
    printf("[BUS] sender_node_id set to %d\n", sender_node_id);
}

/**
 * Get sender node ID (for broker to query current node ID)
 */
uint8_t z1_bus_get_node_id(void) {
    return sender_node_id;
}

/**
 * Check if bus is busy (SELECT0 HIGH indicates active transmission)
 * Used by broker for collision avoidance
 */
bool z1_bus_carrier_sense(void) {
    return gpio_get(BUS_SELECT0_PIN);
}

// Phase 3a: ACK/NAK tracking
static volatile bool last_ack_received = false;
static volatile uint8_t last_ack_stream = 0;
static volatile uint8_t last_ack_src = 0;

// Error tracking
static volatile uint32_t crc_error_count = 0;

/**
 * Try to receive a complete frame (non-blocking)
 * Processes available beats and returns when a complete valid frame is assembled
 * 
 * @param frame Pointer to frame structure to populate
 * @return true if complete valid frame received, false if no frame ready
 */
bool z1_bus_try_receive_frame(z1_frame_t *frame) {
    if (!frame) return false;
    
    // CRITICAL: Loop protection - prevent infinite loop if something goes wrong
    // At 10MHz: 259 beats per frame, reasonable to process ~1000 beats per call
    const uint32_t MAX_BEATS_PER_CALL = 1000;
    uint32_t beats_processed = 0;
    
    uint16_t beat;
    while (z1_bus_rx_read(&beat)) {
        beats_processed++;
        
        // Hard safety limit - if we've processed too many beats in one call, exit
        if (beats_processed >= MAX_BEATS_PER_CALL) {
            break;  // Force exit - will continue on next call
        }
        
        switch (rx_state) {
            case RX_STATE_WAIT_HEADER:
                rx_header = beat;
                
                // Start timing on first beat of frame
                rx_frame_start = get_absolute_time();
                
                // Parse header: Type[15:14] | Src[13:9] | Dest[8:4] | NoACK[3] | Stream[2:0]
                uint8_t type = (rx_header >> 14) & 0x3;
                uint8_t src = (rx_header >> 9) & 0x1F;
                uint8_t dest = (rx_header >> 4) & 0x1F;
                bool no_ack = (rx_header & 0x08) != 0;
                uint8_t stream = rx_header & 0x07;
                
                // Filter: accept frames addressed to us or broadcast (31)
                // For broadcasts, MUST accept from ourselves (intra-node synapses!)
                // For unicast, reject loopback to avoid confusion
                bool is_broadcast = (dest == 31);
                bool addressed_to_me = (dest == sender_node_id || is_broadcast);
                bool not_loopback = (src != sender_node_id);
                
                // DEBUG: Log ALL CTRL frames on streams 1/2/3 (not just stream 3)
                if (type == 3 && stream >= 1 && stream <= 3) {
                    printf("[BUS-RX] CTRL stream %d: header=0x%04X src=%d dest=%d me=%d myself=%d\n", 
                           stream, rx_header, src, dest, addressed_to_me, sender_node_id);
                }
                
                if (addressed_to_me && (is_broadcast || not_loopback)) {
                    // Store parsed header fields
                    frame->type = type;
                    frame->src = src;
                    frame->dest = dest;
                    frame->no_ack = no_ack;
                    frame->stream = stream;
                    rx_state = RX_STATE_WAIT_LENGTH;
                } else {
                    // Frame not for us - need to skip it WITHOUT flushing buffer
                    // Transition to DISCARD states to skip this frame only
                    rx_state = RX_STATE_DISCARD_WAIT_LENGTH;
                }
                break;
                
            case RX_STATE_WAIT_LENGTH:
                rx_length = beat;
                
                // CRITICAL: Validate length - max payload is 1200 bytes (600 words)
                // This must match Z1_BROKER_MAX_PAYLOAD_WORDS in z1_broker.h
                // If invalid, we're desynchronized - flush buffer and wait for fresh frames
                if (rx_length > 1200) {
                    z1_bus_rx_flush();  // Clear all buffered data
                    rx_state = RX_STATE_WAIT_HEADER;
                    return false;  // Exit immediately, wait for fresh data
                }
                
                frame->length = rx_length;
                rx_payload_words = (rx_length + 1) / 2;
                rx_payload_idx = 0;
                
                if (rx_payload_words > 0) {
                    rx_state = RX_STATE_WAIT_PAYLOAD;
                } else {
                    // Zero-length payload, go straight to CRC
                    rx_state = RX_STATE_WAIT_CRC;
                }
                break;
                
            case RX_STATE_WAIT_PAYLOAD:
                rx_payload[rx_payload_idx++] = beat;
                
                if (rx_payload_idx >= rx_payload_words) {
                    // Payload complete, wait for CRC
                    rx_state = RX_STATE_WAIT_CRC;
                }
                break;
                
            case RX_STATE_DISCARD_WAIT_LENGTH:
                // Read length of rejected frame so we know how much to skip
                rx_length = beat;
                
                // CRITICAL: Validate length - max payload is 1200 bytes (600 words)
                // This must match Z1_BROKER_MAX_PAYLOAD_WORDS in z1_broker.h
                // If invalid, we're desynchronized - flush buffer and wait for fresh frames
                if (rx_length > 1200) {
                    z1_bus_rx_flush();  // Clear all buffered data
                    rx_state = RX_STATE_WAIT_HEADER;
                    return false;  // Exit immediately, wait for fresh data
                }
                
                rx_payload_words = (rx_length + 1) / 2;
                // Need to skip: payload_words + 1 CRC beat
                rx_discard_remaining = rx_payload_words + 1;
                
                if (rx_discard_remaining > 0) {
                    rx_state = RX_STATE_DISCARD_SKIP;
                } else {
                    // Empty frame, just had CRC - back to waiting for next header
                    rx_state = RX_STATE_WAIT_HEADER;
                }
                break;
                
            case RX_STATE_DISCARD_SKIP:
                // Consume and discard payload/CRC beats of rejected frames
                // This is the PERFORMANCE BOTTLENECK at high traffic loads
                rx_discard_remaining--;
                
                // CRITICAL: Prevent race with DMA write pointer
                // Problem: CPU processes buffer faster than DMA writes (34ns vs 100ns per beat)
                //   - CPU read pointer can catch DMA write pointer mid-frame
                //   - Without barrier: CPU cache reads stale data → desync
                //   - With 1μs delay (Dec 2): Safe but 257μs per frame (813% CPU @ 30K spikes/sec)
                //   - With memory barrier (Dec 9): Safe and 25.7μs per frame (72% CPU @ 30K spikes/sec)
                // 
                // Memory barrier: Forces CPU to see DMA writes in order (no cache stale reads)
                //   - __sync_synchronize() = full compiler + hardware memory fence
                //   - Ensures read from rx_buffer sees latest DMA write
                //   - 10× faster than 1μs delay, still prevents race
                //
                // Future optimization (Phase 3c): SOP-synchronized RX PIO
                //   - PIO waits for SOP (SELECT1 pulse) before capturing frame
                //   - PIO auto-stops after 259 beats (beat counter in X register)
                //   - Rejected frames: PIO skips to next SOP in HARDWARE (0 CPU)
                //   - DISCARD_SKIP state eliminated entirely (640× speedup)
                //   - See: docs/SOP_EOP_IMPLEMENTATION_ANALYSIS.md
                __sync_synchronize();  // Full memory barrier (compiler + hardware)
                
                if (rx_discard_remaining == 0) {
                    // Done skipping this rejected frame, ready for next frame
                    rx_state = RX_STATE_WAIT_HEADER;
                }
                break;
                
            case RX_STATE_WAIT_CRC: {
                // Validate CRC16
                uint16_t received_crc = beat;
                
                // Build frame for CRC calculation: header + length + payload
                // Max: 1 header + 1 length + 600 payload = 602 words
                static uint16_t crc_frame[602];
                crc_frame[0] = rx_header;
                crc_frame[1] = rx_length;
                // Optimized: memcpy is 2-3X faster than loop for block copies
                if (rx_payload_words > 0) {
                    memcpy(&crc_frame[2], rx_payload, rx_payload_words * sizeof(uint16_t));
                }
                
                uint16_t calculated_crc = z1_bus_crc16(crc_frame, rx_payload_words + 2);
                frame->crc_valid = (calculated_crc == received_crc);
                
                // Track CRC errors
                if (!frame->crc_valid) {
                    crc_error_count++;
                }
                
                // Copy payload to output frame (optimized with memcpy)
                if (rx_payload_words > 0) {
                    memcpy(frame->payload, rx_payload, rx_payload_words * sizeof(uint16_t));
                }
                
                // End timing and update statistics
                absolute_time_t rx_end = get_absolute_time();
                uint64_t elapsed_us = absolute_time_diff_us(rx_frame_start, rx_end);
                frame->rx_time_us = elapsed_us;
                bus_stats.last_rx_us = elapsed_us;
                bus_stats.total_rx_us += elapsed_us;
                bus_stats.rx_count++;
                
                // Phase 3a: Detect ACK frames (CTRL frames with ACK opcode)
                if (frame->type == Z1_FRAME_TYPE_CTRL && frame->crc_valid && frame->length >= 4) {
                    uint16_t opcode = frame->payload[0];
                    if (opcode == Z1_OPCODE_ACK) {
                        last_ack_received = true;
                        last_ack_stream = frame->stream;
                        last_ack_src = frame->src;
                    }
                }
                
                // Phase 3a: Auto-send ACK for UNICAST frames only (if ACK required)
                // CTRL frames are NEVER auto-ACKed (application handles responses)
                // BROADCAST frames are NEVER ACKed (can't ACK to all)
                if (frame->type == Z1_FRAME_TYPE_UNICAST &&  // Only UNICAST
                    frame->crc_valid && 
                    !frame->no_ack) {                         // And ACK requested
                    // Optimized: 200ns turnaround (was 1μs) - safe with HW CRC acceleration
                    busy_wait_us_32(0);  // Minimal delay - HW CRC is fast enough
                    z1_bus_send_ack(frame->src, frame->stream);
                    // NO flush - let DMA continue capturing
                }
                
                // Process control frames (PING, TOPOLOGY)
                if (frame->type == Z1_FRAME_TYPE_CTRL && frame->crc_valid && frame->length >= 4) {
                    uint16_t opcode = frame->payload[0];
                    
                    // Auto-reply to PING requests (but NOT to our own pings!)
                    if (opcode == Z1_OPCODE_PING && frame->length >= 12 && frame->src != sender_node_id) {
                        // Echo back: PING_REPLY + sequence + same 4 data words
                        uint16_t reply_payload[6];
                        reply_payload[0] = Z1_OPCODE_PING_REPLY;
                        reply_payload[1] = frame->payload[1];  // Echo sequence
                        reply_payload[2] = frame->payload[2];  // Echo data[0]
                        reply_payload[3] = frame->payload[3];  // Echo data[1]
                        reply_payload[4] = frame->payload[4];  // Echo data[2]
                        reply_payload[5] = frame->payload[5];  // Echo data[3]
                        
                        // Send reply (NO_ACK to avoid loops)
                        z1_bus_send_frame(Z1_FRAME_TYPE_CTRL, frame->src, Z1_STREAM_NO_ACK, 
                                         reply_payload, 6);
                        // Note: No RX flush needed - controller filters by sequence number
                    }
                    
                    // Auto-process TOPOLOGY broadcasts
                    if (opcode == Z1_OPCODE_TOPOLOGY && frame->length >= (Z1_MAX_NODES + 2) * 2) {
                        z1_bus_process_topology_broadcast(frame->payload, frame->length / 2);
                    }
                    // PING_REPLY handled by application
                }
                
                // Reset state machine for next frame
                rx_state = RX_STATE_WAIT_HEADER;
                
                // Return frame (even if CRC failed - caller can check crc_valid)
                return true;
            }
        }
    }
    
    return false;  // No complete frame yet
}

/**
 * Receive a complete frame (blocking)
 * Waits until a valid frame is received
 * 
 * @param frame Pointer to frame structure to populate
 * @return true if valid frame received
 */
bool z1_bus_receive_frame(z1_frame_t *frame) {
    while (true) {
        if (z1_bus_try_receive_frame(frame)) {
            if (frame->crc_valid) {
                return true;  // Valid frame received
            }
            // CRC error - discard and keep waiting
        }
        // Brief sleep to avoid tight loop
        sleep_us(10);
    }
}

/**
 * Get bus timing statistics
 */
void z1_bus_get_stats(z1_bus_stats_t *stats) {
    if (stats) {
        *stats = bus_stats;
    }
}

void z1_bus_set_speed_mhz(float bus_mhz) {
    // Calculate clock divider: CPU_FREQ / (bus_mhz * 4 instructions per beat)
    // At 266 MHz: divider = 266 / (bus_mhz * 4)
    float divider = 266.0f / (bus_mhz * 4.0f);
    
    // Reconfigure TX state machine clock divider
    pio_sm_set_clkdiv(bus_pio, tx_sm, divider);
    
    // Brief pause to let new divider take effect
    sleep_us(10);
}

/**
 * Reset bus timing statistics
 */
void z1_bus_reset_stats(void) {
    bus_stats.last_tx_us = 0;
    bus_stats.last_rx_us = 0;
    bus_stats.total_tx_us = 0;
    bus_stats.total_rx_us = 0;
    bus_stats.tx_count = 0;
    bus_stats.rx_count = 0;
}

/**
 * Check if ACK was received (Phase 3a)
 */
bool z1_bus_check_ack(uint8_t expected_src, uint8_t expected_stream) {
    if (last_ack_received && 
        last_ack_src == expected_src && 
        last_ack_stream == expected_stream) {
        return true;
    }
    return false;
}

/**
 * Clear ACK received flag (Phase 3a)
 */
void z1_bus_clear_ack(void) {
    last_ack_received = false;
    last_ack_stream = 0;
    last_ack_src = 0;
}

/**
 * Get last received ACK details (Phase 3a)
 */
bool z1_bus_get_last_ack(uint8_t *out_src, uint8_t *out_stream) {
    if (out_src) {
        *out_src = last_ack_src;
    }
    if (out_stream) {
        *out_stream = last_ack_stream;
    }
    return last_ack_received;
}

/**
 * Get frame type from header word
 */
uint8_t z1_bus_get_frame_type(uint16_t header) {
    return (header >> 14) & 0x03;
}

/**
 * Get source node ID from header word
 */
uint8_t z1_bus_get_frame_src(uint16_t header) {
    return (header >> 9) & 0x1F;
}

/**
 * Get destination node ID from header word
 */
uint8_t z1_bus_get_frame_dest(uint16_t header) {
    return (header >> 4) & 0x1F;
}

/**
 * Get stream ID from header word
 */
uint8_t z1_bus_get_frame_stream(uint16_t header) {
    return header & 0x0F;
}

/**
 * Check if bus is currently receiving a frame
 */
bool z1_bus_is_receiving(void) {
    return (rx_state != RX_STATE_WAIT_HEADER);
}

/**
 * Check if bus is currently transmitting a frame
 */
bool z1_bus_is_transmitting(void) {
    // Check if TX DMA channel is busy
    return dma_channel_is_busy(tx_dma_chan);
}

/**
 * Get total CRC error count
 */
uint32_t z1_bus_get_crc_error_count(void) {
    return crc_error_count;
}

/**
 * Reset CRC error counter
 */
void z1_bus_reset_crc_error_count(void) {
    crc_error_count = 0;
}

// ============================================================================
// Phase 3a: ACK/NAK Protocol Implementation
// ============================================================================

/**
 * Send ACK frame (simplified wrapper using common TX path)
 * ACKs always use NO_ACK flag to prevent ACK-of-ACK loops
 */
bool z1_bus_send_ack(uint8_t dest_id, uint8_t stream_id) {
    uint16_t ack_payload[2];
    ack_payload[0] = Z1_OPCODE_ACK;  // 0x0001
    ack_payload[1] = stream_id;      // Echo stream for matching
    return z1_bus_send_frame(Z1_FRAME_TYPE_CTRL, dest_id, stream_id | Z1_STREAM_NO_ACK, ack_payload, 2);
}

/**
 * Send frame with ACK confirmation
 * Single attempt - no retries (retransmission handled by broker layer)
 */
bool z1_bus_send_frame_with_ack(uint8_t dest_id, const uint16_t *data, 
                                 uint32_t num_beats, uint8_t stream_id) {
    // Clear any pending ACK before sending
    z1_bus_clear_ack();
    
    // Send the frame with specified stream_id (UNICAST type, stream without NO_ACK)
    if (!z1_bus_send_frame(Z1_FRAME_TYPE_UNICAST, dest_id, stream_id & Z1_STREAM_MASK, data, num_beats)) {
        return false;  // Send failed
    }
    
    // Flush our own transmission from RX buffer immediately
    // At 10MHz: 259 beats × 100ns = 25.9μs transmission time
    z1_bus_rx_flush();
    
    // Wait for ACK with timeout
    // Expected ACK arrival: ~5-10μs (node turnaround + 0.5μs ACK transmission)
    absolute_time_t timeout = make_timeout_time_ms(Z1_ACK_TIMEOUT_MS);
    
    while (!time_reached(timeout)) {
        // Check if ACK received
        if (z1_bus_check_ack(dest_id, stream_id)) {
            return true;  // Success!
        }
        
        // Process any incoming frames (might be the ACK)
        z1_frame_t frame;
        z1_bus_try_receive_frame(&frame);
        
        // Small delay to avoid tight loop
        sleep_us(10);
    }
    
    // Timeout - return failure (broker will handle retry if needed)
    return false;
}

// ============================================================================
// Topology Discovery (Phase 3b)
// ============================================================================

/**
 * Ping a node to check if it's online (with 4 random data words)
 * Waits for PING_REPLY control frame
 * 
 * @param node_id Target node ID (0-16)
 * @return true if node responded with valid ping reply
 */
bool z1_bus_ping_node(uint8_t node_id) {
    if (node_id >= Z1_MAX_NODES) {
        return false;  // Invalid node ID
    }
    
    // Build ping payload: opcode + sequence + 4 random data words
    static uint16_t ping_seq = 0;
    uint16_t ping_payload[6];
    ping_payload[0] = Z1_OPCODE_PING;
    ping_payload[1] = ping_seq++;
    
    // Generate 4 random data words (simple PRNG)
    uint32_t seed = to_us_since_boot(get_absolute_time());
    for (int i = 0; i < 4; i++) {
        seed = seed * 1103515245 + 12345;  // LCG algorithm
        ping_payload[2 + i] = (uint16_t)(seed >> 16);
    }
    
    // Flush RX buffer to clear any old frames
    z1_bus_rx_flush();
    
    // Send PING control frame (NO_ACK, we wait for PING_REPLY instead)
    if (!z1_bus_send_frame(Z1_FRAME_TYPE_CTRL, node_id, Z1_STREAM_NO_ACK, ping_payload, 6)) {
        return false;
    }
    
    // Wait for PING_REPLY
    absolute_time_t timeout = make_timeout_time_ms(Z1_PING_TIMEOUT_MS);
    while (!time_reached(timeout)) {
        z1_frame_t reply;
        if (z1_bus_try_receive_frame(&reply)) {
            // Check if it's a PING_REPLY from the target node
            if (reply.type == Z1_FRAME_TYPE_CTRL &&
                reply.src == node_id &&
                reply.crc_valid &&
                reply.length >= 12 &&  // 6 words = 12 bytes
                reply.payload[0] == Z1_OPCODE_PING_REPLY &&
                reply.payload[1] == ping_payload[1]) {  // Match sequence
                
                // Verify echoed data matches
                bool data_match = true;
                for (int i = 0; i < 4; i++) {
                    if (reply.payload[2 + i] != ping_payload[2 + i]) {
                        data_match = false;
                        break;
                    }
                }
                return data_match;
            }
        }
    }
    
    return false;  // Timeout
}

/**
 * Discover cluster topology (Controller only)
 */
uint8_t z1_bus_discover_topology(z1_topology_t *topology) {
    if (!topology) {
        return 0;
    }
    
    uint8_t online_count = 0;
    uint32_t current_time_ms = to_ms_since_boot(get_absolute_time());
    
    // Ping all nodes (0-16)
    for (uint8_t node_id = 0; node_id < Z1_MAX_NODES; node_id++) {
        topology->nodes[node_id].node_id = node_id;
        topology->nodes[node_id].online = z1_bus_ping_node(node_id);
        
        if (topology->nodes[node_id].online) {
            topology->nodes[node_id].last_seen_ms = current_time_ms;
            online_count++;
        }
    }
    
    topology->online_count = online_count;
    topology->last_update_ms = current_time_ms;
    
    // Update library's internal copy
    memcpy(&cluster_topology, topology, sizeof(z1_topology_t));
    
    return online_count;
}

/**
 * Broadcast topology to all nodes (Controller only)
 */
bool z1_bus_broadcast_topology(const z1_topology_t *topology) {
    if (!topology) {
        return false;
    }
    
    // Pack topology into broadcast payload
    // Format: [OPCODE_TOPOLOGY, online_count, node0_status, node1_status, ...]
    uint16_t payload[Z1_MAX_NODES + 2];
    payload[0] = Z1_OPCODE_TOPOLOGY;
    payload[1] = topology->online_count;
    
    for (uint8_t i = 0; i < Z1_MAX_NODES; i++) {
        // Pack node status: bit 0 = online flag
        payload[i + 2] = topology->nodes[i].online ? 1 : 0;
    }
    
    // Broadcast to all nodes (NO_ACK enforced by z1_bus_broadcast)
    return z1_bus_broadcast(payload, Z1_MAX_NODES + 2, 0);
}

/**
 * Get local topology copy (All nodes)
 */
const z1_topology_t* z1_bus_get_topology(void) {
    return &cluster_topology;
}

/**
 * Process received topology broadcast (called internally by receive handler)
 * This should be called when a TOPOLOGY opcode is received
 */
static void z1_bus_process_topology_broadcast(const uint16_t *payload, uint16_t length) {
    if (length < Z1_MAX_NODES + 2) {
        return;  // Invalid topology payload
    }
    
    uint32_t current_time_ms = to_ms_since_boot(get_absolute_time());
    
    cluster_topology.online_count = payload[1];
    cluster_topology.last_update_ms = current_time_ms;
    
    for (uint8_t i = 0; i < Z1_MAX_NODES; i++) {
        cluster_topology.nodes[i].node_id = i;
        cluster_topology.nodes[i].online = (payload[i + 2] != 0);
        if (cluster_topology.nodes[i].online) {
            cluster_topology.nodes[i].last_seen_ms = current_time_ms;
        }
    }
}
