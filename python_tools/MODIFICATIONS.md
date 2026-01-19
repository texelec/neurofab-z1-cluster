# Python Tools Modifications

**Original Source:** https://github.com/ahtx/neurofab-z1-cluster  
**Version:** 4.0 (Production Ready, January 18, 2026)  
**Local Branch:** RP2350 hardware adaptation with OTA support

This document tracks ALL changes made to the original GitHub python_tools to work with our RP2350-based Z1 cluster implementation.

---

## Latest Updates - December 11, 2025

### Controller Firmware Enhancement: neuron_count Field Added

**Issue:** `/api/snn/status` endpoint was missing `neuron_count` field  
**Impact:** `nstat -s` showed "Neurons: 0" despite network deployed  
**Fix Applied:** December 11, 2025

**Changes:**
1. **Controller firmware** (`controller/z1_http_api.c`):
   - Added global variable: `g_total_neurons_deployed`
   - Tracks total neurons across all nodes during deployment
   - Included in `/api/snn/status` JSON response
   - Reset on `/api/snn/reset` call

2. **nsnn tool** (`bin/nsnn`):
   - Added automatic reset before deployment
   - Calls `POST /api/snn/reset` to clear neuron count
   - Prevents accumulation across multiple deployments
   - Gracefully handles older firmware (ignores reset failures)

**API Response (Updated):**
```json
{
  "state": "running",
  "neuron_count": 5,        ← NEW FIELD
  "active_neurons": 3,
  "total_spikes": 6,
  "spike_rate_hz": 4.0
}
```

**Test Results:**
```bash
# Before fix
python nstat -s
  Neurons:         0        ← WRONG

# After fix
python nstat -s
  Neurons:         5        ← CORRECT (XOR network)
  Active Neurons:  3
  Total Spikes:    6
  Spike Rate:      4.00 Hz
```

**Deployment Workflow (Updated):**
1. `nsnn deploy` → Calls `/api/snn/reset` (clears neuron count to 0)
2. For each node: Writes neuron table to PSRAM
3. For each node: Calls `/api/nodes/{id}/snn/load` → Increments `g_total_neurons_deployed`
4. `nstat -s` → Shows correct total neuron count

**Files Modified:**
- `controller/z1_http_api.c` (firmware)
- `bin/nsnn` (deployment tool)
- Firmware: `FirmwareReleases/16node/z1_controller_16.uf2` (V2 hardware)

---

## Changes from Original GitHub Repository

### Hardware Platform Differences

**Original (GitHub):**
- Reference neuromorphic hardware platform
- SRAM-based memory @ 0x20000000
- 21 HTTP API endpoints (including READ_MEMORY)
- W5500 Ethernet controller

**Our Implementation (RP2350):**
- Raspberry Pi Pico 2 (RP2350B) nodes
- PSRAM @ 0x00000000 (16MB per node)
- HTTP API subset (WRITE_MEMORY working, READ_MEMORY not implemented)
- W5500 Ethernet (same)
- Custom bus protocol (different from original)

---

## Modified Files from Original

This document tracks all changes made to the original GitHub python_tools.

---

## Comprehensive Test Results - December 11, 2025

**Test Script:** `test_all_tools.py` (automated validation)

### ✅ FULLY WORKING (5/8 tools - All Critical)

1. **nls** - List cluster nodes ✅
   - Original: ✅ No changes needed
   - Test: Lists 16 nodes with memory/uptime
   - Status: 100% compatible with current hardware

2. **nping** - Ping cluster nodes ✅
   - Original: ✅ No changes needed
   - Test: 0% packet loss to node 0
   - Status: 100% compatible with current hardware

3. **nstat** - Cluster statistics ✅
   - Original: ✅ No changes needed
   - Test: Shows 16 nodes online with full status
   - Status: 100% compatible with current hardware

4. **nsnn** - SNN management ✅
   - Original: ⚠️ Modified (backplane fallback logic + deployment reset)
   - Changes: 
     - Backplane fallback for single-node compatibility
     - **NEW (Dec 11):** Auto-reset before deployment to clear neuron count
     - Calls `POST /api/snn/reset` before each deployment
   - Test: Status, deploy, start, stop all working
   - Status: Enhanced for single-backplane + proper neuron tracking

