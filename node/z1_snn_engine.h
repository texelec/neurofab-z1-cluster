/**
 * Z1 Neuromorphic Compute Node - SNN Execution Engine
 * Code by NeuroFab Corp: 2025-2026
 * 
 * Implements Leaky Integrate-and-Fire (LIF) neuron model with spike processing
 * for distributed spiking neural network execution on RP2350B nodes.
 * 
 * Key principles:
 * - Membrane potential integrates weighted spikes
 * - Leak applied every timestep (V *= leak_rate)
 * - Fire when V >= threshold (during leak check, not spike integration)
 * - Reset to 0.0 after firing
 * - Refractory period prevents immediate re-firing
 * 
 * Copyright NeuroFab Corp. All rights reserved.
 */

#ifndef Z1_SNN_ENGINE_H
#define Z1_SNN_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include "../z1_broker/z1_broker.h"
#include <stdbool.h>
#include "../z1_broker/z1_broker.h"

// ============================================================================
// Configuration
// ============================================================================

#define Z1_SNN_MAX_NEURONS      16    // Maximum neurons per node (reduced for testing - XOR needs 7)
#define Z1_SNN_MAX_SYNAPSES     60    // Maximum synapses per neuron
#define Z1_SNN_MAX_SPIKE_QUEUE  256   // Spike queue size

// PSRAM addresses (absolute uncached addresses for new PSRAM API)
#define Z1_SNN_PSRAM_BASE           0x15000000  // PSRAM uncached base (was 0x00000000 offset)
#define Z1_SNN_NEURON_TABLE_ADDR    0x15100000  // PSRAM base + 1MB offset (was 0x00100000)
#define Z1_NEURON_ENTRY_SIZE        256         // Bytes per neuron entry

// Neuron flags (matches reference_github format)
#define Z1_NEURON_FLAG_ACTIVE       0x0001  // Neuron is active
#define Z1_NEURON_FLAG_INHIBITORY   0x0002  // Inhibitory neuron
#define Z1_NEURON_FLAG_INPUT        0x0004  // Input neuron (no processing)
#define Z1_NEURON_FLAG_OUTPUT       0x0008  // Output neuron
#define Z1_NEURON_FLAG_REFRACTORY   0x0010  // In refractory period

// ============================================================================
// PSRAM Neuron Table Structures (Persistent, 256 bytes per neuron)
// ============================================================================

/**
 * Neuron state in PSRAM (16 bytes)
 */
typedef struct __attribute__((packed)) {
    uint16_t neuron_id;           // Local neuron ID (0-4095)
    uint16_t flags;               // Status flags
    float    membrane_potential;  // Current membrane potential
    float    threshold;           // Spike threshold
    uint32_t last_spike_time_us;  // Timestamp of last spike
} z1_neuron_state_t;

/**
 * Synapse entry (4 bytes, packed)
 * 
 * Bits [31:8]  - Source neuron global ID (24 bits: node_id << 16 | local_id)
 * Bits [7:0]   - Weight (8-bit fixed point, 0-255)
 */
typedef uint32_t z1_synapse_t;

/**
 * Complete neuron table entry in PSRAM (256 bytes)
 * 
 * Binary format:
 *   Offset 0-15:   Neuron state (16 bytes)
 *   Offset 16-23:  Synapse metadata (8 bytes)
 *   Offset 24-31:  Neuron parameters (8 bytes)
 *   Offset 32-39:  Reserved (8 bytes)
 *   Offset 40-279: Synapses (60 × 4 bytes = 240 bytes)
 * 
 * End marker: neuron_id = 0xFFFF
 */
typedef struct __attribute__((packed)) {
    // Neuron state (16 bytes)
    z1_neuron_state_t state;
    
    // Synapse metadata (8 bytes)
    uint16_t synapse_count;       // Number of incoming synapses
    uint16_t synapse_capacity;    // Max synapses allocated
    uint32_t reserved1;
    
    // Neuron parameters (8 bytes)
    float    leak_rate;           // Membrane leak rate (0.0-1.0, typical 0.95)
    uint32_t refractory_period_us; // Refractory period
    
    // Reserved (8 bytes)
    uint32_t reserved2[2];
    
    // Synapse entries (60 × 4 bytes = 240 bytes)
    z1_synapse_t synapses[Z1_SNN_MAX_SYNAPSES];
} z1_neuron_entry_t;

// ============================================================================
// Runtime Structures (in RAM for fast access)
// ============================================================================

/**
 * Runtime synapse structure (decoded from packed format)
 */
typedef struct {
    uint32_t source_neuron_id;  // Global ID (node_id << 16 | local_id)
    float weight;               // Decoded weight (-2.0 to 2.0)
    uint16_t delay_us;          // Synaptic delay (future use)
} z1_synapse_runtime_t;

/**
 * Runtime neuron structure (loaded from PSRAM, optimized for simulation)
 */
typedef struct {
    uint16_t neuron_id;                                    // Local neuron ID
    uint16_t flags;                                        // Status flags
    uint32_t global_id;                                    // Global ID for routing
    
    // LIF parameters
    float membrane_potential;                              // Current V_mem
    float threshold;                                       // Spike threshold
    float leak_rate;                                       // Membrane leak (0.0-1.0)
    
    // Timing
    uint32_t last_spike_time_us;                           // Last spike time
    uint32_t refractory_period_us;                         // Refractory period
    uint32_t refractory_until_us;                          // Refractory end time
    
    // Statistics
    uint32_t spike_count;                                  // Total spikes generated
    
    // Synapses
    uint16_t synapse_count;                                // Number of synapses
    z1_synapse_runtime_t synapses[Z1_SNN_MAX_SYNAPSES];    // Synapse array
} z1_neuron_t;

