# SNN Engine Specification and Required Fixes

**Date**: January 18, 2026  
**Status**: REFERENCE DOCUMENT - Engine operational, document preserved for historical context  
**Source**: Analysis of working Python implementation from `ahtx/neurofab-z1-cluster`

---

## Executive Summary

Our current Z1 Onyx SNN engine implementation has **CRITICAL LOGIC ERRORS** that prevent proper operation. This document provides:

1. **Canonical specification** derived from the working Python implementation
2. **Detailed comparison** of our current vs. correct implementation
3. **TODO list** of required fixes with priority levels

**Key Finding**: The working Python implementation (`emulator/core/snn_engine.py`) demonstrates proper LIF neuron behavior that our current C implementation fails to replicate.

---

## Part 1: Canonical SNN Engine Specification

### 1.1 Core Algorithm (from working Python implementation)

```python
def _simulation_step(self):
    """Execute one simulation step."""
    self.stats['simulation_steps'] += 1
    self.current_time_us += self.timestep_us
    
    # STEP 1: Process incoming spikes
    while self.incoming_spikes:
        spike = self.incoming_spikes.popleft()
        self._process_spike(spike)
    
    # STEP 2: Update all neurons (leak)
    for neuron in self.neurons.values():
        if neuron.membrane_potential > 0:
            neuron.membrane_potential *= neuron.leak_rate
```

**CRITICAL INSIGHT**: The working implementation separates concerns:
- **Spike processing**: Adds synaptic weights to membrane potential
- **Leak application**: Decays membrane potential by multiplicative factor
- **Threshold checking**: Done DURING spike processing, not during leak

### 1.2 Spike Processing Algorithm

```python
def _process_spike(self, spike: Spike):
    """Process incoming spike."""
    # Calculate global ID of spiking neuron
    spike_global_id = (spike.source_backplane << 24) | (spike.source_node << 16) | spike.neuron_id
    
    # Find all neurons that have synapses from this source
    for target_neuron_id, synapses in self.synapses.items():
        neuron = self.neurons.get(target_neuron_id)
        if not neuron:
            continue
        
        # Check if in refractory period
        if self.current_time_us - neuron.last_spike_time_us < neuron.refractory_period_us:
            continue
        
        # Apply synaptic input
        for synapse in synapses:
            if synapse.source_neuron_global_id == spike_global_id:
                # Add weighted input to membrane potential
                neuron.membrane_potential += synapse.weight * spike.value
                
                # Check for spike IMMEDIATELY after integration
                if neuron.membrane_potential >= neuron.threshold:
                    self._generate_spike(neuron)
                    break  # Stop processing synapses for this neuron
```

**KEY BEHAVIORS**:
1. **Immediate threshold check**: After EACH synaptic integration, check if neuron should fire
2. **Early exit**: Once neuron fires, stop processing remaining synapses for that neuron
3. **Refractory period**: Checked BEFORE applying synaptic input
4. **Leak is separate**: NOT applied during spike processing

### 1.3 Input Spike Injection

```python
def inject_spike(self, neuron_id: int, value: float = 1.0):
    """Inject external spike - directly causes the neuron to fire."""
    self.stats['total_spikes_received'] += 1
    
    # Get the neuron
    neuron = self.neurons.get(neuron_id)
    if not neuron:
        return
    
    # Add to membrane potential (same for all neurons)
    neuron.membrane_potential += value
    
    # Check if neuron should fire
    if neuron.membrane_potential >= neuron.threshold:
        self._generate_spike(neuron)
```

**KEY BEHAVIOR**: Input injection is IDENTICAL to synaptic integration:
- Add value to membrane potential
- Check threshold IMMEDIATELY
- Fire if threshold exceeded

### 1.4 Neuron Firing

```python
def _generate_spike(self, neuron: Neuron):
    """Generate output spike from neuron."""
    # Reset neuron
    neuron.membrane_potential = 0.0
    neuron.last_spike_time_us = self.current_time_us
    
    # Create output spike
    spike = Spike(
        neuron_id=neuron.neuron_id,
        source_node=self.node_id,
        source_backplane=self.backplane_id,
        timestamp_us=self.current_time_us,
        value=1.0
    )
    
    self.outgoing_spikes.append(spike)
    self.stats['total_spikes_sent'] += 1
    self.stats['neurons_spiked'] += 1
```