5. **ncp** - Copy files to PSRAM ✅
   - Original: ⚠️ Modified (memory addresses updated)
   - Changes: MEMORY_LOCATIONS dict updated for RP2350 PSRAM
   - Test: Successfully wrote file to node 0 @ 0x00400000
   - Status: Fully functional with RP2350 hardware

### ⚠️ FIRMWARE LIMITATION (1/8 tools)

6. **ncat** - Display node memory ⚠️
   - Original: ⚠️ Modified (memory addresses updated)
   - Changes: MEMORY_LOCATIONS dict updated for RP2350 PSRAM
   - Limitation: Controller firmware missing READ_MEMORY handler
   - Test: Returns 0 bytes (expected - firmware not implemented)
   - Status: Tool updated, waiting on firmware feature

### ○ NOT CRITICAL (2/8 tools)

7. **nconfig** - Configuration management ○
   - Original: ✅ Works as-is
   - Usage: Multi-backplane cluster configuration
   - Test: Skipped (single-backplane deployment only)
   - Status: Not needed for current use case

8. **nflash** - Firmware flashing ○
   - Original: Reference implementation
   - Test: Skipped (out of scope)
   - Status: Manual firmware flashing via BOOTSEL

---

## Summary vs. Original GitHub

**Unchanged (Working as-is):** nls, nping, nstat, nconfig  
**Modified (Enhancements):** nsnn (backplane fallback), ncp (PSRAM addresses), ncat (PSRAM addresses)  
**Not Tested:** nflash (out of scope)

**Overall Compatibility:** 5/5 critical tools fully functional (100%)

---

## Modified Files

### `lib/z1_client.py` - December 10, 2025

**Original GitHub:** Used `/snn/*` endpoints

**Change 1 Location:** Lines 135-165 (deploy_snn function - save controller IP)

**Added Code:**
```python
        # Get controller IP from first deployed backplane
        first_bp_name = list(deployment_plan.backplane_nodes.keys())[0]
        if backplane_config:
            bp_info = [bp for bp in backplane_config['backplanes'] if bp['name'] == first_bp_name]
            controller_ip = bp_info[0]['controller_ip'] if bp_info else '192.168.1.222'
            controller_port = bp_info[0].get('controller_port', 80) if bp_info else 80
        else:
            controller_ip = '192.168.1.222'  # Default single-backplane IP
            controller_port = 80
        
        with open(deployment_info_file, 'w') as f:
            json.dump({
                'topology_file': args.topology,
                'controller_ip': controller_ip,  # NEW
                'controller_port': controller_port,  # NEW
                'deployment_plan': {
                    ...
                }
            }, f, indent=2)
```

**Reason:**
- Save controller IP/port during deployment for use by start/stop/status commands
- Allows start_snn() to work without requiring backplane name match in cluster config

---

**Change 2 Location:** Lines 197-231 (start_snn function - use saved controller IP)

**Modified Code:**
```python
    # Start SNN on each backplane
    for bp_name in deployment_plan['backplane_nodes'].keys():
        # Try to get backplane from config
        bp = config.get_backplane(bp_name)
        
        # If not found, use deployment info controller IP (saves during deployment)
        if not bp:
            print(f"Warning: Backplane '{bp_name}' not found in configuration", file=sys.stderr)
            # Use controller IP from deployment info
            controller_ip = info.get('controller_ip', '192.168.1.222')
            controller_port = info.get('controller_port', 80)
            print(f"  Using deployment controller: {controller_ip}:{controller_port}")
        else:
            controller_ip = bp.controller_ip
            controller_port = bp.controller_port
        
        try:
            client = Z1Client(controller_ip=controller_ip, port=controller_port)
            client.start_snn()
            print(f"  {bp_name}: Started ✓")
```

**Reason:**
- Fixed bug where `nsnn start` would silently skip execution if deployed backplane name didn't match cluster config
- Uses controller IP saved during deployment as fallback
- Prevents silent failures when using 'default' backplane with cluster_config.json that has named backplanes

**Impact:**
- `nsnn start` now works even when backplane names don't match between deployment and config
- Allows single-backplane deployments to use saved controller IP without modifying cluster_config.json

---

### `lib/z1_client.py` - December 10, 2025

**Change:** Lines 355, 366, 377, 387 (all SNN API endpoints)

**Problem:** Python client used `/snn/*` but controller expects `/api/snn/*`

