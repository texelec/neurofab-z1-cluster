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
│   ├── bin/             # CLI tools (nls, nstat, nsnn, nreset, nupdate, zconfig, etc.)
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
  - Bidirectional: Both controller and nodes can send unicast frames
  - Used for: OTA updates, memory operations, node commands
- **BROADCAST (Type 1):** All nodes receive, no ACK
  - Typically controller → nodes, but nodes can broadcast responses
  - Used for: Discovery, global commands, spike propagation
- **ACK (Type 2):** ACK response frame
- **CTRL (Type 3):** High-priority commands
  - Used for: Bootloader OTA mode entry, critical commands

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
- **Inject input spikes into network (ASYNCHRONOUS - HIGH EFFICIENCY)**
- HTTP returns immediately after queuing, spikes injected in background
- Single request can queue thousands of spikes (replaces thousands of individual operations)
- Request body:
  ```json
  {
    "spikes": [
      {"neuron_id": 0, "count": 1250},
      {"neuron_id": 1, "count": 1250}
    ]
  }
  ```
  - `neuron_id`: Global neuron ID (encoded as `node_id << 16 | local_id`)
  - `count`: Number of spikes to inject (default: 1, max: 10000 per neuron)
  - `value`: Spike amplitude (deprecated, ignored - always treated as 1.0)
- Response (immediate):
  ```json
  {
    "status": "queued",
    "jobs": 2,
    "spikes": 2500
  }
  ```
  - `status`: "queued" (spikes queued for background injection)
  - `jobs`: Number of spike generation jobs queued
  - `spikes`: Total spike count queued
- **Architecture:**
  - HTTP handler: Parses request → queues jobs → returns immediately (< 1ms)
  - Background task: Injects spikes at controlled rate (100 spikes/sec)
  - Processing time: spike_count / 100 seconds (e.g., 2500 spikes = 25 seconds)
  - Maximum queue depth: 8 jobs (fail if queue full)
- **Efficiency Gains:**
  - **OLD (blocking)**: 1 HTTP request + 2500 broker operations + 2500 × 10ms = 25 seconds blocked
  - **NEW (async)**: 1 HTTP request (< 1ms) → poll status as desired
  - **Controller stays responsive**: Can handle other HTTP requests during spike injection
  - **Frame budget optimization**: Only 2500 frames used (vs. 2500 + HTTP overhead)
  - **Scalability**: Can inject 10,000+ spikes without blocking HTTP for minutes
- **Monitoring Progress:**
  - Poll `GET /api/nodes` to check node activity
  - Poll `GET /api/snn/status` to check SNN running state
  - Serial console shows progress: `[SPIKE] Job start/done` messages
  - Expected completion: spikes / 100 seconds (100 spikes/sec injection rate)

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

#### Over-The-Air (OTA) Firmware Updates

The Z1 Onyx cluster supports remote firmware updates via the HTTP API. The controller orchestrates the update process, sending firmware chunks to nodes over the Matrix bus.

**OTA Update Workflow:**

**Method 1: Direct HTTP Update (Single Node)**

1. **Start Update Session** - Tell node to prepare for firmware
2. **Send Chunks** - Transfer firmware in 4KB chunks (default)
3. **Verify** - Node computes CRC32 and verifies integrity
4. **Commit** - Node writes firmware to flash (app partition at 0x00080000)
5. **Restart** - Node reboots into bootloader, which validates and launches new firmware

**Method 2: Controller-Based Deployment (Multiple Nodes, Recommended)**