**KEY BEHAVIORS**:
1. **Reset to 0.0**: Membrane potential is reset, not set to negative value
2. **Record timestamp**: Used for refractory period calculation
3. **Create spike immediately**: Spike is generated and queued for broadcast

### 1.5 Leak Rate Semantics

**CRITICAL SPECIFICATION**:

```python
# From working Python implementation
neuron.membrane_potential *= neuron.leak_rate  # leak_rate is RETENTION factor (0.95 = keep 95%)
```

**Definition**: `leak_rate` is the **fraction RETAINED**, not the fraction lost.

**Examples**:
- `leak_rate = 0.95`: Keep 95%, lose 5%
- `leak_rate = 0.80`: Keep 80%, lose 20%
- `leak_rate = 1.00`: No leak (keep 100%)
- `leak_rate = 0.00`: Total leak (keep 0%)

**Typical values**: 0.90-0.99 for realistic neural decay

---

## Part 2: Current Implementation Analysis

### 2.1 CRITICAL ERROR #1: Leak Rate Formula

**LOCATION**: `node/z1_snn_engine.c`, line 398-399

```c
// CURRENT (WRONG)
neuron->membrane_potential *= (1.0f - neuron->leak_rate);
```

**PROBLEM**: This inverts the leak rate semantics!

**Example with leak_rate=0.95**:
- **Current code**: `V_mem *= (1.0 - 0.95) = V_mem * 0.05` (keeps 5%, loses 95%) ❌
- **Correct code**: `V_mem *= 0.95` (keeps 95%, loses 5%) ✅

**IMPACT**: 
- Neurons decay **20× faster** than intended
- Membrane potentials collapse to zero within a few timesteps
- Network cannot maintain temporal integration
- XOR SNN fails because neuron activity decays before decision neurons can integrate

### 2.2 CRITICAL ERROR #2: Threshold Check Location

**LOCATION**: `node/z1_snn_engine.c`, line 405-409

```c
// CURRENT (WRONG)
// Check threshold AFTER leak (critical for proper firing)
// ALL neurons can fire, including input neurons
if (neuron->membrane_potential >= neuron->threshold) {
    if (g_engine.current_time_us >= neuron->refractory_until_us) {
        fire_neuron(neuron);
    }
}
```

**PROBLEM**: Threshold is checked ONLY during leak step, not during spike processing!

**Correct behavior** (from Python):
```python
# Check threshold IMMEDIATELY after each synaptic integration
neuron.membrane_potential += synapse.weight * spike.value
if neuron.membrane_potential >= neuron.threshold:
    self._generate_spike(neuron)
    break
```

**IMPACT**:
- Neurons can accumulate membrane potential above threshold without firing
- Firing is delayed by up to 1 full timestep (1ms)
- Spike timing is incorrect, breaking temporal coding
- Fast transient responses are lost

### 2.3 CRITICAL ERROR #3: Broadcast Loopback Logic

**LOCATION**: `node/z1_snn_engine.c`, line 195-203

```c
// CURRENT (PARTIALLY WRONG)
if (source_node == g_engine.node_id) {
    // This is a broadcast loopback - spike originated from THIS node
    // Skip entire spike processing to prevent infinite loops
    return;  // EXIT EARLY
}
```

**PROBLEM**: This logic is CORRECT for preventing loops, but the Python implementation shows a more nuanced approach:

```python
# From snn_engine_stdp.py
local_neuron_globals = {n.global_id for n in self.neurons.values()}
if source_id in local_neuron_globals:
    # Skip - this spike is from a local neuron, already processed
    return
```

**Better approach**: Check if spike's global ID matches ANY local neuron's global ID, not just node_id.

**IMPACT**: Minor - current approach works but is less precise

### 2.4 ERROR #4: Spike Injection Logic

