# Z1 Onyx - Complete File Management Guide

## Overview

The Z1 Onyx controller provides a complete HTTP-based file management system for uploading, downloading, listing, and deleting files on the SD card. This system supports **unlimited file sizes** through streaming (tested up to 1MB+) and is the foundation for OTA firmware updates.

**Status**: Production-ready (tested and validated with 2KB to 1MB files)

---

## Quick Start

### Prerequisites
- Controller connected to network at 192.168.1.222 (or custom IP)
- Python 3.7+ with `requests` library
- SD card formatted as FAT32 and inserted in controller

### Basic Operations

```bash
# List all topology files
python python_tools/bin/zengine list -c 192.168.1.222

# Upload a file
python python_tools/bin/zengine upload myfile.json -c 192.168.1.222

# Download a file
python python_tools/bin/zengine download myfile.json -c 192.168.1.222

# Delete a file
python python_tools/bin/zengine delete myfile.json -c 192.168.1.222
```

---

## Architecture

### Three-Layer System

```
┌─────────────────────────────────────────────────────────┐
│  Python Tools (zengine, z1pack)                         │
│  • High-level interface for users                       │
│  • Handles file I/O, chunking, SHA256 verification      │
└─────────────────────────────────────────────────────────┘
                           ▼ HTTP/JSON
┌─────────────────────────────────────────────────────────┐
│  Controller HTTP API (w5500_eth.c, z1_http_api.c)      │
│  • Streaming upload/download handlers                   │
│  • Directory listing with f_findfirst/f_findnext        │
│  • Automatic directory creation                         │
└─────────────────────────────────────────────────────────┘
                           ▼ FatFS
┌─────────────────────────────────────────────────────────┐
│  SD Card Storage (carlk3/no-OS-FatFS)                   │
│  • FAT32 filesystem                                     │
│  • SPI1 interface @ 12.5 MHz                            │
│  • PSRAM buffers for reliability                        │
└─────────────────────────────────────────────────────────┘
```

### Memory Layout

```
PSRAM (8MB @ 0x11000000):
├─ 0x11000000 - 0x1100FFFF (64KB)   FatFS working buffers
├─ 0x11010000 - 0x11010FFF (4KB)    HTTP response buffer
└─ 0x11020000 - 0x11020FFF (4KB)    File streaming chunk buffer

W5500 Buffers:
├─ RX Buffer: 2KB circular buffer per socket
└─ TX Buffer: 2KB circular buffer per socket
```

---

## Streaming Upload System

### How It Works

```
Client                      Controller                    SD Card
  │                              │                           │
  │─PUT /api/files/file.json────>│                           │
  │  Content-Length: 30912       │                           │
  │                              │                           │
  │─(Header + 1460 bytes)───────>│                           │
  │                              │──f_open(FA_WRITE)────────>│
  │                              │──f_write(1460 bytes)─────>│
  │                              │                           │
  │─(1460 bytes chunk)──────────>│                           │
  │                              │──f_write(1460 bytes)─────>│
  │                              │                           │
  │─(1460 bytes chunk)──────────>│                           │
  │                              │──f_write(1460 bytes)─────>│
  │                              │                           │
  │  ... (repeat until complete) │                           │
  │                              │                           │
  │                              │──f_sync()────────────────>│
  │                              │──f_close()───────────────>│
  │                              │                           │
  │<─{"success":true,"size":N}───│                           │
```

### Key Features

1. **Unlimited File Size**
   - No 4KB buffer limit - streams directly from network to SD
   - Tested: 2KB to 1MB with perfect integrity
   - Only uses ~6KB working memory for any file size

2. **Automatic Directory Creation**
   - Parses filepath for directory component
   - Calls `f_mkdir()` before `f_open()`
   - Ignores `FR_EXIST` error if directory exists
   - Example: `topologies/file.json` creates `topologies/` automatically

3. **Circular Buffer Handling**
   - W5500 RX buffer is 2KB circular (wraps at 0x0800)
   - Tracks local `stream_rd_ptr` to avoid double-consumption
   - Handles wraparound: reads to end, then from start

4. **Error Handling**
   - 5-second timeout per chunk
   - FatFS error codes logged to serial
   - Returns JSON error if upload fails

### Implementation Details

**Location**: `controller/w5500_eth.c` lines ~720-880

