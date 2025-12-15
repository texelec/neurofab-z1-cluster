/**
 * Core Queue - Thread-Safe Inter-Core Communication
 * 
 * Lock-free FIFO queue for passing frames between RP2350 cores.
 * Uses atomic operations for head/tail pointers (no mutex overhead).
 * 
 * Core 0 (Bus Engine):  Pushes RX frames, pops TX frames
 * Core 1 (Application): Pops RX frames, pushes TX frames
 */

#ifndef CORE_QUEUE_H
#define CORE_QUEUE_H

#include <stdint.h>
#include <stdbool.h>
#include "../z1_onyx_bus/z1_bus.h"

#define CORE_QUEUE_SIZE 64  // Must be power of 2 for mask optimization

/**
 * Lock-free circular buffer for z1_frame_t
 * Safe for single producer, single consumer (SPSC) pattern
 */
typedef struct {
    z1_frame_t frames[CORE_QUEUE_SIZE];
    volatile uint32_t head;  // Write index (producer)
    volatile uint32_t tail;  // Read index (consumer)
} core_queue_t;

/**
 * Initialize queue (call before launching cores)
 */
void core_queue_init(core_queue_t *queue);

/**
 * Push frame to queue (producer side)
 * 
 * @param queue Queue to push to
 * @param frame Frame to copy into queue
 * @return true if pushed, false if queue full
 * 
 * Thread-safe for SPSC (single producer, single consumer)
 */
bool core_queue_push(core_queue_t *queue, const z1_frame_t *frame);

/**
 * Pop frame from queue (consumer side)
 * 
 * @param queue Queue to pop from
 * @param frame [out] Buffer to copy frame into
 * @return true if popped, false if queue empty
 * 
 * Thread-safe for SPSC (single producer, single consumer)
 */
bool core_queue_pop(core_queue_t *queue, z1_frame_t *frame);

/**
 * Get number of frames in queue (approximate, lock-free read)
 * 
 * @param queue Queue to check
 * @return Number of frames pending
 */
uint32_t core_queue_count(const core_queue_t *queue);

/**
 * Check if queue is empty (lock-free)
 */
static inline bool core_queue_is_empty(const core_queue_t *queue) {
    return queue->head == queue->tail;
}

/**
 * Check if queue is full (lock-free)
 */
static inline bool core_queue_is_full(const core_queue_t *queue) {
    return ((queue->head + 1) & (CORE_QUEUE_SIZE - 1)) == queue->tail;
}

#endif // CORE_QUEUE_H
