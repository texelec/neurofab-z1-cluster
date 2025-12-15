# Z1 Onyx Cluster - Technical Reference

---

## System Overview

The Z1 Onyx Cluster is a distributed spiking neural network (SNN) platform based on Raspberry Pi Pico 2 (RP2350) microcontrollers connected via a custom high-speed parallel bus.

**Hardware Variants:**
- **V2 (Current)**: 17 nodes (16 compute + 1 controller), OLED display, global reset
- **V1 (Legacy)**: 13 nodes (12 compute + 1 controller), no OLED, no global reset

This document primarily describes V2 hardware. See [BUILD_INSTRUCTIONS.md](BUILD_INSTRUCTIONS.md) for V1/V2 differences.

### Hardware Specifications (V2)
- **Platform:** Raspberry Pi Pico 2 (RP2350B)
  - 8MB PSRAM (QSPI)
  - 266 MHz overclock
- **Cluster:** 17 nodes on 7.5" backplane
  - 1.2" node spacing
  - 4-layer PCB
- **Bus:** Custom parallel protocol (Matrix bus)
  - PCIe x4 connector (26-pin)
  - DATA[15:0] - 16-bit data bus
  - CLK - 10 MHz bus clock
  - SELECT0/1 - SOP/EOP markers (hardware-level, software-parsed, not fully implemented)
  - CTL[2:0] - Reserved for future use
  - 22Ω series resistors, 1.5" stubs
- **Peripherals (per node):**
  - RGB LEDs (GPIO44/45/46)
  - Hardware node ID (GPIO40-43, 4-bit auto-detect on V2, hardcoded on V1)

### Project Structure
```
Z1_Onyx/
├── controller/          # Controller firmware (V2: Node 16, V1: Node 12)
├── node/                # Node firmware (V2: auto-detect 0-15, V1: hardcoded 0-11)
├── common/              # Shared libraries
│   ├── z1_onyx_bus/     # Matrix bus protocol
│   ├── z1_broker/       # Message broker
│   ├── psram/           # PSRAM driver
│   └── oled/            # OLED display driver
├── python_tools/        # Python management utilities
│   ├── bin/             # CLI tools (nls, nstat, nsnn, etc.)
│   └── lib/             # Python libraries
├── FirmwareReleases/    # Pre-built UF2 files
├── test_deployment.py   # Automated test suite
├── API_REFERENCE.md     # This file
└── PIN_REFERENCE.md     # Hardware pin assignments
```

---

## Matrix Bus Protocol

### Performance Metrics
| Metric | Value | Notes |
|--------|-------|-------|
| Clock Speed | 10 MHz | 100ns per beat |
| TX Throughput | 10.31 MB/s | Measured over 2000 frames |
| RX Throughput | 8.07 MB/s | Multi-node validated |
| Frame Size | Max 387 beats | Header + Length + 384 payload + CRC |
| CRC Errors | 0 | Over 2000+ frames tested |
| ACK Round-Trip | ~200μs | Controller ↔ Node |
| Reliability | 100% | Zero frame drops in testing |

### Frame Format
```
[Header:16] [Length:16] [Payload:N×16] [CRC16:16]

Header bits:
  [15:14] Type (UNICAST=0, BROADCAST=1, ACK=2, CTRL=3)
  [13:9]  Source Node ID (0-31)
  [8:4]   Dest Node ID (0-31, broadcast=31)
  [3]     NO_ACK flag (0=require ACK, 1=fire-and-forget)
  [2:0]   Stream ID (0-7 for priority/filtering)
```

### Frame Types
- **UNICAST (Type 0):** Point-to-point with ACK (default)
- **BROADCAST (Type 1):** All nodes receive, no ACK
- **ACK (Type 2):** ACK response frame
- **CTRL (Type 3):** High-priority commands

### CRC16-CCITT
- Polynomial: 0x1021
- Hardware-accelerated via DMA sniffer (10× faster than software)
- 100% validation over 200000+ frames