**LOCATION**: `node/z1_snn_engine.c`, line 415-421

```c
bool z1_snn_inject_spike(z1_spike_t spike) {
    g_engine.stats.spikes_received++;
    return spike_queue_push(spike);
}
```

**PROBLEM**: Spike is queued but not processed until next `z1_snn_step()` call!

**Correct behavior** (from Python):
```python
def inject_spike(self, neuron_id: int, value: float = 1.0):
    neuron.membrane_potential += value
    if neuron.membrane_potential >= neuron.threshold:
        self._generate_spike(neuron)  # IMMEDIATE firing
```

**IMPACT**:
- Input spikes have 1 timestep delay before being processed
- Network response is slower than intended
- Synchronous input patterns are desynchronized

### 2.5 Design Choice: Input Neuron Handling

**OBSERVATION**: Python implementation treats input neurons identically to hidden neurons:

```python
# NO special case for input neurons - all neurons processed identically
for neuron in self.neurons.values():
    if neuron.membrane_potential > 0:
        neuron.membrane_potential *= neuron.leak_rate
```

**Current C implementation**: Sets `leak_rate = 0.0` for input neurons (from topology file)

**Verdict**: This is ACCEPTABLE as long as:
1. Topology file correctly sets `leak_rate = 1.0` for input neurons (no leak)
2. Input neurons still check threshold and fire normally
3. No special-case logic prevents input neurons from firing

---

## Part 3: XOR SNN Failure Root Cause

### 3.1 XOR Network Topology

```
INPUT[0,1] -> HIDDEN[2,3] -> OUTPUT[4]
```

**Expected behavior**:
1. Inject spikes into neurons 0 and 1 (inputs)
2. Spikes propagate to neurons 2 and 3 (hidden)
3. Hidden neurons integrate inputs and fire
4. Output neuron 4 receives spikes from hidden layer
5. Output neuron fires if input pattern is XOR-valid

### 3.2 Why Current Implementation Fails

**Step-by-step failure analysis**:

1. **T=0ms**: Inject spike into neuron 0 (value=1.0)
   - `neuron[0].membrane_potential = 1.0`
   - Threshold = 1.0, so neuron 0 should fire IMMEDIATELY
   - ❌ **ERROR**: Spike is queued but not processed until next step

2. **T=1ms**: Process step 1
   - Process queued spike from neuron 0
   - Applies synaptic weights to neurons 2 and 3
   - `neuron[2].membrane_potential += weight[0→2]` (e.g., 0.8)
   - `neuron[3].membrane_potential += weight[0→3]` (e.g., 0.8)
   - ❌ **ERROR**: No threshold check after integration
   - Apply leak: `neuron[2].V_mem *= (1.0 - 0.95) = 0.8 * 0.05 = 0.04` ❌ ❌ ❌
   - **CATASTROPHIC**: Membrane potential collapses from 0.8 to 0.04!

3. **T=2ms**: Network is effectively dead
   - All membrane potentials < 0.05
   - No neurons can reach threshold
   - No spikes generated

**Root cause**: The `(1.0 - leak_rate)` error causes membrane potentials to decay by 95% instead of 5%, killing all network activity within 2-3 timesteps.

---

## Part 4: Required Fixes (TODO List)

### 🔴 PRIORITY 1: CRITICAL FIXES (Required for basic functionality)

#### ✅ TODO 1.1: Fix Leak Rate Formula

**File**: `node/z1_snn_engine.c`, line 398-399

**Current code**:
```c
neuron->membrane_potential *= (1.0f - neuron->leak_rate);
```

**Required change**:
```c
neuron->membrane_potential *= neuron->leak_rate;
```

**Rationale**: `leak_rate` is the retention factor, not the loss factor. This is **THE** primary bug.

**Verification**: After fix, neuron with `leak_rate=0.95` should decay by 5% per timestep, not 95%.

---

#### ✅ TODO 1.2: Add Threshold Check After Synaptic Integration

**File**: `node/z1_snn_engine.c`, function `process_spike()`, after line 215

