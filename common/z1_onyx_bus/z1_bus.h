/**
 * Z1 Onyx Bus - Public API Header
 */

#ifndef Z1_BUS_H
#define Z1_BUS_H

#include <stdint.h>
#include <stdbool.h>

/**
 * Frame type definitions
 */
#define Z1_FRAME_TYPE_UNICAST   0
#define Z1_FRAME_TYPE_BROADCAST 1
#define Z1_FRAME_TYPE_ACK       2
#define Z1_FRAME_TYPE_CTRL      3

/**
 * Header flag for NO_ACK (bit 3 of header word)
 * Set this bit in stream_id parameter to disable ACK requirement
 */
#define Z1_STREAM_NO_ACK        0x08    // Bit 3: No ACK required
#define Z1_STREAM_MASK          0x07    // Bits 2:0: Stream ID (0-7)

/**
 * Protocol configuration
 */
#define Z1_ACK_TIMEOUT_MS       10      // ACK wait timeout (milliseconds)
#define Z1_PING_TIMEOUT_MS      50      // PING reply timeout (milliseconds)
#define Z1_ACK_MAX_RETRIES      3       // Maximum retry attempts
#define Z1_MAX_NODES            17      // Maximum nodes in cluster (0-16)

/**
 * Special opcodes for control frames
 */
#define Z1_OPCODE_ACK           0x0001  // ACK response (CTRL frame)
#define Z1_OPCODE_PING          0x0002  // Topology ping request
#define Z1_OPCODE_PING_REPLY    0x0003  // Ping reply
#define Z1_OPCODE_TOPOLOGY      0x0004  // Topology broadcast

/**
 * Received frame structure
 */
typedef struct {
    uint8_t type;           // Frame type (UNICAST/BROADCAST/ACK/CTRL)
    uint8_t src;            // Sender node ID (0-31)
    uint8_t dest;           // Destination node ID (0-31, 31=broadcast)
    uint8_t stream;         // Stream ID (0-7, 3 bits)
    bool no_ack;            // No ACK required flag (bit 3)
    uint16_t length;        // Payload length in bytes
    // CRITICAL: Max payload size (must match Z1_BROKER_MAX_PAYLOAD_WORDS)
    // WARNING: Changing this requires updating ALL buffers in chain:
    //   - HTTP API decoded[1200] and frame[606]
    //   - Broker MAX_PAYLOAD_WORDS and request payload[600]
    //   - Bus TX validation, tx_buffer[1203]
    //   - Bus RX validation (1200 bytes), rx_payload[600], crc_frame[602]
    uint16_t payload[600];  // Payload data (max 600 words = 1200 bytes)
    bool crc_valid;         // CRC validation result
    uint64_t rx_time_us;    // Time to receive this frame (microseconds)
} z1_frame_t;

/**
 * Bus timing statistics
 */
typedef struct {
    uint64_t last_tx_us;    // Last TX frame time (microseconds)
    uint64_t last_rx_us;    // Last RX frame time (microseconds)
    uint64_t total_tx_us;   // Total TX time (microseconds)
    uint64_t total_rx_us;   // Total RX time (microseconds)
    uint32_t tx_count;      // Number of frames transmitted
    uint32_t rx_count;      // Number of frames received
    uint32_t collision_count; // Collisions detected during TX
} z1_bus_stats_t;

/**
 * Node status for topology
 */
typedef struct {
    uint8_t node_id;        // Node ID (0-16)
    bool online;            // Online status (true if responds to ping)
    uint32_t last_seen_ms;  // Time since last response (milliseconds)
} z1_node_status_t;

/**
 * Cluster topology table
 */
typedef struct {
    z1_node_status_t nodes[Z1_MAX_NODES];  // All 17 nodes
    uint8_t online_count;                   // Number of online nodes
    uint32_t last_update_ms;                // Time of last topology update
} z1_topology_t;

/**
 * Initialize bus as controller (ID 16)
 * Enables pullups on CLK, DATA, and SELECT lines
 */
void z1_bus_init_controller(void);

/**
 * Initialize bus as node (ID 0-15)
 * Does NOT enable pullups (controller provides them)
 */