### Collision Avoidance (CSMA/CD)
- Controller: 10μs backoff between retries
- Nodes: 20-310μs priority-based backoff (node_id × 10μs + 20μs base)
- Automatic retry on collision (3 attempts with ACK timeout)

---

## SNN Engine

### Neuron Model (Leaky Integrate-and-Fire)
```c
V_mem += (spike_value × weight) - (V_mem × leak_factor)
if (V_mem >= threshold) {
    fire_spike(neuron_id, V_mem);
    V_mem = 0;  // Reset after firing
}
```

### Weight Encoding

Synaptic weights are encoded as 8-bit integers for efficient storage and transmission.

**Python Encoder (snn_compiler.py):**
```python
def encode_weight(weight_float):
    """Encode float weight (-2.0 to +2.0) as 8-bit integer."""
    if weight_float < 0:
        # Negative: -2.0 to -0.01 → 255 to 128
        return max(128, min(255, 128 + int(abs(weight_float) * 63.5)))
    else:
        # Positive: 0.0 to +2.0 → 0 to 127
        return min(127, int(weight_float * 63.5))
```

**C Decoder (z1_snn_engine.c):**
```c
float decode_weight(uint8_t byte) {
    if (byte >= 128) {
        // Negative: 128-255 → -0.01 to -2.0
        return -(byte - 128) / 63.5f;
    } else {
        // Positive: 0-127 → 0.0 to +2.0
        return byte / 63.5f;
    }
}
```

**Examples:**
- `0.0` → `0` → `0.0`
- `1.0` → `64` → `1.0063` (rounding)
- `2.0` → `127` → `2.0`
- `-1.0` → `192` → `-1.0079` (rounding)
- `-2.0` → `255` → `-2.0`

### Neuron Table Format (256 bytes per neuron)

Binary format stored in PSRAM:

```
Offset  Field                    Size    Description
------  -----                    ----    -----------
0-15    Neuron State (16 bytes)
  0       neuron_id               2      Local neuron ID (0-4095)
  2       flags                   2      Status flags (active, input, output, etc.)
  4       membrane_potential      4      Current V_mem (float)
  8       threshold               4      Firing threshold (float)
  12      last_spike_time_us      4      Timestamp of last spike

16-23   Synapse Metadata (8 bytes)
  16      synapse_count           2      Number of incoming synapses
  18      synapse_capacity        2      Max synapses (always 60)
  20      reserved                4      

24-31   Neuron Parameters (8 bytes)
  24      leak_rate               4      Membrane leak (0.0-1.0, float)
  28      refractory_period_us    4      Refractory period (μs)

32-39   Reserved (8 bytes)

40-279  Synapses (60 × 4 bytes = 240 bytes)
  Each synapse is 32-bit packed:
    Bits [31:8]  - Source neuron global ID (node_id << 16 | local_id)
    Bits [7:0]   - Weight (8-bit encoded: 0-127=positive, 128-255=negative)

End marker: neuron_id = 0xFFFF (indicates end of table)
```

### Multi-Node Deployment
- **Sequential distribution:** Neurons assigned to nodes in order
- **Example (XOR, 5 neurons → 2 nodes):**
  - Node 0: Neurons 0, 1, 4 (input + output)
  - Node 1: Neurons 2, 3 (hidden layer)
- **Compiler:** `python_tools/lib/snn_compiler.py`
- **Format:** 256-byte aligned tables in PSRAM (0x00100000 base)

---

## HTTP API

### Controller Endpoints (Default: 192.168.1.222)

All endpoints return JSON responses. The controller must be on the same network and accessible at the configured IP address.

**Note:** The default IP is 192.168.1.222. To change this, edit `controller/w5500_eth.c` and rebuild. See `BUILD_INSTRUCTIONS.md` for details.

#### Node Management