**Current code**:
```c
neuron->membrane_potential += delta_v;
g_engine.stats.spikes_processed++;
g_engine.stats.membrane_updates++;
```

**Required addition**:
```c
neuron->membrane_potential += delta_v;
g_engine.stats.spikes_processed++;
g_engine.stats.membrane_updates++;

// CRITICAL: Check threshold IMMEDIATELY after integration
if (neuron->membrane_potential >= neuron->threshold) {
    // Check refractory period
    if (g_engine.current_time_us >= neuron->refractory_until_us) {
        fire_neuron(neuron);
    }
    break;  // Stop processing remaining synapses for this neuron
}
```

**Rationale**: Python implementation fires neurons immediately after integration, not waiting for leak step.

**Verification**: Neurons should fire within the same timestep as spike reception, not 1 timestep later.

---

#### ✅ TODO 1.3: Fix Input Spike Injection to Process Immediately

**File**: `node/z1_snn_engine.c`, function `z1_snn_inject_spike()`

**Current code**:
```c
bool z1_snn_inject_spike(z1_spike_t spike) {
    g_engine.stats.spikes_received++;
    return spike_queue_push(spike);
}
```

**Required change**: Add immediate processing option for input spikes:

```c
bool z1_snn_inject_spike_immediate(uint16_t local_neuron_id, float value) {
    if (local_neuron_id >= g_engine.neuron_count) {
        return false;
    }
    
    z1_neuron_t* neuron = &g_engine.neurons[local_neuron_id];
    
    // Add value to membrane potential
    neuron->membrane_potential += value;
    g_engine.stats.spikes_received++;
    g_engine.stats.membrane_updates++;
    
    // Check threshold IMMEDIATELY
    if (neuron->membrane_potential >= neuron->threshold) {
        if (g_engine.current_time_us >= neuron->refractory_until_us) {
            fire_neuron(neuron);
        }
    }
    
    return true;
}

// Keep existing function for queued spikes from network
bool z1_snn_inject_spike(z1_spike_t spike) {
    g_engine.stats.spikes_received++;
    return spike_queue_push(spike);
}
```

**Rationale**: Input neurons should respond immediately to injected spikes, matching Python behavior.

**Verification**: Input neuron with threshold=1.0 should fire immediately when injected with value=1.0.

---

### 🟡 PRIORITY 2: IMPORTANT FIXES (Improves correctness)

#### ✅ TODO 2.1: Remove Threshold Check from Leak Step

**File**: `node/z1_snn_engine.c`, lines 403-409

**Current code**:
```c
// Check threshold AFTER leak (critical for proper firing)
// ALL neurons can fire, including input neurons
if (neuron->membrane_potential >= neuron->threshold) {
    // Check refractory period
    if (g_engine.current_time_us >= neuron->refractory_until_us) {
        fire_neuron(neuron);
    }
}
```

**Required change**: Keep this code, but update comment:

```c
// Check threshold after leak for any remaining activity
// This catches neurons that accumulated potential but didn't fire during spike processing
// (This is a secondary check - most firing should happen during spike processing)
if (neuron->membrane_potential >= neuron->threshold) {
    if (g_engine.current_time_us >= neuron->refractory_until_us) {
        fire_neuron(neuron);
    }
}
```

**Rationale**: This check is still useful for neurons that accumulate potential over multiple timesteps without receiving spikes. It's not wrong, just secondary to spike-time threshold checks.

---

#### ✅ TODO 2.2: Update Topology Validator for Leak Rate

**File**: `python_tools/lib/snn_compiler.py` (or equivalent)

**Required change**: Ensure topology files use correct leak rate semantics:

```python
# Validate leak_rate is in range [0.0, 1.0]
if not 0.0 <= neuron['leak_rate'] <= 1.0:
    raise ValueError(f"leak_rate must be in [0.0, 1.0], got {neuron['leak_rate']}")

# Warn if leak_rate is suspiciously low (suggests old semantics)
if neuron['leak_rate'] < 0.5 and neuron['layer_type'] != 'input':
    print(f"WARNING: Neuron {neuron['id']} has leak_rate={neuron['leak_rate']}, "
          f"which means it retains only {neuron['leak_rate']*100}% per timestep. "
          f"Did you mean {1.0 - neuron['leak_rate']:.2f}?")
```

