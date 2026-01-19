/**
 * Z1 Neuromorphic Compute Node - SNN Execution Engine
 * Code by NeuroFab Corp: 2025-2026
 * 
 * Implements Leaky Integrate-and-Fire (LIF) neuron model with spike processing
 * for distributed spiking neural network execution on RP2350B nodes.
 * 
 * Key algorithm:
 * 1. Process spikes: integrate into membrane potentials
 * 2. Apply leak: V_mem *= leak_rate for all neurons
 * 3. Check threshold DURING leak step (not during spike integration)
 * 4. Fire if V >= threshold AND not in refractory period
 * 5. Reset to 0.0 after firing
 * 
 * Copyright NeuroFab Corp. All rights reserved.
 */

#include "z1_snn_engine.h"
#include "psram_rp2350.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

// Debug flags
#define DEBUG_SPIKE_QUEUE 0      // Log every spike queued
#define DEBUG_SPIKE_PROCESS 0    // Log spike processing details
#define DEBUG_STEP_COUNT 0       // Log processed spike count per step
#define DEBUG_NEURON_FIRE 0      // Log neuron firing

// ============================================================================
// Global State
// ============================================================================

static z1_snn_engine_t g_engine;
static z1_spike_t g_output_spikes[Z1_SNN_MAX_SPIKE_QUEUE];
static uint16_t g_output_spike_count = 0;

// ============================================================================
// Weight Encoding/Decoding
// ============================================================================

/**
 * Decode 8-bit weight to float
 * 
 * Encoding:
 * - 0-127: Positive weights (0.0 to 2.0)
 * - 128-255: Negative weights (-0.01 to -2.0)
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
// Global Neuron ID Functions
// ============================================================================

static inline uint32_t encode_global_neuron_id(uint8_t node_id, uint16_t local_id) {
    return ((uint32_t)node_id << 16) | local_id;
}

static inline void decode_global_neuron_id(uint32_t global_id, uint8_t *node_id, uint16_t *local_id) {
    *node_id = (global_id >> 16) & 0xFF;
    *local_id = global_id & 0xFFFF;
}

// ============================================================================
// PSRAM Neuron Table Parsing
// ============================================================================

/**
 * Parse single 256-byte neuron entry from PSRAM
 * 
 * Binary format:
 *   Offset 0-15:   Neuron state (16 bytes)
 *   Offset 16-23:  Synapse metadata (8 bytes)
 *   Offset 24-31:  Neuron parameters (8 bytes)
 *   Offset 32-39:  Reserved (8 bytes)
 *   Offset 40-279: Synapses (60 × 4 bytes = 240 bytes)
 */
static bool parse_neuron_entry(const uint8_t* data, z1_neuron_t* neuron) {
    if (!data || !neuron) return false;
    
    // Parse neuron state (offset 0-15)
    memcpy(&neuron->neuron_id, data + 0, 2);
    memcpy(&neuron->flags, data + 2, 2);
    memcpy(&neuron->membrane_potential, data + 4, 4);
    memcpy(&neuron->threshold, data + 8, 4);
    memcpy(&neuron->last_spike_time_us, data + 12, 4);
    
    // Parse synapse metadata (offset 16-23)
    memcpy(&neuron->synapse_count, data + 16, 2);
    
    // Parse neuron parameters (offset 24-31)
    memcpy(&neuron->leak_rate, data + 24, 4);
    memcpy(&neuron->refractory_period_us, data + 28, 4);
    
    // Validate
    if (neuron->synapse_count > Z1_SNN_MAX_SYNAPSES) {
        printf("[SNN] ERROR: neuron %u has %u synapses (max %u)\n",
               neuron->neuron_id, neuron->synapse_count, Z1_SNN_MAX_SYNAPSES);
        return false;
    }
    
    // Initialize runtime state
    neuron->global_id = encode_global_neuron_id(g_engine.node_id, neuron->neuron_id);
    neuron->refractory_until_us = 0;
    neuron->spike_count = 0;
    
    // Parse synapses (offset 40+, 4 bytes each)
    const uint8_t* synapse_data = data + 40;
    for (uint16_t i = 0; i < neuron->synapse_count; i++) {
        // Read packed synapse (4 bytes)
        uint32_t synapse_packed;
        memcpy(&synapse_packed, synapse_data + (i * 4), 4);
        
        // Extract source ID (24 bits) and weight (8 bits)
        uint32_t source_id = (synapse_packed >> 8) & 0xFFFFFF;
        uint8_t weight_int = synapse_packed & 0xFF;
        
        // Store in runtime structure
        neuron->synapses[i].source_neuron_id = source_id;
        neuron->synapses[i].weight = decode_weight(weight_int);
        neuron->synapses[i].delay_us = 1000;  // Default 1ms delay
    }
    
    return true;
}

