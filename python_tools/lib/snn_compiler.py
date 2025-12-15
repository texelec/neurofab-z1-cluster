#!/usr/bin/env python3
"""
SNN Topology Compiler

Compiles high-level SNN topology definitions into distributed neuron tables
for deployment on Z1 cluster. Supports multi-backplane configurations with
hundreds of nodes.
"""

import json
import struct
import random
import numpy as np
from typing import Dict, List, Tuple, Any, Optional
from dataclasses import dataclass


@dataclass
class NeuronConfig:
    """Configuration for a single neuron."""
    neuron_id: int          # Local neuron ID on the node
    global_id: int          # Global neuron ID across entire cluster
    node_id: int            # Node ID (0-15) on backplane
    backplane_id: str       # Backplane name/ID
    flags: int
    threshold: float
    leak_rate: float
    refractory_period_us: int
    synapses: List[Tuple[int, int]]  # List of (source_global_id, weight)


@dataclass
class DeploymentPlan:
    """Plan for deploying SNN across cluster."""
    neuron_tables: Dict[Tuple[str, int], bytes]  # (backplane_id, node_id) -> table_data
    neuron_map: Dict[int, Tuple[str, int, int]]  # global_id -> (backplane, node, local_id)
    backplane_nodes: Dict[str, List[int]]        # backplane_id -> list of node_ids
    total_neurons: int
    total_synapses: int


