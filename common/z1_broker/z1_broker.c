/**
 * Z1 Onyx Bus - Broker Layer Implementation
 * 
 * Collision-aware multi-master arbitration for SNN spike traffic
 */

#include "z1_broker.h"
#include "../z1_onyx_bus/z1_bus.h"
#include "../z1_onyx_bus/z1_onyx_bus_pins.h"
#include "hardware/gpio.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

// ============================================================================
// Module State
// ============================================================================

static z1_broker_queue_t broker_queue = {0};
static z1_broker_stats_t broker_stats = {0};
static uint8_t local_node_id = 0xFF;  // Set during init

// Burst control state
static uint16_t burst_frame_count = 0;      // Frames sent in current burst
static absolute_time_t backoff_until = {0}; // Backoff timer

// Backoff lookup table (17 nodes × 30μs priority)
// Pre-calculated: node_id × PRIORITY_WEIGHT × SLOT_TIME_US
static const uint16_t backoff_lut[17] = {
    0,    // Node 0:  0μs
    30,   // Node 1:  30μs
    60,   // Node 2:  60μs
    90,   // Node 3:  90μs
    120,  // Node 4:  120μs
    150,  // Node 5:  150μs
    180,  // Node 6:  180μs
    210,  // Node 7:  210μs
    240,  // Node 8:  240μs
    270,  // Node 9:  270μs
    300,  // Node 10: 300μs
    330,  // Node 11: 330μs
    360,  // Node 12: 360μs
    390,  // Node 13: 390μs
    420,  // Node 14: 420μs
    450,  // Node 15: 450μs
    480   // Controller (16): 480μs
};

// ============================================================================
// Private Helpers
// ============================================================================

// Forward declaration of helper
static bool z1_broker_try_send(z1_broker_request_t *req, bool is_spike);

// ============================================================================
// Queue Management (Dual-Queue Architecture)
// ============================================================================

// Spike queue helpers
static inline bool spike_queue_is_full(void) {
    return broker_queue.spike_count >= Z1_BROKER_SPIKE_QUEUE_DEPTH;
}

static inline bool spike_queue_is_empty(void) {
    return broker_queue.spike_count == 0;
}

static bool spike_queue_enqueue(z1_broker_request_t *req) {
    if (spike_queue_is_full()) return false;
    
    memcpy(&broker_queue.spike_queue[broker_queue.spike_tail], req, sizeof(z1_broker_request_t));
    broker_queue.spike_tail = (broker_queue.spike_tail + 1) % Z1_BROKER_SPIKE_QUEUE_DEPTH;
    broker_queue.spike_count++;
    
    if (broker_queue.spike_count > broker_queue.spike_peak) {
        broker_queue.spike_peak = broker_queue.spike_count;
    }
    return true;
}

static z1_broker_request_t* spike_queue_peek(void) {
    if (spike_queue_is_empty()) return NULL;
    return &broker_queue.spike_queue[broker_queue.spike_head];
}

static void spike_queue_dequeue(void) {
    if (spike_queue_is_empty()) return;
    broker_queue.spike_head = (broker_queue.spike_head + 1) % Z1_BROKER_SPIKE_QUEUE_DEPTH;
    broker_queue.spike_count--;
}

// Command queue helpers
static inline bool cmd_queue_is_full(void) {
    return broker_queue.cmd_count >= Z1_BROKER_CMD_QUEUE_DEPTH;
}

static inline bool cmd_queue_is_empty(void) {
    return broker_queue.cmd_count == 0;
}

static bool cmd_queue_enqueue(z1_broker_request_t *req) {
    if (cmd_queue_is_full()) return false;
    
    memcpy(&broker_queue.cmd_queue[broker_queue.cmd_tail], req, sizeof(z1_broker_request_t));
    broker_queue.cmd_tail = (broker_queue.cmd_tail + 1) % Z1_BROKER_CMD_QUEUE_DEPTH;
    broker_queue.cmd_count++;
    
    if (broker_queue.cmd_count > broker_queue.cmd_peak) {
        broker_queue.cmd_peak = broker_queue.cmd_count;
    }
    return true;
}

static z1_broker_request_t* cmd_queue_peek(void) {
    if (cmd_queue_is_empty()) return NULL;
    return &broker_queue.cmd_queue[broker_queue.cmd_head];
}

static void cmd_queue_dequeue(void) {
    if (cmd_queue_is_empty()) return;
    broker_queue.cmd_head = (broker_queue.cmd_head + 1) % Z1_BROKER_CMD_QUEUE_DEPTH;
    broker_queue.cmd_count--;
}

