# Node Reset Testing Guide

**Date:** January 15, 2026  
**Purpose:** Test individual node software reset for OTA preparation

---

## Changes Made

✅ **Default to Software Reset** (even on V2)
- Allows individual node testing
- Hardware reset available via `?mode=hardware` when needed

✅ **Single Node Reset Support**
- New parameter: `-n <node_id>` or `?node=<id>`
- Resets only specified node
- Perfect for OTA testing (reset node 0, update it, reset node 1, update it, etc.)

---

## Test Plan

### Step 1: Flash Firmware

```bash
# Build firmware
python build.py

# Flash controller
# Copy FirmwareReleases/16node/controller_16.uf2 to controller Pico (BOOTSEL mode)

# Flash node 0
# Copy FirmwareReleases/16node/node_16.uf2 to node 0 Pico (BOOTSEL mode)

# Flash node 1
# Copy FirmwareReleases/16node/node_16.uf2 to node 1 Pico (BOOTSEL mode)
```

### Step 2: Verify Cluster Communication

```bash
# Discover active nodes
python python_tools/bin/nls -c 192.168.1.222

# Expected output:
# Node 0: online (uptime: 10s, memory: 8MB)
# Node 1: online (uptime: 10s, memory: 8MB)
```

### Step 3: Test Single Node Reset

```bash
# Reset node 0 only
python python_tools/bin/nreset -c 192.168.1.222 -n 0

# Expected output:
# Resetting cluster nodes via software (node 0 only)...
# Controller: 192.168.1.222
# ✓ Reset successful
#   Method: software
#   Nodes: 0
#
# Node(s) will reboot into bootloader mode.
# Bootloader will wait 5 seconds before launching application.
# Use OTA tools during this window to update firmware.
```

**What to observe on node 0:**
- USB serial: `[CMD] RESET_TO_BOOTLOADER from node 16`
- USB serial: `[RESET] Rebooting into bootloader in 100ms...`
- LED: Brief off → Red (bootloader) → 5 second pause → RGB color (app running)
- USB serial (after reboot): `Z1 Onyx Bootloader v1.0.0`
- USB serial: `[BOOT] Validating application partition...`
- USB serial: `[BOOT] App header valid:` (shows name, version, size)
- USB serial: `[BOOT] CRC32 valid ✓`
- USB serial: `Starting application in 5 seconds...`
- USB serial: Countdown (5, 4, 3, 2, 1)
- USB serial: `[BOOT] Performing direct jump to app...`
- Node rejoins cluster

**What to observe on node 1:**
- No change (continues running normally)
- Still responds to `nls` command

### Step 4: Test Reset Node 1

```bash
# Reset node 1 only
python python_tools/bin/nreset -c 192.168.1.222 -n 1

# Node 1 should reboot through same sequence as node 0
# Node 0 should remain running
```

### Step 5: Test Reset All Nodes

```bash
# Reset all nodes (software)
python python_tools/bin/nreset -c 192.168.1.222

# Expected output:
# Resetting cluster nodes via software (default, all nodes)...
# Controller: 192.168.1.222
# ✓ Reset successful
#   Method: software
#   Nodes: all
```

**What to observe:**
- Both node 0 and node 1 reboot simultaneously
- Both go through bootloader sequence
- Both rejoin cluster after 5-second delay

### Step 6: Test Hardware Reset (V2 Only)

```bash
# Hardware reset (GPIO 33, resets ALL nodes)
python python_tools/bin/nreset -c 192.168.1.222 --hardware

# Expected output:
# Resetting cluster nodes via hardware (GPIO, all nodes)...
# Controller: 192.168.1.222
# ✓ Reset successful
#   Method: hardware
#   Nodes: all
```

**What to observe:**
- Instant reset (no ACK transmission delay)
- Both nodes reboot simultaneously
- Faster than software reset (~100ms vs ~200ms)

---

## Expected Timing

