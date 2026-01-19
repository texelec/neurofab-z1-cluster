#!/usr/bin/env python3
"""
Prepend app header to node application binary

Creates a 192-byte header with magic, version, CRC32, etc.
Header format matches app_header_t in bootloader_main.c
"""

import sys
import struct
from pathlib import Path

# CRC32 lookup table (IEEE 802.3 polynomial 0xEDB88320)
CRC32_TABLE = None

def init_crc32_table():
    """Initialize CRC32 lookup table (matches bootloader implementation)"""
    global CRC32_TABLE
    if CRC32_TABLE is not None:
        return
    
    CRC32_TABLE = []
    for i in range(256):
        crc = i
        for j in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xEDB88320
            else:
                crc >>= 1
        CRC32_TABLE.append(crc)

def calculate_crc32(data):
    """Calculate CRC32 (same algorithm as bootloader)"""
    init_crc32_table()
    
    crc = 0xFFFFFFFF
    for byte in data:
        crc = (crc >> 8) ^ CRC32_TABLE[(crc ^ byte) & 0xFF]
    
    return (~crc) & 0xFFFFFFFF

def create_app_header(binary_data, name="Z1 Node App", version=(1, 0, 0)):
    """Create 192-byte app header"""
    
    magic = 0x5A314150  # "Z1AP"
    version_major, version_minor, version_patch = version
    flags = 0
    binary_size = len(binary_data)
    crc32 = calculate_crc32(binary_data)
    entry_point = 0xC0  # Standard RP2350 entry point
    
    # Pack header (192 bytes total)
    header = struct.pack('<IIIIIIII',
        magic,           # 0: magic
        version_major,   # 4: version_major
        version_minor,   # 8: version_minor
        version_patch,   # 12: version_patch
        flags,           # 16: flags
        binary_size,     # 20: binary_size
        crc32,           # 24: crc32
        entry_point      # 28: entry_point
    )
    
    # Add name (32 bytes, null-terminated)
    name_bytes = name.encode('utf-8')[:31] + b'\x00'
    name_bytes += b'\x00' * (32 - len(name_bytes))
    header += name_bytes
    
    # Add description (64 bytes, null-terminated)
    description = "Full node firmware - runs on top of bootloader"
    desc_bytes = description.encode('utf-8')[:63] + b'\x00'
    desc_bytes += b'\x00' * (64 - len(desc_bytes))
    header += desc_bytes
    
    # Add reserved (64 bytes)
    header += b'\x00' * 64
    
    assert len(header) == 192, f"Header must be 192 bytes, got {len(header)}"
    
    return header

def prepend_header(input_bin, output_bin, name="Z1 Node App", version=(1, 0, 0)):
    """Prepend header to binary file"""
    
    # Read input binary
    binary_data = Path(input_bin).read_bytes()
    print(f"Input binary: {len(binary_data)} bytes")
    
    # Create header
    header = create_app_header(binary_data, name, version)
    print(f"Header: 192 bytes")
    print(f"  Magic: 0x{struct.unpack('<I', header[0:4])[0]:08X}")
    print(f"  Version: {version[0]}.{version[1]}.{version[2]}")
    print(f"  Binary size: {len(binary_data)} bytes")
    print(f"  CRC32: 0x{struct.unpack('<I', header[24:28])[0]:08X}")
    print(f"  Entry point: 0x{struct.unpack('<I', header[28:32])[0]:08X}")
    
    # Write output with header
    with open(output_bin, 'wb') as f:
        f.write(header)
        f.write(binary_data)
    
    output_size = len(header) + len(binary_data)
    print(f"Output binary: {output_size} bytes")
    print(f"Created: {output_bin}")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: prepend_app_header.py <input.bin> <output.bin> [name] [major.minor.patch]")
        sys.exit(1)
    
    input_bin = sys.argv[1]
    output_bin = sys.argv[2]
    name = sys.argv[3] if len(sys.argv) > 3 else "Z1 Node App"
    
    version = (1, 0, 0)
    if len(sys.argv) > 4:
        version_str = sys.argv[4].split('.')
        version = tuple(int(x) for x in version_str)
    
    prepend_header(input_bin, output_bin, name, version)