1. **Upload Firmware** - PUT /api/files/*.z1app to upload packaged firmware to SD card
2. **Deploy** - POST /api/firmware/deploy with engine name and node list
3. **Controller Actions:**
   - Sends global reset to all nodes
   - Waits 3 seconds for boot
   - Sends ENTER_OTA_MODE frame to selected nodes during 5-second bootloader countdown
   - Nodes receiving frame stay in bootloader safe mode
   - Reads .z1app from SD card
   - Performs OTA update to each selected node sequentially
   - Restarts updated nodes

---

### Firmware Deployment (Multi-Node)

**`POST /api/firmware/deploy`**
- Deploy firmware from SD card to multiple nodes
- **Recommended method** for updating production clusters
- Request body:
  ```json
  {
    "engine": "xor_snn_v1.0.0.z1app",
    "nodes": [0, 1, 5, 7]
  }
  ```
  - `engine`: Filename of .z1app package on SD card (uploaded via PUT /api/files/*)
  - `nodes`: Array of node IDs to update (1-16 nodes)
- Response: `{"status": "ok", "nodes_updated": [0, 1, 5, 7], "time_ms": 45000}`
- **Deployment sequence:**
  1. Controller reads .z1app from SD (validates 192-byte header)
  2. Sends global reset (GPIO 33) to all nodes
  3. Waits 3 seconds for nodes to boot
  4. Sends ENTER_OTA_MODE (0x0080) frame to each target node during 5-second countdown
  5. Target nodes receive frame and stay in bootloader safe mode
  6. Non-target nodes complete countdown and boot to application
  7. For each target node:
     - Send UPDATE_START (binary size, CRC32)
     - Send UPDATE_CHUNK (512-byte chunks, ~15,000 for 7.5MB)
     - Send UPDATE_VERIFY (verify CRC32)
     - Send UPDATE_COMMIT (write to flash)
  8. Send UPDATE_RESTART to all updated nodes
- **Timing:**
  - Single node: ~30-45 seconds (7.5MB firmware)
  - 16 nodes: ~8-12 minutes (sequential updates)
- **Bus protocol:** Uses bidirectional Matrix bus (controller and nodes both unicast)
- **Advantages:**
  - No 16× HTTP uploads (single .z1app upload to controller)
  - Controller manages timing and retries
  - Partial updates (e.g., only nodes [0, 5, 7])
  - Atomic updates (all nodes get same firmware version)

---

### Direct Node OTA (Single Node)

**`POST /api/ota/update_start`**
- Initialize OTA update session for target node
- Request body:
  ```json
  {
    "node_id": 0,
    "firmware_size": 524288,
    "crc32": 3735928559,
    "chunk_size": 4096
  }
  ```
  - `node_id`: Target node (0-15)
  - `firmware_size`: Total firmware size in bytes
  - `crc32`: Expected CRC32 of complete firmware (used for verification)
  - `chunk_size`: Chunk size in bytes (default 4096, max 4096)
- Response: `{"status": "ok", "node_ready": true, "total_chunks": 128}`
- Node allocates PSRAM buffer at 0x11010000 and prepares to receive chunks

**`POST /api/ota/update_chunk`**
- Send single firmware chunk to node
- Request body:
  ```json
  {
    "chunk_num": 0,
    "data": "base64_encoded_chunk_data"
  }
  ```
  - `chunk_num`: Sequential chunk number (0-based)
  - `data`: Base64-encoded chunk data (up to 4096 bytes decoded)
- Response: `{"status": "ok", "chunk_num": 0, "ack": true, "progress": "1/128"}`
- Node writes chunk to PSRAM and sends ACK
- Controller tracks which chunks have been sent

**`POST /api/ota/update_verify`**
- Request node to verify firmware integrity
- Request body: `{}`
- Response: 
  ```json
  {
    "status": "ok",
    "crc_match": true,
    "computed_crc": "0xDEADBEEF",
    "expected_crc": "0xDEADBEEF"
  }
  ```
- Node computes CRC32 of PSRAM buffer and compares to expected value
- May take several seconds for large firmware (e.g., 2MB = ~5 seconds)

**`POST /api/ota/update_commit`**
- Commit firmware from PSRAM to flash
- Request body: `{}`
- Response: `{"status": "ok", "flash_ok": true}`
- Node erases application partition (0x00080000-0x007FFFFF)
- Programs flash in 256-byte pages
- Verifies flash contents by re-reading and computing CRC32
- **WARNING:** This is destructive - old firmware is erased
- May take 10-30 seconds depending on firmware size

**`POST /api/ota/update_restart`**
- Restart node with new firmware
- Request body: `{}`
- Response: `{"status": "ok", "message": "Node 0 restarting with new firmware"}`
- Node performs watchdog reset after 1 second delay
- Bootloader validates new firmware (header + CRC32)
- If valid: boots new firmware
- If invalid: enters safe mode (OTA recovery)

**`GET /api/ota/status`**
- Get current OTA session status
- Response:
  ```json
  {
    "active": true,
    "node_id": 0,
    "progress": "64/128",
    "firmware_size": 524288,
    "last_activity_ms": 12345678
  }
  ```
- Returns `{"active": false}` if no update in progress

**OTA Update Example (Python):**

```python
import requests
import base64
import zlib

controller = "http://192.168.1.222"
node_id = 0

# Read firmware binary
with open("node_16.uf2", "rb") as f:
    firmware = f.read()

# Calculate CRC32
crc32 = zlib.crc32(firmware) & 0xFFFFFFFF

# Start update session
chunk_size = 4096
resp = requests.post(f"{controller}/api/ota/update_start", json={
    "node_id": node_id,
    "firmware_size": len(firmware),
    "crc32": crc32,
    "chunk_size": chunk_size
})
print(resp.json())

# Send chunks
total_chunks = (len(firmware) + chunk_size - 1) // chunk_size
for chunk_num in range(total_chunks):
    offset = chunk_num * chunk_size
    chunk_data = firmware[offset:offset + chunk_size]
    encoded = base64.b64encode(chunk_data).decode('ascii')
    
    resp = requests.post(f"{controller}/api/ota/update_chunk", json={
        "chunk_num": chunk_num,
        "data": encoded
    })
    print(f"Chunk {chunk_num}/{total_chunks}: {resp.json()}")

# Verify
resp = requests.post(f"{controller}/api/ota/update_verify", json={})
print(f"Verify: {resp.json()}")

# Commit to flash
resp = requests.post(f"{controller}/api/ota/update_commit", json={})
print(f"Commit: {resp.json()}")

# Restart
resp = requests.post(f"{controller}/api/ota/update_restart", json={})
print(f"Restart: {resp.json()}")
```

**Troubleshooting OTA:**
- **Chunk timeout**: Increase `chunk_size` to 2048 or 1024 if ACKs are slow
- **CRC mismatch**: Check firmware integrity, try re-uploading chunks
- **Flash commit failure**: Node may be out of flash space or have hardware issue
- **Node won't restart**: Flash may be corrupted, requires manual reflash via USB
- **Watchdog issues**: See [OTA_TROUBLESHOOTING_GUIDE.md](OTA_TROUBLESHOOTING_GUIDE.md) for RP2350 watchdog behavior
- **Scratch register problems**: See troubleshooting guide for scratch preservation details
- **Full debugging guide**: See [OTA_TROUBLESHOOTING_GUIDE.md](OTA_TROUBLESHOOTING_GUIDE.md)

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

# SD Card Operations
# Check SD card status
resp = requests.get(f"{controller}/api/sd/status")
print(resp.json())  # {"mounted": true, "free_mb": 30983}

# Read configuration
resp = requests.get(f"{controller}/api/config")
print(resp.json())  # {"ip": "192.168.1.222", "current_engine": "xor_demo"}

# Update configuration
config = {"current_engine": "mnist_classifier"}
resp = requests.post(f"{controller}/api/config", json=config)
print(resp.json())  # {"success": true}

# Upload SNN engine file
with open("my_engine.json", "rb") as f:
    resp = requests.put(f"{controller}/api/files/engines/my_engine.json", data=f.read())
print(resp.json())  # {"success": true, "size": 1234}

# List engine files
resp = requests.get(f"{controller}/api/files/engines")
print(resp.json())  # {"files": [{"name": "my_engine.json", "size": 1234}], "count": 1}

# Delete file
resp = requests.delete(f"{controller}/api/files/engines/my_engine.json")
print(resp.json())  # {"success": true}
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

# SD Card Operations
# Check SD card status
curl http://192.168.1.222/api/sd/status

# Read configuration
curl http://192.168.1.222/api/config

# Update configuration
curl -X POST http://192.168.1.222/api/config \
  -H "Content-Type: application/json" \
  -d '{"current_engine": "mnist_classifier"}'

# Upload file
curl -X PUT http://192.168.1.222/api/files/engines/test.json \
  --data-binary @test.json

# List files in engines directory
curl http://192.168.1.222/api/files/engines

# Delete file
curl -X DELETE http://192.168.1.222/api/files/engines/test.json
```

---

## SD Card Storage (Controller Only)

### Overview
The controller includes a microSD card slot for persistent storage of SNN topologies, configuration files, and Over-The-Air (OTA) update firmware. The SD card uses FAT32 filesystem for PC compatibility.

### Hardware
- **Interface**: SPI1
- **Pins**:
  - MISO: GPIO 40
  - CS: GPIO 41
  - CLK: GPIO 42
  - MOSI: GPIO 43
- **Speed**: 12.5 MHz
- **Drive Strength**: 4mA
- **Format**: FAT32 (PC-compatible)

### Memory Layout (8MB PSRAM)
```
Zone 1 (0-64KB):      FatFS working buffers (~30KB used)
Zone 2 (64-128KB):    HTTP response buffer (4KB used at 0x11010000)
Zone 3 (128KB-8MB):   Reserved for OTA firmware caching (~7.8MB)
```

### API Endpoints

#### GET /api/sd/status
Check SD card mount status and free space.

**Response:**
```json
{
  "mounted": true,
  "free_mb": 30983
}
```

#### GET /api/config
Read cluster configuration file (`z1.cfg` in root directory).

**Response:**
```json
{
  "ip": "192.168.1.222",
  "subnet": "255.255.255.0",
  "gateway": "192.168.1.1",
  "current_engine": "xor_demo"
}
```

#### POST /api/config
Update configuration file. Partial updates supported (only specified fields are modified).

**Request:**
```json
{
  "current_engine": "mnist_classifier"
}
```

**Response:**
```json
{
  "success": true
}
```

#### GET /api/files/{directory}
List files in specified directory. 

**Features:**
- Returns JSON array with filename and size
- Automatically filters out hidden/system files, directories, and corrupt entries
- Returns empty array if directory doesn't exist (not an error)
- Supports any directory path (topologies/, engines/, etc.)

**Filtering Rules:**
- Skips directories (AM_DIR attribute)
- Skips hidden/system files (AM_HID, AM_SYS attributes)
- Skips dot files (`.`, `..`, `.hidden`)
- Validates first character is alphanumeric/underscore
- Rejects files >100MB (likely corrupt FAT entries)

**Example:** `GET /api/files/topologies`

**Response:**
```json
{
  "files": [
    {"name": "xor_working.json", "size": 2149},
    {"name": "mnist_snn.json", "size": 15234},
    {"name": "xor_snn_v1.0.0.z1app", "size": 30912}
  ],
  "count": 3
}
```

#### GET /api/files/{directory}/{filename}
Download file from SD card using streaming.

**Features:**
- **Streaming download** - Handles unlimited file sizes (tested: 2KB to 1MB+)
- Automatic Content-Type: `application/octet-stream`
- Content-Length header for progress tracking
- 1KB chunks with 5ms delay between sends
- No memory limit - streams directly from SD to network

**Example:** `GET /api/files/topologies/xor_working.json`

**Response:**
- Status: 200 OK
- Headers: Content-Type, Content-Length
- Body: Raw file contents (binary or text)

#### PUT /api/files/{directory}/{filename}
Upload file to SD card using streaming.

**Features:**
- **Streaming upload** - Handles unlimited file sizes (tested: 2KB to 1MB+)
- Automatic directory creation (creates parent directory if needed)
- 2KB chunk processing from W5500 RX buffer
- CRC32 integrity via FatFS f_sync()
- Handles circular buffer wraparound correctly
- No heap allocations - uses PSRAM buffer at 0x11020000

**Request:**
- Method: PUT
- Headers: Content-Length (required)
- Body: Raw file contents (binary or text)

**Example:** `PUT /api/files/engines/xor_snn_v1.0.0.z1app`

**Response:**
```json
{
  "success": true,
  "size": 30912
}
```

**Directory Creation:**
- If filepath contains `/`, extracts parent directory
- Calls `f_mkdir()` before `f_open()` 
- Ignores `FR_EXIST` error if directory already exists
- Example: `topologies/file.json` creates `topologies/` automatically

#### DELETE /api/files/{directory}/{filename}
Delete file from SD card.

**Example:** `DELETE /api/files/topologies/old_file.json`

**Response:**
```json
{
  "success": true
}
```

Or on error:
```json
{
  "error": "Failed to delete file"
}
```

**Example:** `PUT /api/files/engines/my_topology.json`

**Request Body:** Raw file data (binary or text)

**Response:**
```json
{
  "success": true,
  "size": 1234
}
```

#### DELETE /api/files/{directory}/{filename}
Delete file from SD card.

**Example:** `DELETE /api/files/engines/old_topology.json`

**Response:**
```json
{
  "success": true
}
```

#### POST /api/system/reboot
Reboot the controller. Sends response before rebooting, allowing client to receive confirmation. Controller waits 1 second before triggering watchdog reset to ensure response is transmitted.

**Note**: After reboot, controller will read `z1.cfg` and apply network configuration (IP/MAC). If IP address changed, connect to new IP after reboot completes (~5 seconds).

**Example:** `POST /api/system/reboot`

**Request Body:** None (or empty JSON)

**Response:**
```json
{
  "success": true,
  "message": "Rebooting in 1 second..."
}
```

**Usage with `zconfig` tool:**
```bash
# Change IP and reboot
python python_tools/bin/zconfig --ip 192.168.1.100 --reboot

# Change MAC and reboot
python python_tools/bin/zconfig --mac 02:A1:B2:C3:D4:05 --reboot

# Show current config
python python_tools/bin/zconfig --show
```

### Directory Structure
```
SD Card Root/
├── z1.cfg                    # Cluster configuration
└── engines/                  # SNN topology storage
    ├── xor_demo.json
    ├── mnist_classifier.json
    └── custom_network.json
```

### FatFS Implementation Details

**Library**: carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico v3.7.0

**Critical Configuration** (for RP2350B compatibility):
- `FF_FS_NORTC=1` - Disables RTC (avoids RP2350B RTC incompatibility)
- `FF_USE_LFN=1` - Long filename support (static BSS allocation)
- `FF_FS_EXFAT=0` - ExFAT disabled (not needed for <32GB cards)
- `FF_LBA64=0` - 32-bit sector addressing (standard for FAT32)

**Directory Listing Pattern** (prevents PSRAM timing issues):
```c
// CORRECT: Stack allocation with proper initialization
DIR dir = {};
FILINFO fno = {};
FRESULT fr = f_findfirst(&dir, &fno, path, "*");
while (fr == FR_OK && fno.fname[0]) {
    // Process fno.fname, fno.fsize, fno.fattrib
    fr = f_findnext(&dir, &fno);
}
f_closedir(&dir);

// WRONG: PSRAM buffers require delays/memset and are unreliable
// DIR* dir = (DIR*)psram_buffer;  // Don't do this!
```

**Robust File Filtering** (skips corrupt FAT entries):
```c
// Skip invalid entries that may appear on corrupted cards
if (!(fno.fattrib & AM_DIR) &&                    // Not a directory
    !(fno.fattrib & (AM_HID | AM_SYS)) &&         // Not hidden/system
    fno.fname[0] != '.' &&                         // Not dot file
    isalnum(fno.fname[0]) &&                       // Valid first char
    fno.fsize < 100*1024*1024) {                   // <100MB (sanity check)
    // Valid file
}
```

**HTTP Chunked Encoding Fix** (handles null bytes in PSRAM):
```c
// Binary-safe transmission using explicit length
void w5500_send_response_len(uint8_t sock, const char* data, uint16_t length) {
    // Write 'length' bytes without using strlen()
    // Critical for PSRAM buffers that may contain null bytes
}
```

### Troubleshooting

**Issue**: Directory listing hangs or returns corrupt entries  
**Solution**: Ensure using stack-allocated `DIR`/`FILINFO` with `f_findfirst/f_findnext` pattern (NOT PSRAM buffers)

**Issue**: HTTP response truncated or JSON parse errors  
**Solution**: Use `w5500_send_response_len()` for chunk data (not `w5500_send_response()` which uses `strlen`)

**Issue**: Files not appearing in listing  
**Solution**: Check filename starts with alphanumeric character and is <100MB

**Issue**: SD card not mounting  
**Solution**: 
1. Verify card formatted as FAT32 (not exFAT or NTFS)
2. Check SPI1 connections (MISO=40, CS=41, CLK=42, MOSI=43)
3. Try different SD card (some high-speed cards incompatible at 12.5MHz)
4. Check serial output for `FRESULT` error code

### Testing

**Automated Test**: `test_sd_card.py`
```bash
python test_sd_card.py 192.168.1.222
```

**Test Coverage** (6 tests):
1. SD card status and health
2. Configuration file read
3. Configuration file write
4. File upload (380 bytes)
5. Directory listing (with filtering)
6. File deletion

**Expected Result**: `6/6 tests passed`

---
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

### nreset - Node Reset
Reset (reboot) specific nodes or the controller via software watchdog.

```bash
nreset -n <node_id> [-c IP]     # Reset single node
nreset -n 0,1,2 [-c IP]         # Reset multiple nodes
nreset --controller [-c IP]     # Reset controller
nreset --all [-c IP]            # Reset all nodes + controller
```

**Arguments:**
- `-n` / `--nodes` - Comma-separated node IDs (0-15) to reset
- `--controller` - Reset controller (node ID 16)
- `--all` - Reset all nodes and controller
- `-c` - Controller IP (default: 192.168.1.222)

**Example:**
```bash
$ python python_tools/bin/nreset -n 0,1 -c 192.168.1.201
Resetting nodes: [0, 1]
✓ Node 0 reset command sent
✓ Node 1 reset command sent
Nodes will reboot in ~3 seconds

$ python python_tools/bin/nreset --controller
✓ Controller reset command sent
Controller will reboot in ~3 seconds
```

**Uses:**
- Reset hung nodes before OTA update
- Clear node state after testing
- Reboot controller after config changes
- Emergency cluster reset with `--all`

### zconfig - Configuration Management
Modify network configuration and optionally reboot controller. Changes are written to `z1.cfg` on SD card and applied on next boot.

```bash
zconfig --show                                    # Show current config
zconfig --ip 192.168.1.100 --reboot              # Change IP and reboot
zconfig --mac C3:D4:05 --reboot                  # Change MAC (last 3 bytes) and reboot
zconfig --ip 192.168.1.100 --mac C3:D4:05 --reboot  # Both
zconfig --engine iaf                             # Change engine type (no reboot)
zconfig -c 192.168.1.222 --ip 192.168.1.100      # Specify controller IP
```

**Options:**
- `--ip` - New IP address (e.g., 192.168.1.100)
- `--mac` - New MAC suffix (last 3 bytes, e.g., C3:D4:05). Prefix 02:5A:31 fixed.
- `--engine` - SNN engine type (iaf, lif, adaptive)
- `--reboot` - Reboot after config change to apply immediately
- `--show` - Display current configuration and exit
- `-c` - Controller IP address (default: 192.168.1.222)
- `--wait` - Seconds to wait after reboot (default: 5)

**Example Session:**
```bash
$ python python_tools/bin/zconfig --show
Current Configuration:
  IP Address: 192.168.1.222
  MAC Address: 02:5A:31:C3:D4:01
  Engine: iaf
  Hardware Version: 2
  Node Count: 16

$ python python_tools/bin/zconfig --ip 192.168.1.100 --reboot
Connecting to controller at 192.168.1.222...
Downloading current config...
Applying changes: IP: 192.168.1.100
Uploading new config...
✓ Config updated successfully

Rebooting controller...
✓ Rebooting in 1 second...
Waiting 5 seconds for reboot...

Attempting to connect to 192.168.1.100...
✓ Controller online at 192.168.1.100
  IP: 192.168.1.100
  MAC: 02:5A:31:C3:D4:01
  Engine: iaf
```

**How It Works:**
1. Downloads current `z1.cfg` from controller
2. Modifies specified fields (IP, MAC, engine)
3. Uploads modified config back to SD card
4. Triggers reboot via `POST /api/system/reboot` (if `--reboot` specified)
5. Waits for controller to come back online
6. Connects to new IP address and verifies config

**Note**: Without `--reboot`, changes are written to SD card but not applied until next manual reboot.

### nupdate - OTA Firmware Update (Method 1: Direct HTTP)
Deploy firmware updates directly to compute nodes via HTTP. This tool implements the direct OTA protocol without requiring SD card upload.

```bash
nupdate <firmware.bin> -n <node_id> [-c IP]     # Update single node
nupdate <firmware.bin> -n 0,1,2 [-c IP]         # Update specific nodes
nupdate <firmware.bin> --all [-c IP]            # Update all 16 nodes
```

**Arguments:**
- `firmware.bin` - Raw node binary (e.g., `build/node/node_app_16.bin`)
- `-n` / `--nodes` - Comma-separated node IDs (0-15)
- `--all` - Update all nodes in cluster
- `-c` / `--controller` - Controller IP (default: 192.168.1.201)
- `--version` - Firmware version (default: 1.0.0)
- `--name` - Firmware name (default: "Z1 Node App")
- `--no-reset` - Skip automatic reset (assume node already in bootloader)
- `--chunk-size` - Upload chunk size in bytes (default: 4096)

**Automatic Process:**
1. **Reset node to bootloader** via OPCODE_RESET_TO_BOOTLOADER (0x38)
2. **Wait 4-8 seconds** with 500ms polling for bootloader ready status
3. **Prepend 192-byte Z1 header** (magic: 0x5A314150) and calculate CRC32
4. **Start OTA session** - Send firmware size and CRC to node
5. **Upload in 4KB chunks** with real-time progress bar
6. **Verify CRC32** in PSRAM buffer
7. **Commit to flash** - Write to app partition (0x00080000)
8. **Restart node** via watchdog
9. **Verify online** - Wait for node to boot with new firmware

**Example Session:**
```bash
$ python python_tools/bin/nupdate build/node/node_app_16.bin -n 0 -c 192.168.1.201

============================================================
Z1 Onyx Node OTA Update
============================================================

Controller: 192.168.1.201
Target nodes: 0
Firmware: node_app_16.bin (45,104 bytes)
Version: 1.0.0

============================================================

Resetting node 0 to bootloader... ✓
Waiting for node 0 bootloader (up to 8s)... ✓ Ready after 4.5s

============================================================
OTA Update - Node 0
============================================================

[1/5] Starting OTA session...
✓ Node 0 ready for firmware
  PSRAM available: 8,388,608 bytes

[2/5] Uploading firmware (11 chunks)...
[████████████████████████████████████] 100% (11/11) 45104 bytes

[3/5] Verifying CRC32...
✓ CRC32 match: 0x1A2B3C4D

[4/5] Flashing to app partition...
✓ Flash complete (45104 bytes written)

[5/5] Restarting node...
✓ Node reboot initiated

⏳ Waiting for node to come online...
✓ Node 0 online and responding

✓ Node 0 updated successfully

============================================================
Update Summary
============================================================

Total nodes: 1
Success: 1
Failed: 0

All nodes updated successfully!
```

**Notes:**
- Automatic reset and bootloader detection (use `--no-reset` to skip)
- Firmware must be raw .bin file <7.5MB (app partition limit)
- Update takes ~30-60s per node depending on firmware size
- Sequential updates: 16 nodes = ~8-16 minutes total
- Progress bar shows real-time upload status

**Alternative Method 2 (SD-Based):**
For multi-node deployments, consider the SD card workflow:
1. Upload `.z1app` to SD card via `/api/files/engines/`
2. Call `POST /api/firmware/deploy` with node list
3. Controller reads from SD and deploys to all nodes
⚠️ **Status**: Method 2 stub not yet implemented (see `/api/firmware/deploy` docs)

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
- **W5500 (Ethernet)** - SPI0
  - RST: GPIO 34 (V2), GPIO 35 (V1)
  - INT: GPIO 35 (V2), GPIO 34 (V1)
- **SD Card** - SPI1 (FAT32 filesystem)
  - MISO: GPIO 40
  - CS: GPIO 41
  - CLK: GPIO 42 (12.5 MHz, 4mA drive)
  - MOSI: GPIO 43
- **Reset output** - GPIO 33 (multi-node reset, V2 only)

---

## Source Code Reference

### Firmware
- `common/z1_onyx_bus/` - Matrix bus protocol implementation
- `common/z1_broker/` - Message broker and CSMA/CD
- `common/z1_commands/` - Command definitions
- `common/sd_card/` - SD card FatFS wrapper
- `controller/controller_main.c` - Controller (dual-core: bus + HTTP)
- `controller/z1_http_api.c` - REST API implementation (includes SD card endpoints)
- `controller/hw_config.c` - FatFS hardware configuration
- `node/node_main.c` - Compute node main loop
- `node/z1_snn_engine.c` - SNN processing engine

### Python Tools
- `python_tools/lib/z1_client.py` - HTTP client library
- `python_tools/lib/snn_compiler.py` - Topology compiler
- `python_tools/bin/` - CLI utilities:
  - `nls` - List all online nodes
  - `nstat` - Node status and SNN statistics
  - `nsnn` - SNN deployment and management
  - `nping` - Ping nodes
  - `nreset` - Software reset for nodes/controller
  - `nupdate` - OTA firmware updates (direct HTTP)
  - `zconfig` - Network/config management
- `test_sd_card.py` - SD card API test suite (6 tests)