// ============================================================================
// Spike Queue Management
// ============================================================================

static bool spike_queue_push(z1_spike_t spike) {
    if (g_engine.spike_queue_size >= Z1_SNN_MAX_SPIKE_QUEUE) {
        g_engine.stats.spikes_dropped++;
        return false;
    }
    
    g_engine.spike_queue[g_engine.spike_queue_tail] = spike;
    g_engine.spike_queue_tail = (g_engine.spike_queue_tail + 1) % Z1_SNN_MAX_SPIKE_QUEUE;
    g_engine.spike_queue_size++;
    
    return true;
}

static bool spike_queue_pop(z1_spike_t* spike) {
    if (g_engine.spike_queue_size == 0) {
        return false;
    }
    
    *spike = g_engine.spike_queue[g_engine.spike_queue_head];
    g_engine.spike_queue_head = (g_engine.spike_queue_head + 1) % Z1_SNN_MAX_SPIKE_QUEUE;
    g_engine.spike_queue_size--;
    
    return true;
}

// ============================================================================
// Spike Processing
// ============================================================================

// Forward declaration
static void fire_neuron(z1_neuron_t* neuron);

/**
 * Process single incoming spike
 * 
 * Handles two cases:
 * 1. Direct stimulation: If spike targets a local neuron, directly stimulate it
 *    (This handles external input injection from controller)
 * 2. Synaptic propagation: Apply spike to all neurons with synapses from this source
 *    (This handles spike propagation between neurons)
 * 
 * Note: Both cases can apply simultaneously - input neurons can receive direct
 *       stimulation AND serve as synaptic sources to other neurons.
 */