**Algorithm**:
```c
1. Parse Content-Length header
2. Calculate header_size and body_in_buffer
3. Extract directory from filepath (if contains '/')
4. Create directory: f_mkdir(dirpath) [ignore FR_EXIST]
5. Open file: f_open(filepath, FA_WRITE | FA_CREATE_ALWAYS)
6. Write initial chunk from request buffer
7. Loop while remaining > 0:
   a. Wait for data in RX buffer (5s timeout)
   b. Read chunk_size (min of available, 2048, remaining)
   c. Handle circular buffer wraparound
   d. w5500_read_buffer() into PSRAM chunk buffer
   e. f_write() to SD card
   f. Update stream_rd_ptr and W5500 Sn_RX_RD registers
8. f_sync() to ensure data written to SD
9. f_close()
10. Return JSON: {"success":true,"size":N}
```

**Critical Bug Fixes Applied**:
- Use local `stream_rd_ptr` instead of re-reading from W5500 registers
- Consume RX data progressively (not all at once at start)
- Move chunk buffer to 0x11020000 (was 0x11008000, overlapped HTTP buffer)
- Reset HTTP metadata after upload to prevent leak from previous downloads

---

## Streaming Download System

### How It Works

```
Client                      Controller                    SD Card
  │                              │                           │
  │─GET /api/files/file.json────>│                           │
  │                              │                           │
  │                              │──f_stat(check AM_DIR)────>│
  │                              │<─Not a directory──────────│
  │                              │                           │
  │                              │──f_open(FA_READ)─────────>│
  │                              │──f_size()────────────────>│
  │                              │<─30912 bytes──────────────│
  │                              │                           │
  │<─HTTP/1.1 200 OK─────────────│                           │
  │<─Content-Length: 30912───────│                           │
  │<─Content-Type: application/──│                           │
  │   octet-stream               │                           │
  │                              │                           │
  │<─(1024 bytes)────────────────│<─f_read(1024)─────────────│
  │<─(1024 bytes)────────────────│<─f_read(1024)─────────────│
  │  ... (repeat 30 times)       │                           │
  │<─(864 bytes final)───────────│<─f_read(864)──────────────│
  │                              │                           │
  │                              │──f_close()───────────────>│
```

### Key Features

1. **Streaming Chunks**
   - 1KB chunks (smaller than upload for reliability)
   - 5ms delay between chunks for network pacing
   - Reads directly from SD to PSRAM to W5500

2. **Directory Detection**
   - Uses `f_stat()` with `AM_DIR` flag
   - If path is directory, skips streaming → falls through to API listing handler
   - This allows `GET /api/files/topologies` to list instead of download

3. **Early Return**
   - Bypasses normal HTTP response system
   - Sends headers and body manually
   - Consumes RX data and returns immediately

### Implementation Details

**Location**: `controller/w5500_eth.c` lines ~630-710

**Algorithm**:
```c
1. Check if path is directory: f_stat(filepath, &fno)
2. If (fno.fattrib & AM_DIR), goto normal_routing for listing
3. Open file: f_open(filepath, FA_READ)
4. Get file size: f_size(&fil)
5. Send HTTP headers with Content-Length
6. Loop while bytes_sent < file_size:
   a. Calculate bytes_to_read (min of 1024, remaining)
   b. f_read() into PSRAM chunk buffer
   c. w5500_send_response_len() to send chunk
   d. sleep_ms(5) for pacing
7. f_close()
8. Consume RX data and return early
```

---

## Directory Listing System

### How It Works

```
Client                      Controller                    SD Card
  │                              │                           │
  │─GET /api/files/topologies───>│                           │
  │                              │                           │
  │                              │──f_stat("topologies")────>│
  │                              │<─AM_DIR flag set──────────│
  │                              │  (skip streaming)         │
  │                              │                           │
  │                              │──f_findfirst(topologies)─>│
  │                              │<─file1.json (2149 bytes)──│
  │                              │──f_findnext()────────────>│
  │                              │<─file2.json (30912 bytes)─│
  │                              │──f_findnext()────────────>│
  │                              │<─(no more files)──────────│
  │                              │                           │
  │<─{"files":[{"name":"file1──  │                           │
  │   .json","size":2149}, ...]} │                           │
```

### Key Features

1. **Reliable Iteration**
   - Uses `f_findfirst/f_findnext` pattern (most reliable)
   - Stack-allocated DIR/FILINFO structures (no PSRAM timing issues)
   - Safety limit: 1000 entries max to prevent infinite loops

