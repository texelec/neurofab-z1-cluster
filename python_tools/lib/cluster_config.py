#!/usr/bin/env python3
"""
Cluster Configuration Management

Manages multi-backplane Z1 cluster configurations with environment variable support.
"""

import os
import json
from typing import List, Dict, Optional
from dataclasses import dataclass


@dataclass
class BackplaneConfig:
    """Configuration for a single backplane."""
    name: str
    controller_ip: str
    controller_port: int = 8000  # Default to emulator port; hardware uses 80
    node_count: int = 16
    description: str = ""
    
    def __str__(self):
        return f"{self.name} ({self.controller_ip}:{self.controller_port})"


class ClusterConfig:
    """Multi-backplane cluster configuration with environment variable support."""
    
    def __init__(self, config_file: Optional[str] = None):
        """
        Initialize cluster configuration.
        
        Priority order:
        1. Environment variables (Z1_CONTROLLER_IP, Z1_CONTROLLER_PORT) - HIGHEST
        2. Specified config file
        3. Default config file locations
        4. Empty config (will use get_default_backplane() fallback)
        
        Args:
            config_file: Path to configuration file (JSON)
        """
        self.backplanes: List[BackplaneConfig] = []
        self.config_file = config_file
        
        # PRIORITY 1: Check environment variables FIRST
        controller_ip = os.environ.get('Z1_CONTROLLER_IP')
        if controller_ip:
            # Environment variables take precedence over everything
            self._load_from_environment()
        else:
            # PRIORITY 2-3: Load from config file if no env vars
            if config_file and os.path.exists(config_file):
                self.load(config_file)
            else:
                # Try default locations
                self._load_default_config()
    
    def _load_default_config(self):
        """Load configuration from default locations."""
        default_paths = [
            os.path.expanduser('~/.neurofab/cluster.json'),
            '/etc/neurofab/cluster.json',
            './cluster.json'
        ]
        
        for path in default_paths:
            if os.path.exists(path):
                self.load(path)
                return
    
    def _load_from_environment(self):
        """Load configuration from environment variables."""
        controller_ip = os.environ.get('Z1_CONTROLLER_IP')
        controller_port_str = os.environ.get('Z1_CONTROLLER_PORT')
        
        if controller_ip:
            # Auto-detect port based on IP if not explicitly set
            if controller_port_str:
                try:
                    controller_port = int(controller_port_str)
                except ValueError:
                    controller_port = self._auto_detect_port(controller_ip)
            else:
                controller_port = self._auto_detect_port(controller_ip)
            
            self.backplanes.append(BackplaneConfig(
                name='env-backplane',
                controller_ip=controller_ip,
                controller_port=controller_port,
                node_count=16,
                description='From environment variables'
            ))
    
    def _auto_detect_port(self, ip: str) -> int:
        """Auto-detect port based on IP address."""
        # Localhost/127.0.0.1 → emulator (port 8000)
        if ip in ['127.0.0.1', 'localhost']:
            return 8000
        # Real hardware → port 80
        return 80
    
    def load(self, config_file: str):
        """
        Load configuration from file.
        
        Args:
            config_file: Path to configuration file
        """
        with open(config_file, 'r') as f:
            config_data = json.load(f)
        
        self.backplanes = []
        for bp_data in config_data.get('backplanes', []):
            backplane = BackplaneConfig(
                name=bp_data['name'],
                controller_ip=bp_data['controller_ip'],
                controller_port=bp_data.get('controller_port', 80),
                node_count=bp_data.get('node_count', 16),
                description=bp_data.get('description', '')
            )
            self.backplanes.append(backplane)
        
        self.config_file = config_file
    
    def save(self, config_file: Optional[str] = None):
        """
        Save configuration to file.
        
        Args:
            config_file: Path to save configuration (uses loaded path if None)
        """
        if config_file is None:
            config_file = self.config_file
        
        if config_file is None:
            raise ValueError("No configuration file specified")
        
        config_data = {
            'backplanes': [
                {
                    'name': bp.name,
                    'controller_ip': bp.controller_ip,
                    'controller_port': bp.controller_port,
                    'node_count': bp.node_count,
                    'description': bp.description
                }
                for bp in self.backplanes
            ]
        }
        
        # Create directory if it doesn't exist
        os.makedirs(os.path.dirname(os.path.abspath(config_file)), exist_ok=True)
        
        with open(config_file, 'w') as f:
            json.dump(config_data, f, indent=2)
    
    def add_backplane(self, backplane: BackplaneConfig):
        """Add a backplane to the configuration."""
        self.backplanes.append(backplane)
    
    def remove_backplane(self, name: str):
        """Remove a backplane by name."""
        self.backplanes = [bp for bp in self.backplanes if bp.name != name]
    
    def get_backplane(self, name: str) -> Optional[BackplaneConfig]:
        """Get a backplane by name."""
        for bp in self.backplanes:
            if bp.name == name:
                return bp
        return None
    
    def get_backplane_by_ip(self, ip: str) -> Optional[BackplaneConfig]:
        """Get a backplane by controller IP."""
        for bp in self.backplanes:
            if bp.controller_ip == ip:
                return bp
        return None
    
    def get_all_controllers(self) -> List[str]:
        """Get list of all controller IPs."""
        return [bp.controller_ip for bp in self.backplanes]
    
    def get_total_nodes(self) -> int:
        """Get total number of nodes across all backplanes."""
        return sum(bp.node_count for bp in self.backplanes)
    
    def get_default_backplane(self) -> BackplaneConfig:
        """
        Get default backplane with smart detection.
        
        Priority:
        1. Environment variable Z1_CONTROLLER_IP
        2. First backplane in config
        3. Auto-detect emulator at localhost:8000
        4. Default to real hardware at 192.168.1.201:80
        """
        # Check environment variable
        controller_ip = os.environ.get('Z1_CONTROLLER_IP')
        if controller_ip:
            controller_port_str = os.environ.get('Z1_CONTROLLER_PORT')
            if controller_port_str:
                try:
                    controller_port = int(controller_port_str)
                except ValueError:
                    controller_port = self._auto_detect_port(controller_ip)
            else:
                controller_port = self._auto_detect_port(controller_ip)
            
            return BackplaneConfig(
                name='default',
                controller_ip=controller_ip,
                controller_port=controller_port,
                node_count=16,
                description='From environment'
            )
        
        # Return first backplane if available
        if self.backplanes:
            return self.backplanes[0]
        
        # Try to auto-detect emulator
        try:
            import requests
            response = requests.get('http://127.0.0.1:8000/api/emulator/status', timeout=0.5)
            if response.status_code == 200 and response.json().get('emulator'):
                return BackplaneConfig(
                    name='emulator',
                    controller_ip='127.0.0.1',
                    controller_port=8000,
                    node_count=16,
                    description='Auto-detected emulator'
                )
        except:
            pass
        
        # Default to real hardware
        return BackplaneConfig(
            name='default',
            controller_ip='192.168.1.201',
            controller_port=80,
            node_count=16,
            description='Default hardware'
        )
    
    def __len__(self):
        """Return number of backplanes."""
        return len(self.backplanes)
    
    def __iter__(self):
        """Iterate over backplanes."""
        return iter(self.backplanes)


def create_default_config(output_file: str):
    """
    Create a default configuration file.
    
    Args:
        output_file: Path to output file
    """
    config = ClusterConfig()
    
    # Add example backplanes
    config.add_backplane(BackplaneConfig(
        name="backplane-0",
        controller_ip="192.168.1.201",
        controller_port=80,
        node_count=16,
        description="Primary backplane"
    ))
    
    config.add_backplane(BackplaneConfig(
        name="backplane-1",
        controller_ip="192.168.1.223",
        controller_port=80,
        node_count=16,
        description="Secondary backplane"
    ))
    
    config.save(output_file)
    print(f"Created default configuration: {output_file}")


def get_cluster_config(config_file: Optional[str] = None) -> ClusterConfig:
    """
    Get cluster configuration (singleton pattern).
    
    Args:
        config_file: Optional path to configuration file
        
    Returns:
        ClusterConfig instance
    """
    return ClusterConfig(config_file)


if __name__ == '__main__':
    import sys
    
    if len(sys.argv) > 1:
        create_default_config(sys.argv[1])
    else:
        create_default_config('cluster.json')