**`GET /api/status`**
- Returns controller health and bus statistics
- Response:
  ```json
  {
    "bus_rx_count": 1234,
    "bus_tx_count": 5678,
    "uptime_ms": 45000
  }
  ```

**`GET /api/nodes`**
- Lists all nodes (0-15) with status information
- Queries each node with READ_STATUS command (100ms timeout per node)
- Response:
  ```json
  {
    "nodes": [
      {
        "id": 0,
        "status": "online",
        "uptime_ms": 45123,
        "memory_free": 8388608,
        "snn_running": true,
        "neuron_count": 3
      },
      {
        "id": 1,
        "status": "online",
        "uptime_ms": 45098,
        "memory_free": 8388608,
        "snn_running": true,
        "neuron_count": 2
      }
    ]
  }
  ```

**`GET /api/nodes/{id}`**
- Get detailed status for specific node (0-15)
- Response: Single node object (see above)
- Returns `{"error": "Timeout"}` if node offline

**`POST /api/nodes/{id}/ping`**
- Test connectivity and measure round-trip latency
- Sends PING command, waits for PONG response
- Response:
  ```json
  {
    "node_id": 0,
    "status": "online",
    "latency_us": 187
  }
  ```

**`POST /api/nodes/discover`**
- Fast discovery of all active nodes
- Pings all nodes in parallel (50ms timeout each)
- V2: Checks nodes 0-15 (16 nodes), V1: Checks nodes 0-11 (12 nodes)
- Response:
  ```json
  {
    "active_nodes": [0, 1, 2, 3]
  }
  ```

#### SNN Control (Global)

**`POST /api/snn/start`**
- Starts SNN execution on ALL nodes (0-15)
- Broadcasts START_SNN command to entire cluster
- Response: `{"status": "ok"}`

**`POST /api/snn/stop`**
- Stops SNN execution on ALL nodes
- Broadcasts STOP_SNN command to entire cluster
- Response: `{"status": "ok"}`

**`GET /api/snn/status`**
- Get cluster-wide SNN statistics
- Queries first responding node for aggregated stats
- Response:
  ```json
  {
    "state": "running",
    "neuron_count": 5,
    "active_neurons": 3,
    "total_spikes": 47,
    "spike_rate_hz": 9.40
  }
  ```

**`POST /api/snn/reset`**
- Resets all neuron spike counters and statistics
- Sends global reset command to all nodes
- Response: `{"status": "reset"}`

#### SNN Control (Per-Node)

**`POST /api/nodes/{id}/snn/start`**
- Start SNN execution on specific node
- Response: `{"status": "ok"}`

**`POST /api/nodes/{id}/snn/stop`**
- Stop SNN execution on specific node
- Response: `{"status": "ok"}`

#### Spike Injection

**`POST /api/snn/input`**
- Inject input spikes into network
- Sends spikes to target nodes via UNICAST frames (no ACK for speed)
- Request body:
  ```json
  {
    "spikes": [
      {"neuron_id": 0, "value": 1.0},
      {"neuron_id": 1, "value": 0.5}
    ]
  }
  ```
  - `neuron_id`: Global neuron ID (encoded as `node_id << 16 | local_id`)
  - `value`: Spike amplitude (currently ignored, always treated as 1.0)
- Response:
  ```json
  {
    "spikes_injected": 2
  }
  ```
- Note: The controller queues all spikes and pumps the broker to transmit them

#### Memory & Deployment

**`POST /api/nodes/{id}/memory`**
- Write raw data to node PSRAM
- Used for deploying neuron tables and configuration
- Request body:
  ```json
  {
    "addr": 1048576,
    "data": "base64_encoded_binary_data_here"
  }
  ```
  - `addr`: PSRAM address (e.g., 0x00100000 = 1048576 for neuron table)
  - `data`: Base64-encoded binary payload (max 1500 bytes decoded)
- Response: `{"status": "ok"}` or `{"error": "..."}`
- Process: Decodes base64 → splits into 384-byte chunks → sends WRITE_MEMORY commands