2. **Filtering**
   - Skips directories (AM_DIR)
   - Skips hidden/system files (AM_HID, AM_SYS)
   - Skips dot files (`.`, `..`, `.hidden`)
   - Validates first character is alphanumeric/underscore
   - Rejects files >100MB (likely corrupt FAT entries)

3. **Empty Directory Handling**
   - Returns `{files: [], count: 0}` if directory doesn't exist
   - Not treated as error - allows creating on first upload

### Implementation Details

**Location**: `common/sd_card/sd_card.c` lines ~220-320

**Algorithm**:
```c
1. f_findfirst(&dir, &fno, dirpath, "*")
2. If (fr != FR_OK), return -1 (directory doesn't exist)
3. While (fr == FR_OK && fno.fname[0]):
   a. Skip if (fno.fattrib & AM_DIR) [directory]
   b. Skip if (fno.fattrib & (AM_HID | AM_SYS)) [hidden/system]
   c. Skip if fno.fname[0] == '.' [dot files]
   d. Skip if first char not alphanumeric/underscore
   e. Skip if fno.fsize > 100MB [corrupt]
   f. Call callback(fno.fname, fno.fsize)
   g. f_findnext(&dir, &fno)
4. f_closedir(&dir)
5. Return count
```

---

## Python Tools

### zengine - Topology/Engine File Manager

**Location**: `python_tools/bin/zengine`

**Commands**:

```bash
# List files
zengine list [-c CONTROLLER_IP]

# Upload file
zengine upload FILE [-c CONTROLLER_IP] [--name REMOTE_NAME]

# Download file
zengine download REMOTE_NAME [-c CONTROLLER_IP]

# Delete file
zengine delete REMOTE_NAME [-c CONTROLLER_IP]
```

**Features**:
- Automatically prepends `topologies/` directory
- SHA256 verification on download (checks if response is JSON error vs binary data)
- Progress indication with file size
- Handles files up to 1MB+ with streaming

**Directory**: Hardcoded to `topologies/` (can be modified for engines/)

### z1pack - Engine Binary Packager

**Location**: `python_tools/bin/z1pack`

**Commands**:

```bash
# Package binary into .z1app format
z1pack -i INPUT.bin -o OUTPUT.z1app --name "APP_NAME" \
  --version "1.0.0" --description "Description text"

# Inspect package contents
z1pack --inspect PACKAGE.z1app
```

**Package Format**:
```
Offset      Size        Description
------      ----        -----------
0x000       192 bytes   app_header_t structure
0x0C0       Variable    Application binary (raw ARM code)
```

**Header Fields** (app_header_t):
- Magic: 0x5A314150 ("Z1AP")
- Version: MAJOR.MINOR.PATCH (3 × uint32_t)
- Size: Binary size excluding header
- CRC32: CRC32 of binary only (excludes header)
- Entry Offset: 0xC0 (192 bytes after start)
- Timestamp: Unix epoch build time
- Name: 32-byte string (null-terminated)
- Description: 64-byte string (null-terminated)
- Reserved: 64 bytes (future metadata)

**Total Header Size**: 192 bytes (8 uint32_t + 32 + 64 + 64)

**Entry Point Calculation**:
```
Flash Base:    0x00200000  (OTA partition 1)
Header:        + 0x0000C0  (192 bytes)
--------------------------------------
Entry Point:   = 0x002000C0  (CPU starts here after bootloader)
```

---

## Testing

### Test Files

Located in `packages/`:
- `xor_snn_v1.0.0.z1app` - 30.9KB test engine binary
- Comprehensive test suite in `packages/xor_snn_v1.0.0/`

### Upload/Download Test

```bash
# Upload test package
python python_tools/bin/zengine upload packages/xor_snn_v1.0.0.z1app -c 192.168.1.222

# Download for verification
python python_tools/bin/zengine download xor_snn_v1.0.0.z1app -c 192.168.1.222

# Verify integrity (PowerShell)
$orig = Get-FileHash packages/xor_snn_v1.0.0.z1app -Algorithm SHA256
$down = Get-FileHash xor_snn_v1.0.0.z1app -Algorithm SHA256
$orig.Hash -eq $down.Hash  # Should be True
```

### Inspect Package

```bash
python python_tools/bin/z1pack --inspect packages/xor_snn_v1.0.0.z1app
```

