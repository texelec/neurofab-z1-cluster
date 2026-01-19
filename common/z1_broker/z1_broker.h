/**
 * Z1 Onyx Bus - Broker Layer
 * Code by NeuroFab Corp: 2025-2026
 * 
 * Provides collision-aware multi-master arbitration for distributed SNN.
 * Implements priority CSMA with carrier sense and adaptive retry.
 * 
 * FRAME TYPE ARCHITECTURE:
 * - Type 0: Spike traffic (NOACK, high-volume, broker-queued)
 * - Type 1: Commands (ACK, low-volume, direct z1_bus_send_frame)
 * - Type 2/3: REMOVED (legacy test code, use Type 1 instead)
 * 
 * USAGE:
 * - Spikes: z1_broker_send() → enqueues Type 0 frame
 * - Commands: z1_cmd_send() → sends Type 1 frame directly (z1_commands layer)
 * - Receive: z1_broker_try_receive() → returns ANY type, caller filters
 * 
 * BACKPRESSURE & FLOW CONTROL:
 * - z1_broker_send() returns FALSE if queue is full (64 slots max)
 * - Application MUST check return value and retry later
 * - Burst limit: 10 consecutive frames, then automatic backoff
 * - Queue full = natural rate limiting, DO NOT add artificial delays
 * - Core 0 calls z1_broker_task() to drain queue at hardware speed
 * 
 * EXAMPLE (Dual-Core):
 *   // Core 1: Generate spikes (fast, non-blocking)
 *   while (has_spikes) {
 *       if (!z1_broker_send(spike, 4, dest, NOACK)) {
 *           break;  // Queue full, retry next iteration
 *       }
 *   }
 *   
 *   // Core 0: Drain broker queue (runs continuously)
 *   while (true) {
 *       z1_broker_task();  // Sends queued frames to bus
 *       sleep_us(1);       // Minimal delay for DMA
 *   }
 * 
 * Key Features:
 * - Lightweight queue (64 slots, inline payloads for <32 byte spikes)
 * - Carrier sense using SELECT0 busy flag
 * - Priority backoff (controller wins, low ID = high priority)
 * - Automatic retry (up to 3 attempts)
 * - Stale spike detection (>50ms age = drop)
 * - Statistics tracking (collisions, retries, latency)
 * 
 * Design Goals:
 * - <1ms latency (target: 200μs average)
 * - <5% collision rate under normal load
 * - 99.99% delivery success after retries
 * - <3KB SRAM overhead (queue + stats)
 */

#ifndef Z1_BROKER_H
#define Z1_BROKER_H

#include <stdint.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "../z1_onyx_bus/z1_bus.h"  // For z1_frame_t definition

// ============================================================================
// Configuration Constants
// ============================================================================

// BOOTLOADER MODE: Minimal queues (OTA only, no SNN)
#ifdef BOOTLOADER_BUILD
    #define Z1_BROKER_SPIKE_QUEUE_DEPTH 0       // No spike traffic in bootloader
    #define Z1_BROKER_CMD_QUEUE_DEPTH   8       // Small command queue for OTA chunks (8 × 1216 = 9,728 bytes)
    #define Z1_BROKER_MAX_PAYLOAD_WORDS 256     // OTA chunks are 512 bytes max
// APPLICATION MODE: Full-size queues for SNN operation
#else
    #define Z1_BROKER_SPIKE_QUEUE_DEPTH 64      // Spike queue (64 × 1216 bytes = 77,824 bytes)
    #define Z1_BROKER_CMD_QUEUE_DEPTH   16      // Command queue (16 × 1216 bytes = 19,456 bytes)
    #define Z1_BROKER_MAX_PAYLOAD_WORDS 600     // Max 1200 bytes per command (memory vs latency tradeoff)
#endif

// CRITICAL LIMIT: Max single-frame payload size
// NOTE: Spikes are small (<16 words), this limit is for CTRL frame commands only
// WARNING: Python tools MUST fragment payloads >1KB with delays for broker processing

// Legacy compatibility
#define Z1_BROKER_QUEUE_DEPTH       Z1_BROKER_SPIKE_QUEUE_DEPTH
#define Z1_BROKER_MAX_RETRIES       3       // Retry attempts before drop
#define Z1_BROKER_STALE_TIMEOUT_US  5000000   // 5 second max age (bulk spike injection tolerance)

