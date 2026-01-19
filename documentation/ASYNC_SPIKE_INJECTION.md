# Async Spike Injection Architecture

**Date:** January 18, 2026  
**Status:** ✅ Implemented and Tested  
**Impact:** Massive efficiency improvement for spike injection

---

## Problem Statement

**Previous implementation (blocking):**
- HTTP handler injected spikes synchronously with 10ms throttling per spike
- Blocked HTTP for entire duration (200 spikes × 10ms = 2+ seconds)
- Controller unresponsive to other HTTP requests during injection
- Poor user experience: HTTP appears to hang during large spike injections

**Example:** Injecting 2500 spikes took 25+ seconds with HTTP completely blocked

---

## New Architecture

### High-Level Flow

```
1. HTTP Request  →  2. Parse & Queue  →  3. Return Immediately  →  4. Background Injection
   (< 1ms)              (< 1ms)             (< 1ms)                  (spikes/100 seconds)
```

### Components

#### 1. Spike Job Queue (controller/z1_http_api.c)
```c
typedef struct {
    uint32_t neuron_id;  // Global neuron ID (node_id << 16 | local_id)
    uint32_t count;      // Number of spikes to inject
} spike_job_t;

static struct {
    spike_job_t jobs[MAX_SPIKE_JOBS];  // Circular buffer: 8 jobs max
    uint8_t head, tail, count;
    uint32_t total_injected;           // Running total
    uint32_t current_remaining;        // Current job progress
    // ... other fields ...
} spike_queue;
```

#### 2. HTTP Handler (POST /api/snn/input)
```c
void handle_snn_input(const char* body, char* response, int size) {
    // Parse JSON: {"spikes": [{"neuron_id": N, "count": C}, ...]}
    // For each spike entry:
    //   - Extract neuron_id and count
    //   - spike_queue_enqueue(neuron_id, count)
    // Return: {"status": "queued", "jobs": N, "spikes": M}
}
```
**Key:** NO BLOCKING - returns in < 1ms regardless of spike count

#### 3. Background Spike Processor (Core 0 main loop)
```c
void z1_http_api_process_spikes(void) {
    // Rate limiting: 10ms between spikes = 100 spikes/sec
    if (now_us - last_spike_time_us < 10000) return;
    
    // If no active job, start next one from queue
    // Inject ONE spike per call
    // Dequeue job when complete
}
```
**Called from:** `controller_main.c` Core 0 loop (every iteration)

#### 4. Core 0 Integration
```c
void core0_main(void) {
    while (true) {
        w5500_eth_process();              // HTTP server
        z1_http_api_process_spikes();     // Background spike injection
        // ... RX/TX paths, broker task ...
    }
}
```

---

## Efficiency Analysis

### Traffic Comparison

**OLD (Blocking):**
```
HTTP Request (1 frame)
  ↓
Parse JSON (1ms)
  ↓
Loop: 2500 iterations
  ├─ z1_broker_send() × 2500          ← Queues 2500 broker operations
  ├─ sleep_us(10000) × 2500           ← Blocks HTTP for 25 seconds
  └─ Retry loop if queue full
  ↓
HTTP Response (1 frame)               ← After 25+ seconds

Total HTTP time: 25+ seconds BLOCKED
Controller state: UNRESPONSIVE during injection
```

**NEW (Async):**
```
HTTP Request (1 frame)
  ↓
Parse JSON (< 1ms)
  ↓
Queue 2 jobs (< 1ms)
  ↓
HTTP Response (1 frame)               ← After < 1ms

Background (Core 0):
  ├─ Process job 1: 1250 spikes @ 100/sec = 12.5 seconds
  └─ Process job 2: 1250 spikes @ 100/sec = 12.5 seconds

Total HTTP time: < 1ms
Controller state: FULLY RESPONSIVE during injection
Background processing: 25 seconds (same total time, but non-blocking)
```

### Frame Budget Comparison

**OLD:**
- HTTP overhead: ~10 frames (request parsing, retry logic, response)
- Spike frames: 2500 frames
- **Total: ~2510 frames** (over 500 limit, causing issues)