**Rationale**: Prevent confusion between old and new leak rate semantics.

---

#### ✅ TODO 2.3: Add Unit Tests for Core Behaviors

**File**: New file `node/test_snn_engine.c` (or Python test harness)

**Required tests**:

```python
def test_leak_rate_decay():
    """Test that leak_rate=0.95 decays by 5%, not 95%"""
    neuron.membrane_potential = 1.0
    neuron.leak_rate = 0.95
    apply_leak(neuron, 1000)  # 1ms
    assert abs(neuron.membrane_potential - 0.95) < 0.001
    
def test_immediate_threshold_check():
    """Test that neuron fires immediately after integration"""
    neuron.membrane_potential = 0.5
    neuron.threshold = 1.0
    process_spike(spike_with_weight_0_5)  # Should fire now
    assert neuron.membrane_potential == 0.0  # Reset after firing
    assert spike_was_generated()
    
def test_input_spike_immediate():
    """Test that input spikes fire immediately"""
    inject_spike(neuron_id=0, value=1.0)
    assert neuron[0].spike_count == 1  # Fired immediately
    assert len(get_output_spikes()) == 1
```

**Rationale**: Prevent regressions and validate fixes.

---

### 🟢 PRIORITY 3: ENHANCEMENTS (Improves performance/clarity)

#### ✅ TODO 3.1: Add Early Exit After Neuron Fires

**File**: `node/z1_snn_engine.c`, function `process_spike()`, after firing neuron

**Current code**: Continues processing remaining synapses even after neuron fires

**Required change**: Add `break` after firing:

```c
if (neuron->membrane_potential >= neuron->threshold) {
    if (g_engine.current_time_us >= neuron->refractory_until_us) {
        fire_neuron(neuron);
    }
    break;  // Stop processing remaining synapses for this neuron
}
```

**Rationale**: Matches Python behavior, saves CPU cycles, prevents over-integration.

---

#### ✅ TODO 3.2: Improve Debug Output

**File**: `node/z1_snn_engine.c`, various locations

**Required changes**:

```c
#define DEBUG_LEAK 1  // Add leak debugging
#define DEBUG_THRESHOLD 1  // Add threshold check debugging

#if DEBUG_LEAK
printf("[SNN-%u] Neuron %u: V_mem %.3f -> %.3f (leak_rate=%.3f)\n",
       node_id, neuron->neuron_id, old_vmem, neuron->membrane_potential, neuron->leak_rate);
#endif

#if DEBUG_THRESHOLD
printf("[SNN-%u] Neuron %u: V_mem=%.3f %s threshold=%.3f\n",
       node_id, neuron->neuron_id, neuron->membrane_potential,
       (neuron->membrane_potential >= neuron->threshold) ? ">=" : "<",
       neuron->threshold);
#endif
```

**Rationale**: Easier debugging of integration and firing behavior.

---

#### ✅ TODO 3.3: Document Leak Rate Semantics

**File**: `node/z1_snn_engine.h`, header comment

**Required addition**:

```c
/**
 * LEAK RATE SEMANTICS:
 * 
 * leak_rate is the RETENTION FACTOR, not the loss factor.
 * 
 * Formula: V_mem(t+1) = V_mem(t) * leak_rate
 * 
 * Examples:
 * - leak_rate = 0.95: Retain 95%, lose 5% per timestep
 * - leak_rate = 0.80: Retain 80%, lose 20% per timestep
 * - leak_rate = 1.00: No leak (retain 100%)
 * - leak_rate = 0.00: Total leak (retain 0%)
 * 
 * Typical values: 0.90 - 0.99 for realistic neural decay
 * 
 * DO NOT use (1.0 - leak_rate) in the decay formula!
 */
```

---

## Part 5: Testing Strategy

### 5.1 Unit Tests (Per-Function Validation)

