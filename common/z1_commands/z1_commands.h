/**
 * Z1 Neuromorphic Cluster - Command Protocol Definitions
 * 
 * CTRL frame opcodes and payload structures for node management,
 * memory operations, and SNN control over the production bus.
 * 
 * Protocol: CTRL frames use Type=CTRL (0b11), with opcode in payload[0]
 * Streams are used to separate command categories for priority routing.
 * 
 * Copyright NeuroFab Corp. All rights reserved.
 */

#ifndef Z1_COMMANDS_H
#define Z1_COMMANDS_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// Stream Assignments
// ============================================================================

#define STREAM_NODE_MGMT        0  // Node management commands
#define STREAM_MEMORY           1  // Memory operations
#define STREAM_SNN_CONFIG       2  // SNN configuration
#define STREAM_SNN_CONTROL      3  // SNN execution control
#define STREAM_SPIKE            4  // Spike frames (no opcode, direct payload)

// ============================================================================
// Node Management Opcodes (Stream 0)
// ============================================================================

#define OPCODE_PING             0x01  // Test node connectivity
#define OPCODE_RESET            0x02  // Reset node
#define OPCODE_READ_STATUS      0x03  // Get node status (uptime, memory, LED, SNN state)
#define OPCODE_SET_LED          0x04  // Set RGB LED color
#define OPCODE_DISCOVER         0x05  // Respond if active (for topology discovery)

// Responses
#define OPCODE_PONG             0x81  // Ping response
#define OPCODE_STATUS_RESPONSE  0x83  // Status data response
#define OPCODE_DISCOVER_ACK     0x85  // Discovery acknowledgment

// ============================================================================
// Memory Operations Opcodes (Stream 1)
// ============================================================================

#define OPCODE_READ_MEMORY      0x10  // Read PSRAM block
#define OPCODE_WRITE_MEMORY     0x11  // Write PSRAM block
#define OPCODE_EXECUTE_CODE     0x12  // Execute code at address (future)

// Responses
#define OPCODE_MEMORY_DATA      0x90  // Memory read response
#define OPCODE_WRITE_ACK        0x91  // Memory write acknowledgment

// ============================================================================
// SNN Configuration Opcodes (Stream 2)
// ============================================================================

#define OPCODE_DEPLOY_TOPOLOGY  0x20  // Deploy neuron table to PSRAM
#define OPCODE_READ_TOPOLOGY    0x21  // Get topology summary
#define OPCODE_UPDATE_WEIGHTS   0x22  // Update synaptic weights
#define OPCODE_UPDATE_PARAMS    0x23  // Update neuron parameters (threshold, leak, etc.)
#define OPCODE_CLEAR_NEURONS    0x24  // Clear neuron table

// Responses
#define OPCODE_TOPOLOGY_INFO    0xA0  // Topology summary response
#define OPCODE_DEPLOY_ACK       0xA1  // Deployment acknowledgment
#define OPCODE_UPDATE_ACK       0xA2  // Weight/param update acknowledgment

// ============================================================================
// SNN Execution Control Opcodes (Stream 3)
// ============================================================================

#define OPCODE_START_SNN        0x30  // Start SNN execution
#define OPCODE_STOP_SNN         0x31  // Stop SNN execution
#define OPCODE_GET_SNN_STATUS   0x32  // Get execution status and statistics
#define OPCODE_READ_SPIKE_LOG   0x33  // Read spike event log
#define OPCODE_RESET_STATS      0x34  // Reset statistics counters

// Responses
#define OPCODE_SNN_STATUS       0xB0  // SNN status response
#define OPCODE_SPIKE_LOG_DATA   0xB1  // Spike log data

// ============================================================================
// Command Payload Structures
// ============================================================================

/**
 * Node Status Response (OPCODE_STATUS_RESPONSE, 64 bytes)
 * 
 * Sent by node in response to OPCODE_READ_STATUS.
 * Contains runtime information about node state.
 */