// Backoff algorithm tuning (calculated for 17-node @ 5MHz bus)
// Frame time: 7 beats × 200ns = 1.4μs (actual spike size, not 259 max)
// Node spacing: 30μs ensures no overlap even with fast polling
#define Z1_BROKER_SLOT_TIME_US      30      // 30μs per slot
#define Z1_BROKER_PRIORITY_WEIGHT   1       // Node N gets N × 30μs base delay
#define Z1_BROKER_RANDOM_SLOTS      0       // No random jitter (priority only)

// Burst control (fairness for multi-node operation)
#define Z1_BROKER_MAX_BURST         10      // Max consecutive frames before backoff
#define Z1_BROKER_BACKOFF_US        500     // 500μs backoff after burst (allow other nodes)

// Carrier sense timeout
#define Z1_BROKER_CARRIER_SENSE_TIMEOUT_US  500  // 500μs max wait (fast reaction)

// Fast ACK timeout for collision detection
#define Z1_BROKER_FAST_ACK_TIMEOUT_US       100   // 100μs (vs 10ms normal ACK)

// ============================================================================
// Request Flags
// ============================================================================

#define Z1_BROKER_NOACK     0x01    // Fire-and-forget (no ACK required)
#define Z1_BROKER_ACK       0x00    // Wait for ACK (reliable delivery)
#define Z1_BROKER_PRIORITY  0x02    // High priority (reserved for future use)

// ============================================================================
// Data Structures
// ============================================================================

/**
 * Broker request structure
 * Stores spike data inline (no pointer indirection for cache efficiency)
 */
typedef struct {
    uint16_t payload[Z1_BROKER_MAX_PAYLOAD_WORDS];  // Inline data (up to 1024 bytes)
    uint16_t num_words;                             // Payload size in words (1-512)
    uint8_t dest;                                   // Destination node (0-15, 31=broadcast)
    uint8_t flags;                                  // Z1_BROKER_ACK | Z1_BROKER_NOACK (legacy)
    uint8_t stream;                                 // Stream ID (0-7)
    uint8_t is_broadcast;                           // 1=broadcast spike, 0=unicast
    uint8_t retry_count;                            // Current retry attempt (0-3)
    uint8_t _padding[1];                            // Align to 4 bytes
    uint64_t queued_time_us;                        // Timestamp when queued
} z1_broker_request_t;

/**
 * Broker queue structure
 * Dual queues: spike queue (high priority) + command queue (low priority)
 * NOTE: Bootloader mode has no spike queue (SPIKE_QUEUE_DEPTH=0)
 */
typedef struct {
#if Z1_BROKER_SPIKE_QUEUE_DEPTH > 0
    // Spike queue (Type 0, NOACK, high volume) - only in application mode
    z1_broker_request_t spike_queue[Z1_BROKER_SPIKE_QUEUE_DEPTH];
    uint8_t spike_head;
    uint8_t spike_tail;
    uint8_t spike_count;
    uint8_t spike_peak;
#endif
    
    // Command queue (Type 1, ACK, low volume)
    z1_broker_request_t cmd_queue[Z1_BROKER_CMD_QUEUE_DEPTH];
    uint8_t cmd_head;
    uint8_t cmd_tail;
    uint8_t cmd_count;
    uint8_t cmd_peak;
} z1_broker_queue_t;

/**
 * Broker statistics
 * Tracks performance and collision metrics
 */
typedef struct {
    uint32_t total_sent;            // Successfully delivered
    uint32_t total_dropped;         // Dropped (queue full or stale)
    uint32_t total_collisions;      // Detected collisions (no ACK)
    uint32_t retry_histogram[4];    // Count of retries (0=first try, 1-3=retries)
    uint32_t current_queue_depth;   // Current pending requests
    uint32_t peak_queue_depth;      // Max queue depth observed
    
    // Latency tracking (microseconds)
    uint32_t min_latency_us;        // Best case latency
    uint32_t max_latency_us;        // Worst case latency
    uint32_t avg_latency_us;        // Running average
    
    // Bus utilization
    uint32_t carrier_sense_busy_count;   // Times bus was busy
    uint32_t carrier_sense_idle_count;   // Times bus was idle
} z1_broker_stats_t;

