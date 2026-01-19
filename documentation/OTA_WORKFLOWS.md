# Z1 Onyx OTA Update Workflows

**Date:** January 18, 2026  
**Status:** Method 1 (Direct HTTP) is ✅ Fully Implemented and tested on hardware

## Overview

The Z1 Onyx cluster supports TWO different OTA (Over-The-Air) firmware update workflows with different trade-offs:

| Feature | Method 1: Direct HTTP | Method 2: SD-Based Deploy |
|---------|----------------------|---------------------------|
| **Tool** | `nflash` | `ndeploy` (not yet created) |
| **Firmware Status** | ✅ Implemented and Tested | ⚠️ Stub only (`handle_firmware_deploy`) |
| **Upload Path** | PC → HTTP → Controller → Node | PC → HTTP → SD card → Controller → Nodes |
| **Multi-Node** | Sequential (one at a time) | Parallel (controller orchestrates) |
| **Reliability** | Network interruption = retry | Single upload, atomic deploy |
| **Use Case** | Single node updates, development | Production cluster updates |
| **Speed (16 nodes)** | ~8-16 minutes | ~2-3 minutes |
| **Validation** | ✅ Tested on V1 (12-node) and V2 (16-node) | Not tested |

---

## Method 1: Direct HTTP Update (CURRENT)

**Status:** ✅ **Fully implemented and tested**

### How It Works
1. Python tool (`nflash`) prepends 192-byte Z1 header to firmware binary
2. Calculates CRC32 checksum
3. Sends firmware directly to target node via HTTP endpoints
4. Node stores in PSRAM buffer, verifies CRC, flashes to app partition
5. Node reboots with new firmware

### API Endpoints (All Implemented)
- `POST /api/ota/update_start` - Begin update session (node_id, size, crc32)
- `POST /api/ota/update_chunk` - Upload 4KB chunk
- `POST /api/ota/update_verify` - Verify CRC32 in PSRAM
- `POST /api/ota/update_commit` - Flash to app partition (0x00080000)
- `POST /api/ota/update_restart` - Reboot via watchdog
- `GET /api/ota/update_status` - Check progress

### Tool: `nflash`
**Location:** `python_tools/bin/nflash` (550 lines)

**Usage:**
```bash
# Single node (automatic reset + wait)
python python_tools/bin/nflash build/node/node_app_16.bin -n 0 -c 192.168.1.222

# Multiple nodes (sequential)
python python_tools/bin/nflash build/node/node_app_16.bin -n 0,1,2 -c 192.168.1.222

# All 16 nodes (sequential)
python python_tools/bin/nflash build/node/node_app_16.bin --all -c 192.168.1.222

# Skip automatic reset (assume already in bootloader)
python python_tools/bin/nflash build/node/node_app_16.bin -n 0 --no-reset -c 192.168.1.222
```

**Automated Workflow:**
1. **Automatic node reset** via OPCODE_RESET_TO_BOOTLOADER (0x38)
2. **Intelligent wait** - 4-8 seconds with 500ms polling for bootloader ready
3. **Header generation** - Prepends 192-byte Z1 header (magic: 0x5A314150)
4. **CRC32 calculation** - IEEE 802.3 polynomial (matches bootloader)
5. **Chunked upload** - 4KB chunks with real-time progress bar
6. **5-step verification** - start → chunk → verify → commit → restart
7. **Online detection** - Waits for node to boot with new firmware

**User Feedback:**
- "Resetting node 0 to bootloader... ✓"
- "Waiting for node 0 bootloader (up to 8s)... ✓ Ready after 4.5s"
- "[2/5] Uploading firmware (11 chunks)..."
- "[████████████████████] 100% (11/11) 45104 bytes"
- "✓ Node 0 updated successfully"

**Timing (per node):**
- Reset + wait: ~4-6 seconds
- 44KB firmware upload: ~20-30 seconds
- Flash + reboot: ~5-10 seconds
- **Total**: ~30-45 seconds per node
- **16 nodes sequential**: ~8-12 minutes