**Symptom:** HTTP 404 errors, spike injection silent failures

**Solution:** Added `/api` prefix to all SNN endpoints:
- `inject_spikes()`: `/snn/input` → `/api/snn/input` (line 355)
- `start_snn()`: `/snn/start` → `/api/snn/start` (line 366)
- `stop_snn()`: `/snn/stop` → `/api/snn/stop` (line 377)
- `get_snn_status()`: `/snn/status` → `/api/snn/status` (line 387)

**Impact:** All SNN operations now reach controller correctly

---

**Change 3 Location:** Lines 363-386 (inject_spikes function - backplane fallback)

**Problem:** Same as start/stop - silent skip when backplane 'default' not in cluster config

**Solution:** Added fallback to deployment controller IP:
```python
bp = config.get_backplane(bp_name)
if not bp:
    # Use controller from deployment info or CLI argument
    controller_ip = info.get('controller_ip', '192.168.1.222')
    controller_port = info.get('controller_port', 80)
else:
    controller_ip = bp.controller_ip
    controller_port = bp.controller_port

client = Z1Client(controller_ip=controller_ip, port=controller_port)
```

**Bug Fixes:**
- Line 374: Changed `{count}` to `{len(bp_spikes)}` (undefined variable)
- Added warning message when backplane not found (instead of silent skip)

**Impact:** Spike injection now works with single-backplane deployments

---

### `lib/snn_compiler.py` - December 10, 2025

**Change 1:** Lines 376-380 (synapse metadata packing)

**Original Code:**
```python
        # Synapse metadata (8 bytes)
        struct.pack_into('<HHI', entry, 16,
                        len(neuron.synapses),  # synapse_count
                        54,                    # synapse_capacity (256-40)/4 = 54 max
                        neuron.global_id)      # global_id
```

**Modified Code:**
```python
        # Synapse metadata (8 bytes)
        struct.pack_into('<HHI', entry, 16,
                        len(neuron.synapses),  # synapse_count
                        60,                    # synapse_capacity (240/4 = 60 max, matches Z1_SNN_MAX_SYNAPSES)
                        0)                     # reserved1 (firmware doesn't use global_id here)
```

**Reason:**
- Fixed critical struct packing bug causing neuron parameters to load incorrectly
- Firmware expects `reserved1` (zeros) at offset 20-23, not `global_id`
- Synapse capacity corrected from 54 to 60 to match firmware's `Z1_SNN_MAX_SYNAPSES` constant

**Impact:**
- Neurons now load with correct threshold and leak_rate values
- PSRAM struct layout now matches firmware's `z1_neuron_entry_t` definition

---

**Change 2:** Lines 388-389 (synapse array size)

**Original Code:**
```python
        # Synapses (216 bytes, 54 × 4 bytes)
        for i, (source_global_id, weight) in enumerate(neuron.synapses[:54]):
```

**Modified Code:**
```python
        # Synapses (240 bytes, 60 × 4 bytes)
        for i, (source_global_id, weight) in enumerate(neuron.synapses[:60]):
```

**Reason:**
- Match firmware's synapse capacity (60 synapses per neuron)
- Corrects memory layout to use full 240-byte synapse array

---

**Change 3:** Lines 340-368 (added debug logging)

**Added Code:**
```python
            print(f"[COMPILER DEBUG] Node {node_id}: {len(node_neurons)} neurons")
            for neuron in node_neurons:
                # Pack neuron entry (256 bytes)
                entry = self._pack_neuron_entry(neuron)
                print(f"[COMPILER DEBUG]   Neuron {neuron.neuron_id} (global {neuron.global_id}): " +
                      f"threshold={neuron.threshold:.1f}, leak={neuron.leak_rate:.1f}, synapses={len(neuron.synapses)}")
                # Print first 32 bytes of entry for debugging
                hex_str = ' '.join(f'{b:02X}' for b in entry[:32])
                print(f"[COMPILER DEBUG]     Entry bytes: {hex_str}")
                table_data.extend(entry)
            
            # Add end marker (256-byte entry with neuron_id = 0xFFFF)
            end_marker = bytearray(256)
            struct.pack_into('<H', end_marker, 0, 0xFFFF)
            print(f"[COMPILER DEBUG]   End marker at offset {len(table_data)}")
            table_data.extend(end_marker)
```

