#!/usr/bin/env python3
"""
Z1 Cluster API Client Library

Provides Python interface to NeuroFab Z1 neuromorphic cluster via HTTP REST API.
"""

import requests
import json
import base64
import time
from typing import List, Dict, Optional, Any, Tuple
from dataclasses import dataclass


@dataclass
class NodeInfo:
    """Information about a Z1 compute node."""
    node_id: int
    status: str
    memory_free: int = 0
    uptime_ms: int = 0
    led_state: Dict[str, int] = None
    
    def __post_init__(self):
        if self.led_state is None:
            self.led_state = {"r": 0, "g": 0, "b": 0}


@dataclass
class SpikeEvent:
    """Represents a neuron spike event."""
    neuron_id: int
    timestamp_us: int
    node_id: Optional[int] = None


class Z1ClusterError(Exception):
    """Base exception for Z1 cluster operations."""
    pass


class Z1NodeNotFoundError(Z1ClusterError):
    """Raised when a node is not found or not responding."""
    pass


class Z1CommunicationError(Z1ClusterError):
    """Raised when communication with cluster fails."""
    pass


class Z1Client:
    """Client for interacting with Z1 neuromorphic cluster."""
    
    def __init__(self, controller_ip: str = "192.168.1.222", port: int = 80, timeout: int = 10):
        """
        Initialize Z1 cluster client.
        
        Args:
            controller_ip: IP address of controller node
            port: HTTP port (default: 80)
            timeout: Request timeout in seconds
        """
        self.controller_ip = controller_ip
        self.port = port
        self.timeout = timeout
        self.base_url = f"http://{controller_ip}:{port}/api"
        
    def _request(self, method: str, endpoint: str, **kwargs) -> Dict[str, Any]:
        """
        Make HTTP request to cluster API.
        
        Args:
            method: HTTP method (GET, POST, etc.)
            endpoint: API endpoint path
            **kwargs: Additional arguments for requests
            
        Returns:
            Parsed JSON response
            
        Raises:
            Z1CommunicationError: If request fails
        """
        url = f"{self.base_url}{endpoint}"
        kwargs.setdefault('timeout', self.timeout)
        
        try:
            response = requests.request(method, url, **kwargs)
            response.raise_for_status()
            
            # Handle empty responses
            if not response.content:
                return {"status": "ok"}
                
            return response.json()
            
        except requests.exceptions.RequestException as e:
            raise Z1CommunicationError(f"Failed to communicate with cluster: {e}")
        except json.JSONDecodeError as e:
            raise Z1CommunicationError(f"Invalid JSON response: {e}")
    
    # ========================================================================
    # Node Management
    # ========================================================================
    
    def list_nodes(self) -> List[NodeInfo]:
        """
        List all nodes in the cluster.
        
        Returns:
            List of NodeInfo objects
        """
        response = self._request('GET', '/nodes')
        nodes = []
        for node_data in response.get('nodes', []):
            nodes.append(NodeInfo(
                node_id=node_data['id'],
                status=node_data.get('status', 'unknown'),
                memory_free=node_data.get('memory_free', 0),
                uptime_ms=node_data.get('uptime_ms', 0),
                led_state=node_data.get('led_state', {"r": 0, "g": 0, "b": 0})
            ))
        return nodes
    
    def get_node(self, node_id: int) -> NodeInfo:
        """
        Get detailed information about a specific node.
        
        Args:
            node_id: Node ID (0-15)
            
        Returns:
            NodeInfo object
            
        Raises:
            Z1NodeNotFoundError: If node not found
        """
        try:
            response = self._request('GET', f'/nodes/{node_id}')
            return NodeInfo(
                node_id=response['id'],
                status=response.get('status', 'unknown'),
                memory_free=response.get('memory_free', 0),
                uptime_ms=response.get('uptime_ms', 0),
                led_state=response.get('led_state', {"r": 0, "g": 0, "b": 0})
            )
        except Z1CommunicationError as e:
            if '404' in str(e):
                raise Z1NodeNotFoundError(f"Node {node_id} not found")
            raise
    
    def reset_node(self, node_id: int) -> bool:
        """
        Reset a specific node.
        
        Args:
            node_id: Node ID (0-15)
            
        Returns:
            True if successful
        """
        response = self._request('POST', f'/nodes/{node_id}/reset', json={})
        return response.get('status') == 'ok'
    
    def ping_node(self, node_id: int) -> Tuple[bool, float]:
        """
        Ping a node to test connectivity.
        
        Args:
            node_id: Node ID (0-15)
            
        Returns:
            Tuple of (success, latency_ms)
        """
        start_time = time.time()
        try:
            response = self._request('POST', f'/nodes/{node_id}/ping', json={})
            latency = (time.time() - start_time) * 1000
            return (response.get('status') == 'ok', latency)
        except Z1CommunicationError:
            return (False, 0.0)
    
    def discover_nodes(self) -> List[int]:
        """
        Discover all active nodes in the cluster.
        
        Returns:
            List of active node IDs
        """
        response = self._request('POST', '/nodes/discover', json={})
        return response.get('active_nodes', [])
    
    def set_led(self, node_id: int, r: int = 0, g: int = 0, b: int = 0) -> bool:
        """
        Set LED color on a node.
        
        Args:
            node_id: Node ID (0-15)
            r: Red value (0-255)
            g: Green value (0-255)
            b: Blue value (0-255)
            
        Returns:
            True if successful
        """
        response = self._request('POST', f'/nodes/{node_id}/led', 
                                json={"r": r, "g": g, "b": b})
        return response.get('status') == 'ok'
    
    # ========================================================================
    # Memory Operations
    # ========================================================================
    
    def read_memory(self, node_id: int, addr: int, length: int) -> bytes:
        """
        Read memory from a node.
        
        Args:
            node_id: Node ID (0-15)
            addr: Memory address
            length: Number of bytes to read
            
        Returns:
            Memory contents as bytes
        """
        response = self._request('GET', f'/nodes/{node_id}/memory',
                                params={'addr': addr, 'len': length})
        data_b64 = response.get('data', '')
        return base64.b64decode(data_b64)
    
    def write_memory(self, node_id: int, addr: int, data: bytes) -> int:
        """
        Write memory to a node.
        
        Args:
            node_id: Node ID (0-15)
            addr: Memory address
            data: Data to write
            
        Returns:
            Number of bytes written
        """
        data_b64 = base64.b64encode(data).decode('ascii')
        response = self._request('POST', f'/nodes/{node_id}/memory',
                                json={'addr': addr, 'data': data_b64})
        return response.get('bytes_written', 0)
    
    def execute_code(self, node_id: int, entry_point: int, params: Optional[List[int]] = None) -> int:
        """
        Execute code on a node.
        
        Args:
            node_id: Node ID (0-15)
            entry_point: Entry point address
            params: Optional parameters to pass
            
        Returns:
            Execution ID
        """
        if params is None:
            params = []
        response = self._request('POST', f'/nodes/{node_id}/execute',
                                json={'entry_point': entry_point, 'params': params})
        return response.get('execution_id', 0)
    
    # ========================================================================
    # SNN Operations
    # ========================================================================
    
    def load_topology(self, node_id: int, neuron_count: int) -> bool:
        """
        Load neuron topology from PSRAM on a node.
        
        Args:
            node_id: Node ID (0-15)
            neuron_count: Number of neurons to load
            
        Returns:
            True if successful
        """
        response = self._request('POST', f'/nodes/{node_id}/snn/load',
                                json={'neuron_count': neuron_count})
        return response.get('status') == 'loaded'
    
    def deploy_snn(self, topology: Dict[str, Any]) -> Dict[str, Any]:
        """
        Deploy SNN topology to cluster.
        
        Args:
            topology: SNN topology definition (JSON-serializable dict)
            
        Returns:
            Deployment result with statistics
        """
        response = self._request('POST', '/snn/deploy', json=topology)
        return response
    
    def get_snn_topology(self) -> Dict[str, Any]:
        """
        Get current SNN topology.
        
        Returns:
            SNN topology definition
        """
        return self._request('GET', '/snn/topology')
    
    def update_weights(self, updates: List[Dict[str, Any]]) -> int:
        """
        Update synaptic weights.
        
        Args:
            updates: List of weight updates, each with:
                     {neuron_id, synapse_idx, weight}
            
        Returns:
            Number of weights updated
        """
        response = self._request('POST', '/snn/weights', 
                                json={'updates': updates})
        return response.get('weights_updated', 0)
    
    def get_spike_activity(self, duration_ms: int = 1000) -> List[SpikeEvent]:
        """
        Capture spike activity for specified duration.
        
        Args:
            duration_ms: Capture duration in milliseconds
            
        Returns:
            List of SpikeEvent objects
        """
        response = self._request('GET', '/snn/activity',
                                params={'duration_ms': duration_ms})
        spikes = []
        for spike_data in response.get('spikes', []):
            spikes.append(SpikeEvent(
                neuron_id=spike_data['neuron_id'],
                timestamp_us=spike_data['timestamp_us'],
                node_id=spike_data.get('node_id')
            ))
        return spikes
    
    def inject_spikes(self, spikes: List[Dict[str, Any]]) -> int:
        """
        Inject input spikes into network.
        
        Args:
            spikes: List of spike events, each with:
                    {neuron_id, value}
            
        Returns:
            Number of spikes injected
        """
        response = self._request('POST', '/snn/input',
                                json={'spikes': spikes})
        return response.get('spikes_injected', 0)
    
    def start_snn(self) -> bool:
        """
        Start SNN execution on all nodes.
        
        Returns:
            True if successful
        """
        response = self._request('POST', '/snn/start', json={})
        return response.get('status') == 'ok'
    
    def stop_snn(self) -> bool:
        """
        Stop SNN execution on all nodes.
        
        Returns:
            True if successful
        """
        response = self._request('POST', '/snn/stop', json={})
        return response.get('status') == 'ok'
    
    def get_snn_status(self) -> Dict[str, Any]:
        """
        Get SNN execution status.
        
        Returns:
            Status dictionary with execution state and statistics
        """
        return self._request('GET', '/snn/status')


