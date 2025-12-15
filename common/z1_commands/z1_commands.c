/**
 * Z1 Command Layer Implementation
 * 
 * Thin wrapper over existing broker/bus - NO timing impact
 */

#include "z1_commands.h"
#include "../z1_broker/z1_broker.h"
#include <string.h>

// ============================================================================
// Command Send (Type 1 Frame)
// ============================================================================

bool z1_cmd_send(uint8_t dest, uint8_t opcode, const void *payload, uint16_t len) {
    if (len > Z1_CMD_MAX_PAYLOAD) {
        return false;  // Payload too large
    }
    
    // Pack command into word buffer
    uint16_t buffer[256];
    buffer[0] = opcode | (0 << 8);  // opcode in low byte, flags in high byte
    
    // Copy payload (if any)
    if (len > 0 && payload != NULL) {
        memcpy(&buffer[1], payload, len);
    }
    
    // Calculate total size in words (round up)
    uint32_t total_words = (len + 2 + 1) / 2;  // +2 for opcode+flags, round up
    
    // Send Type 1 frame via existing bus (with ACK by default)
    // NOTE: Uses z1_bus_send_frame directly - broker queue doesn't support Type 1 yet
    return z1_bus_send_frame(1, dest, 0, buffer, total_words);
}

bool z1_cmd_broadcast(uint8_t opcode, const void *payload, uint16_t len) {
    if (len > Z1_CMD_MAX_PAYLOAD) {
        return false;
    }
    
    uint16_t buffer[256];
    buffer[0] = opcode | (0 << 8);
    
    if (len > 0 && payload != NULL) {
        memcpy(&buffer[1], payload, len);
    }
    
    uint32_t total_words = (len + 2 + 1) / 2;
    
    // Broadcast: dest=31, NOACK flag set
    return z1_bus_send_frame_no_ack(31, buffer, total_words, 0);
}

// ============================================================================
// Command Receive (Type 1 Filter)
// ============================================================================

bool z1_cmd_receive(z1_command_t *cmd, uint8_t *src) {
    z1_frame_t frame;
    
    // Use existing broker receive (NO timing change - just a wrapper call)
    if (!z1_broker_try_receive(&frame)) {
        return false;  // No frames available
    }
    
    // Filter: only process Type 1 frames
    if (frame.type != 1) {
        return false;  // Not a command (probably Type 0 spike)
    }
    
    // Validate CRC
    if (!frame.crc_valid) {
        return false;  // Corrupted frame
    }
    
    // Parse command structure from payload
    cmd->opcode = frame.payload[0] & 0xFF;
    cmd->flags = (frame.payload[0] >> 8) & 0xFF;
    
    // Payload length = frame length - 2 bytes (opcode+flags)
    cmd->payload_len = (frame.length >= 2) ? (frame.length - 2) : 0;
    
    // Copy payload data (skip first word which is opcode+flags)
    if (cmd->payload_len > 0) {
        memcpy(cmd->payload, &frame.payload[1], cmd->payload_len);
    }
    
    *src = frame.src;
    
    return true;
}