static void process_spike(const z1_spike_t* spike) {
    uint32_t source_id = spike->neuron_id;
    
    // Decode neuron ID
    uint8_t source_node;
    uint16_t source_local;
    decode_global_neuron_id(source_id, &source_node, &source_local);
    
    // SPECIAL CASE: Input neuron direct stimulation
    // If spike targets a LOCAL neuron (from controller injection), check if it's an input neuron
    // Input neurons have NO incoming synapses (synapse_count == 0 for that neuron in the topology)
    // However, synapses are stored on TARGET neurons, not source neurons.
    // So we check: does ANY neuron have a synapse FROM this neuron? If not, it's an input.
    if (source_node == g_engine.node_id && source_local < g_engine.neuron_count) {
        z1_neuron_t* target = &g_engine.neurons[source_local];
        
        // Input neurons have synapse_count == 0 (no incoming connections in topology)
        // Directly stimulate them like external current injection
        if (target->synapse_count == 0) {
            target->membrane_potential += spike->value;
            g_engine.stats.spikes_processed++;
            g_engine.stats.membrane_updates++;
            
#if DEBUG_SPIKE_PROCESS
            printf("[SNN-%u] Input injection: Neuron %u, V_mem += %.3f (now %.3f, threshold %.3f)\n",
                   g_engine.node_id, source_local, spike->value,
                   target->membrane_potential, target->threshold);
#endif
            
            // Check threshold IMMEDIATELY (input neurons should fire right away)
            if (target->membrane_potential >= target->threshold) {
                if (g_engine.current_time_us >= target->refractory_until_us) {
                    fire_neuron(target);
                }
            }
            // Don't return - continue to synaptic processing
        }
    }
    
    // Process spike to ALL neurons that have synapses from this source
    // This handles BOTH local and remote spikes:
    // - Remote spikes: Normal inter-node communication
    // - Local spikes: Intra-node communication (e.g., Neuron 0 -> Neuron 2 on same node)
    //
    // IMPORTANT: We process local spikes because when a neuron fires:
    // 1. Spike is broadcast over Matrix bus
    // 2. ALL nodes receive it (including origin node)
    // 3. Each node applies it to neurons with synapses from that source
    // 4. Refractory period prevents source neuron from re-firing immediately
    //
    // Example: XOR network with Neuron 0 and Neuron 2 both on Node 0
    // - Neuron 0 fires -> broadcast spike (node=0, local=0)
    // - Node 0 receives its own broadcast
    // - Neuron 2 has synapse from Neuron 0 -> applies weight
    // - Neuron 0 is in refractory period -> won't re-fire from its own spike
    
#if DEBUG_SPIKE_PROCESS
    printf("[SNN-%u] Processing spike from node %u, neuron %u (global_id=0x%08lX)\n",
           g_engine.node_id, source_node, source_local, source_id);
#endif
    
    // Apply spike to all neurons with synapses from this source
    for (uint16_t i = 0; i < g_engine.neuron_count; i++) {
        z1_neuron_t* neuron = &g_engine.neurons[i];
        
        for (uint16_t j = 0; j < neuron->synapse_count; j++) {
            z1_synapse_runtime_t* synapse = &neuron->synapses[j];
            
            if (synapse->source_neuron_id == source_id) {
                // Apply synaptic weight
                float delta_v = synapse->weight * spike->value;
                neuron->membrane_potential += delta_v;
                
                g_engine.stats.spikes_processed++;
                g_engine.stats.membrane_updates++;
                
#if DEBUG_SPIKE_PROCESS
                printf("[SNN-%u] Synapse: Spike %lu -> Neuron %u: V_mem += %.3f (now %.3f, threshold %.3f)\n",
                       g_engine.node_id, source_id, neuron->neuron_id,
                       delta_v, neuron->membrane_potential, neuron->threshold);
#endif
                
                // CRITICAL: Check threshold IMMEDIATELY after integration
                // Reference: Python snn_engine.py line 246-249
                if (neuron->membrane_potential >= neuron->threshold) {
                    // Check refractory period
                    if (g_engine.current_time_us >= neuron->refractory_until_us) {
                        fire_neuron(neuron);
                    }
                    break;  // Stop processing remaining synapses for this neuron
                }
            }
        }
    }
}

/**
 * Fire a neuron and generate output spike
 * 
 * Called when V_mem >= threshold during leak step.
 * 
 * Actions:
 * 1. Record spike timestamp
 * 2. Set refractory period (prevents immediate re-firing)
 * 3. Reset membrane potential to 0.0
 * 4. Generate spike with GLOBAL neuron ID (node_id << 16 | local_id)
 * 5. Add to output queue for broadcast transmission
 * 
 * Global ID encoding allows other nodes to identify spike source
 * and apply correct synaptic weights.
 */
static void fire_neuron(z1_neuron_t* neuron) {
#if DEBUG_NEURON_FIRE
    printf("[SNN-%u] ⚡ Neuron %u FIRED! (V_mem=%.3f, threshold=%.3f)\n",
           g_engine.node_id, neuron->neuron_id,
           neuron->membrane_potential, neuron->threshold);
#endif
    
    // Record spike time
    neuron->last_spike_time_us = g_engine.current_time_us;
    neuron->refractory_until_us = g_engine.current_time_us + neuron->refractory_period_us;
    
    // Reset membrane potential
    neuron->membrane_potential = 0.0f;
    
    // Generate outgoing spike with GLOBAL ID
    if (g_output_spike_count < Z1_SNN_MAX_SPIKE_QUEUE) {
        g_output_spikes[g_output_spike_count].neuron_id = neuron->global_id;
        g_output_spikes[g_output_spike_count].timestamp_us = g_engine.current_time_us;
        g_output_spikes[g_output_spike_count].value = 1.0f;
        g_output_spike_count++;
    }
    
    // Update statistics
    neuron->spike_count++;
    g_engine.stats.spikes_generated++;
    g_engine.stats.neurons_fired++;
}