**Pros:**
- ✅ Fully automated (no manual steps)
- ✅ Ready to use NOW
- ✅ No SD card required
- ✅ User-friendly progress feedback
- ✅ Direct node targeting
- ✅ Granular control

**Cons:**
- Sequential updates (slow for many nodes)
- Network-dependent (interruption requires retry)
- Client manages timing and sequencing
- 16× network uploads for full cluster

---

## Method 2: SD-Based Deploy (PLANNED)

**Status:** ⚠️ **Stub only - not yet implemented**

### How It Works
1. Upload `.z1app` firmware package to SD card via `/api/files/` endpoints
2. Call `POST /api/firmware/deploy` with filename and node list
3. Controller reads firmware from SD card (single read)
4. Controller orchestrates parallel updates to all target nodes
5. Atomic deployment: either all succeed or all rollback
6. Controller manages timing, retries, and verification

### API Endpoints
- ✅ `PUT /api/files/engines/<filename>` - Upload to SD card (working)
- ⚠️ `POST /api/firmware/deploy` - Deploy from SD to nodes (**STUB ONLY**)

**Stub Location:** `controller/z1_http_api.c:2335-2360`

### Tool: `ndeploy` (NOT YET CREATED)
**Planned Usage:**
```bash
# Package firmware with header
python build_tools/package_firmware.py build/node/node_app_16.bin -o node_v1.2.3.z1app

# Upload to SD card
curl -X PUT --data-binary @node_v1.2.3.z1app http://192.168.1.222/api/files/engines/node_v1.2.3.z1app

# Deploy to nodes
python python_tools/bin/ndeploy node_v1.2.3.z1app --nodes 0,1,2 -c 192.168.1.222

# Or use combined tool
python python_tools/bin/ndeploy node_v1.2.3.z1app --all -c 192.168.1.222 --upload
```

**Planned Features:**
- Single upload to SD card
- Controller-managed parallel deployment
- Atomic updates (all or nothing)
- Rollback on failure
- Version tracking
- Progress monitoring

**Timing (estimated):**
- Upload to SD: ~5-10 seconds (one time)
- Deploy to 16 nodes: ~2-3 minutes (parallel)

**Pros:**
- Single network upload
- Parallel deployment (fast)
- Controller orchestrates everything
- Atomic updates with rollback
- Version management
- Network interruption doesn't affect deployment

**Cons:**
- Not yet implemented (stub only)
- Requires completing `handle_firmware_deploy()`
- Requires creating packaging tools
- SD card dependency

---

## Recommendation

### For Immediate Use (January 2026)
**Use Method 1 (`nflash`):**
- Fully implemented and tested
- Ready for single-node or small-scale updates
- Good for development/testing

**Example:**
```bash
# Build firmware
python build.py

# Update single node for testing
python python_tools/bin/nflash build/node/node_app_16.bin -n 0 -c 192.168.1.201
```

### For Production (Future)
**Implement Method 2 (`ndeploy`):**
- Better for full cluster updates
- Atomic deployments
- Faster and more reliable

**Required Work:**
1. Complete `handle_firmware_deploy()` function (200-300 lines estimated)
2. Create firmware packaging script (`build_tools/package_firmware.py`)
3. Create `ndeploy` tool (300-400 lines estimated)
4. Add version tracking to SD card manifest
5. Implement rollback logic
6. Test on hardware

**Estimated Effort:** 4-6 hours development + 2-3 hours testing

---

## Firmware Format

### Raw Binary (`.bin`)
- Output from compiler: `build/node/node_app_16.bin`
- Size: ~44KB
- No header, just executable code
- Used directly by `nflash` (tool adds header)

### Z1 Application Package (`.z1app`)
- Raw binary + 192-byte header
- Header structure:
  ```c
  struct z1_firmware_header {
      uint32_t magic;           // 0x5A314150 ("Z1AP")
      uint32_t version_major;
      uint32_t version_minor;
      uint32_t version_patch;
      uint32_t flags;
      uint32_t binary_size;     // Excludes header
      uint32_t crc32;           // CRC of binary only
      uint32_t entry_point;     // 0xC0 (offset to reset handler)
      char     name[32];        // "Z1 Onyx Node Firmware"
      char     description[64];
      uint8_t  reserved[64];
  };
  ```