void z1_bus_init_node(void);

/**
 * Set sender node ID (must be called after init for nodes)
 * Controller automatically sets ID=16 during init
 * 
 * @param node_id This node's ID (0-15 for workers, 16=controller)
 */
void z1_bus_set_node_id(uint8_t node_id);

/**
 * Get this node's ID
 * 
 * @return Node ID (0-15 for nodes, 16 for controller, 0xFF if not yet set)
 */
uint8_t z1_bus_get_node_id(void);

/**
 * Check if bus is busy (carrier sense for collision avoidance)
 * 
 * @return true if SELECT0 is HIGH (bus active), false if idle
 */
bool z1_bus_carrier_sense(void);

/**
/**
 * Send a frame using DMA (zero-CPU overhead) - LOW LEVEL
 * 
 * This is the low-level send function. For most use cases, prefer:
 *   - z1_bus_send_frame_with_ack() for reliable delivery (ACK enabled, default)
 *   - z1_bus_send_frame_no_ack() for fire-and-forget (ACK disabled)
 * 
 * Frame format per spec:
 *   Beat 0: Type[15:14], Src[13:9], Dest[8:4], NoACK[3], Stream[2:0] with SOP flag
 *   Beat 1: Length in bytes
 *   Beat 2..N: Payload data
 *   Beat N+1: CRC16-CCITT checksum
 * 
 * @param frame_type Frame type (UNICAST=0, BROADCAST=1, ACK=2, CTRL=3)
 * @param dest_id Destination node ID (0-15=workers, 16=controller, 31=broadcast)
 * @param stream_id Stream ID (0-7) with optional NO_ACK flag in bit 3
 * @param data Pointer to data buffer (16-bit beats)
 * @param num_beats Number of 16-bit beats to send (max 256)
 * @return true if sent successfully (does NOT wait for ACK)
 */
bool z1_bus_send_frame(uint8_t frame_type, uint8_t dest_id, uint8_t stream_id, const uint16_t *data, uint32_t num_beats);

/**
 * Check if RX buffer has data available
 * (DMA continuously fills buffer in background)
 */
bool z1_bus_rx_available(void);

/**
 * Read one beat from RX buffer (non-blocking)
 * 
 * @param data Pointer to receive 16-bit beat
 * @return true if data read, false if buffer empty
 */
bool z1_bus_rx_read(uint16_t *data);

/**
 * Get current RX buffer depth
 */
uint32_t z1_bus_rx_depth(void);

/**
 * Flush RX FIFO (discard all data)
 */
void z1_bus_rx_flush(void);

/**
 * Calculate CRC16-CCITT over 16-bit word array
 * (Internal utility, exposed for test validation)
 * 
 * @param data Pointer to 16-bit word array
 * @param word_count Number of 16-bit words to process
 * @return CRC16 value
 */
uint16_t z1_bus_crc16_validate(const uint16_t *data, uint32_t word_count);

/**
 * Receive a complete frame (blocking)
 * Handles RX state machine, frame parsing, and CRC validation internally
 * Only returns frames addressed to this node or broadcast
 * 
 * @param frame Pointer to frame structure to populate
 * @return true if valid frame received, false on error
 */
bool z1_bus_receive_frame(z1_frame_t *frame);

/**
 * Try to receive a frame (non-blocking)
 * Returns immediately if no complete frame is available
 * 
 * @param frame Pointer to frame structure to populate
 * @return true if valid frame received, false if no frame ready or error
 */
bool z1_bus_try_receive_frame(z1_frame_t *frame);

/**
 * Get bus timing statistics
 * 
 * @param stats Pointer to statistics structure to populate
 */
void z1_bus_get_stats(z1_bus_stats_t *stats);

/**
 * Set bus clock speed (MHz)
 * Reconfigures TX PIO clock divider for new bus frequency
 * Formula: divider = CPU_FREQ_MHZ / (bus_mhz * 4)
 * 
 * @param bus_mhz Bus speed in MHz (1-20, typical 5-12)
 */
void z1_bus_set_speed_mhz(float bus_mhz);

/**
 * Reset bus timing statistics
 */
void z1_bus_reset_stats(void);

