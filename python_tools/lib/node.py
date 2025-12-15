"""
Z1 Compute Node Emulator

Simulates an RP2350B-based compute node with 2MB Flash and 8MB PSRAM.
"""

import time
import struct
from typing import Dict, List, Optional, Tuple
from dataclasses import dataclass, field
from enum import Enum


class NodeStatus(Enum):
    """Node status states."""
    INACTIVE = 0
    ACTIVE = 1
    ERROR = 2
    BOOTLOADER = 3


@dataclass
class LEDState:
    """RGB LED state."""
    r: int = 0
    g: int = 0
    b: int = 0


@dataclass
class FirmwareHeader:
    """Firmware header structure (256 bytes)."""
    magic: int = 0x4E465A31  # 'NF' 'Z1'
    version: int = 1
    firmware_size: int = 0
    crc32: int = 0
    name: str = ""
    description: str = ""
    build_timestamp: int = 0
    
    @classmethod
    def from_bytes(cls, data: bytes) -> 'FirmwareHeader':
        """Parse firmware header from bytes."""
        if len(data) < 256:
            raise ValueError("Firmware header must be 256 bytes")
        
        magic, version, firmware_size, crc32 = struct.unpack_from('<4I', data, 0)
        name = data[16:48].decode('utf-8', errors='ignore').rstrip('\x00')
        description = data[48:176].decode('utf-8', errors='ignore').rstrip('\x00')
        build_timestamp = struct.unpack_from('<Q', data, 176)[0]
        
        return cls(
            magic=magic,
            version=version,
            firmware_size=firmware_size,
            crc32=crc32,
            name=name,
            description=description,
            build_timestamp=build_timestamp
        )
    
    def to_bytes(self) -> bytes:
        """Convert firmware header to bytes."""
        header = bytearray(256)
        struct.pack_into('<4I', header, 0, self.magic, self.version, 
                        self.firmware_size, self.crc32)
        name_bytes = self.name.encode('utf-8')[:32]
        header[16:16+len(name_bytes)] = name_bytes
        desc_bytes = self.description.encode('utf-8')[:128]
        header[48:48+len(desc_bytes)] = desc_bytes
        struct.pack_into('<Q', header, 176, self.build_timestamp)
        return bytes(header)


@dataclass
class ParsedNeuron:
    """Parsed neuron from memory."""
    neuron_id: int
    flags: int
    membrane_potential: float
    threshold: float
    last_spike_time: int
    synapse_count: int
    leak_rate: float
    refractory_period_us: int
    synapses: List[Tuple[int, int]]  # (source_global_id, weight)