// ============================================================================
// Carrier Sense & Backoff
// ============================================================================

static inline bool z1_broker_carrier_sense(void) {
    // Use bus layer's carrier sense implementation
    return z1_bus_carrier_sense();
}

static inline uint32_t z1_broker_calculate_backoff(uint8_t node_id) {
    // Use pre-calculated lookup table for priority component
    if (node_id > 16) node_id = 16;  // Safety clamp
    uint32_t priority_us = backoff_lut[node_id];
    
    // Random component disabled (RANDOM_SLOTS=0), return priority only
    // If random needed: (1 + (rand() % (RANDOM_SLOTS+1))) * SLOT_TIME
    return priority_us;
}

// ============================================================================
// Public API
// ============================================================================

void z1_broker_init(void) {
    // Initialize queue
    memset(&broker_queue, 0, sizeof(broker_queue));
    
    // Initialize stats
    memset(&broker_stats, 0, sizeof(broker_stats));
    broker_stats.min_latency_us = UINT32_MAX;
    
    // Get local node ID from bus layer
    local_node_id = z1_bus_get_node_id();
}

bool z1_broker_send_spike(const uint16_t *data, uint8_t num_words, uint8_t dest, uint8_t stream) {
    if (num_words == 0 || num_words > Z1_BROKER_MAX_PAYLOAD_WORDS) {
        return false;
    }
    
    if (spike_queue_is_full()) {
        broker_stats.total_dropped++;
        return false;
    }
    
    z1_broker_request_t req = {0};
    memcpy(req.payload, data, num_words * sizeof(uint16_t));
    req.num_words = num_words;
    req.dest = dest;
    req.flags = Z1_BROKER_NOACK;  // Spikes are always fire-and-forget
    req.is_broadcast = (dest == 31) ? 1 : 0;  // Broadcast if dest=31
    req.stream = stream & 0x7;  // Clamp to 0-7
    req.retry_count = 0;
    req.queued_time_us = time_us_64();
    
    return spike_queue_enqueue(&req);
}

bool z1_broker_send_command(const uint16_t *data, uint16_t num_words, uint8_t dest, uint8_t stream) {
    printf("[BROKER] z1_broker_send_command() called: dest=%d, words=%d, stream=%d\n", dest, num_words, stream);
    
    if (num_words == 0 || num_words > Z1_BROKER_MAX_PAYLOAD_WORDS) {
        printf("[BROKER] Invalid num_words=%d (max=%d)\n", num_words, Z1_BROKER_MAX_PAYLOAD_WORDS);
        return false;
    }
    
    if (cmd_queue_is_full()) {
        printf("[BROKER] cmd_queue is FULL!\n");
        broker_stats.total_dropped++;
        return false;
    }
    
    z1_broker_request_t req = {0};
    memcpy(req.payload, data, num_words * sizeof(uint16_t));
    req.num_words = num_words;
    req.dest = dest;
    req.flags = Z1_BROKER_ACK;  // Commands expect application response
    req.is_broadcast = 0;  // Commands are always unicast CTRL frames
    req.stream = stream & 0x7;  // Clamp to 0-7
    req.retry_count = 0;
    req.queued_time_us = time_us_64();
    
    bool result = cmd_queue_enqueue(&req);
    printf("[BROKER] cmd_queue_enqueue() returned %d\n", result);
    return result;
}

bool z1_broker_send(const uint16_t *data, uint8_t num_words, 
                    uint8_t dest, uint8_t stream, uint8_t flags) {
    // Route to appropriate queue based on flags
    if (flags & Z1_BROKER_NOACK) {
        // NOACK → spike queue (Type 0)
        return z1_broker_send_spike(data, num_words, dest, stream);
    } else {
        // ACK → command queue (Type 1)
        return z1_broker_send_command(data, num_words, dest, stream);
    }
}

uint16_t z1_broker_receive(uint16_t *rx_buffer, uint8_t *src, uint8_t *stream) {
    // Wrapper around bus layer RX
    z1_frame_t frame;
    if (z1_bus_try_receive_frame(&frame)) {
        // Copy payload
        uint16_t words = frame.length / 2;  // Convert bytes to words
        if (words > 256) words = 256;  // Safety clamp
        memcpy(rx_buffer, frame.payload, words * sizeof(uint16_t));
        
        // Return metadata
        if (src) *src = frame.src;
        if (stream) *stream = frame.stream;
        
        return words;
    }
    
    return 0;  // No data
}