**Reason:**
- Added diagnostic output to verify neuron compilation
- Shows exact parameters being packed (threshold, leak, synapses)
- Displays binary layout for PSRAM verification
- Helpful for debugging struct packing issues

**Impact:**
- Can verify neuron parameters match topology JSON before deployment
- Hex dump allows comparison with firmware PSRAM reads

---

## December 11, 2025 - Memory Address Fixes

### `bin/ncp` and `bin/ncat` - Memory Map Update

**Problem:** Tools used reference hardware memory addresses (SRAM @ 0x20000000)
**Hardware:** RP2350 with PSRAM @ 0x00000000 base

**Changes Made:**

Both files updated at lines 18-25 (MEMORY_LOCATIONS dict):

**OLD (Reference Hardware):**
```python
MEMORY_LOCATIONS = {
    'weights': 0x20000000,      # Start of PSRAM
    'neurons': 0x20100000,      # 1MB offset
    'code': 0x20200000,         # 2MB offset
    'data': 0x20300000,         # 3MB offset
    'scratch': 0x20700000,      # 7MB offset
}
```

**NEW (RP2350 PSRAM @ 0x00000000, 16MB):**
```python
MEMORY_LOCATIONS = {
    'neurons': 0x00100000,      # 1MB offset - neuron table (Z1_SNN_NEURON_TABLE_ADDR)
    'weights': 0x00200000,      # 2MB offset - weight matrices
    'code': 0x00300000,         # 3MB offset - executable code
    'data': 0x00400000,         # 4MB offset - general data
    'scratch': 0x00F00000,      # 15MB offset - scratch space
}
```

**Alignment:**
- `neurons` location now matches firmware `Z1_SNN_NEURON_TABLE_ADDR = 0x00100000`
- Addresses stay within 16MB PSRAM range
- Named locations updated to match RP2350 architecture

**Limitation Discovered:**
- **ncat (read)**: ⚠️ Controller firmware missing `READ_MEMORY` opcode/handler
  - `GET /nodes/{id}/memory` endpoint returns empty response
  - Would require implementing OPCODE_READ_MEMORY on nodes and handler on controller
  - Memory address fixes are correct, but READ not functional until firmware updated
  
- **ncp (write)**: ✅ Memory addresses fixed, WRITE_MEMORY working
  - Can write to correct PSRAM locations
  - Already tested with neuron table deployment (uses same endpoint)

**Testing:**
```bash
# ncp - Should work (write uses existing endpoint)
python ncp local_file.bin 0/neurons  # Write to 0x00100000

# ncat - Won't work until READ_MEMORY implemented
python ncat 0/neurons -x              # Returns 0 bytes (missing handler)
python ncat 0@0x00100000 -l 64 -x     # Same issue
```

**Impact:**
- ncp: Ready to use for writing data files to PSRAM
- ncat: Address fix complete, but needs firmware READ_MEMORY implementation

---

## Original GitHub Repository Info

**Source:** https://github.com/ahtx/neurofab-z1-cluster  
**Version:** 3.0 (Production Ready)  
**Release Date:** November 13, 2025  
**Status:** ✅ QA Verified - Production Ready  

### Original Features (from GitHub README)

**Tools List (8 total):**
- `nls` - List all nodes
- `nping` - Ping specific node  
- `ncat` - Display node memory
- `ncp` - Copy file to node
- `nstat` - Cluster status
- `nconfig` - Manage configuration
- `nsnn` - SNN management (deploy/start/stop)
- `nflash` - Flash firmware

**Original Documentation:**
- QA_VERIFICATION_REPORT.md - 42 functions tested
- USER_GUIDE.md - Hardware setup guide
- API_REFERENCE.md - 21 HTTP API endpoints
- ARCHITECTURE.md - System design
- SNN_GUIDE.md - SNN implementation
- DEVELOPER_GUIDE.md - Firmware development

**Original Hardware Target:**
- Reference neuromorphic platform
- Memory: SRAM @ 0x20000000
- Multi-backplane support (200+ nodes)
- Matrix bus communication