class SNNCompiler:
    """Compiles SNN topology to node-specific neuron tables."""
    
    def __init__(self, topology: Dict[str, Any], backplane_config: Optional[Dict[str, Any]] = None):
        """
        Initialize compiler with topology definition.
        
        Args:
            topology: SNN topology dictionary
            backplane_config: Optional backplane configuration for multi-backplane deployment
        """
        self.topology = topology
        self.backplane_config = backplane_config or {}
        self.neurons = []
        self.node_assignments = {}  # (backplane_id, node_id) -> [global_neuron_ids]
        self.layer_map = {}
        self.neuron_map = {}  # global_id -> (backplane, node, local_id)
        
    def compile(self) -> DeploymentPlan:
        """
        Compile topology to neuron tables.
        
        Returns:
            DeploymentPlan with neuron tables and mapping information
        """
        # Step 1: Assign neurons to nodes (potentially across backplanes)
        self._assign_neurons_to_nodes()
        
        # Step 2: Build neuron configurations
        self._build_neuron_configs()
        
        # Step 3: Generate connections
        self._generate_connections()
        
        # Step 4: Compile neuron tables
        neuron_tables = self._compile_neuron_tables()
        
        # Step 5: Build deployment plan
        deployment_plan = self._build_deployment_plan(neuron_tables)
        
        return deployment_plan
    
    def _assign_neurons_to_nodes(self):
        """Assign neurons to compute nodes across backplanes."""
        assignment = self.topology.get('node_assignment', {})
        strategy = assignment.get('strategy', 'balanced')
        
        # Get available backplanes and nodes
        if self.backplane_config:
            # Multi-backplane mode
            backplanes = self.backplane_config.get('backplanes', [])
            available_nodes = []
            for bp in backplanes:
                bp_name = bp['name']
                node_count = bp.get('node_count', 16)
                for node_id in range(node_count):
                    available_nodes.append((bp_name, node_id))
        else:
            # Single backplane mode
            nodes = assignment.get('nodes', list(range(12)))
            backplane_name = assignment.get('backplane', 'default')
            available_nodes = [(backplane_name, node_id) for node_id in nodes]
        
        total_neurons = self.topology['neuron_count']
        
        if strategy == 'balanced':
            # Evenly distribute neurons across all available nodes
            neurons_per_node = total_neurons // len(available_nodes)
            
            neuron_id = 0
            for bp_name, node_id in available_nodes:
                key = (bp_name, node_id)
                self.node_assignments[key] = []
                
                for _ in range(neurons_per_node):
                    if neuron_id < total_neurons:
                        self.node_assignments[key].append(neuron_id)
                        neuron_id += 1
            
            # Assign remaining neurons
            node_idx = 0
            while neuron_id < total_neurons:
                key = available_nodes[node_idx]
                self.node_assignments[key].append(neuron_id)
                neuron_id += 1
                node_idx = (node_idx + 1) % len(available_nodes)
        
        elif strategy == 'layer_based':
            # Assign entire layers to nodes
            layers = self.topology['layers']
            node_idx = 0
            
            for layer in layers:
                start_id = layer['neuron_ids'][0]
                end_id = layer['neuron_ids'][1]
                
                key = available_nodes[node_idx % len(available_nodes)]
                if key not in self.node_assignments:
                    self.node_assignments[key] = []
                
                for neuron_id in range(start_id, end_id + 1):
                    self.node_assignments[key].append(neuron_id)
                
                node_idx += 1
    
    def _build_neuron_configs(self):
        """Build neuron configurations from layers."""
        layers = self.topology['layers']
        
        for layer in layers:
            layer_id = layer['layer_id']
            layer_type = layer['layer_type']
            start_id = layer['neuron_ids'][0]
            end_id = layer['neuron_ids'][1]
            
            # Determine neuron flags
            flags = 0x0001  # ACTIVE
            if layer_type == 'input':
                flags |= 0x0004  # INPUT
            elif layer_type == 'output':
                flags |= 0x0008  # OUTPUT
            
            # Get layer parameters
            threshold = layer.get('threshold', 1.0)
            leak_rate = layer.get('leak_rate', 0.95)
            refractory_period_us = layer.get('refractory_period_us', 1000)
            
            # Create neuron configs
            for global_id in range(start_id, end_id + 1):
                # Find which node this neuron is assigned to
                bp_name, node_id, local_id = self._find_node_for_neuron(global_id)
                
                neuron = NeuronConfig(
                    neuron_id=local_id,
                    global_id=global_id,
                    node_id=node_id,
                    backplane_id=bp_name,
                    flags=flags,
                    threshold=threshold,
                    leak_rate=leak_rate,
                    refractory_period_us=refractory_period_us,
                    synapses=[]
                )
                
                self.neurons.append(neuron)
                self.layer_map[global_id] = layer_id
                self.neuron_map[global_id] = (bp_name, node_id, local_id)
    
    def _find_node_for_neuron(self, global_id: int) -> Tuple[str, int, int]:
        """
        Find which node a neuron is assigned to.
        
        Returns:
            Tuple of (backplane_name, node_id, local_neuron_id)
        """
        for (bp_name, node_id), neuron_list in self.node_assignments.items():
            if global_id in neuron_list:
                local_id = neuron_list.index(global_id)
                return (bp_name, node_id, local_id)
        raise ValueError(f"Neuron {global_id} not assigned to any node")
    
    def _generate_connections(self):
        """Generate synaptic connections based on topology."""
        connections = self.topology.get('connections', [])
        layers = self.topology['layers']
        
        for conn in connections:
            # Check if this is an explicit neuron-to-neuron connection
            if 'source_neuron' in conn and 'target_neuron' in conn:
                self._add_explicit_connection(conn)
                continue
            
            source_layer_id = conn['source_layer']
            target_layer_id = conn['target_layer']
            conn_type = conn['connection_type']
            
            # Find source and target layers
            source_layer = next(l for l in layers if l['layer_id'] == source_layer_id)
            target_layer = next(l for l in layers if l['layer_id'] == target_layer_id)
            
            source_start = source_layer['neuron_ids'][0]
            source_end = source_layer['neuron_ids'][1]
            target_start = target_layer['neuron_ids'][0]
            target_end = target_layer['neuron_ids'][1]
            
            if conn_type == 'fully_connected':
                self._generate_fully_connected(
                    source_start, source_end,
                    target_start, target_end,
                    conn
                )
            elif conn_type in ['sparse_random', 'random']:
                # Map 'probability' to 'connection_probability' if needed
                if 'probability' in conn and 'connection_probability' not in conn:
                    conn['connection_probability'] = conn['probability']
                self._generate_sparse_random(
                    source_start, source_end,
                    target_start, target_end,
                    conn
                )
    
    def _add_explicit_connection(self, conn_config: Dict[str, Any]):
        """Add an explicit neuron-to-neuron connection."""
        source_id = conn_config['source_neuron']
        target_id = conn_config['target_neuron']
        weight_float = conn_config.get('weight', 0.5)
        
        # Find target neuron
        target_neuron = next((n for n in self.neurons if n.global_id == target_id), None)
        if not target_neuron:
            print(f"Warning: Target neuron {target_id} not found")
            return
        
        # Convert weight to 8-bit integer
        # Use full 8-bit range: 0-255 maps to 0.0-2.0
        # Negative weights use values 128-255 (bit 7 set)
        if weight_float < 0:
            # Negative weight: 128-255 range
            # Map -2.0 to -0.01 → 255 to 128
            weight = max(128, min(255, 128 + int(abs(weight_float) * 63.5)))
        else:
            # Positive weight: 0-127 range  
            # Map 0.0 to 2.0 → 0 to 127
            weight = min(127, int(weight_float * 63.5))
        
        # Add synapse (limit to max synapses)
        if len(target_neuron.synapses) < 54:
            target_neuron.synapses.append((source_id, weight))
    
    def _generate_fully_connected(self, source_start: int, source_end: int,
                                  target_start: int, target_end: int,
                                  conn_config: Dict[str, Any]):
        """Generate fully connected layer."""
        weight_init = conn_config.get('weight_init', 'random_normal')
        weight_mean = conn_config.get('weight_mean', 0.5)
        weight_stddev = conn_config.get('weight_stddev', 0.1)
        
        for target_id in range(target_start, target_end + 1):
            target_neuron = next(n for n in self.neurons if n.global_id == target_id)
            
            for source_id in range(source_start, source_end + 1):
                # Generate weight
                if weight_init == 'random_normal':
                    weight_float = random.gauss(weight_mean, weight_stddev)
                    weight_float = max(0.0, min(1.0, weight_float))
                elif weight_init == 'random_uniform':
                    weight_min = conn_config.get('weight_min', 0.0)
                    weight_max = conn_config.get('weight_max', 1.0)
                    weight_float = random.uniform(weight_min, weight_max)
                elif weight_init == 'constant':
                    weight_float = conn_config.get('weight_value', 0.5)
                else:
                    weight_float = 0.5
                
                # Convert to 8-bit integer
                weight = int(weight_float * 255)
                
                # Add synapse (limit to max synapses)
                if len(target_neuron.synapses) < 54:
                    target_neuron.synapses.append((source_id, weight))
    
    def _generate_sparse_random(self, source_start: int, source_end: int,
                                target_start: int, target_end: int,
                                conn_config: Dict[str, Any]):
        """Generate sparse random connections."""
        connection_prob = conn_config.get('connection_probability', 0.1)
        
        # Support both weight_range and weight_mean/stddev
        if 'weight_range' in conn_config:
            weight_min, weight_max = conn_config['weight_range']
            use_range = True
        else:
            weight_mean = conn_config.get('weight_mean', 0.5)
            weight_stddev = conn_config.get('weight_stddev', 0.1)
            use_range = False
        
        for target_id in range(target_start, target_end + 1):
            target_neuron = next(n for n in self.neurons if n.global_id == target_id)
            
            for source_id in range(source_start, source_end + 1):
                if random.random() < connection_prob:
                    # Generate weight
                    if use_range:
                        weight_float = random.uniform(weight_min, weight_max)
                    else:
                        weight_float = random.gauss(weight_mean, weight_stddev)
                        weight_float = max(0.0, min(1.0, weight_float))
                    
                    # Convert to 8-bit: 0-127 for positive weights
                    weight = min(127, int(weight_float * 63.5))
                    
                    # Add synapse (limit to max synapses)
                    if len(target_neuron.synapses) < 54:
                        target_neuron.synapses.append((source_id, weight))
    
    def _compile_neuron_tables(self) -> Dict[Tuple[str, int], bytes]:
        """Compile neuron tables for each node."""
        neuron_tables = {}
        
        for (bp_name, node_id), neuron_ids in self.node_assignments.items():
            table_data = bytearray()
            
            # Get neurons for this node
            node_neurons = [n for n in self.neurons 
                          if n.backplane_id == bp_name and n.node_id == node_id]
            node_neurons.sort(key=lambda n: n.neuron_id)
            
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
            
            neuron_tables[(bp_name, node_id)] = bytes(table_data)
        
        return neuron_tables
    
    def _pack_neuron_entry(self, neuron: NeuronConfig) -> bytes:
        """Pack neuron entry into 256-byte binary format."""
        entry = bytearray(256)
        
        # Neuron state (16 bytes)
        struct.pack_into('<HHffI', entry, 0,
                        neuron.neuron_id,  # Use local neuron ID (0-based on this node)
                        neuron.flags,
                        0.0,  # Initial membrane potential
                        neuron.threshold,
                        0)    # Last spike time
        
        # Synapse metadata (8 bytes)
        struct.pack_into('<HHI', entry, 16,
                        len(neuron.synapses),  # synapse_count
                        60,                    # synapse_capacity (240/4 = 60 max, matches Z1_SNN_MAX_SYNAPSES)
                        0)                     # reserved1 (firmware doesn't use global_id here)
        
        # Neuron parameters (8 bytes)
        struct.pack_into('<fI', entry, 24,
                        neuron.leak_rate,
                        neuron.refractory_period_us)
        
        # Reserved (8 bytes) - already zero
        
        # Synapses (240 bytes, 60 × 4 bytes)
        for i, (source_global_id, weight) in enumerate(neuron.synapses[:60]):
            # Convert global ID to encoded format: (node_id << 16) | local_neuron_id
            if source_global_id in self.neuron_map:
                source_bp, source_node, source_local = self.neuron_map[source_global_id]
                source_encoded = (source_node << 16) | source_local
            else:
                # Fallback: use global ID as-is
                source_encoded = source_global_id
            
            # Pack synapse: [source_id:24][weight:8]
            synapse_value = ((source_encoded & 0xFFFFFF) << 8) | (weight & 0xFF)
            struct.pack_into('<I', entry, 40 + i * 4, synapse_value)
        
        return bytes(entry)
    
    def _build_deployment_plan(self, neuron_tables: Dict[Tuple[str, int], bytes]) -> DeploymentPlan:
        """Build deployment plan from compiled neuron tables."""
        # Group nodes by backplane
        backplane_nodes = {}
        for (bp_name, node_id) in neuron_tables.keys():
            if bp_name not in backplane_nodes:
                backplane_nodes[bp_name] = []
            if node_id not in backplane_nodes[bp_name]:
                backplane_nodes[bp_name].append(node_id)
        
        # Sort node lists
        for bp_name in backplane_nodes:
            backplane_nodes[bp_name].sort()
        
        total_synapses = sum(len(n.synapses) for n in self.neurons)
        
        return DeploymentPlan(
            neuron_tables=neuron_tables,
            neuron_map=self.neuron_map,
            backplane_nodes=backplane_nodes,
            total_neurons=len(self.neurons),
            total_synapses=total_synapses
        )
    
    def get_deployment_info(self) -> Dict[str, Any]:
        """Get deployment information."""
        neurons_per_node = {}
        for (bp_name, node_id), neurons in self.node_assignments.items():
            key = f"{bp_name}:{node_id}"
            neurons_per_node[key] = len(neurons)
        
        backplanes_used = set(bp for bp, _ in self.node_assignments.keys())
        
        return {
            'network_name': self.topology.get('network_name', 'unnamed'),
            'neuron_count': self.topology['neuron_count'],
            'backplanes_used': len(backplanes_used),
            'nodes_used': len(self.node_assignments),
            'neurons_per_node': neurons_per_node,
            'total_synapses': sum(len(n.synapses) for n in self.neurons)
        }