| Reset Type | Command to Reboot | Bootloader Window | App Start | Total |
|------------|-------------------|-------------------|-----------|-------|
| Software (single node) | ~200ms | 5000ms | instant | ~5.2s |
| Software (all nodes) | ~200ms | 5000ms | instant | ~5.2s |
| Hardware (V2 only) | ~100ms | 5000ms | instant | ~5.1s |

**Key Point:** 5-second bootloader window is the same regardless of reset method.

---

## Common Issues & Solutions

### Issue: Node doesn't respond to reset command

**Symptoms:**
- `nreset` returns success but node doesn't reboot
- USB serial shows no RESET_TO_BOOTLOADER message

**Causes:**
1. Node not running application firmware (still in bootloader)
2. Matrix bus communication failure
3. Node ID mismatch

**Solutions:**
```bash
# 1. Check if node is online
python python_tools/bin/nls -c 192.168.1.222

# 2. Ping specific node
python python_tools/bin/nping -c 192.168.1.222 -n 0

# 3. Check USB serial output for error messages

# 4. If node is stuck in bootloader, manually flash application firmware
```

### Issue: Node resets but doesn't boot to app

**Symptoms:**
- Node reboots to bootloader
- Red LED blinks continuously
- USB serial: `[BOOT] CRC32 mismatch` or `[BOOT] Invalid magic`

**Causes:**
- Corrupted application partition
- Firmware not flashed correctly

**Solutions:**
```bash
# Re-flash node firmware
# 1. Put node in BOOTSEL mode
# 2. Copy FirmwareReleases/16node/node_16.uf2
# 3. Wait for reboot
# 4. Verify with: python python_tools/bin/nls -c 192.168.1.222
```

### Issue: Python tool not found

**Symptoms:**
- `nreset: command not found` or similar

**Solutions:**
```bash
# Use relative path from project root
python python_tools/bin/nreset -c 192.168.1.201 -n 0

# Or add to PATH (PowerShell - from project root)
$env:PATH += ";$PWD\python_tools\bin"
nreset -c 192.168.1.201 -n 0
```

---

## Success Criteria

✅ **Single node reset works:**
- Node 0 resets independently
- Node 1 remains running
- Node 0 rejoins cluster after 5 seconds

✅ **All nodes reset works:**
- Both nodes reset simultaneously
- Both rejoin cluster after 5 seconds

✅ **Hardware reset works (V2):**
- Faster than software reset
- All nodes reset via GPIO pin

✅ **Bootloader validation works:**
- CRC32 check passes
- Application launches after 5-second window
- Nodes rejoin cluster

---

## Next Steps (OTA Testing)

Once reset testing passes:

1. **Build OTA firmware package**
   ```bash
   python build_dual.py  # Builds with dual-partition support
   python python_tools/bin/z1pack create xor_snn build/node/node_app_16.bin
   ```

2. **Create `nota` tool** (Python)
   - Upload firmware to controller
   - Start OTA session
   - Transfer chunks
   - Verify CRC32
   - Commit to flash
   - Restart node

3. **Test single-node OTA**
   ```bash
   nreset -c 192.168.1.222 -n 0  # Reset node 0
   nota update 0 firmware.z1app -c 192.168.1.222  # Update node 0
   # Repeat for other nodes
   ```

4. **Test multi-node OTA**
   ```bash
   nreset -c 192.168.1.222  # Reset all nodes
   nota update all firmware.z1app -c 192.168.1.222  # Parallel update
   ```

---

## Quick Reference Commands

```bash
# Discover nodes
nls -c 192.168.1.222

# Reset single node
nreset -c 192.168.1.222 -n 0

# Reset all nodes (software, default)
nreset -c 192.168.1.222

# Reset all nodes (hardware, V2 only)
nreset -c 192.168.1.222 --hardware

# Ping node
nping -c 192.168.1.222 -n 0

# Get node status
curl http://192.168.1.222/api/nodes/0
```

---

**Ready to test! Flash controller + nodes 0-1 and run through the test sequence above.**