**Test 1: Leak Rate**
```python
def test_leak_rate():
    neuron = create_neuron(leak_rate=0.95)
    neuron.membrane_potential = 1.0
    
    for i in range(10):
        apply_leak(neuron)
        expected = 0.95 ** (i + 1)
        assert abs(neuron.membrane_potential - expected) < 0.001
```

**Test 2: Immediate Firing**
```python
def test_immediate_firing():
    neuron = create_neuron(threshold=1.0)
    neuron.membrane_potential = 0.5
    
    spike = create_spike(weight=0.6)
    process_spike(spike, neuron)
    
    # Should fire immediately (0.5 + 0.6 = 1.1 >= 1.0)
    assert neuron.membrane_potential == 0.0  # Reset
    assert neuron.spike_count == 1
```

**Test 3: Input Injection**
```python
def test_input_injection():
    neuron = create_neuron(threshold=1.0, is_input=True)
    
    inject_spike(neuron.id, value=1.0)
    
    # Should fire immediately
    assert neuron.spike_count == 1
    assert len(get_output_spikes()) == 1
```

### 5.2 Integration Tests (Multi-Neuron Behavior)

**Test 4: XOR Network**
```python
def test_xor_network():
    deploy_topology('xor_working.json')
    
    # Test case: (1, 0) should produce output
    inject_spike(neuron_id=0, value=1.0)  # Input A
    inject_spike(neuron_id=1, value=0.0)  # Input B
    
    for _ in range(10):
        step()
    
    # Verify output neuron fired
    assert get_neuron_spike_count(neuron_id=4) > 0
```

### 5.3 System Tests (Full Cluster)

**Test 5: Multi-Node XOR**
```python
def test_multi_node_xor():
    # Deploy XOR across 2 nodes
    deploy_topology('xor_working.json')
    
    # All test cases
    test_cases = [
        ((0, 0), 0),  # 0 XOR 0 = 0
        ((0, 1), 1),  # 0 XOR 1 = 1
        ((1, 0), 1),  # 1 XOR 0 = 1
        ((1, 1), 0),  # 1 XOR 1 = 0
    ]
    
    for (a, b), expected in test_cases:
        reset_network()
        inject_spike(0, value=float(a))
        inject_spike(1, value=float(b))
        
        for _ in range(20):
            step()
        
        output_spikes = get_neuron_spike_count(4)
        assert (output_spikes > 0) == (expected == 1)
```

---

## Part 6: Migration Path

### Phase 1: Apply Priority 1 Fixes (CRITICAL)

1. **Fix leak rate formula** (TODO 1.1)
   - Change `(1.0 - leak_rate)` to `leak_rate`
   - **Estimated time**: 5 minutes
   - **Risk**: LOW - Simple arithmetic fix

2. **Add threshold check after integration** (TODO 1.2)
   - Insert immediate threshold check in `process_spike()`
   - **Estimated time**: 15 minutes
   - **Risk**: MEDIUM - Changes firing logic

3. **Add immediate input injection** (TODO 1.3)
   - Create `z1_snn_inject_spike_immediate()` function
   - Update controller to use immediate injection for input neurons
   - **Estimated time**: 30 minutes
   - **Risk**: MEDIUM - New function, needs testing

**Total Phase 1 Time**: ~1 hour  
**Expected Result**: XOR SNN should START WORKING

### Phase 2: Validate and Test

1. **Run unit tests** (TODO 2.3)
   - Create basic test harness
   - Validate leak, firing, injection
   - **Estimated time**: 2 hours
   - **Risk**: LOW

2. **Run XOR test** (`test_deployment.py`)
   - Deploy XOR topology
   - Inject test patterns
   - Verify outputs
   - **Estimated time**: 30 minutes
   - **Risk**: LOW

**Total Phase 2 Time**: ~2.5 hours  
**Expected Result**: XOR SNN CONFIRMED WORKING

### Phase 3: Apply Priority 2/3 Fixes (POLISH)

1. **Update comments** (TODO 2.1, 3.3)
2. **Add debug output** (TODO 3.2)
3. **Add topology validator** (TODO 2.2)
4. **Add early exit optimization** (TODO 3.1)