/**
 * Spike structure (matches spike_frame_t from z1_commands.h)
 */
typedef struct {
    uint32_t neuron_id;      // Global neuron ID
    uint32_t timestamp_us;   // Spike timestamp
    float value;             // Spike value (usually 1.0)
} z1_spike_t;

/**
 * SNN statistics
 */
typedef struct {
    uint32_t spikes_received;   // Spikes from bus
    uint32_t spikes_injected;   // Spikes injected locally (input layer)
    uint32_t spikes_processed;  // Spikes integrated into neurons
    uint32_t spikes_generated;  // Spikes generated by local neurons
    uint32_t spikes_dropped;    // Spikes dropped (queue full)
    uint32_t membrane_updates;  // Number of membrane potential updates
    uint32_t simulation_steps;  // Total timesteps executed
    uint32_t neurons_fired;     // Number of neuron firing events
} z1_snn_stats_t;

/**
 * SNN engine runtime state
 */
typedef struct {
    uint8_t node_id;                                       // This node's ID
    bool initialized;                                      // Engine initialized
    bool running;                                          // Simulation running
    bool paused;                                           // Paused for stats collection
    
    uint16_t neuron_count;                                 // Number of neurons loaded
    uint32_t current_time_us;                              // Current simulation time
    uint32_t timestep_us;                                  // Simulation timestep (1000us default)
    
    z1_neuron_t neurons[Z1_SNN_MAX_NEURONS];               // Neuron array
    
    z1_spike_t spike_queue[Z1_SNN_MAX_SPIKE_QUEUE];        // Circular spike queue
    uint16_t spike_queue_head;                             // Queue read pointer
    uint16_t spike_queue_tail;                             // Queue write pointer
    uint16_t spike_queue_size;                             // Current queue size
    
    z1_snn_stats_t stats;                                  // Statistics
} z1_snn_engine_t;

// ============================================================================
// Public API
// ============================================================================

/**
 * Initialize SNN engine
 * 
 * @param node_id This node's ID (0-15)
 * @return true if successful
 */
bool z1_snn_init(uint8_t node_id);

/**
 * Load neuron tables from PSRAM
 * 
 * Reads neuron entries from Z1_SNN_NEURON_TABLE_ADDR until end marker (0xFFFF).
 * Decodes synapses and stores in runtime structures.
 * 
 * @return true if successful, false if PSRAM read error or invalid data
 */
bool z1_snn_load_topology_from_psram(void);

/**
 * Start SNN execution
 * 
 * Begins simulation loop (call z1_snn_step() repeatedly).
 */
void z1_snn_start(void);

/**
 * Stop SNN execution
 */
void z1_snn_stop(void);

/**
 * Pause SNN execution (freezes timestep timer, keeps all state)
 */
void z1_snn_pause(void);

/**
 * Resume SNN execution from pause
 */
void z1_snn_resume(void);

/**
 * Execute single timestep
 * 
 * CRITICAL ALGORITHM (from reference_github, WORKING implementation):
 * 1. Process all queued spikes (integrate into membrane potentials)
 * 2. Apply leak to all neurons (V_mem *= leak_rate)
 * 3. Check threshold during leak (fire if V >= threshold AND not refractory)
 * 4. Fire neurons (reset V_mem to 0, send spikes to bus)
 * 
 * NOTE: Threshold check AFTER leak, not during spike integration!
 * This is critical for proper neuron firing dynamics.
 */
void z1_snn_step(void);

/**
 * Inject spike with immediate processing (for input neurons)
 * 
 * Directly adds value to membrane potential and checks threshold immediately.
 * Use this for controller-injected input spikes that should fire within the
 * same timestep. Matches Python reference implementation behavior.
 * 
 * @param local_neuron_id Local neuron ID on this node (0-based)
 * @param value Spike value (typically 1.0)
 * @return true if successful, false if invalid neuron ID
 */
bool z1_snn_inject_spike_immediate(uint16_t local_neuron_id, float value);

/**
 * Inject spike into queue (from bus or local input)
 * 
 * Spike is queued and processed at start of next simulation step.
 * Use this for spikes received from other nodes via Matrix bus.
 * 
 * @param spike Spike to inject
 * @return true if successful, false if queue full
 */
bool z1_snn_inject_spike(z1_spike_t spike);

/**
 * Get generated spikes (for transmission over bus)
 * 
 * Returns pointer to internal buffer of spikes generated during last timestep.
 * Caller should transmit these immediately as they are cleared on next step.
 * 
 * @param count Output: number of spikes available
 * @return Pointer to spike array (valid until next z1_snn_step())
 */
const z1_spike_t* z1_snn_get_output_spikes(uint16_t* count);

/**
 * Get current statistics
 * 
 * @param stats Output: current statistics
 */
void z1_snn_get_stats(z1_snn_stats_t* stats);

/**
 * Reset statistics counters
 */
void z1_snn_reset_stats(void);

/**
 * Get engine state
 * 
 * @return true if running, false if stopped
 */
bool z1_snn_is_running(void);

/**
 * Get neuron count
 * 
 * @return Number of neurons loaded
 */
uint16_t z1_snn_get_neuron_count(void);

/**
 * Get current simulation time
 * 
 * @return Current time in microseconds
 */
uint32_t z1_snn_get_current_time(void);

/**
 * Get SNN statistics
 * 
 * @param stats Output: statistics structure
 */
void z1_snn_get_stats(z1_snn_stats_t* stats);

/**
 * Reset statistics counters
 */
void z1_snn_reset_stats(void);

/**
 * Check if SNN is running
 * 
 * @return true if running
 */
bool z1_snn_is_running(void);

#endif // Z1_SNN_ENGINE_H
