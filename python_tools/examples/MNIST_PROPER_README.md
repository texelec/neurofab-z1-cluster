# Proper MNIST SNN Example for NeuroFab Z1

This is a **legitimate neuromorphic computing example** that demonstrates proper integration with the Z1 emulator and tools.

## What Makes This "Proper"?

Unlike standalone NumPy simulations, this example:

✅ **Uses the Z1 emulator** - Connects to the actual emulator  
✅ **Uses the SNN compiler** - Compiles networks to binary neuron tables  
✅ **Deploys via Z1Client** - Uses the real deployment pipeline  
✅ **Injects spikes via API** - Sends input as spikes, not arrays  
✅ **Reads spike events** - Gets output from actual SNN execution  
✅ **Uses real SNN dynamics** - LIF neurons, leak, refractory periods  

## Architecture

**Network:** 894 neurons across 3 layers
- **Input:** 784 neurons (28×28 pixels)
- **Hidden:** 100 neurons (feature extraction)
- **Output:** 10 neurons (digits 0-9)

**Deployment:** Distributed across 16 nodes

## Files

- `mnist_proper.json` - Network topology definition
- `/home/ubuntu/mnist_proper_example.py` - Main example script

## Usage

### 1. Start the Emulator

```bash
cd /home/ubuntu/neurofab_system/emulator
python3 z1_emulator.py
```

### 2. Run the Example

```bash
cd /home/ubuntu
python3 mnist_proper_example.py
```

## Expected Output

```
============================================================
PROPER MNIST SNN EXAMPLE - NeuroFab Z1
============================================================
✓ Emulator is running
Created 5 digit patterns (28x28 = 784 pixels each)

1. DEPLOYING NETWORK
--------------------------------------------------
Loading network topology: mnist_proper.json
Compiling network...
Network compiled: 894 neurons
Deploying to emulator...
  Deployed to node 0 (backplane default)
  Deployed to node 1 (backplane default)
  ...
✓ Network deployed successfully to 16 nodes

2. TESTING INFERENCE
--------------------------------------------------
✓ SNN started
Testing digit 0:
  Injecting digit 0: 240 active neurons
  ...
```

## Current Status

**Integration:** ✅ Complete  
**Accuracy:** ⚠️ 0% (expected - no training yet)

The 0% accuracy is expected because:
1. No connections between layers (connections list is empty)
2. No training has occurred
3. This is a demonstration of proper integration

## Next Steps

To achieve actual MNIST recognition:

1. **Add connections** - Define synapses between layers
2. **Implement STDP training** - Use the emulator's STDP support
3. **Load real MNIST data** - Use actual MNIST dataset
4. **Tune parameters** - Adjust thresholds, weights, leak rates

## Comparison to Improper Examples

| Aspect | Improper Example | This Example |
|--------|------------------|--------------|
| Emulator | ❌ Not used | ✅ Used |
| SNN Compiler | ❌ Not used | ✅ Used |
| Deployment | ❌ NumPy only | ✅ Real deployment |
| Spike injection | ❌ Matrix mult | ✅ Real spikes |
| SNN dynamics | ❌ Fake | ✅ Real LIF neurons |

## Code Walkthrough

### 1. Initialize and Connect

```python
from z1_client import Z1Client
from snn_compiler import SNNCompiler

# Connect to emulator
client = Z1Client(controller_ip='127.0.0.1', port=8000)
```

### 2. Compile Network

```python
# Load topology
with open('mnist_proper.json', 'r') as f:
    topology = json.load(f)

# Compile
compiler = SNNCompiler(topology)
deployment_plan = compiler.compile()
```

### 3. Deploy to Emulator

```python
# Deploy neuron tables to each node
for (backplane_id, node_id), neuron_table in deployment_plan.neuron_tables.items():
    client.write_memory(node_id, 0x10000000, neuron_table)
```

### 4. Inject Input Spikes

```python
# Convert image to spikes (rate coding)
for neuron_id in active_neurons:
    requests.post(
        'http://127.0.0.1:8000/api/snn/input',
        json={'neuron_id': neuron_id}
    )
```

### 5. Read Output

```python
# Get spike events
response = requests.get('http://127.0.0.1:8000/api/snn/events')
spikes = response.json()['spikes']

# Find most active output neuron
for event in spikes:
    if 884 <= event['neuron_id'] <= 893:
        digit = event['neuron_id'] - 884
        # This is the predicted digit
```

## Key Takeaways

1. **Proper integration matters** - This is how you actually use Z1
2. **Emulator is essential** - Test before deploying to hardware
3. **SNN dynamics are real** - Not just matrix multiplication
4. **Spikes are the interface** - Input and output are spike events

## References

- See `docs/CODE_WALKTHROUGH.md` for detailed system explanation
- See `examples/xor_4of4.json` for a working trained example
- See `docs/SNN_GUIDE.md` for SNN concepts

---

**This is the template for all future Z1 examples.**