// ============================================================================
// Public API Implementation
// ============================================================================

bool z1_snn_init(uint8_t node_id) {
    memset(&g_engine, 0, sizeof(g_engine));
    
    g_engine.node_id = node_id;
    g_engine.initialized = true;
    g_engine.running = false;
    g_engine.timestep_us = 1000;  // 1ms default
    g_engine.current_time_us = 0;
    
    g_output_spike_count = 0;
    
    printf("[SNN-%u] Engine initialized\n", node_id);
    return true;
}

bool z1_snn_load_topology_from_psram(void) {
    if (!g_engine.initialized) {
        printf("[SNN] ERROR: Engine not initialized\n");
        return false;
    }
    
    uint8_t entry_buffer[Z1_NEURON_ENTRY_SIZE];
    uint32_t psram_addr = Z1_SNN_NEURON_TABLE_ADDR;
    
    g_engine.neuron_count = 0;
    
    // printf("[SNN-%u] Loading neuron table from PSRAM @ 0x%08lX...\n",
    //        g_engine.node_id, psram_addr);
    
    // Read neuron entries until end marker (neuron_id = 0xFFFF)
    for (uint16_t i = 0; i < Z1_SNN_MAX_NEURONS; i++) {
        // Read 256-byte entry from PSRAM (void function, always succeeds)
        psram_read(psram_addr, entry_buffer, Z1_NEURON_ENTRY_SIZE);
        
        // Check for end marker
        uint16_t neuron_id;
        memcpy(&neuron_id, entry_buffer, 2);
        if (neuron_id == 0xFFFF) {
            // printf("[SNN-%u] Found end marker, table complete\n", g_engine.node_id);
            break;
        }
        
        // Parse neuron
        z1_neuron_t* neuron = &g_engine.neurons[g_engine.neuron_count];
        if (!parse_neuron_entry(entry_buffer, neuron)) {
            printf("[SNN] ERROR: Failed to parse neuron entry %u\n", i);
            return false;
        }
        
        // printf("[SNN-%u] Loaded neuron %u (global 0x%08lX): threshold=%.3f, leak=%.3f, synapses=%u\n",
        //        g_engine.node_id, neuron->neuron_id, neuron->global_id,
        //        neuron->threshold, neuron->leak_rate, neuron->synapse_count);
        
        g_engine.neuron_count++;
        psram_addr += Z1_NEURON_ENTRY_SIZE;
    }
    
    // printf("[SNN-%u] Loaded %u neurons from PSRAM\n",
    //        g_engine.node_id, g_engine.neuron_count);
    
    return g_engine.neuron_count > 0;
}

void z1_snn_start(void) {
    g_engine.running = true;
    g_engine.paused = false;
    g_engine.current_time_us = 0;
    memset(&g_engine.stats, 0, sizeof(g_engine.stats));
    // printf("[SNN-%u] Started\n", g_engine.node_id);
}

void z1_snn_stop(void) {
    g_engine.running = false;
    printf("[SNN-%u] Stopped\n", g_engine.node_id);
}

void z1_snn_pause(void) {
    g_engine.paused = true;
}

void z1_snn_resume(void) {
    g_engine.paused = false;
}