typedef struct __attribute__((packed)) {
    uint32_t uptime_ms;           // Milliseconds since boot
    uint32_t memory_free;         // Free PSRAM bytes
    uint8_t  led_r;               // LED red value (0-255)
    uint8_t  led_g;               // LED green value (0-255)
    uint8_t  led_b;               // LED blue value (0-255)
    uint8_t  snn_state;           // 0=stopped, 1=running, 2=error
    uint16_t neuron_count;        // Number of neurons loaded
    uint16_t reserved1;
    uint32_t total_spikes;        // Total spikes processed since boot
    uint16_t spikes_per_sec;      // Current spike rate (rolling average)
    uint16_t reserved2;
    uint32_t bus_frames_rx;       // Bus frames received
    uint32_t bus_frames_tx;       // Bus frames transmitted
    uint32_t bus_errors;          // Bus errors (CRC, timeout, etc.)
    uint8_t  reserved[28];        // Reserved for future use
} node_status_t;

/**
 * LED Command (OPCODE_SET_LED, 4 bytes)
 */
typedef struct __attribute__((packed)) {
    uint8_t opcode;  // OPCODE_SET_LED
    uint8_t r;       // Red (0-255)
    uint8_t g;       // Green (0-255)
    uint8_t b;       // Blue (0-255)
} cmd_set_led_t;

/**
 * Memory Read Command (OPCODE_READ_MEMORY, 12 bytes)
 */
typedef struct __attribute__((packed)) {
    uint8_t  opcode;       // OPCODE_READ_MEMORY
    uint8_t  reserved;
    uint16_t length;       // Number of bytes to read (max 512)
    uint32_t address;      // PSRAM address
    uint32_t reserved2;
} cmd_read_memory_t;

/**
 * Memory Write Command (OPCODE_WRITE_MEMORY, 12+ bytes)
 * 
 * Followed by data payload (up to 512 bytes).
 * For large writes (>512 bytes), use multiple commands.
 */
typedef struct __attribute__((packed)) {
    uint8_t  opcode;       // OPCODE_WRITE_MEMORY
    uint8_t  reserved;
    uint16_t length;       // Number of bytes to write
    uint32_t address;      // PSRAM address
    uint32_t reserved2;
    // Followed by data[length]
} cmd_write_memory_t;

/**
 * Topology Summary Response (OPCODE_TOPOLOGY_INFO, 32 bytes)
 */
typedef struct __attribute__((packed)) {
    uint16_t neuron_count;        // Number of neurons on this node
    uint16_t total_synapses;      // Total synapse count across all neurons
    uint32_t psram_table_addr;    // PSRAM address of neuron table
    uint32_t psram_table_size;    // Size of neuron table in bytes
    uint32_t neurons_active;      // Number of active (non-input) neurons
    uint32_t neurons_input;       // Number of input neurons
    uint32_t neurons_output;      // Number of output neurons
    uint8_t  reserved[8];
} topology_info_t;

/**
 * SNN Status Response (OPCODE_SNN_STATUS, 64 bytes)
 */
typedef struct __attribute__((packed)) {
    uint8_t  state;               // 0=stopped, 1=running, 2=error
    uint8_t  reserved1;
    uint16_t neuron_count;        // Number of neurons
    uint32_t current_time_us;     // Current simulation time
    uint32_t timestep_us;         // Simulation timestep
    uint32_t spikes_received;     // Spikes from other nodes
    uint32_t spikes_injected;     // Spikes injected locally
    uint32_t spikes_processed;    // Spikes processed
    uint32_t spikes_generated;    // Spikes generated by local neurons
    uint32_t spikes_dropped;      // Spikes dropped (queue full)
    uint16_t spike_queue_size;    // Current spike queue size
    uint16_t spike_queue_max;     // Max queue size seen
    uint32_t membrane_updates;    // Number of membrane potential updates
    uint8_t  reserved[24];
} snn_status_t;