// ============================================================================
// Public API
// ============================================================================

/**
 * Initialize broker layer
 * Must be called after z1_bus_init_*() and z1_bus_set_node_id()
 */
void z1_broker_init(void);

/**
 * Send spike (UNICAST or BROADCAST, fire-and-forget, high priority queue)
 * 
 * @param data Pointer to spike data (max 16 words / 32 bytes)
 * @param num_words Payload size in words (1-16)
 * @param dest Destination node (0-15 for unicast, 31 for broadcast)
 * @param stream Stream ID (0-7)
 * @return true if queued, false if queue full
 * 
 * Note: dest=31 triggers BROADCAST frame, otherwise UNICAST with NO_ACK
 */
bool z1_broker_send_spike(const uint16_t *data, uint8_t num_words, uint8_t dest, uint8_t stream);

/**
 * Send command (CTRL frame, application-level protocol, low priority queue)
 * 
 * Commands are sent as CTRL frames (type=3) and are NOT auto-ACKed by bus layer.
 * Application must handle responses explicitly (send response CTRL frame).
 * 
 * Payload format: [opcode, param1, param2, ...]
 * 
 * @param data Pointer to command data (max 16 words / 32 bytes)
 * @param num_words Payload size in words (1-16)
 * @param dest Destination node (0-15)
 * @param stream Stream ID (0-7)
 * @return true if queued, false if queue full
 */
bool z1_broker_send_command(const uint16_t *data, uint16_t num_words, uint8_t dest, uint8_t stream);

/**
 * Send data (legacy API, auto-routes to spike or command queue)
 * 
 * @param data Pointer to data (max 16 words / 32 bytes)
 * @param num_words Payload size in words (1-16)
 * @param dest Destination node (0-15, 31=broadcast)
 * @param stream Stream ID (0-7)
 * @param flags Z1_BROKER_ACK (→cmd queue) or Z1_BROKER_NOACK (→spike queue)
 * @return true if queued, false if queue full
 * 
 * NOTE: Data is COPIED into queue (no pointer tracking)
 * NOTE: Returns immediately - broker drains queue asynchronously
 */
bool z1_broker_send(const uint16_t *data, uint8_t num_words, 
                    uint8_t dest, uint8_t stream, uint8_t flags);

/**
 * Receive spike data (non-blocking)
 * 
 * @param rx_buffer Buffer to copy data into (max 256 words)
 * @param src [out] Source node ID
 * @param stream [out] Stream ID
 * @return Words received (0 = no data available)
 * 
 * NOTE: Wrapper around z1_bus_try_receive_frame() for consistency
 */
uint16_t z1_broker_receive(uint16_t *rx_buffer, uint8_t *src, uint8_t *stream);

/**
 * Broker task (must be called repeatedly in main loop)
 * Drains TX queue with collision avoidance
 * 
 * Recommended: Call every 10-100μs for low latency
 */
void z1_broker_task(void);

/**
 * Get broker statistics
 * 
 * @param stats [out] Statistics structure to fill
 */
void z1_broker_get_stats(z1_broker_stats_t *stats);

/**
 * Reset broker statistics
 */
void z1_broker_reset_stats(void);

/**
 * Get queue depth (for monitoring)
 * 
 * @return Number of pending requests (0-64)
 */
uint8_t z1_broker_queue_depth(void);

/**
 * Get spike queue depth (for priority testing)
 * 
 * @return Number of pending spikes in high-priority queue
 */
uint32_t z1_broker_get_spike_queue_depth(void);

/**
 * Get command queue depth (for priority testing)
 * 
 * @return Number of pending commands in low-priority queue
 */
uint32_t z1_broker_get_cmd_queue_depth(void);

/**
 * Flush spike queue (drop all pending spikes)
 * Use when stopping SNN to prevent stale spikes from blocking commands
 */
void z1_broker_flush_spike_queue(void);

/**
 * Request statistics from a remote node (sends query frame)
 * Controller can use this to poll nodes for packet loss analysis
 * 


/**
 * Receive frame (non-blocking, returns full frame structure)
 * 
 * @param frame [out] Frame structure to fill
 * @return true if frame received, false if no data
 */
bool z1_broker_try_receive(z1_frame_t *frame);

#endif // Z1_BROKER_H