void z1_snn_step(void) {
    if (!g_engine.running || g_engine.paused) return;
    
    // Update time
    g_engine.current_time_us += g_engine.timestep_us;
    g_engine.stats.simulation_steps++;
    
    // Clear output spike buffer
    g_output_spike_count = 0;
    
    // STEP 1: Process queued spikes (LIMITED to prevent blocking)
    // If queue has more spikes, they'll be processed in next timestep
    // This ensures the main loop can respond to controller queries
    // After broker priority fix, commands can interrupt spike processing safely
    const uint16_t MAX_SPIKES_PER_TIMESTEP = 100;
    z1_spike_t spike;
    uint16_t spikes_processed = 0;
    while (spike_queue_pop(&spike) && spikes_processed < MAX_SPIKES_PER_TIMESTEP) {
        process_spike(&spike);
        spikes_processed++;
    }
    
    // STEP 2: Apply leak and check threshold (CRITICAL ORDER from working implementation)
    for (uint16_t i = 0; i < g_engine.neuron_count; i++) {
        z1_neuron_t* neuron = &g_engine.neurons[i];
        
        // Apply leak - CRITICAL: leak_rate is RETENTION factor (what we KEEP, not what leaks)
        // Formula: V_new = V_old * leak_rate
        // E.g., leak_rate=0.95 means keep 95%, lose 5% per timestep
        // Reference: Python snn_engine.py line 195: neuron.membrane_potential *= neuron.leak_rate
        if (neuron->membrane_potential > 0.0f && neuron->leak_rate > 0.0f) {
            neuron->membrane_potential *= neuron->leak_rate;
            g_engine.stats.membrane_updates++;
        }
        
        // Check threshold after leak (secondary check)
        // This catches neurons that accumulated potential over multiple timesteps
        // without receiving spikes. Most firing should happen during spike processing.
        if (neuron->membrane_potential >= neuron->threshold) {
            // Check refractory period
            if (g_engine.current_time_us >= neuron->refractory_until_us) {
                fire_neuron(neuron);
            }
        }
    }
}

/**
 * Inject spike with immediate processing (for input neurons)
 * Reference: Python snn_engine.py lines 137-163
 * 
 * This function directly adds to membrane potential and checks threshold,
 * without queuing. Use this for controller-injected input spikes that should
 * take effect immediately within the same timestep.
 */
bool z1_snn_inject_spike_immediate(uint16_t local_neuron_id, float value) {
    if (local_neuron_id >= g_engine.neuron_count) {
        return false;
    }
    
    z1_neuron_t* neuron = &g_engine.neurons[local_neuron_id];
    
    // Add value to membrane potential
    neuron->membrane_potential += value;
    g_engine.stats.spikes_received++;
    g_engine.stats.membrane_updates++;
    
#if DEBUG_SPIKE_PROCESS
    printf("[SNN-%u] Inject immediate: Neuron %u, V_mem += %.3f (now %.3f, threshold %.3f)\n",
           g_engine.node_id, local_neuron_id, value, 
           neuron->membrane_potential, neuron->threshold);
#endif
    
    // Check threshold IMMEDIATELY (Python behavior)
    if (neuron->membrane_potential >= neuron->threshold) {
        if (g_engine.current_time_us >= neuron->refractory_until_us) {
            fire_neuron(neuron);
        }
    }
    
    return true;
}

/**
 * Inject spike via queue (for network spikes)
 * Use this for spikes received from other nodes via Matrix bus.
 */
bool z1_snn_inject_spike(z1_spike_t spike) {
    g_engine.stats.spikes_received++;
#if DEBUG_SPIKE_QUEUE
    printf("[SNN-%u] 📥 Queued spike: neuron_id=%lu, queue_size=%u/%u\n",
           g_engine.node_id, spike.neuron_id, 
           g_engine.spike_queue_size + 1, Z1_SNN_MAX_SPIKE_QUEUE);
#endif
    return spike_queue_push(spike);
}

const z1_spike_t* z1_snn_get_output_spikes(uint16_t* count) {
    *count = g_output_spike_count;
    return g_output_spikes;
}

void z1_snn_get_stats(z1_snn_stats_t* stats) {
    if (stats) {
        memcpy(stats, &g_engine.stats, sizeof(z1_snn_stats_t));
    }
}

void z1_snn_reset_stats(void) {
    memset(&g_engine.stats, 0, sizeof(z1_snn_stats_t));
}

bool z1_snn_is_running(void) {
    return g_engine.running;
}

uint16_t z1_snn_get_neuron_count(void) {
    return g_engine.neuron_count;
}

uint32_t z1_snn_get_current_time(void) {
    return g_engine.current_time_us;
}

// ============================================================================
// Debug Functions
// ============================================================================
