# Z1 Onyx OTA Troubleshooting Guide

**Date:** January 18, 2026  
**Status:** Operational  
**Hardware:** V1 (12-node) and V2 (16-node)

---

## Overview

This guide provides solutions to common issues encountered during Over-The-Air (OTA) firmware updates on the Z1 Onyx cluster. It covers both hardware variants and addresses known pitfalls related to the RP2350 watchdog mechanism.

---

## Table of Contents

1. [Common OTA Failures](#common-ota-failures)
2. [Watchdog-Related Issues](#watchdog-related-issues)
3. [Scratch Register Behavior](#scratch-register-behavior)
4. [Hardware-Specific Issues](#hardware-specific-issues)
5. [Network and Communication Issues](#network-and-communication-issues)
6. [Recovery Procedures](#recovery-procedures)

---

## Common OTA Failures

### Node Hangs After "Rebooting into bootloader..."

**Symptoms:**
- Node prints "Rebooting into bootloader in 100ms..."
- Node never resets
- Serial console stops responding
- Node disappears from `nls` output

**Root Cause:** Watchdog not enabled or not triggered correctly

**Solution:**
1. Verify firmware has watchdog enabled at startup:
   ```c
   #ifdef APP_PARTITION_MODE
       watchdog_enable(8000, false);  // Must be called once at init
   #endif
   ```

2. Verify watchdog is updated in main loop:
   ```c
   #ifdef APP_PARTITION_MODE
       watchdog_update();  // Prevent timeout
   #endif
   ```

3. Verify OTA reset handler triggers without re-enabling:
   ```c
   case Z1_OPCODE_RESET_TO_BOOTLOADER:
       // Write scratch register
       watchdog_hw->scratch[4] = 0xDEADBE00 | node_id;
       
       // Trigger already-enabled watchdog (DO NOT RE-ENABLE!)
       hw_set_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_TRIGGER_BITS);
       break;
   ```

4. If firmware is old (built before January 18, 2026), reflash with updated firmware

**Validation:** Node should reset within 100ms and appear in bootloader mode

---

### Node Reboots Automatically Every 8 Seconds

**Symptoms:**
- Node runs for exactly ~8 seconds
- Automatic reboot without command
- Happens repeatedly in a cycle
- Node works normally but unstable

**Root Cause:** Watchdog enabled but not updated in main loop

**Solution:**
Add `watchdog_update()` to main loop:
```c
void idle_node_loop(int my_node_id) {
    while (1) {
        z1_broker_process();
        z1_snn_step();
        
        #ifdef APP_PARTITION_MODE
            watchdog_update();  // <-- Add this line
        #endif
        
        tight_loop_contents();
    }
}
```

**Validation:** Node should run indefinitely without spontaneous reboots

---

### Scratch Register Corruption During OTA

**Symptoms:**
- Node writes `0xDEADBE0E` (correct) to scratch[4]
- Bootloader reads different value (e.g., `0x6AB73121`)
- OTA fails because bootloader can't determine node ID
- V2 nodes fall back to GPIO detection (works anyway)
- V1 nodes use placeholder ID 0 (breaks communication)

**Root Cause:** Watchdog re-enabled during reset, which clears scratch registers on RP2350

**Solution:**
**DO NOT** call any of these during OTA reset:
```c
// ❌ WRONG - These clear scratch registers:
watchdog_enable(1, false);
watchdog_reboot(0, 0, 0);
```

**DO** trigger already-enabled watchdog:
```c
// ✅ CORRECT - Preserves scratch registers:
hw_set_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_TRIGGER_BITS);
```

**Validation:** Bootloader should read correct scratch value (0xDEADBE00 | node_id)

---

## Watchdog-Related Issues

### Understanding RP2350 Watchdog Behavior

**Key Differences from RP2040:**
- RP2350: `watchdog_enable()` clears scratch registers
- RP2350: `watchdog_reboot()` clears scratch registers
- RP2040: Neither function clears scratch registers

**Correct Implementation Pattern:**
```c
// 1. Enable ONCE at startup
#ifdef APP_PARTITION_MODE
    watchdog_enable(8000, false);  // 8-second timeout
#endif

// 2. Update in main loop
while (1) {
    #ifdef APP_PARTITION_MODE
        watchdog_update();  // Keep watchdog alive
    #endif
    // ... do work ...
}

// 3. Trigger for OTA reset (do NOT re-enable)
watchdog_hw->scratch[4] = 0xDEADBE00 | node_id;
hw_set_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_TRIGGER_BITS);
```

---

## Scratch Register Behavior

### Empty Scratch After OTA Completion (NORMAL)

**Symptoms:**
- Node completes OTA successfully
- Bootloader reports `scratch value: 0x00000000` during final boot
- Node starts with correct ID anyway
- Everything works fine

**Explanation:** This is **EXPECTED BEHAVIOR**

**OTA Flow (Two Different Resets):**

**Reset #1: App → Bootloader (OTA Start)**
- App writes scratch[4] = 0xDEADBE00 | node_id
- App triggers watchdog (preserves scratch)
- Bootloader reads scratch[4] to get node ID
- Used for Matrix bus communication during firmware download

**Reset #2: Bootloader → App (OTA Completion)**
- Bootloader validates new firmware CRC
- Bootloader does NORMAL REBOOT (not watchdog)
- Scratch register is empty (cold boot)
- **V1**: App uses `NODE_ID_HARDCODED` (doesn't need scratch)
- **V2**: App reads GPIO pins (doesn't need scratch)

**Validation:** Node comes online with correct ID after OTA

---

### When to Worry About Scratch Register

**Worry if:**
- ✅ Scratch empty during OTA START (before firmware download)
- ✅ Bootloader can't communicate with controller during OTA
- ✅ OTA fails with "timeout" or "no response" errors

**Don't worry if:**
- ❌ Scratch empty after OTA COMPLETION (final reboot)
- ❌ Node uses hardcoded ID (V1) or GPIO ID (V2) after OTA
- ❌ Everything works fine

---

## Hardware-Specific Issues

### V1 Hardware (12-node)

**Issue:** Bootloader uses placeholder ID 0 on cold boot

**Why:** V1 has no hardware node ID detection

**Behavior:**
- OTA start: Bootloader reads scratch[4] for node ID (correct)
- OTA completion: Bootloader uses placeholder ID 0 (doesn't matter)
- App starts: Uses `NODE_ID_HARDCODED` from compile-time constant

**Solution:** This is normal. App doesn't rely on bootloader ID.

---

### V2 Hardware (16-node)

**Issue:** Node ID auto-detection via GPIO

**Pins:** GPIO 40-43 (4-bit binary)

**Behavior:**
- OTA start: Bootloader reads scratch[4] for node ID (faster)
- OTA completion: Bootloader reads GPIO for node ID (fallback)
- App starts: Reads GPIO for node ID (authoritative)

**Solution:** GPIO pins must be properly connected. Check for:
- Loose connections
- Incorrect pull-up/pull-down configuration
- Shorts between ID pins

**Validation:** Run `nls` to verify all nodes have correct IDs

---

## Network and Communication Issues

### OTA Upload Slow or Stalling

**Symptoms:**
- Upload starts but throughput below 50 Kbps
- Upload hangs partway through
- "Chunk N timeout" errors

**Solutions:**
1. **Check network latency:** Use `nping` to test round-trip time
2. **Reduce chunk size:** Edit nflash to use 2KB chunks instead of 4KB
3. **Increase timeout:** Edit `z1_http_api.c` OTA chunk timeout
4. **Check Matrix bus:** Run `nls` to verify all nodes responding
5. **Restart controller:** Power cycle if Matrix bus corrupted

---

### CRC Mismatch Errors

**Symptoms:**
- Upload completes
- Node reports "CRC32 mismatch"
- Firmware not flashed

**Solutions:**
1. **Verify firmware binary:** Check file size matches expected
2. **Re-upload:** Network corruption may have damaged chunks
3. **Check PSRAM:** Node may have bad PSRAM (rare)
4. **Serial console:** Check for memory errors during upload

---

## Recovery Procedures

### Node Won't Boot After OTA

**Symptoms:**
- Node accepts firmware
- Node doesn't appear in `nls` after reboot
- LED pattern shows error (if implemented)

**Recovery Steps:**
1. **Connect serial console** to see boot errors
2. **Check bootloader logs** for CRC failure messages
3. **Manual reflash via USB:**
   ```bash
   python flash_node.py --hw-v2  # V2 hardware
   python flash_node.py --hw-v1 --node 3  # V1 hardware
   ```
4. **Verify firmware binary** was built correctly
5. **Check for flash corruption** (very rare)

---

### Controller Loses Connection to Nodes

**Symptoms:**
- All nodes disappear from `nls`
- Controller LED shows normal operation
- Network ping works

**Recovery Steps:**
1. **Power cycle cluster** (hard reset)
2. **Check Matrix bus connections** (loose cables)
3. **Controller software reset:**
   ```bash
   python python_tools/bin/zconfig reboot -c 192.168.1.222
   ```
4. **Reflash controller if persistent:**
   ```bash
   python flash_controller.py --hw-v2
   ```

---

### OTA Works But Node Behavior Wrong

**Symptoms:**
- OTA completes successfully
- Node comes online with correct ID
- Node doesn't respond to commands
- SNN engine not working

**Solutions:**
1. **Verify firmware version** matches expected
2. **Check for compile errors** in firmware build
3. **Run test deployment:**
   ```bash
   python test_deployment.py -c 192.168.1.222
   ```
4. **Reflash known-good firmware** to verify hardware

---

## Diagnostic Commands

### Check Node Status
```bash
# List all nodes
python python_tools/bin/nls -c 192.168.1.222

# Get detailed statistics
python python_tools/bin/nstat -s -c 192.168.1.222

# Ping specific node
python python_tools/bin/nping -n 0 -c 192.168.1.222
```

### Check Firmware Version
Look for version string in serial console output:
```
[APP] Z1 Node App v1.0.0
[APP] Built: Jan 18 2026 15:30:45
[APP] Node ID: 5 (GPIO detected)
```

### Check OTA Session Status
```bash
# Via HTTP API
curl http://192.168.1.222/api/ota/status
```

---

## Known Issues and Limitations

### Sequential Updates Only
**Issue:** Can only update one node at a time  
**Workaround:** Use `nflash -n all` to automate sequential updates  
**Future:** Broadcast chunk support for parallel updates

### No Resume Support
**Issue:** Failed update must restart from beginning  
**Workaround:** Ensure stable network before starting OTA  
**Future:** Chunk bitmap persistence on SD card

### Controller Cannot Self-Update
**Issue:** Controller must be flashed via USB  
**Workaround:** Use `flash_controller.py` for USB flashing  
**Future:** Secondary controller partition for self-update

---

## Getting Help

### Before Reporting Issues

1. ✅ Check this troubleshooting guide
2. ✅ Verify firmware built with latest code (January 18, 2026+)
3. ✅ Test with single node first
4. ✅ Collect serial console logs from both controller and node
5. ✅ Note exact error messages and symptoms

### Information to Provide

- Hardware variant (V1 or V2)
- Number of nodes in cluster
- Firmware version (build date)
- Network configuration (controller IP)
- Error messages from serial console
- Steps to reproduce issue

### Contact

- GitHub Issues: https://github.com/texelec/neurofab-z1-cluster/issues
- Documentation: See [DOCUMENTATION_INDEX.md](DOCUMENTATION_INDEX.md)

---

## Quick Reference

### OTA Update Command
```bash
# Single node
python python_tools/bin/nflash -n 5 build/node/node_app_16.bin -c 192.168.1.222

# Multiple nodes
python python_tools/bin/nflash -n 0,1,2,3 build/node/node_app_16.bin -c 192.168.1.222

# All nodes
python python_tools/bin/nflash -n all build/node/node_app_16.bin -c 192.168.1.222
```

### Verify Deployment
```bash
python test_deployment.py -c 192.168.1.222
```

### Manual USB Flash (Recovery)
```bash
# V2 node
python flash_node.py --hw-v2

# V1 node
python flash_node.py --hw-v1 --node 3

# Controller
python flash_controller.py --hw-v2
```

---

**End of Guide**
