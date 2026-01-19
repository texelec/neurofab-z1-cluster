/**
 * Z1 Onyx Bus - Broker Layer Implementation
 * Code by NeuroFab Corp: 2025-2026
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

// ============================================================================
// Spike Queue Functions (Application mode only)
// ============================================================================

#if Z1_BROKER_SPIKE_QUEUE_DEPTH > 0

// Spike queue helpers
static inline bool spike_queue_is_full(void) {
    return broker_queue.spike_count >= Z1_BROKER_SPIKE_QUEUE_DEPTH;
}

static inline bool spike_queue_is_empty(void) {
    return broker_queue.spike_count == 0;
}

static bool spike_queue_enqueue(z1_broker_request_t *req) {
    if (spike_queue_is_full()) return false;
    
    // OPTIMIZED: Only copy the used payload, not the entire 1200-byte buffer!
    z1_broker_request_t *dest = &broker_queue.spike_queue[broker_queue.spike_tail];
    memcpy(dest->payload, req->payload, req->num_words * sizeof(uint16_t));  // Copy ONLY used data
    dest->num_words = req->num_words;
    dest->dest = req->dest;
    dest->flags = req->flags;
    dest->stream = req->stream;
    dest->is_broadcast = req->is_broadcast;
    dest->retry_count = req->retry_count;
    dest->queued_time_us = req->queued_time_us;
    
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

#endif  // Z1_BROKER_SPIKE_QUEUE_DEPTH > 0

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
    printf("[BROKER] Starting broker_init...\n");
    
    // Initialize queue
    memset(&broker_queue, 0, sizeof(broker_queue));
    printf("[BROKER] Queue initialized\n");
    
    // Initialize stats
    memset(&broker_stats, 0, sizeof(broker_stats));
    broker_stats.min_latency_us = UINT32_MAX;
    printf("[BROKER] Stats initialized\n");
    
    // Get local node ID from bus layer
    local_node_id = z1_bus_get_node_id();
    printf("[BROKER] Got node ID: %d\n", local_node_id);
    printf("[BROKER] Broker init complete\n");
}

bool z1_broker_send_spike(const uint16_t *data, uint8_t num_words, uint8_t dest, uint8_t stream) {
#if Z1_BROKER_SPIKE_QUEUE_DEPTH > 0
    if (num_words == 0 || num_words > Z1_BROKER_MAX_PAYLOAD_WORDS) {
        printf("[BROKER] send_spike FAILED: invalid num_words=%d\n", num_words);
        return false;
    }
    
    if (spike_queue_is_full()) {
        broker_stats.total_dropped++;
        // Silently fail - backpressure handled by caller retry
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
    
    // printf("[BROKER] Spike queued: dest=%d, broadcast=%d, words=%d\n", dest, req.is_broadcast, num_words);
    
    return spike_queue_enqueue(&req);
#else
    // Bootloader mode: spike sending disabled
    return false;
#endif
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
        // CRITICAL: Cast num_words to uint16_t to match function signature
        return z1_broker_send_command(data, (uint16_t)num_words, dest, stream);
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
    // Priority scheduler: Always service COMMAND queue before SPIKE queue
    // Commands (GET_SNN_STATUS, etc.) need prompt response, spikes can tolerate delay
    
    static uint32_t debug_count = 0;
    static bool first_command_logged = false;
    
    // Check COMMAND queue first (HIGH PRIORITY - ignores backoff)
    if (!cmd_queue_is_empty()) {
        // Peek at next command
        z1_broker_request_t *req = cmd_queue_peek();
        if (req == NULL) {
            return;
        }
        
        if (!first_command_logged) {
#if Z1_BROKER_SPIKE_QUEUE_DEPTH > 0
            printf("[BROKER-TASK] CMD pending: dest=%d stream=%d (spike_queue=%d)\n", 
                   req->dest, req->stream, broker_queue.spike_count);
#else
            printf("[BROKER-TASK] CMD pending: dest=%d stream=%d (bootloader mode)\n", 
                   req->dest, req->stream);
#endif
            first_command_logged = true;
        }
        
        // Commands don't go stale (reliable delivery required)
        
        // Try to send command (commands ignore backoff period)
        if (z1_broker_try_send(req, false)) {  // false = command queue
            cmd_queue_dequeue();
            first_command_logged = false;  // Reset for next command
        }
        // CRITICAL: Return even if send failed to give command another chance next iteration
        // This prevents spike queue from being serviced while commands are pending
        return;
    }
    
#if Z1_BROKER_SPIKE_QUEUE_DEPTH > 0
    // Check if still in backoff period (applies to SPIKE queue only)
    if (!time_reached(backoff_until)) {
        return;  // Still backing off from previous burst
    }
    
    // Check SPIKE queue second (LOW PRIORITY)
    // Only process spikes when NO commands are waiting
    if (!spike_queue_is_empty()) {
        // Peek at next spike
        z1_broker_request_t *req = spike_queue_peek();
        if (req == NULL) return;
        
        // Check if spike is stale
        uint64_t now_us = time_us_64();
        uint64_t age_us = now_us - req->queued_time_us;
        if (age_us > Z1_BROKER_STALE_TIMEOUT_US) {
            printf("[BROKER] Dropping stale spike (age=%llu us)\n", age_us);
            spike_queue_dequeue();
            broker_stats.total_dropped++;
            return;
        }
        
        // Try to send spike (returns immediately if bus busy)
        bool tx_result = z1_broker_try_send(req, true);  // true = spike queue
        if (tx_result) {
            spike_queue_dequeue();
            req->retry_count = 0;  // Reset on success
        } else {
            // TX failed - increment retry count
            req->retry_count++;
            
            // After 3 quick retries, assume DMA hardware failure and flush queue
            if (req->retry_count > 3) {
                printf("[BROKER] CRITICAL: DMA hardware failure detected after 3 retries\n");
                printf("[BROKER] Flushing entire spike queue (%d spikes dropped)\n", broker_queue.spike_count);
                
                // Drop all pending spikes - they will all fail anyway
                while (!spike_queue_is_empty()) {
                    spike_queue_dequeue();
                    broker_stats.total_dropped++;
                }
                
                return;  // Exit immediately
            }
        }
        return;  // Exit - one frame per task() call
    }
#endif  // Z1_BROKER_SPIKE_QUEUE_DEPTH > 0
    
    // Both queues empty - reset burst counter
    burst_frame_count = 0;
}

// Helper function to attempt sending a request
static bool z1_broker_try_send(z1_broker_request_t *req, bool is_spike) {
    // Adaptive timeout: Commands get progressively longer waits on retries
    // This ensures commands eventually get through without blocking main loop on first attempt
    uint32_t bus_wait_timeout_us;
    if (is_spike) {
        bus_wait_timeout_us = 50;  // Spikes: Fast fail if bus busy
    } else {
        // Commands: Progressive backoff (50µs → 100µs → 200µs → 500µs → 1ms max)
        bus_wait_timeout_us = 50 + (req->retry_count * 50);
        if (bus_wait_timeout_us > 1000) bus_wait_timeout_us = 1000;
    }
    
    // Wait for bus idle
    uint32_t wait_start_us = time_us_32();
    while (z1_broker_carrier_sense()) {
        uint32_t elapsed = time_us_32() - wait_start_us;
        if (elapsed >= bus_wait_timeout_us) {
            // Bus busy, return false to try again next iteration
            broker_stats.carrier_sense_busy_count++;
            return false;
        }
        tight_loop_contents();
    }
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
        // printf("[BROKER-TX] Sending SPIKE frame\n");
        // Spike: UNICAST or BROADCAST, always with NO_ACK
        if (req->is_broadcast) {
            // Broadcast spike to all nodes
            printf("[BROKER-TX] -> BROADCAST spike to all nodes\n");
            tx_success = z1_bus_send_frame(Z1_FRAME_TYPE_BROADCAST, 31,
                                           req->stream | Z1_STREAM_NO_ACK,
                                           req->payload, req->num_words);
        } else {
            // Unicast spike to specific node
            // printf("[BROKER-TX] -> UNICAST spike to node %d\n", req->dest);
            tx_success = z1_bus_send_frame(Z1_FRAME_TYPE_UNICAST, req->dest,
                                           req->stream | Z1_STREAM_NO_ACK,
                                           req->payload, req->num_words);
        }
        // printf("[BROKER-TX] Spike TX result: %s\n", tx_success ? "SUCCESS" : "FAILED");
    } else {
        // Command: CTRL frame (application handles response)
        tx_success = z1_bus_send_frame(Z1_FRAME_TYPE_CTRL, req->dest,
                                       req->stream,  // No NO_ACK flag
                                       req->payload, req->num_words);
        printf("[BROKER-TX] CMD->%d stream=%d %s\n", req->dest, req->stream, tx_success ? "OK" : "FAIL");
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
            printf("[BROKER-TX] DROP %d\n", req->dest);
            broker_stats.total_dropped++;
            return true;  // Max retries - drop and dequeue
        }
        printf("[BROKER-TX] RETRY %d #%d\n", req->dest, req->retry_count);
        broker_stats.total_collisions++;  // Track retry as collision
        return false;  // Keep in queue for retry
    }
}

void z1_broker_flush_spike_queue(void) {
#if Z1_BROKER_SPIKE_QUEUE_DEPTH > 0
    uint32_t dropped = broker_queue.spike_count;
    if (dropped > 0) {
        printf("[BROKER] Flushing %lu pending spikes\n", dropped);
        broker_stats.total_dropped += dropped;
    }
    
    broker_queue.spike_head = 0;
    broker_queue.spike_tail = 0;
    broker_queue.spike_count = 0;
#endif
}

void z1_broker_get_stats(z1_broker_stats_t *stats) {
    if (stats == NULL) return;
    
    // Copy stats
    memcpy(stats, &broker_stats, sizeof(z1_broker_stats_t));
    
    // Add current queue depths (dual-queue)
#if Z1_BROKER_SPIKE_QUEUE_DEPTH > 0
    stats->current_queue_depth = broker_queue.spike_count + broker_queue.cmd_count;
    stats->peak_queue_depth = broker_queue.spike_peak + broker_queue.cmd_peak;
#else
    stats->current_queue_depth = broker_queue.cmd_count;
    stats->peak_queue_depth = broker_queue.cmd_peak;
#endif
}

void z1_broker_reset_stats(void) {
    memset(&broker_stats, 0, sizeof(broker_stats));
    broker_stats.min_latency_us = UINT32_MAX;
#if Z1_BROKER_SPIKE_QUEUE_DEPTH > 0
    broker_queue.spike_peak = broker_queue.spike_count;
#endif
    broker_queue.cmd_peak = broker_queue.cmd_count;
}

uint8_t z1_broker_queue_depth(void) {
#if Z1_BROKER_SPIKE_QUEUE_DEPTH > 0
    return broker_queue.spike_count + broker_queue.cmd_count;
#else
    return broker_queue.cmd_count;
#endif
}

uint32_t z1_broker_get_spike_queue_depth(void) {
#if Z1_BROKER_SPIKE_QUEUE_DEPTH > 0
    return broker_queue.spike_count;
#else
    return 0;
#endif
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
    bool received = z1_bus_try_receive_frame(frame);
    if (received) {
        printf("[BROKER] RX: type=%d src=%d dest=%d len=%d\n", 
               frame->type, frame->src, frame->dest, frame->length);
    }
    return received;
}