def compile_snn_topology(topology_file: str, 
                        backplane_config: Optional[Dict[str, Any]] = None) -> DeploymentPlan:
    """
    Compile SNN topology file to deployment plan.
    
    Args:
        topology_file: Path to topology JSON file
        backplane_config: Optional backplane configuration for multi-backplane deployment
        
    Returns:
        DeploymentPlan with neuron tables and mapping
    """
    with open(topology_file, 'r') as f:
        topology = json.load(f)
    
    compiler = SNNCompiler(topology, backplane_config)
    deployment_plan = compiler.compile()
    
    return deployment_plan


if __name__ == '__main__':
    import sys
    
    if len(sys.argv) < 2:
        print("Usage: snn_compiler.py <topology.json> [backplane_config.json]")
        sys.exit(1)
    
    topology_file = sys.argv[1]
    backplane_config = None
    
    if len(sys.argv) > 2:
        with open(sys.argv[2], 'r') as f:
            backplane_config = json.load(f)
    
    deployment_plan = compile_snn_topology(topology_file, backplane_config)
    
    print(f"Compiled SNN Deployment Plan")
    print(f"  Total Neurons: {deployment_plan.total_neurons}")
    print(f"  Total Synapses: {deployment_plan.total_synapses}")
    print(f"  Backplanes: {len(deployment_plan.backplane_nodes)}")
    print(f"  Nodes: {len(deployment_plan.neuron_tables)}")
    print(f"\nNeurons per node:")
    for (bp_name, node_id), table_data in deployment_plan.neuron_tables.items():
        neuron_count = len(table_data) // 256
        print(f"  {bp_name}:{node_id:2d} - {neuron_count:4d} neurons ({len(table_data):6d} bytes)")