void z1_broker_task(void) {
    // Priority scheduler: Always service spike queue before command queue
    
    static uint32_t debug_count = 0;
    static bool first_command_logged = false;
    
    // Check if still in backoff period (applies to BOTH queues)
    if (!time_reached(backoff_until)) {
        return;  // Still backing off from previous burst
    }
    
    // Check spike queue first (high priority)
    if (!spike_queue_is_empty()) {
        // Peek at next spike
        z1_broker_request_t *req = spike_queue_peek();
        if (req == NULL) return;
        
        // Check if spike is stale
        uint64_t now_us = time_us_64();
        uint64_t age_us = now_us - req->queued_time_us;
        if (age_us > Z1_BROKER_STALE_TIMEOUT_US) {
            spike_queue_dequeue();
            broker_stats.total_dropped++;
            return;
        }
        
        // Try to send spike (returns immediately if bus busy)
        if (z1_broker_try_send(req, true)) {  // true = spike queue
            spike_queue_dequeue();
        }
        return;  // Exit - one frame per task() call
    }
    
    // Check command queue second (low priority)
    if (!cmd_queue_is_empty()) {
        printf("[BROKER DEBUG] Command queue not empty! Attempting send...\n");
        
        // Peek at next command
        z1_broker_request_t *req = cmd_queue_peek();
        if (req == NULL) {
            printf("[BROKER DEBUG] cmd_queue_peek() returned NULL!\n");
            return;
        }
        
        // Commands don't go stale (reliable delivery required)
        
        // Try to send command
        printf("[BROKER DEBUG] Calling z1_broker_try_send() for command...\n");
        if (z1_broker_try_send(req, false)) {  // false = command queue
            printf("[BROKER DEBUG] z1_broker_try_send() SUCCESS! Dequeuing command.\n");
            cmd_queue_dequeue();
        } else {
            printf("[BROKER DEBUG] z1_broker_try_send() FAILED (bus busy or lost arbitration)\n");
        }
        return;  // Exit - one frame per task() call
    }
    
    // Both queues empty - reset burst counter
    burst_frame_count = 0;
}

// Helper function to attempt sending a request
static bool z1_broker_try_send(z1_broker_request_t *req, bool is_spike) {
    printf("[BROKER DEBUG] z1_broker_try_send() called (is_spike=%d)\n", is_spike);
    
    // Wait for bus idle (with timeout)
    uint32_t wait_start_us = time_us_32();
    uint32_t loop_count = 0;
    while (z1_broker_carrier_sense()) {
        loop_count++;
        uint32_t elapsed = time_us_32() - wait_start_us;
        if (elapsed >= Z1_BROKER_CARRIER_SENSE_TIMEOUT_US) {
            // Bus stuck busy, exit to allow RX processing
            printf("[BROKER DEBUG] Carrier sense timeout after %lu loops!\n", loop_count);
            broker_stats.carrier_sense_busy_count++;
            return false;  // Try again next iteration
        }
        tight_loop_contents();
    }
    printf("[BROKER DEBUG] Bus idle after %lu loops, proceeding to transmit...\n", loop_count);
    broker_stats.carrier_sense_idle_count++;
    
    // Calculate backoff with priority-based arbitration
    uint32_t backoff_us = z1_broker_calculate_backoff(local_node_id);
    sleep_us(backoff_us);
    
    // Double-check bus still idle (higher priority node may have claimed bus)
    if (z1_broker_carrier_sense()) {
        return false;  // Lost arbitration
    }
    
    // Transmit frame using proper bus layer type
    bool tx_success = false;
    
    if (is_spike) {
        // Spike: UNICAST or BROADCAST, always with NO_ACK
        if (req->is_broadcast) {
            // Broadcast spike to all nodes
            printf("[BROKER DEBUG] About to send BROADCAST spike: dest=31, stream=%d, words=%lu\n", 
                   req->stream, req->num_words);
            tx_success = z1_bus_send_frame(Z1_FRAME_TYPE_BROADCAST, 31, 
                                           req->stream | Z1_STREAM_NO_ACK,
                                           req->payload, req->num_words);
        } else {
            // Unicast spike to specific node
            printf("[BROKER DEBUG] About to send UNICAST spike: dest=%d, stream=%d, words=%lu\n", 
                   req->dest, req->stream, req->num_words);
            tx_success = z1_bus_send_frame(Z1_FRAME_TYPE_UNICAST, req->dest,
                                           req->stream | Z1_STREAM_NO_ACK,
                                           req->payload, req->num_words);
        }
        printf("[BROKER DEBUG] Spike send completed, success=%d\n", tx_success);
    } else {
        // Command: CTRL frame (application handles response)
        printf("[BROKER DEBUG] About to send CTRL: dest=%d, stream=%d, words=%lu\n", 
               req->dest, req->stream, req->num_words);
        tx_success = z1_bus_send_frame(Z1_FRAME_TYPE_CTRL, req->dest,
                                       req->stream,  // No NO_ACK flag
                                       req->payload, req->num_words);
        printf("[BROKER DEBUG] Bus send completed, success=%d\n", tx_success);
    }
    
    // Increment burst counter
    burst_frame_count++;
    
    // Check if burst limit reached - enforce backoff to allow other nodes
    if (burst_frame_count >= Z1_BROKER_MAX_BURST) {
        backoff_until = make_timeout_time_us(Z1_BROKER_BACKOFF_US);
        burst_frame_count = 0;
    }
    
    // Update statistics
    uint64_t now_us = time_us_64();
    if (tx_success) {
        broker_stats.total_sent++;
        broker_stats.retry_histogram[req->retry_count]++;
        
        // Track latency
        uint32_t latency_us = (uint32_t)(now_us - req->queued_time_us);
        if (latency_us < broker_stats.min_latency_us) {
            broker_stats.min_latency_us = latency_us;
        }
        if (latency_us > broker_stats.max_latency_us) {
            broker_stats.max_latency_us = latency_us;
        }
        return true;  // Success - dequeue
    } else {
        // TX failed - increment retry count IN THE QUEUE
        // (Safe because req is a pointer to queue entry, and we're the only writer)
        req->retry_count++;
        if (req->retry_count >= Z1_BROKER_MAX_RETRIES) {
            broker_stats.total_dropped++;
            return true;  // Max retries - drop and dequeue
        }
        broker_stats.total_collisions++;  // Track retry as collision
        return false;  // Keep in queue for retry
    }
}