/**
 * Spike Frame Payload (Stream 4, 12 bytes, no opcode)
 * 
 * Sent as UNICAST or BROADCAST frame with NO_ACK flag.
 * Type field in header indicates UNICAST or BROADCAST.
 */
typedef struct __attribute__((packed)) {
    uint32_t neuron_id;      // Global neuron ID (node_id << 16 | local_id)
    uint32_t timestamp_us;   // Spike timestamp (microseconds)
    float    value;          // Spike value (usually 1.0, or rate-coded 0.0-1.0)
} spike_frame_t;

// Helper macros for spike frames (12 bytes = 6 words)
#define SPIKE_FRAME_WORDS       6
#define SPIKE_FRAME_BYTES       12

// ============================================================================
// Global Neuron ID Encoding/Decoding
// ============================================================================

/**
 * Encode global neuron ID from node ID and local neuron ID
 * 
 * Encoding: [23:16] = Node ID, [15:0] = Local Neuron ID
 */
static inline uint32_t encode_global_neuron_id(uint8_t node_id, uint16_t local_id) {
    return ((uint32_t)node_id << 16) | local_id;
}

/**
 * Decode global neuron ID into node ID and local neuron ID
 */
static inline void decode_global_neuron_id(uint32_t global_id, uint8_t *node_id, uint16_t *local_id) {
    *node_id = (global_id >> 16) & 0xFF;
    *local_id = global_id & 0xFFFF;
}

// ============================================================================
// Weight Encoding/Decoding (8-bit fixed point)
// ============================================================================

/**
 * Encode float weight (-2.0 to +2.0) to 8-bit fixed point
 * 
 * Range: -2.0 to +2.0
 * Resolution: ~0.031 per step
 * Encoding: 0-127 = positive (0.0 to 2.0), 128-255 = negative (-0.01 to -2.0)
 */
static inline uint8_t encode_weight(float weight) {
    if (weight >= 0.0f) {
        // Positive: 0-127 → 0.0 to 2.0
        int val = (int)(weight * 63.5f + 0.5f);
        if (val > 127) val = 127;
        return (uint8_t)val;
    } else {
        // Negative: 128-255 → -0.01 to -2.0
        int val = (int)((-weight) * 63.5f + 0.5f);
        if (val > 127) val = 127;
        return (uint8_t)(128 + val);
    }
}

/**
 * Decode 8-bit weight to float
 */
static inline float decode_weight(uint8_t weight_int) {
    if (weight_int >= 128) {
        // Negative: 128-255 → -0.01 to -2.0
        return -(weight_int - 128) / 63.5f;
    } else {
        // Positive: 0-127 → 0.0 to 2.0
        return weight_int / 63.5f;
    }
}

// ============================================================================
// Command Validation
// ============================================================================

/**
 * Validate opcode is in valid range for stream
 */
static inline bool is_valid_opcode(uint8_t opcode, uint8_t stream) {
    switch (stream) {
        case STREAM_NODE_MGMT:   return (opcode >= 0x01 && opcode <= 0x05) || 
                                        (opcode >= 0x81 && opcode <= 0x85);
        case STREAM_MEMORY:      return (opcode >= 0x10 && opcode <= 0x12) || 
                                        (opcode >= 0x90 && opcode <= 0x91);
        case STREAM_SNN_CONFIG:  return (opcode >= 0x20 && opcode <= 0x24) || 
                                        (opcode >= 0xA0 && opcode <= 0xA2);
        case STREAM_SNN_CONTROL: return (opcode >= 0x30 && opcode <= 0x34) || 
                                        (opcode >= 0xB0 && opcode <= 0xB1);
        case STREAM_SPIKE:       return true;  // No opcode validation for spikes
        default:                 return false;
    }
}

#endif // Z1_COMMANDS_H
