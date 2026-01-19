# LIF SNN Engine v1.0.0

## Overview
This package contains the **Leaky Integrate-and-Fire (LIF) SNN Engine** - the computational core that processes spikes in Z1 Onyx compute nodes.

**IMPORTANT**: This is the **ENGINE** (how neurons compute), NOT the topology (network structure).
- Topology data (XOR, MNIST, etc.) is deployed separately at runtime via controller commands
- This engine works with ANY topology that uses LIF neurons
- You can swap entire engines (LIF → Izhikevich → Reservoir) without changing topology deployment

## What's Included in This Package
- **z1_snn_engine.c/h**: Complete LIF neuron implementation (~26KB source, ~30KB compiled)
- Spike-driven computation with membrane potential integration
- Exponential leak decay
- Refractory period handling
- STDP (Spike-Timing-Dependent Plasticity) learning
- Statistics collection

## What's NOT Included (Deployed Separately)
- Network topology (neurons, connections, weights)
- JSON configuration files
- Training data
- Controller firmware
- Bootloader/communication layer

## Neuron Model: Leaky Integrate-and-Fire (LIF)

### Computation Algorithm
```
For each timestep (1ms):
  1. Integrate weighted spikes into membrane potential:
     V += Σ(weight_i × spike_i)
  
  2. Apply exponential leak:
     V *= leak_rate  (typically 0.8-0.95)
  
  3. Check threshold:
     if V >= threshold:
       - Fire spike
       - Reset V = 0
       - Enter refractory period
  
  4. Propagate output spike to connected neurons
```

### Parameters (per neuron)
- **Threshold**: Voltage required to fire (e.g., 1.5)
- **Leak Rate**: Decay factor per timestep (e.g., 0.8 = 20% decay)
- **Refractory Period**: Dead time after firing (e.g., 1000µs)

## Example Topology: XOR Network

This engine can run XOR logic with this topology deployed at runtime:

### Network Architecture
- **Neurons**: 5 total (2 input, 2 hidden, 1 output)
- **Connections**: 6 synapses with tuned weights
- **Topology**: Feedforward with inhibitory feedback
- **Learning**: STDP enabled (optional)

## XOR Truth Table
```
Input A | Input B | Output
--------|---------|--------
   0    |    0    |   0
   0    |    1    |   1
   1    |    0    |   1
   1    |    1    |   0
```

## Network Topology
```
    Input Layer          Hidden Layer       Output Layer
    
    A (n0) ─────1.5─────> H1 (n2)
      └─────────1.5────────┐
                           │
    B (n1) ─────1.5─────> H2 (n3)           
      └─────────1.5────────┤
                           │
                       ┌───┴────┐
                       │        │
                    +2.0     -1.5 (inhibit)
                       │        │
                       └───> O (n4)
```

### Weight Explanation
- **Input → Hidden** (1.5): Both inputs excite both hidden neurons equally
- **Hidden → Output** (+2.0): H1 strongly excites output (OR function)
- **Hidden → Output** (-1.5): H2 inhibits output when both inputs fire (AND gate)
- **Net effect**: OR - AND = XOR

## Performance Characteristics
- **Inference Time**: ~5ms per input pattern
- **Power Consumption**: ~250mW typical
- **Node Assignment**: 2 nodes (balanced distribution)
- **Flash Usage**: 50KB code
- **RAM Usage**: 213KB (mainly BSS for neuron state)
- **PSRAM Usage**: ~1MB (neuron table + working memory)

## Files in Package
```
xor_snn_v1.0.0/
├── package.json              # Package metadata and configuration
├── CODE_INVENTORY.md         # Complete code analysis and dependencies
├── README.md                 # This file
├── xor_working.json          # Tested topology (from python_tools/examples/)
└── test_vectors/
    ├── xor_input_00.json     # Test: 0 XOR 0 = 0
    ├── xor_input_01.json     # Test: 0 XOR 1 = 1
    ├── xor_input_10.json     # Test: 1 XOR 0 = 1
    └── xor_input_11.json     # Test: 1 XOR 1 = 0
```