void z1_broker_get_stats(z1_broker_stats_t *stats) {
    if (stats == NULL) return;
    
    // Copy stats
    memcpy(stats, &broker_stats, sizeof(z1_broker_stats_t));
    
    // Add current queue depths (dual-queue)
    stats->current_queue_depth = broker_queue.spike_count + broker_queue.cmd_count;
    stats->peak_queue_depth = broker_queue.spike_peak + broker_queue.cmd_peak;
}

void z1_broker_reset_stats(void) {
    memset(&broker_stats, 0, sizeof(broker_stats));
    broker_stats.min_latency_us = UINT32_MAX;
    broker_queue.spike_peak = broker_queue.spike_count;
    broker_queue.cmd_peak = broker_queue.cmd_count;
}

uint8_t z1_broker_queue_depth(void) {
    return broker_queue.spike_count + broker_queue.cmd_count;
}

uint32_t z1_broker_get_spike_queue_depth(void) {
    return broker_queue.spike_count;
}

uint32_t z1_broker_get_cmd_queue_depth(void) {
    return broker_queue.cmd_count;
}

// ============================================================================
// Legacy Type 2/3 Stats Protocol REMOVED
// Use z1_commands layer: z1_cmd_send(node, Z1_CMD_SNN_QUERY, ...)
// ============================================================================

// ============================================================================
// Command Protocol Helpers (LEGACY - for reference_github compatibility)
// ============================================================================

bool z1_broker_send_legacy_command(uint8_t dest, uint8_t cmd, 
                                    const uint16_t *payload, uint8_t num_words) {
    // Build command frame: [opcode] [payload...]
    uint16_t cmd_frame[Z1_BROKER_MAX_PAYLOAD_WORDS];
    cmd_frame[0] = cmd;  // First word is opcode
    
    // Copy payload if present
    uint16_t total_words = 1;  // Start with opcode
    if (payload && num_words > 0) {
        uint16_t copy_words = num_words;
        if (copy_words > (uint16_t)(Z1_BROKER_MAX_PAYLOAD_WORDS - 1)) {
            copy_words = (uint16_t)(Z1_BROKER_MAX_PAYLOAD_WORDS - 1);  // Leave room for opcode
        }
        memcpy(&cmd_frame[1], payload, copy_words * sizeof(uint16_t));
        total_words += copy_words;
    }
    
    // Send via command queue (Type 1, ACK)
    return z1_broker_send_command(cmd_frame, total_words, dest, 0);  // stream=0 for legacy
}

bool z1_broker_try_receive(z1_frame_t *frame) {
    return z1_bus_try_receive_frame(frame);
}
