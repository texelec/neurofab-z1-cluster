"""
Z1 Cluster Emulator

Simulates multi-backplane Z1 cluster with bus communication.
"""

import time
import threading
from typing import Dict, List, Optional
from dataclasses import dataclass
from node import ComputeNode, NodeStatus


@dataclass
class BusMessage:
    """Z1 bus message."""
    source_node: int
    target_node: int  # 255 = broadcast
    command: int
    data: bytes
    timestamp: float = 0.0


class Backplane:
    """Simulates a single Z1 backplane with 16 nodes."""
    
    def __init__(self, backplane_id: int, node_count: int = 16, bus_latency_us: float = 2.0):
        """
        Initialize backplane.
        
        Args:
            backplane_id: Backplane ID
            node_count: Number of compute nodes (max 16)
            bus_latency_us: Bus message latency in microseconds
        """
        self.backplane_id = backplane_id
        self.node_count = min(node_count, 16)
        self.bus_latency_us = bus_latency_us
        
        # Create compute nodes
        self.nodes: Dict[int, ComputeNode] = {}
        for i in range(self.node_count):
            self.nodes[i] = ComputeNode(node_id=i, backplane_id=backplane_id)
        
        # Bus message queue
        self.bus_queue: List[BusMessage] = []
        self.bus_lock = threading.Lock()
        
        # Statistics
        self.stats = {
            'messages_sent': 0,
            'messages_delivered': 0,
            'broadcasts': 0
        }
    
    def get_node(self, node_id: int) -> Optional[ComputeNode]:
        """Get node by ID."""
        return self.nodes.get(node_id)
    
    def send_bus_message(self, source: int, target: int, command: int, data: bytes):
        """
        Send message on bus.
        
        Args:
            source: Source node ID
            target: Target node ID (255 = broadcast)
            command: Command byte
            data: Message data
        """
        msg = BusMessage(
            source_node=source,
            target_node=target,
            command=command,
            data=data,
            timestamp=time.time()
        )
        
        with self.bus_lock:
            self.bus_queue.append(msg)
            self.stats['messages_sent'] += 1
            if target == 255:
                self.stats['broadcasts'] += 1
    
    def process_bus_messages(self):
        """Process pending bus messages (simulate bus latency)."""
        current_time = time.time()
        latency_s = self.bus_latency_us / 1_000_000
        
        with self.bus_lock:
            # Find messages ready for delivery
            ready_messages = []
            remaining_messages = []
            
            for msg in self.bus_queue:
                if current_time - msg.timestamp >= latency_s:
                    ready_messages.append(msg)
                else:
                    remaining_messages.append(msg)
            
            self.bus_queue = remaining_messages
        
        # Deliver messages
        for msg in ready_messages:
            if msg.target_node == 255:
                # Broadcast to all nodes
                for node in self.nodes.values():
                    if node.node_id != msg.source_node:
                        node.receive_message(msg.command, msg.data)
                self.stats['messages_delivered'] += len(self.nodes) - 1
            else:
                # Unicast to specific node
                target = self.nodes.get(msg.target_node)
                if target:
                    target.receive_message(msg.command, msg.data)
                    self.stats['messages_delivered'] += 1
    
    def get_all_nodes_info(self) -> List[Dict]:
        """Get information for all nodes."""
        return [node.get_info() for node in self.nodes.values()]
    
    def reset_all_nodes(self):
        """Reset all nodes."""
        for node in self.nodes.values():
            node.reset()


class Cluster:
    """Simulates multi-backplane Z1 cluster."""
    
    def __init__(self, config: Optional[Dict] = None):
        """
        Initialize cluster.
        
        Args:
            config: Cluster configuration dictionary
        """
        self.config = config or self._default_config()
        self.backplanes: Dict[int, Backplane] = {}
        
        # Create backplanes
        for i, bp_config in enumerate(self.config.get('backplanes', [])):
            node_count = bp_config.get('node_count', 16)
            bus_latency = self.config.get('simulation', {}).get('bus_latency_us', 2.0)
            self.backplanes[i] = Backplane(
                backplane_id=i,
                node_count=node_count,
                bus_latency_us=bus_latency
            )
        
        # Simulation thread
        self.running = False
        self.sim_thread: Optional[threading.Thread] = None
        
        # Global statistics
        self.stats = {
            'total_nodes': sum(bp.node_count for bp in self.backplanes.values()),
            'total_backplanes': len(self.backplanes),
            'simulation_start': time.time()
        }
    
    def _default_config(self) -> Dict:
        """Get default configuration."""
        return {
            'backplanes': [
                {
                    'name': 'backplane-0',
                    'node_count': 16
                }
            ],
            'simulation': {
                'mode': 'real-time',
                'timestep_us': 1000,
                'bus_latency_us': 2.0
            }
        }
    
    def start_simulation(self):
        """Start simulation thread."""
        if self.running:
            return
        
        self.running = True
        self.sim_thread = threading.Thread(target=self._simulation_loop, daemon=True)
        self.sim_thread.start()
    
    def stop_simulation(self):
        """Stop simulation thread."""
        self.running = False
        if self.sim_thread:
            self.sim_thread.join(timeout=1.0)
    
    def _simulation_loop(self):
        """Main simulation loop."""
        timestep_s = self.config.get('simulation', {}).get('timestep_us', 1000) / 1_000_000
        
        while self.running:
            start = time.time()
            
            # Process bus messages for all backplanes
            for backplane in self.backplanes.values():
                backplane.process_bus_messages()
            
            # Sleep to maintain timestep
            elapsed = time.time() - start
            if elapsed < timestep_s:
                time.sleep(timestep_s - elapsed)
    
    def get_node(self, backplane_id: int, node_id: int) -> Optional[ComputeNode]:
        """
        Get node by backplane and node ID.
        
        Args:
            backplane_id: Backplane ID
            node_id: Node ID (0-15)
            
        Returns:
            ComputeNode or None
        """
        backplane = self.backplanes.get(backplane_id)
        if backplane:
            return backplane.get_node(node_id)
        return None
    
    def get_all_nodes(self) -> List[ComputeNode]:
        """Get all nodes across all backplanes."""
        nodes = []
        for backplane in self.backplanes.values():
            nodes.extend(backplane.nodes.values())
        return nodes
    
    def get_cluster_info(self) -> Dict:
        """Get cluster information."""
        return {
            'total_backplanes': len(self.backplanes),
            'total_nodes': sum(bp.node_count for bp in self.backplanes.values()),
            'active_nodes': sum(
                1 for node in self.get_all_nodes() 
                if node.status == NodeStatus.ACTIVE
            ),
            'simulation_running': self.running,
            'uptime_s': time.time() - self.stats['simulation_start'],
            'backplanes': [
                {
                    'id': bp_id,
                    'node_count': bp.node_count,
                    'stats': bp.stats.copy()
                }
                for bp_id, bp in self.backplanes.items()
            ]
        }
    
    def reset_cluster(self):
        """Reset entire cluster."""
        for backplane in self.backplanes.values():
            backplane.reset_all_nodes()
    
    def send_bus_message(self, backplane_id: int, source: int, target: int, 
                        command: int, data: bytes):
        """
        Send message on specific backplane bus.
        
        Args:
            backplane_id: Backplane ID
            source: Source node ID
            target: Target node ID (255 = broadcast)
            command: Command byte
            data: Message data
        """
        backplane = self.backplanes.get(backplane_id)
        if backplane:
            backplane.send_bus_message(source, target, command, data)
