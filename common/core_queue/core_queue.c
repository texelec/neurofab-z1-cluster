/**
 * Core Queue - Lock-Free Inter-Core FIFO
 * Code by NeuroFab Corp: 2025-2026
 */

#include "core_queue.h"
#include <string.h>

void core_queue_init(core_queue_t *queue) {
    queue->head = 0;
    queue->tail = 0;
    memset(queue->frames, 0, sizeof(queue->frames));
}

bool core_queue_push(core_queue_t *queue, const z1_frame_t *frame) {
    // Check if full (leave one slot empty for full detection)
    uint32_t next_head = (queue->head + 1) & (CORE_QUEUE_SIZE - 1);
    if (next_head == queue->tail) {
        return false;  // Queue full
    }
    
    // Copy frame to queue
    memcpy(&queue->frames[queue->head], frame, sizeof(z1_frame_t));
    
    // Memory barrier before updating head (ensure frame write completes)
    __sync_synchronize();
    
    // Advance head pointer (atomic, visible to consumer)
    queue->head = next_head;
    
    return true;
}

bool core_queue_pop(core_queue_t *queue, z1_frame_t *frame) {
    // Check if empty
    if (queue->tail == queue->head) {
        return false;  // Queue empty
    }
    
    // Copy frame from queue
    memcpy(frame, &queue->frames[queue->tail], sizeof(z1_frame_t));
    
    // Memory barrier before updating tail (ensure frame read completes)
    __sync_synchronize();
    
    // Advance tail pointer (atomic, visible to producer)
    queue->tail = (queue->tail + 1) & (CORE_QUEUE_SIZE - 1);
    
    return true;
}

uint32_t core_queue_count(const core_queue_t *queue) {
    uint32_t head = queue->head;
    uint32_t tail = queue->tail;
    
    if (head >= tail) {
        return head - tail;
    } else {
        return (CORE_QUEUE_SIZE - tail) + head;
    }
}