**`POST /api/nodes/{id}/snn/load`**
- Load neuron topology from PSRAM into active memory
- Triggers node to parse 256-byte neuron entries from PSRAM
- Request body:
  ```json
  {
    "neuron_count": 7
  }
  ```
- Response: `{"status": "loaded", "neuron_count": 7}`
- Must be called after writing neuron table via `/memory` endpoint

**Response Format:**
```json
{
  "state": "running",
  "neuron_count": 5,
  "active_neurons": 3,
  "total_spikes": 47,
  "spike_rate_hz": 9.40
}
```

### API Usage Examples

**Python (using requests):**
```python
import requests

controller = "http://192.168.1.222"  # Default IP (change if customized)

# Discover active nodes
resp = requests.post(f"{controller}/api/nodes/discover")
print(resp.json())  # {"active_nodes": [0, 1]}

# Inject spikes
spikes = {
    "spikes": [
        {"neuron_id": 0, "value": 1.0},
        {"neuron_id": 1, "value": 1.0}
    ]
}
resp = requests.post(f"{controller}/api/snn/input", json=spikes)
print(resp.json())  # {"spikes_injected": 2}

# Check SNN status
resp = requests.get(f"{controller}/api/snn/status")
print(resp.json())
```

**Command Line (curl):**
```bash
# List all nodes (replace IP if you customized it in firmware)
curl http://192.168.1.222/api/nodes

# Start SNN globally
curl -X POST http://192.168.1.222/api/snn/start

# Inject spike
curl -X POST http://192.168.1.222/api/snn/input \
  -H "Content-Type: application/json" \
  -d '{"spikes": [{"neuron_id": 0, "value": 1.0}]}'

# Get statistics
curl http://192.168.1.222/api/snn/status
```

---

## Python Tools

Located in `python_tools/bin/`:

### nls - Node List
```bash
nls                    # List all online nodes
nls                    # Uses default 192.168.1.222
nls -c 192.168.1.100   # Override with custom IP
```

**Output:**
```
NODE  STATUS    MEMORY      UPTIME
--------------------------------------------------
   0  online    8.00 MB            45s
   1  online    8.00 MB            45s
   ...
Total: 16 nodes (V2) or 12 nodes (V1)
```

### nstat - Node Statistics
```bash
nstat              # Show node status
nstat -s           # Show SNN statistics
```

**Output:**
```
SNN Status:
  State:           running
  Neurons:         5
  Active Neurons:  3
  Total Spikes:    47
  Spike Rate:      9.40 Hz
```

### nsnn - SNN Management
```bash
nsnn deploy <topology.json>    # Deploy SNN to cluster
nsnn start                     # Start SNN execution
nsnn stop                      # Stop SNN execution
nsnn status                    # Show SNN status
nsnn inject <spikes.json>      # Inject input spikes
```

**Topology Example (XOR):**
```json
{
  "neurons": [
    {"id": 0, "threshold": 0.1, "leak": 0.0},
    {"id": 1, "threshold": 0.1, "leak": 0.0},
    {"id": 2, "threshold": 2.0, "leak": 0.8},
    {"id": 3, "threshold": 2.0, "leak": 0.8},
    {"id": 4, "threshold": 1.5, "leak": 0.8}
  ],
  "synapses": [
    {"src": 0, "dst": 2, "weight": 1.0, "delay": 1000},
    {"src": 1, "dst": 2, "weight": 1.0, "delay": 1000},
    {"src": 0, "dst": 3, "weight": 1.0, "delay": 1000},
    {"src": 1, "dst": 3, "weight": 1.0, "delay": 1000},
    {"src": 2, "dst": 4, "weight": 1.0, "delay": 2000},
    {"src": 3, "dst": 4, "weight": -1.0, "delay": 2000}
  ]
}
```

### nping - Node Ping
```bash
nping 0            # Ping node 0
nping -a           # Ping all nodes
```