class Memory:
    """Simulates Flash and PSRAM memory."""
    
    def __init__(self, flash_size: int = 2*1024*1024, psram_size: int = 8*1024*1024):
        """
        Initialize memory.
        
        Args:
            flash_size: Flash memory size in bytes (default 2MB)
            psram_size: PSRAM size in bytes (default 8MB)
        """
        self.flash = bytearray(flash_size)
        self.psram = bytearray(psram_size)
        
        # Memory map
        self.FLASH_BASE = 0x10000000
        self.PSRAM_BASE = 0x20000000
        
        # Firmware regions
        self.BOOTLOADER_ADDR = 0x10000000
        self.BOOTLOADER_SIZE = 16 * 1024  # 16KB
        self.APP_FIRMWARE_ADDR = 0x10004000
        self.APP_FIRMWARE_SIZE = 112 * 1024  # 112KB
        self.FIRMWARE_BUFFER_ADDR = 0x10020000
        self.FIRMWARE_BUFFER_SIZE = 128 * 1024  # 128KB
        
        # PSRAM regions
        self.NEURON_TABLE_ADDR = 0x20100000  # 1MB offset in PSRAM
    
    def read(self, addr: int, length: int) -> bytes:
        """Read from memory."""
        if addr >= self.PSRAM_BASE:
            # PSRAM
            offset = addr - self.PSRAM_BASE
            if offset + length > len(self.psram):
                raise ValueError(f"PSRAM read out of bounds: 0x{addr:08X}")
            return bytes(self.psram[offset:offset+length])
        elif addr >= self.FLASH_BASE:
            # Flash
            offset = addr - self.FLASH_BASE
            if offset + length > len(self.flash):
                raise ValueError(f"Flash read out of bounds: 0x{addr:08X}")
            return bytes(self.flash[offset:offset+length])
        else:
            raise ValueError(f"Invalid memory address: 0x{addr:08X}")
    
    def write(self, addr: int, data: bytes) -> int:
        """Write to memory."""
        if addr >= self.PSRAM_BASE:
            # PSRAM (writable)
            offset = addr - self.PSRAM_BASE
            if offset + len(data) > len(self.psram):
                raise ValueError(f"PSRAM write out of bounds: 0x{addr:08X}")
            self.psram[offset:offset+len(data)] = data
            return len(data)
        elif addr >= self.FLASH_BASE:
            # Flash (writable in emulator)
            offset = addr - self.FLASH_BASE
            if offset + len(data) > len(self.flash):
                raise ValueError(f"Flash write out of bounds: 0x{addr:08X}")
            self.flash[offset:offset+len(data)] = data
            return len(data)
        else:
            raise ValueError(f"Invalid memory address: 0x{addr:08X}")
    
    def get_free_psram(self) -> int:
        """Get free PSRAM (simplified - just return size minus 1MB)."""
        return len(self.psram) - 1024*1024