**Changes Required for RP2350:**
1. Memory addresses (0x20000000 → 0x00100000)
2. API endpoint paths (/snn/* → /api/snn/*)
3. Backplane fallback logic (for single-backplane)
4. Tool paths adjusted for local deployment

All modifications documented above maintain compatibility with original tool interfaces and behavior.

---

## Testing Status - December 11, 2025

### ✅ WORKING TOOLS (Verified)

**nls** - List cluster nodes
- Status: ✅ FULLY WORKING
- Tested: `python nls -c 192.168.1.222`
- Output: Lists all 16 nodes with memory/uptime
- No modifications needed

**nping** - Ping cluster nodes
- Status: ✅ FULLY WORKING
- Tested: `python nping -c 192.168.1.222 0`
- Output: 4 packets transmitted, 0% loss, 28-30ms RTT
- No modifications needed

**nsnn** - SNN management
- Status: ✅ FULLY WORKING (with modifications above)
- Tested: `python nsnn -c 192.168.1.222 status`
- Output: Shows topology, neuron count, distribution
- Modifications: Backplane fallback logic added (see above)

**nstat** - Cluster statistics
- Status: ✅ FULLY WORKING
- Tested: `python nstat -c 192.168.1.222`
- Output: Full cluster status with all nodes, memory, uptime, LEDs
- No modifications needed

### ⚠️ PARTIALLY WORKING (Memory Addresses Fixed, READ Not Implemented)

**ncp** - Copy files to node memory
- Status: ⚠️ WRITE WORKING, addresses corrected
- Memory map fixed: Dec 11, 2025 (0x20000000 → 0x00100000 for neurons)
- Write endpoint: ✅ Working (POST /nodes/{id}/memory - tested via deployment)
- Usage: `python ncp local_file.bin 0/neurons` writes to 0x00100000
- **Ready to use for PSRAM uploads**

**ncat** - Display node memory contents
- Status: ⚠️ ADDRESSES FIXED, READ NOT IMPLEMENTED
- Memory map fixed: Dec 11, 2025 (same as ncp)
- Read endpoint: ❌ Returns 0 bytes (GET /nodes/{id}/memory not implemented in controller)
- Would require: OPCODE_READ_MEMORY handler in controller firmware
- **Cannot use until READ_MEMORY opcode added to firmware**

### ⚠️ NEEDS VERIFICATION

**nconfig** - Manage cluster configuration
- Purpose: Create/edit cluster_config.json for multi-backplane setups
- Status: Not tested (not critical for single backplane)
- Usage: `nconfig init cluster.json`
- Impact: Optional - only needed for multi-backplane clusters

### ❌ OUT OF SCOPE

**nflash** - Firmware flashing utility
- User confirmed: Out of scope
- Not tested

---

## Summary - December 11, 2025

**Critical Tools Status:**
- ✅ **nls**: Working perfectly
- ✅ **nping**: Working perfectly  
- ✅ **nsnn**: Working (with backplane fallback fix)
- ✅ **nstat**: Working perfectly

**Additional Tools:**
- ✅ **ncp**: Memory addresses fixed, WRITE working
  - Tested: Wrote 88 bytes to 0x00400000 successfully
  - Command: `python ncp test_file.txt 0/data`
  - Ready for production use
- ⚠️ **ncat**: Memory addresses fixed, READ not implemented
  - Controller firmware missing GET /nodes/{id}/memory handler
  - Would require OPCODE_READ_MEMORY implementation
  - Not critical for current operations
- ⚠️ **nconfig**: Not tested (multi-backplane only, not critical)
- ❌ **nflash**: Out of scope

**Files Modified:**
1. `bin/ncp` - Lines 18-25: MEMORY_LOCATIONS updated for RP2350 PSRAM
2. `bin/ncat` - Lines 18-25: MEMORY_LOCATIONS updated for RP2350 PSRAM

**Test Results:**
```bash
# Working tools
python nls -c 192.168.1.222              # ✅ Lists 16 nodes
python nping -c 192.168.1.222 0          # ✅ 28-30ms RTT, 0% loss
python nsnn -c 192.168.1.222 status      # ✅ Shows topology/stats
python nstat -c 192.168.1.222            # ✅ Full cluster status
python ncp test_file.txt 0/data          # ✅ Wrote 88 bytes to 0x00400000

# Not functional (missing READ_MEMORY)
python ncat 0/neurons -x                 # ❌ Returns 0 bytes
```

**Recommendation:**
All critical monitoring/control tools are fully functional. ncp works for writing data to PSRAM. ncat would require firmware enhancement (READ_MEMORY opcode) if memory inspection capability is needed in future.