**Total Phase 3 Time**: ~1 hour  
**Expected Result**: Clean, documented, optimized code

---

## Part 7: Risk Analysis

### High-Risk Changes

1. **Changing leak formula**: Could break existing (non-XOR) networks if they compensated for the bug
   - **Mitigation**: Test with multiple topologies, not just XOR
   
2. **Immediate threshold check**: Changes spike timing by 1 timestep
   - **Mitigation**: This is the CORRECT behavior per Python implementation

### Low-Risk Changes

1. **Adding immediate injection function**: New function doesn't affect existing code
2. **Debug output**: No functional impact
3. **Documentation**: No code impact

---

## Part 8: Success Criteria

### Minimal Success (Phase 1 Complete)

- [ ] Leak rate formula fixed: `V_mem *= leak_rate` (not `1.0 - leak_rate`)
- [ ] Neurons fire immediately after integration (within same timestep)
- [ ] Input spikes processed immediately (no 1-timestep delay)
- [ ] XOR SNN test: 5/9 tests pass (basic connectivity)

### Full Success (Phase 2 Complete)

- [ ] All unit tests pass
- [ ] XOR SNN test: 9/9 tests pass
- [ ] Spike timing matches Python emulator (±1ms tolerance)
- [ ] Network sustains activity for 10+ timesteps

### Excellent Success (Phase 3 Complete)

- [ ] Code is well-documented with clear leak rate semantics
- [ ] Debug output helps diagnose future issues
- [ ] Topology validator prevents incorrect configurations
- [ ] Performance optimized with early exits

---

## Part 9: Comparison Table

| Feature | Python (Correct) | Our C (Current) | Fix Priority |
|---------|------------------|-----------------|--------------|
| **Leak Formula** | `V *= leak_rate` | `V *= (1.0 - leak_rate)` ❌ | 🔴 P1 |
| **Threshold Check Location** | After each integration | Only during leak step ❌ | 🔴 P1 |
| **Input Spike Processing** | Immediate | Queued (1 timestep delay) ❌ | 🔴 P1 |
| **Refractory Period** | Before integration | Before firing ✅ | ✅ OK |
| **Membrane Reset** | 0.0 | 0.0 ✅ | ✅ OK |
| **Spike ID Encoding** | Global ID | Global ID ✅ | ✅ OK |
| **Loopback Prevention** | Global ID check | Node ID check ⚠️ | 🟢 P3 |
| **Early Exit After Fire** | Yes | No ⚠️ | 🟢 P3 |

---

## Part 10: References

### Source Code References

**Working Python Implementation**:
- `emulator/core/snn_engine.py` - Core LIF neuron logic
- `python_tools/lib/snn_engine.py` - Simplified version
- Lines 180-253 - Simulation step and spike processing

**Our Current C Implementation**:
- `node/z1_snn_engine.c` - SNN engine implementation
- Lines 390-410 - Leak and threshold check (ERROR LOCATION)
- Lines 183-232 - Spike processing (MISSING THRESHOLD CHECK)

### Documentation References

- `docs/ARCHITECTURE.md` - Original SNN design intent
- `docs/CODE_WALKTHROUGH.md` - Algorithm explanation
- `README.md` - Neuron model specification

---

## Conclusion

Our SNN engine has **THREE CRITICAL BUGS** that prevent proper operation:

1. **Inverted leak rate formula**: Causes 95% decay instead of 5%, killing network activity
2. **Missing immediate threshold check**: Delays firing by 1 timestep, breaking temporal coding
3. **Delayed input injection**: Input spikes queued instead of processed immediately

These bugs stem from **misunderstanding the Python reference implementation**. The working Python code provides a clear specification that we must follow.

**Estimated total fix time**: 4-5 hours (including testing)  
**Expected outcome**: XOR SNN will work correctly after Priority 1 fixes

**RECOMMENDATION**: Apply Priority 1 fixes immediately, then validate with XOR test before proceeding to Priority 2/3.