class ComputeNode:
    """Simulates a Z1 compute node (RP2350B + 8MB PSRAM)."""
    
    def __init__(self, node_id: int, backplane_id: int = 0):
        """
        Initialize compute node.
        
        Args:
            node_id: Node ID (0-15)
            backplane_id: Backplane ID
        """
        self.node_id = node_id
        self.backplane_id = backplane_id
        self.status = NodeStatus.ACTIVE
        self.memory = Memory()
        self.led = LEDState()
        
        # Firmware
        self.firmware_header: Optional[FirmwareHeader] = None
        
        # Statistics
        self.boot_time = time.time()
        self.last_activity = time.time()
        self.stats = {
            'bus_messages_sent': 0,
            'bus_messages_received': 0,
            'memory_reads': 0,
            'memory_writes': 0,
            'resets': 0
        }
        
        # Message queue (simulates bus messages)
        self.message_queue: List[Tuple[int, bytes]] = []
        
        # Parsed neuron table
        self.neuron_table: List[ParsedNeuron] = []
    
    def reset(self):
        """Reset node."""
        self.status = NodeStatus.ACTIVE
        self.led = LEDState()
        self.boot_time = time.time()
        self.stats['resets'] += 1
        self.message_queue.clear()
        self.neuron_table.clear()
    
    def get_uptime_ms(self) -> int:
        """Get uptime in milliseconds."""
        return int((time.time() - self.boot_time) * 1000)
    
    def get_free_memory(self) -> int:
        """Get free memory."""
        return self.memory.get_free_psram()
    
    def read_memory(self, addr: int, length: int) -> bytes:
        """Read from node memory."""
        self.stats['memory_reads'] += 1
        self.last_activity = time.time()
        return self.memory.read(addr, length)
    
    def write_memory(self, addr: int, data: bytes) -> int:
        """Write to node memory."""
        self.stats['memory_writes'] += 1
        self.last_activity = time.time()
        return self.memory.write(addr, data)
    
    def load_firmware(self, firmware_data: bytes) -> bool:
        """Load firmware into flash."""
        try:
            # Parse header
            if len(firmware_data) < 256:
                return False
            
            self.firmware_header = FirmwareHeader.from_bytes(firmware_data[:256])
            
            # Write to firmware buffer
            self.memory.write(self.memory.FIRMWARE_BUFFER_ADDR, firmware_data)
            
            # In real hardware, bootloader would verify and copy to app region
            # For emulator, we just mark as loaded
            self.last_activity = time.time()
            return True
        except Exception:
            return False
    
    def send_bus_message(self, command: int, data: bytes):
        """Send message on Z1 bus (simulated)."""
        self.stats['bus_messages_sent'] += 1
        self.last_activity = time.time()
    
    def receive_bus_message(self, command: int, data: bytes):
        """Receive message from Z1 bus."""
        self.message_queue.append((command, data))
        self.stats['bus_messages_received'] += 1
        self.last_activity = time.time()
    
    def parse_neuron_table(self, addr: int = 0x20100000) -> List[ParsedNeuron]:
        """
        Parse neuron table from memory.
        
        Args:
            addr: Address of neuron table in PSRAM
            
        Returns:
            List of parsed neurons
        """
        neurons = []
        
        # Read first entry to check if there's data
        try:
            first_entry = self.memory.read(addr, 256)
            
            # Check if memory is empty (all zeros)
            if all(b == 0 for b in first_entry):
                return neurons
            
            # Parse entries until we hit empty data
            offset = 0
            while offset < 1024 * 1024:  # Max 1MB of neuron tables
                entry_data = self.memory.read(addr + offset, 256)
                
                # Check if this entry is empty
                neuron_id = struct.unpack_from('<H', entry_data, 0)[0]
                if neuron_id == 0 and offset > 0:
                    # Reached end of table
                    break
                
                # Parse neuron entry
                neuron = self._parse_neuron_entry(entry_data)
                if neuron:
                    neurons.append(neuron)
                
                offset += 256
                
                # Safety limit
                if len(neurons) >= 1000:
                    break
            
        except Exception as e:
            print(f"Error parsing neuron table: {e}")
        
        self.neuron_table = neurons
        return neurons
    
    def _parse_neuron_entry(self, entry_data: bytes) -> Optional[ParsedNeuron]:
        """Parse single neuron entry (256 bytes)."""
        if len(entry_data) < 256:
            return None
        
        try:
            # Neuron state (16 bytes)
            neuron_id, flags, membrane_potential, threshold, last_spike_time = \
                struct.unpack_from('<HHffI', entry_data, 0)
            
            # Synapse metadata (8 bytes)
            synapse_count, synapse_capacity, reserved = \
                struct.unpack_from('<HHI', entry_data, 16)
            
            # Neuron parameters (8 bytes)
            leak_rate, refractory_period_us = \
                struct.unpack_from('<fI', entry_data, 24)
            
            # Parse synapses (240 bytes, 60 × 4 bytes)
            synapses = []
            for i in range(min(synapse_count, 60)):
                synapse_value = struct.unpack_from('<I', entry_data, 40 + i * 4)[0]
                source_id = (synapse_value >> 8) & 0xFFFFFF
                weight = synapse_value & 0xFF
                synapses.append((source_id, weight))
            
            return ParsedNeuron(
                neuron_id=neuron_id,
                flags=flags,
                membrane_potential=membrane_potential,
                threshold=threshold,
                last_spike_time=last_spike_time,
                synapse_count=synapse_count,
                leak_rate=leak_rate,
                refractory_period_us=refractory_period_us,
                synapses=synapses
            )
        except Exception:
            return None
    
    def get_info(self) -> Dict:
        """Get node information."""
        free_mem = self.get_free_memory()
        return {
            'id': self.node_id,  # Tools expect 'id' field
            'node_id': self.node_id,
            'backplane_id': self.backplane_id,
            'status': self.status.name.lower(),
            'uptime_ms': self.get_uptime_ms(),
            'memory_free': free_mem,  # Tools expect 'memory_free' field
            'free_memory': free_mem,
            'led_state': {
                'r': self.led.r,
                'g': self.led.g,
                'b': self.led.b
            },
            'stats': self.stats.copy(),
            'neuron_count': len(self.neuron_table)
        }