/**
 * Check if ACK was received (Phase 3a)
 * 
 * @param expected_src Expected source node ID (destination of original frame)
 * @param expected_stream Expected stream ID
 * @return true if matching ACK received
 */
bool z1_bus_check_ack(uint8_t expected_src, uint8_t expected_stream);

/**
 * Clear ACK received flag (Phase 3a)
 */
void z1_bus_clear_ack(void);

/**
 * Get last received ACK details (Phase 3a)
 * 
 * @param out_src Pointer to store ACK source node ID (can be NULL)
 * @param out_stream Pointer to store ACK stream ID (can be NULL)
 * @return true if ACK flag is set
 */
bool z1_bus_get_last_ack(uint8_t *out_src, uint8_t *out_stream);

/**
 * Get frame type from header word
 * 
 * @param header 16-bit header word
 * @return Frame type (0=UNICAST, 1=BROADCAST, 2=ACK, 3=CTRL)
 */
uint8_t z1_bus_get_frame_type(uint16_t header);

/**
 * Get source node ID from header word
 * 
 * @param header 16-bit header word
 * @return Source node ID (0-31)
 */
uint8_t z1_bus_get_frame_src(uint16_t header);

/**
 * Get destination node ID from header word
 * 
 * @param header 16-bit header word
 * @return Destination node ID (0-31)
 */
uint8_t z1_bus_get_frame_dest(uint16_t header);

/**
 * Get stream ID from header word
 * 
 * @param header 16-bit header word
 * @return Stream ID (0-7, 3 bits)
 */
uint8_t z1_bus_get_frame_stream(uint16_t header);

/**
 * Check if bus is currently receiving a frame
 * 
 * @return true if RX state machine is mid-frame
 */
bool z1_bus_is_receiving(void);

/**
 * Check if bus is currently transmitting a frame
 * 
 * @return true if TX operation in progress
 */
bool z1_bus_is_transmitting(void);

/**
 * Get total CRC error count
 * 
 * @return Number of frames received with CRC errors
 */
uint32_t z1_bus_get_crc_error_count(void);

/**
 * Reset CRC error counter
 */
void z1_bus_reset_crc_error_count(void);

// ============================================================================
// Phase 3a: ACK/NAK Protocol Functions
// ============================================================================

/**
 * Send frame with ACK confirmation (Phase 3a) - RECOMMENDED DEFAULT
 * 
 * This is the recommended send function for reliable delivery.
 * Sends UNICAST frame and waits for ACK with timeout/retry logic.
 * Automatically retries up to 3 times with 10ms timeout per attempt.
 * 
 * @param dest_id Destination node ID (0-15=workers, 16=controller)
 * @param data Pointer to payload buffer (16-bit beats)
 * @param num_beats Number of payload beats to send (max 256)
 * @param stream_id Stream ID for this transfer (0-7)
 * @return true if frame sent AND ACK received, false on timeout/failure
 * 
 * Example:
 *   uint16_t data[256] = {...};
 *   if (z1_bus_send_frame_with_ack(5, data, 256, 0)) {
 *       printf("Frame sent and acknowledged!\n");
 *   }
 */
bool z1_bus_send_frame_with_ack(uint8_t dest_id, const uint16_t *data, 
                                 uint32_t num_beats, uint8_t stream_id);

/**
 * Send frame without ACK (fire-and-forget) - CONVENIENCE WRAPPER
 * 
 * Sends UNICAST frame with NO_ACK flag set. Receiver will NOT send ACK.
 * Use for high-frequency data where occasional loss is acceptable.
 * 
 * @param dest_id Destination node ID (0-15=workers, 16=controller)
 * @param data Pointer to payload buffer (16-bit beats)
 * @param num_beats Number of payload beats to send (max 256)
 * @param stream_id Stream ID for this transfer (0-7)
 * @return true if frame sent (does NOT wait for ACK)
 * 
 * Example:
 *   uint16_t sensor_data[64] = {...};
 *   z1_bus_send_frame_no_ack(16, sensor_data, 64, 3);  // Fast sensor stream
 */