Expected output:
```
=== Application Header ===

  Magic:       0x5A314150 (Z1AP)
  Version:     1.0.0
  Name:        XOR_SNN
  Description: XOR logic gate using LIF neurons
  Binary Size: 30,720 bytes
  Entry Point: 0x000000C0 (192 bytes)
  CRC32:       0xD2D0F262 ✓ VALID
  
  Total Size:  30,912 bytes
               (header: 192 + binary: 30,720)
```

### Tested File Sizes

| Size | Status | SHA256 Verified | Notes |
|------|--------|-----------------|-------|
| 2.1KB | ✅ | ✅ | xor_working.json |
| 4KB | ✅ | ✅ | testignore4K.json |
| 8KB | ✅ | ✅ | testignore8K.json |
| 16KB | ✅ | ✅ | testignore16K.json |
| 30.9KB | ✅ | ✅ | xor_snn_v1.0.0.z1app (engine) |
| 64KB | ✅ | ✅ | testignore64K.json |
| 256KB | ✅ | ✅ | testignore256K.json |
| 1MB | ✅ | ✅ | testignore1024K.json |

**Peak Memory Usage**: ~6KB for any file size

---

## API Reference

### GET /api/files/{directory}

**Description**: List all files in directory

**Response**:
```json
{
  "files": [
    {"name": "file1.json", "size": 2149},
    {"name": "file2.z1app", "size": 30912}
  ],
  "count": 2
}
```

### GET /api/files/{directory}/{filename}

**Description**: Download file using streaming

**Response**: Raw binary/text file with Content-Length header

### PUT /api/files/{directory}/{filename}

**Description**: Upload file using streaming

**Request Headers**:
- Content-Length: {file_size} (required)

**Request Body**: Raw binary/text file

**Response**:
```json
{
  "success": true,
  "size": 30912
}
```

### DELETE /api/files/{directory}/{filename}

**Description**: Delete file from SD card

**Response**:
```json
{
  "success": true
}
```

---

## Troubleshooting

### Upload Fails with "Failed to open file"

**Cause**: Parent directory doesn't exist

**Solution**: Already fixed! Code now creates directories automatically. Ensure SD card is mounted and has free space.

### Download Returns JSON Error Instead of File

**Cause**: File doesn't exist or is actually a directory

**Solution**: Check file path with `zengine list`. Ensure correct directory and filename.

### Directory Listing Returns Empty Array

**Cause**: Directory doesn't exist yet (not an error)

**Solution**: Upload a file to the directory - it will be created automatically.

### Serial Shows "FRESULT=4"

**Meaning**: FR_NO_FILE - File or directory not found

**Solution**: Check exact path. FatFS is case-sensitive on some systems.

### File Corruption After Upload

**Cause**: RX buffer double-consumption or incorrect pointer tracking

**Solution**: Already fixed! Code now uses local `stream_rd_ptr` and consumes data progressively.

---

## Future Enhancements

### OTA Phase 0 (Next Step)

**Goal**: Deploy engine binaries to compute nodes via Matrix bus

**Implementation**:
1. Upload `.z1app` to controller SD card (✅ Done)
2. Controller reads package from SD
3. Broadcast UPDATE_MODE_ENTER to all nodes
4. Stream binary chunks via Matrix bus
5. Nodes write to flash partition 1 (0x00200000)
6. Validate CRC32 on each node
7. Broadcast REBOOT command
8. Bootloader validates header and jumps to new app

**Tools Needed**:
- `nsnn ota-update` command (wrapper around existing APIs)
- Node bootloader with flash programming support
- App header validation in bootloader

### Additional Features

- **Compression**: gzip compression for large files
- **Resume**: Partial upload/download support
- **Multi-part**: Chunked transfer encoding
- **Directory creation**: POST /api/files/{directory} endpoint
- **Recursive delete**: Delete directory and all contents

---

## References

- [STREAMING_FILE_SYSTEM.md](STREAMING_FILE_SYSTEM.md) - Technical implementation details
- [OTA_UPDATE_SPEC.md](OTA_UPDATE_SPEC.md) - Complete OTA system specification
- [packages/xor_snn_v1.0.0/](packages/xor_snn_v1.0.0/) - Test package documentation
- [API_REFERENCE.md](API_REFERENCE.md) - Full HTTP API reference

---

**Last Updated**: January 7, 2026  
**System Status**: Production-ready for file management and OTA preparation  
**Tested Capacity**: 2KB to 1MB with perfect integrity