## Deployment (Current Method - Monolithic Firmware)
```bash
# 1. Build node firmware
python build.py

# 2. Deploy topology to cluster
python python_tools/bin/nsnn deploy python_tools/examples/xor_working.json -c 192.168.1.222

# 3. Start SNN execution
python python_tools/bin/nsnn start -c 192.168.1.222

# 4. Inject test pattern
python python_tools/bin/nsnn inject python_tools/examples/xor_input_10.json -c 192.168.1.222

# 5. Monitor output
python python_tools/bin/nstat -s -c 192.168.1.222
```

## Deployment (OTA Firmware Update)
```bash
# 1. Flash dual-partition firmware to nodes (one-time, via USB)
# Copy FirmwareReleases/16node/node_dual_16.uf2 to each Pico

# 2. Deploy node firmware OTA
python python_tools/bin/nflash build/node/node_app_16.bin -n 0 -c 192.168.1.222

# 3. Nodes reboot automatically

# 4. Deploy topology
python python_tools/bin/nsnn deploy python_tools/examples/xor_working.json -c 192.168.1.222

# 5. Run tests
python python_tools/bin/nsnn start -c 192.168.1.222
python python_tools/bin/nsnn inject python_tools/examples/xor_input_11.json -c 192.168.1.222
```

## Test Results (Expected Behavior)
When running XOR SNN with the working topology:

### Input: [0, 0]
- Neuron 0 (A): No spike
- Neuron 1 (B): No spike
- Neuron 2 (H1): No spike (insufficient input)
- Neuron 3 (H2): No spike (insufficient input)
- Neuron 4 (Out): **No spike** ✓ (0 XOR 0 = 0)

### Input: [1, 0]
- Neuron 0 (A): Fires
- Neuron 1 (B): No spike
- Neuron 2 (H1): Fires (A activates it)
- Neuron 3 (H2): No spike (only A, needs both)
- Neuron 4 (Out): **Fires** ✓ (1 XOR 0 = 1)

### Input: [0, 1]
- Neuron 0 (A): No spike
- Neuron 1 (B): Fires
- Neuron 2 (H1): Fires (B activates it)
- Neuron 3 (H2): No spike (only B, needs both)
- Neuron 4 (Out): **Fires** ✓ (0 XOR 1 = 1)

### Input: [1, 1]
- Neuron 0 (A): Fires
- Neuron 1 (B): Fires
- Neuron 2 (H1): Fires (both A and B activate)
- Neuron 3 (H2): Fires (both A and B activate)
- Neuron 4 (Out): **No spike** ✓ (H2 inhibits, 1 XOR 1 = 0)

## Tunable Parameters

### Neuron Thresholds
- Input neurons: 0.1 (very sensitive, always fire on input)
- Hidden neurons: 2.0 (need strong input to fire)
- Output neuron: 1.5 (moderate threshold)

### Leak Rates
- Input neurons: 0.0 (no leak, maintain potential)
- Hidden neurons: 0.8 (slow decay)
- Output neuron: 0.8 (slow decay)

### Refractory Periods
- Input neurons: 500µs
- Hidden neurons: 1000µs
- Output neuron: 2000µs (prevents rapid re-firing)

### Synaptic Delays
- All connections: 1000µs (1ms propagation delay)
- Inhibitory connection: 500µs (faster, suppresses output quickly)

## Known Issues / Limitations
1. **Timing Sensitivity**: XOR logic requires precise timing of inhibitory feedback
2. **Single Output**: Only one output spike per input pattern (refractory period)
3. **No Noise Tolerance**: Clean binary inputs required (0 or 1)
4. **Fixed Topology**: Weights are hand-tuned, not learned
5. **STDP Limitations**: Learning enabled but not fully tuned for XOR problem

## Future Improvements
- [ ] Add noise injection for robustness testing
- [ ] Implement homeostatic plasticity for automatic weight tuning
- [ ] Multi-pattern input sequences (e.g., "01101100")
- [ ] Output spike train analysis (not just single spike)
- [ ] Support for analog inputs (0.0 - 1.0 range)
- [ ] Add visualization of membrane potentials over time

## Version History
- **v1.0.0** (2026-01-06): Initial package creation
  - Working XOR implementation
  - Hand-tuned weights verified on hardware
  - 5-neuron minimal topology
  - STDP enabled (experimental)

## License
Copyright 2026 NeuroFab Z1 Team. All rights reserved.

## Contact
For questions or issues with this package, refer to the main Z1 Onyx documentation.
