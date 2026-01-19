#!/usr/bin/env python3
"""
Merge bootloader and app binaries into single UF2 for dual-partition flash

Creates a UF2 file with two address ranges:
- Bootloader at 0x10000000 (flash 0x00000000)
- App at 0x10200000 (flash 0x00200000)
"""

import sys
import struct
from pathlib import Path

UF2_MAGIC_START0 = 0x0A324655  # "UF2\n"
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_FLAG_FAMILY_ID_PRESENT = 0x00002000
RP2350_ARM_S_FAMILY_ID = 0xe48bff59

def create_uf2_block(block_no, num_blocks, target_addr, data):
    """Create a single UF2 block (512 bytes)"""
    assert len(data) == 256, "Data must be 256 bytes"
    
    block = struct.pack('<IIIIIIII',
        UF2_MAGIC_START0,
        UF2_MAGIC_START1,
        UF2_FLAG_FAMILY_ID_PRESENT,
        target_addr,
        256,  # Payload size
        block_no,
        num_blocks,
        RP2350_ARM_S_FAMILY_ID
    )
    block += data  # 32 bytes header + 256 bytes data = 288 bytes
    
    # Pad to 508 bytes, then add magic end (4 bytes) = 512 bytes total
    padding_needed = 508 - len(block)
    block += b'\x00' * padding_needed
    block += struct.pack('<I', UF2_MAGIC_END)
    
    return block

def bin_to_uf2_blocks(bin_data, base_addr, start_block_no):
    """Convert binary data to UF2 blocks"""
    blocks = []
    num_blocks = (len(bin_data) + 255) // 256
    
    for i in range(num_blocks):
        chunk = bin_data[i*256 : (i+1)*256]
        if len(chunk) < 256:
            chunk += b'\x00' * (256 - len(chunk))  # Pad last block
        
        target_addr = base_addr + i * 256
        blocks.append((start_block_no + i, target_addr, chunk))
    
    return blocks

def merge_binaries_to_uf2(bootloader_uf2, app_bin, output_uf2):
    """Merge bootloader UF2 (with BS2) and app binary into single UF2"""
    
    # Read bootloader UF2 blocks (includes BS2)
    bootloader_blocks = []
    with open(bootloader_uf2, 'rb') as f:
        while True:
            chunk = f.read(512)
            if len(chunk) < 512:
                break
            
            magic1 = struct.unpack('<I', chunk[0:4])[0]
            if magic1 == UF2_MAGIC_START0:
                # UF2 format:
                # 0-3: magic0, 4-7: magic1, 8-11: flags, 12-15: target_addr
                # 16-19: payload_size, 20-23: block_no, 24-27: num_blocks, 28-31: family_id
                # 32+: payload data
                target_addr = struct.unpack('<I', chunk[12:16])[0]
                payload_size = struct.unpack('<I', chunk[16:20])[0]
                data = chunk[32:32+payload_size]
                bootloader_blocks.append((target_addr, data))
    
    print(f"Bootloader: {len(bootloader_blocks)} UF2 blocks (includes BS2)")
    
    # Read app binary
    app_data = Path(app_bin).read_bytes()
    print(f"App:        {len(app_data)} bytes")
    
    # Create blocks for app (XIP base 0x10080000 = flash offset 0x00080000)
    app_blocks = bin_to_uf2_blocks(app_data, 0x10080000, 0)
    
    # Combine: bootloader blocks (with BS2) + app blocks
    total_blocks = len(bootloader_blocks) + len(app_blocks)
    
    print(f"Total UF2 blocks: {total_blocks}")
    print(f"  Bootloader: {len(bootloader_blocks)} blocks")
    print(f"  App:        {len(app_blocks)} blocks")
    
    # Write UF2 file
    with open(output_uf2, 'wb') as f:
        # Write bootloader blocks (preserves BS2 and addresses)
        for block_no, (target_addr, data) in enumerate(bootloader_blocks):
            uf2_block = create_uf2_block(block_no, total_blocks, target_addr, data)
            f.write(uf2_block)
        
        # Write app blocks
        for block_no, target_addr, data in app_blocks:
            uf2_block = create_uf2_block(len(bootloader_blocks) + block_no, total_blocks, target_addr, data)
            f.write(uf2_block)
    
    print(f"\nCreated: {output_uf2}")
    print(f"Size: {Path(output_uf2).stat().st_size} bytes")
    print("\nFlash layout:")
    print("  0x00000000-0x0007FFFF (512KB): Bootloader")
    print("  0x00080000-0x007FFFFF (7.5MB): Application")

def main():
    if len(sys.argv) != 4:
        print("Usage: merge_dual_partition.py <bootloader.bin> <app.bin> <output.uf2>")
        sys.exit(1)
    
    bootloader_bin = sys.argv[1]
    app_bin = sys.argv[2]
    output_uf2 = sys.argv[3]
    
    # Check inputs exist
    if not Path(bootloader_bin).exists():
        print(f"ERROR: Bootloader not found: {bootloader_bin}")
        sys.exit(1)
    
    if not Path(app_bin).exists():
        print(f"ERROR: App not found: {app_bin}")
        sys.exit(1)
    
    merge_binaries_to_uf2(bootloader_bin, app_bin, output_uf2)

if __name__ == '__main__':
    main()
