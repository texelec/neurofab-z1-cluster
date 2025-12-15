import sys
import struct
import os

UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_FLAG_FAMILY_ID_PRESENT = 0x00002000
RP2350_ARM_S_FAMILY_ID = 0xe48bff59

def convert_elf_to_uf2(elf_path, uf2_path):
    # Read binary (we'll use .bin file instead of parsing ELF)
    bin_path = elf_path.replace('.elf', '.bin')
    if not os.path.exists(bin_path):
        print(f"Error: {bin_path} not found")
        return False
    
    with open(bin_path, 'rb') as f:
        data = f.read()
    
    # RP2350 flash starts at 0x10000000
    base_addr = 0x10000000
    block_size = 256
    num_blocks = (len(data) + block_size - 1) // block_size
    
    with open(uf2_path, 'wb') as f:
        for i in range(num_blocks):
            offset = i * block_size
            chunk = data[offset:offset + block_size]
            if len(chunk) < block_size:
                chunk += b'\x00' * (block_size - len(chunk))
            
            # UF2 block header
            block = struct.pack('<IIIIIIII',
                UF2_MAGIC_START0,
                UF2_MAGIC_START1,
                UF2_FLAG_FAMILY_ID_PRESENT,
                base_addr + offset,
                block_size,
                i,
                num_blocks,
                RP2350_ARM_S_FAMILY_ID
            )
            block += chunk
            block += struct.pack('<I', UF2_MAGIC_END)
            
            # Pad to 512 bytes
            block += b'\x00' * (512 - len(block))
            f.write(block)
    
    print(f"Converted {elf_path} to {uf2_path}")
    print(f"  Data size: {len(data)} bytes")
    print(f"  UF2 blocks: {num_blocks}")
    return True

if __name__ == '__main__':
    if len(sys.argv) != 3:
        print("Usage: elf2uf2.py <input.elf> <output.uf2>")
        sys.exit(1)
    
    if not convert_elf_to_uf2(sys.argv[1], sys.argv[2]):
        sys.exit(1)