# Utility functions for common operations

def format_memory_size(bytes_value: int) -> str:
    """Format memory size in human-readable form."""
    for unit in ['B', 'KB', 'MB', 'GB']:
        if bytes_value < 1024.0:
            return f"{bytes_value:.2f} {unit}"
        bytes_value /= 1024.0
    return f"{bytes_value:.2f} TB"


def format_uptime(ms: int) -> str:
    """Format uptime in human-readable form."""
    seconds = ms // 1000
    minutes = seconds // 60
    hours = minutes // 60
    days = hours // 24
    
    if days > 0:
        return f"{days}d {hours % 24}h"
    elif hours > 0:
        return f"{hours}h {minutes % 60}m"
    elif minutes > 0:
        return f"{minutes}m {seconds % 60}s"
    else:
        return f"{seconds}s"


def encode_global_neuron_id(node_id: int, local_neuron_id: int) -> int:
    """
    Encode global neuron ID from node and local neuron IDs.
    
    Args:
        node_id: Node ID (0-255)
        local_neuron_id: Local neuron ID (0-65535)
        
    Returns:
        24-bit global neuron ID
    """
    return (node_id << 16) | (local_neuron_id & 0xFFFF)


def decode_global_neuron_id(global_id: int) -> Tuple[int, int]:
    """
    Decode global neuron ID into node and local neuron IDs.
    
    Args:
        global_id: 24-bit global neuron ID
        
    Returns:
        Tuple of (node_id, local_neuron_id)
    """
    node_id = (global_id >> 16) & 0xFF
    local_neuron_id = global_id & 0xFFFF
    return (node_id, local_neuron_id)