static inline bool z1_bus_send_frame_no_ack(uint8_t dest_id, const uint16_t *data,
                                              uint32_t num_beats, uint8_t stream_id) {
    return z1_bus_send_frame(Z1_FRAME_TYPE_UNICAST, dest_id, stream_id | Z1_STREAM_NO_ACK, data, num_beats);
}

/**
 * Broadcast frame to all nodes (Phase 3a) - NO_ACK ENFORCED
 * 
 * Sends frame to all nodes on the bus (dest=31, broadcast address).
 * NO_ACK flag is ALWAYS set to prevent all nodes from ACKing simultaneously
 * (which would cause bus collision). Fire-and-forget only.
 * 
 * @param data Pointer to data array (max 256 words)
 * @param num_beats Number of 16-bit words to send (1-256)
 * @param stream_id Stream ID (0-7)
 * @return true if sent successfully (does NOT guarantee reception)
 * 
 * Use cases:
 *   - Controller broadcasts topology updates
 *   - Time synchronization beacons
 *   - Global configuration changes
 * 
 * Example:
 *   uint16_t topology[16] = {node_status...};
 *   z1_bus_broadcast(topology, 16, 0);  // All nodes receive
 */
static inline bool z1_bus_broadcast(const uint16_t *data, uint32_t num_beats, uint8_t stream_id) {
    return z1_bus_send_frame(Z1_FRAME_TYPE_BROADCAST, 31, stream_id | Z1_STREAM_NO_ACK, data, num_beats);
}

/**
 * Send ACK frame (Phase 3a) - AUTOMATIC RESPONSE
 * 
 * Used by receiver to acknowledge successful frame reception.
 * Typically called automatically by nodes when receiving frames.
 * ACK frames always have NO_ACK flag set to prevent ACK-of-ACK loops.
 * 
 * @param dest_id Destination node ID (original sender)
 * @param stream_id Stream ID from original frame (0-7)
 * @return true if ACK sent successfully
 * 
 * Note: Most applications don't call this directly - nodes auto-send ACKs
 *       unless the received frame has the NO_ACK flag set.
 */
bool z1_bus_send_ack(uint8_t dest_id, uint8_t stream_id);

/**
 * ============================================================================
 * TOPOLOGY DISCOVERY (Phase 3b)
 * ============================================================================
 */

/**
 * Ping a node to check if it's online (UNICAST with ACK)
 * 
 * Sends PING opcode to target node and waits for ACK response.
 * Used by controller during topology discovery.
 * 
 * @param node_id Target node ID (0-16)
 * @return true if node responded (online), false if timeout (offline)
 * 
 * Example:
 *   if (z1_bus_ping_node(5)) {
 *       printf("Node 5 is online\n");
 *   }
 */
bool z1_bus_ping_node(uint8_t node_id);

/**
 * Discover cluster topology (Controller only)
 * 
 * Pings all nodes (0-16) and updates topology table.
 * 
 * @param topology Pointer to topology structure to update
 * @return Number of online nodes discovered
 * 
 * Example (in controller):
 *   z1_topology_t cluster_topology;
 *   uint8_t online = z1_bus_discover_topology(&cluster_topology);
 *   printf("Discovered %d online nodes\n", online);
 */
uint8_t z1_bus_discover_topology(z1_topology_t *topology);

/**
 * Broadcast topology to all nodes (Controller only)
 * 
 * Sends topology table as broadcast (NO_ACK) to all nodes.
 * Nodes receive and update their local topology copy.
 * 
 * @param topology Pointer to topology structure to broadcast
 * @return true if broadcast sent successfully
 * 
 * Example (in controller):
 *   z1_bus_broadcast_topology(&cluster_topology);
 */
bool z1_bus_broadcast_topology(const z1_topology_t *topology);

/**
 * Get local topology copy (All nodes)
 * 
 * Returns pointer to node's local topology table (updated via broadcasts).
 * 
 * @return Pointer to local topology structure
 * 
 * Example (in node):
 *   const z1_topology_t *topo = z1_bus_get_topology();
 *   printf("Cluster has %d online nodes\n", topo->online_count);
 */
const z1_topology_t* z1_bus_get_topology(void);

#endif // Z1_BUS_H