---

## Dual-Core Architecture (Controller Only)

**Core 0: Bus/Broker Engine (Time-Critical)**
- Polls bus at ~1 MHz
- Processes incoming frames → Core 1 RX queue
- Sends outgoing frames ← Core 1 TX queue
- Runs broker collision avoidance (CSMA/CD)
- **NEVER blocks** - maintains 100% bus capture rate

**Core 1: Application Layer**
- HTTP server (W5500 Ethernet)
- OLED display updates (V2 hardware only)
- Command processing
- SNN statistics aggregation
- Can block without affecting bus traffic

**Inter-Core Communication:**
- Lock-free SPSC queues (64 frames × 2KB each)
- Memory barriers for synchronization
- No mutex overhead

---

## Testing

### Automated Test Suite: `test_deployment.py`

**8-Step Comprehensive Test:**
1. Node discovery (`nls`)
2. Deploy topology (`nsnn deploy`)
3. Node status check (`nstat`)
4. Start SNN (`nsnn start`)
5. Inject spikes (`nsnn inject`)
6. SNN statistics (`nstat -s`)
7. SNN status (`nsnn status`)
8. Stop SNN (`nsnn stop`)

**Usage:**
```bash
python test_deployment.py                   # Verbose mode
python test_deployment.py -q                # Quiet mode
python test_deployment.py -s 100            # Custom spike count
python test_deployment.py -c 192.168.1.100  # Different controller
```

**Recent Test Results (Dec 11, 2025):**
```
8/8 PASS
  Active Neurons: 3
  Total Spikes: 6
  Spike Rate: 4.00 Hz
```

### Hardware Validation Tests (Completed Dec 8-9, 2025)
- ✅ Day 1: Unicast spike, burst limiting (CSMA/CD verified)
- ✅ Day 2: Broadcast spike, CTRL frame echo
- ✅ Day 3: Priority scheduling, memory barrier optimization

---

## Build Instructions

### Requirements
- Pico SDK 2.0.0+
- CMake 3.13+
- ARM GCC toolchain
- PowerShell (Windows)

## Known Issues

### Neuron Count Display
The `/api/snn/status` endpoint may report inflated neuron counts (includes table metadata). Use `nsnn deploy` which automatically resets before deployment. This is cosmetic only - network functionality is not affected.

---

## Pin Reference

### Bus Signals
- DATA[15:0] - GPIO 12-27
- CLK - GPIO 6
- SELECT0 (SOP) - GPIO 7
- SELECT1 (EOP) - GPIO 8
- CTL[2:0] - GPIO 2, 3, 4 (reserved)

### Peripherals
- OLED SDA - GPIO 28 (V2 controller only)
- OLED SCL - GPIO 29 (V2 controller only)
- LED Green - GPIO 44 (all nodes)
- LED Blue - GPIO 45 (all nodes)
- LED Red - GPIO 46 (all nodes)

### Node ID (Nodes Only)
- ID[3:0] - GPIO 40-43 (hardware pull-downs, V2 auto-detect, V1 hardcoded)

### Controller Only
- W5500 (Ethernet) - SPI0
- SD Card - SPI1
- Reset output - GPIO 33 (multi-node reset)

---

## Source Code Reference

### Firmware
- `common/z1_onyx_bus/` - Matrix bus protocol implementation
- `common/z1_broker/` - Message broker and CSMA/CD
- `common/z1_commands/` - Command definitions
- `controller/controller_main.c` - Controller (dual-core: bus + HTTP)
- `controller/z1_http_api.c` - REST API implementation
- `node/node_main.c` - Compute node main loop
- `node/z1_snn_engine.c` - SNN processing engine

### Python Tools
- `python_tools/lib/z1_client.py` - HTTP client library
- `python_tools/lib/snn_compiler.py` - Topology compiler
- `python_tools/bin/` - CLI utilities (nls, nsnn, nstat, etc.)
