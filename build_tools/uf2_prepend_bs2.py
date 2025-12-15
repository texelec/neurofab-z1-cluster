#!/usr/bin/env python3
"""
UF2 Boot Stage 2 Prepender for RP2350B

Prepends the boot stage 2 bootloader as a separate UF2 segment at 0x10FFFF00.
This matches the old SDK behavior required by RP2350B chips.
"""

import sys
import struct
import os

UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_BLOCK_SIZE = 512
UF2_DATA_SIZE = 256

def prepend_boot_stage2(uf2_path, bs2_bin_path):
    """Prepend boot stage 2 as separate segment at 0x10FFFF00"""
    
    if not os.path.exists(uf2_path):
        print(f"Error: {uf2_path} not found")
        return False
    
    if not os.path.exists(bs2_bin_path):
        print(f"Error: {bs2_bin_path} not found")
        return False
    
    # Read boot stage 2 binary
    with open(bs2_bin_path, 'rb') as f:
        bs2_data = f.read()
    
    if len(bs2_data) > UF2_DATA_SIZE * 2:
        print(f"Error: Boot stage 2 is too large ({len(bs2_data)} bytes)")
        return False
    
    # Read existing UF2
    with open(uf2_path, 'rb') as f:
        main_uf2 = f.read()
    
    # Extract flags and family ID from first block of main UF2
    flags = struct.unpack('<I', main_uf2[8:12])[0]
    family_id = struct.unpack('<I', main_uf2[28:32])[0]
    
    # Create boot stage 2 UF2 blocks
    bs2_blocks = bytearray()
    bs2_num_blocks = 2  # Always 2 blocks for boot stage
    bs2_base_addr = 0x10FFFF00
    
    for i in range(bs2_num_blocks):
        offset = i * UF2_DATA_SIZE
        chunk = bs2_data[offset:offset + UF2_DATA_SIZE]
        if len(chunk) < UF2_DATA_SIZE:
            chunk += b'\x00' * (UF2_DATA_SIZE - len(chunk))
        
        block = struct.pack('<IIIIIIII',
            UF2_MAGIC_START0,
            UF2_MAGIC_START1,
            flags,
            bs2_base_addr + offset,
            UF2_DATA_SIZE,
            i,
            bs2_num_blocks,  # Total blocks in THIS segment
            family_id
        )
        block += chunk
        block += struct.pack('<I', UF2_MAGIC_END)
        block += b'\x00' * (UF2_BLOCK_SIZE - len(block))
        bs2_blocks += block
    
    # Write combined UF2: boot stage first, then main program
    with open(uf2_path, 'wb') as f:
        f.write(bs2_blocks)
        f.write(main_uf2)
    
    main_blocks = len(main_uf2) // UF2_BLOCK_SIZE
    print(f"Prepended boot stage 2 to {uf2_path}")
    print(f"  Boot stage: 2 blocks at 0x10FFFF00")
    print(f"  Main program: {main_blocks} blocks at 0x10000000")
    print(f"  Total: {2 + main_blocks} blocks")
    return True

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: uf2_prepend_bs2.py <firmware.uf2> <bs2_default.bin>")
        sys.exit(1)
    
    success = prepend_boot_stage2(sys.argv[1], sys.argv[2])
    sys.exit(0 if success else 1)