**NEW:**
- HTTP overhead: ~2 frames (request + response)
- Spike frames: 2500 frames
- **Total: ~2502 frames** (same spike count, but HTTP doesn't block)

### Controller Responsiveness

| Metric | OLD (Blocking) | NEW (Async) | Improvement |
|--------|---------------|-------------|-------------|
| HTTP response time | 25+ seconds | < 1ms | **25,000×** |
| Can handle other requests? | ❌ No | ✅ Yes | **Infinite** |
| Frame budget | 2510 frames | 2502 frames | **8 frames saved** |
| User experience | Appears hung | Immediate feedback | **Night & day** |
| Scalability | Limited by HTTP timeout | Limited by queue depth | **8× spike count** |

---

## API Changes

### Request (unchanged)
```json
POST /api/snn/input
{
  "spikes": [
    {"neuron_id": 0, "count": 1250},
    {"neuron_id": 1, "count": 1250}
  ]
}
```

### Response (changed)
**OLD:**
```json
{"spikes_injected": 2500}  // After 25 seconds
```

**NEW:**
```json
{"status": "queued", "jobs": 2, "spikes": 2500}  // After < 1ms
```

### Monitoring Progress
Since HTTP returns immediately, poll status:
```bash
# Queue spikes
curl -X POST http://192.168.1.222/api/snn/input -d '{"spikes":[...]}'
# {"status":"queued","jobs":2,"spikes":2500}

# Poll status (every 2-5 seconds)
curl http://192.168.1.222/api/nodes
# Check node activity/stats

# Or watch serial console:
[SPIKE] Job start: neuron_id=0 count=1250 node=0
[SPIKE] Job done (total: 1250)
[SPIKE] Job start: neuron_id=1 count=1250 node=0
[SPIKE] Job done (total: 2500)
```

---

## Test Script Changes

**OLD:**
```python
def test_inject_spikes(controller_ip, spike_count):
    # Send HTTP request
    # BLOCKS here for 25+ seconds
    # Return after completion
```

**NEW:**
```python
def test_inject_spikes(controller_ip, spike_count):
    # Send HTTP request
    # Returns immediately (< 1ms)
    
    # Calculate expected completion time
    expected_time = spike_count / 100  # 100 spikes/sec
    
    # Poll status every 2 seconds
    while elapsed < expected_time + 5:
        # Check controller status
        # Show progress
        time.sleep(2)
```

---

## Performance Characteristics

### Injection Rate
- **Fixed:** 100 spikes/sec (10ms per spike)
- **Reason:** Respects RP2350-E5 hardware frame limit (~500 frames per session)
- **Trade-off:** Slower injection but stable controller operation

### Queue Capacity
- **Max jobs:** 8 (MAX_SPIKE_JOBS)
- **Max spikes per job:** 10,000
- **Total capacity:** 80,000 spikes queued
- **Failure mode:** Returns error if queue full

### Processing Time
| Spike Count | Time @ 100/sec | HTTP Blocking (OLD) | HTTP Blocking (NEW) |
|-------------|----------------|---------------------|---------------------|
| 100 | 1 second | 1 second | < 1ms |
| 500 | 5 seconds | 5 seconds | < 1ms |
| 2500 | 25 seconds | 25 seconds | < 1ms |
| 10000 | 100 seconds | 100 seconds | < 1ms |

**Key insight:** Total processing time unchanged, but HTTP no longer blocks!

---

## Code Files Changed

1. **controller/z1_http_api.c** - `handle_snn_input()`
   - Removed: Synchronous spike injection loop with 10ms sleep per spike
   - Added: Job queue enqueue and immediate return

2. **controller/z1_http_api.c** - `z1_http_api_process_spikes()`
   - Simplified: Direct timing-based throttling (10ms between spikes)
   - Removed: Complex call counting and corruption detection (unnecessary)

3. **controller/controller_main.c** - `core0_main()`
   - Added: Call to `z1_http_api_process_spikes()` in main loop

4. **test_deployment.py** - `test_inject_spikes()`
   - Added: Polling loop to monitor injection progress
   - Changed: Updated documentation and user messaging

5. **API_REFERENCE.md** - Spike Injection section
   - Updated: Architecture description, efficiency analysis, monitoring guidance

---

## Benefits Summary

✅ **Immediate HTTP Response:** < 1ms vs. 25+ seconds  
✅ **Controller Stays Responsive:** Can handle other requests during injection  
✅ **Better UX:** Clear feedback ("queued") instead of appearing hung  
✅ **Scalability:** Can queue 80,000 spikes vs. limited by HTTP timeout  
✅ **Frame Budget Efficiency:** Saves ~8 HTTP overhead frames  
✅ **Monitoring Flexibility:** Poll status as desired, no forced waiting  
✅ **Architecture Extensibility:** Job queue supports future spike generation patterns  

---

## Future Enhancements

1. **Status Endpoint:** `GET /api/snn/spikes/status` returns queue depth and progress
2. **Priority Queue:** High-priority spikes (e.g., external sensors) processed first
3. **Batch Optimization:** Group spikes to same node for fewer frames
4. **Rate Control:** Dynamic injection rate based on broker queue depth
5. **Spike Patterns:** Pre-defined patterns (burst, Poisson, regular) generated on controller

---

## Compatibility

**Backward Compatibility:** ❌ Breaking change (response format changed)  
**Migration Path:**
- Python tools updated to parse new response format
- Test scripts updated to poll for completion
- API documentation clearly describes new behavior

**For users:**
- Update Python tools to latest version
- Read new API_REFERENCE.md section
- Test scripts handle async behavior automatically