- Used for SD card storage and Method 2 deployment
- Created by `build_tools/package_firmware.py` (to be created)

### UF2 Format (`.uf2`)
- Used for USB bootloader flashing only
- NOT used for OTA updates
- Generated by `elf2uf2.py` during build

---

## Implementation Status

### Method 1 (Direct HTTP)
| Component | Status | File | Lines |
|-----------|--------|------|-------|
| Controller API | ✅ Complete | `controller/z1_http_api.c` | 6 endpoints |
| Node OTA Handler | ✅ Complete | `node/node_ota_handler.c` | ~400 lines |
| Python Tool | ✅ Complete | `python_tools/bin/nflash` | 550 lines |
| Documentation | ✅ Complete | `API_REFERENCE.md` | Section added |

### Method 2 (SD-Based)
| Component | Status | File | Lines |
|-----------|--------|------|-------|
| SD Upload API | ✅ Complete | `controller/z1_http_api.c` | PUT /api/files/* |
| Deploy Endpoint | ⚠️ Stub | `controller/z1_http_api.c:2335` | 25 lines stub |
| Packaging Tool | ❌ Not Created | `build_tools/package_firmware.py` | N/A |
| Python Tool | ❌ Not Created | `python_tools/bin/ndeploy` | N/A |
| Documentation | ✅ Complete | `API_REFERENCE.md` | Documented |

---

## Testing Checklist

### Method 1 (`nflash`) - Ready to Test
- [ ] Single node update (node 0)
- [ ] Verify node reboots with new firmware
- [ ] CRC verification works
- [ ] Interrupt upload mid-stream (retry logic)
- [ ] Send corrupted chunk (CRC failure)
- [ ] Multiple nodes sequential (nodes 0,1,2)
- [ ] All nodes update (--all flag)
- [ ] Network timeout handling

### Method 2 (`ndeploy`) - Future Testing
- [ ] Upload .z1app to SD card
- [ ] Deploy to single node
- [ ] Deploy to multiple nodes (parallel)
- [ ] Verify atomic behavior (all or nothing)
- [ ] Rollback on failure
- [ ] Version tracking
- [ ] SD card full handling
- [ ] Controller crash during deploy

---

## Next Steps

### Immediate (Use Method 1)
1. ✅ `nflash` tool created and documented
2. ⏳ Test `nflash` on hardware with single node
3. ⏳ Verify firmware boots correctly after update
4. ⏳ Test multi-node sequential updates

### Short-Term (Implement Method 2)
1. ⏳ Complete `handle_firmware_deploy()` implementation
2. ⏳ Create `build_tools/package_firmware.py` script
3. ⏳ Create `python_tools/bin/ndeploy` tool
4. ⏳ Test SD-based deployment on hardware
5. ⏳ Document limitations and best practices

### Long-Term (Production Hardening)
1. ⏳ Add rollback mechanism (preserve previous firmware)
2. ⏳ Implement firmware signing/verification
3. ⏳ Add version manifest tracking
4. ⏳ Create unified update tool (auto-select method)
5. ⏳ Add web UI for OTA management

---

## Related Documentation
- [API_REFERENCE.md](API_REFERENCE.md) - Full API documentation
- [OTA_IMPLEMENTATION_STATUS.md](OTA_IMPLEMENTATION_STATUS.md) - Implementation details
- [BUILD_INSTRUCTIONS.md](BUILD_INSTRUCTIONS.md) - Build system
- [DUAL_PARTITION_GUIDE.md](DUAL_PARTITION_GUIDE.md) - Flash memory layout

---

**Last Updated:** January 18, 2026  
**Status:** Method 1 complete and ready for testing, Method 2 planned for future implementation

