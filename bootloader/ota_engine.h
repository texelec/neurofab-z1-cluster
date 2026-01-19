/**
 * Z1 Onyx - OTA Update Engine Header
 * Code by NeuroFab Corp: 2025-2026
 */

#ifndef OTA_ENGINE_H
#define OTA_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include "z1_bus.h"

/**
 * Initialize OTA engine
 */
void ota_init(void);

/**
 * Handle UPDATE_MODE_ENTER command
 */
void ota_handle_enter_update_mode(void);

/**
 * Handle DATA_CHUNK command
 */
void ota_handle_data_chunk(z1_frame_t* frame);

/** * Handle UPDATE_VERIFY_REQ command
 */
void ota_handle_verify(void);

/** * Handle UPDATE_FINALIZE command
 */
void ota_handle_finalize(void);

/**
 * Handle UPDATE_MODE_EXIT command
 */
void ota_handle_exit_update_mode(void);

/**
 * Check if in OTA mode
 */
bool ota_is_active(void);

/**
 * Get current OTA state
 */
uint8_t ota_get_state(void);

#endif // OTA_ENGINE_H
