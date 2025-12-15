"""
Z1 Emulator HTTP API Server

Flask-based REST API compatible with real Z1 hardware.
"""

import base64
import json
import struct
from flask import Flask, request, jsonify
from typing import Optional

from cluster import Cluster
from snn_engine import ClusterSNNCoordinator, Spike


class Z1APIServer:
    """HTTP API server for Z1 emulator."""
    
    def __init__(self, cluster: Cluster, host: str = '127.0.0.1', port: int = 8000):
        """
        Initialize API server.
        
        Args:
            cluster: Cluster instance
            host: Server host
            port: Server port
        """
        self.cluster = cluster
        self.host = host
        self.port = port
        self.app = Flask(__name__)
        
        # SNN coordinator
        self.snn_coordinator = ClusterSNNCoordinator()
        
        # Current SNN topology
        self.current_topology: Optional[dict] = None
        
        # Setup routes
        self._setup_routes()
    
    def _setup_routes(self):
        """Setup Flask routes."""
        
        # Node management endpoints
        @self.app.route('/api/nodes', methods=['GET'])
        def get_nodes():
            """Get all nodes."""
            nodes = []
            for backplane_id, backplane in self.cluster.backplanes.items():
                for node in backplane.nodes.values():
                    info = node.get_info()
                    info['backplane_id'] = backplane_id
                    nodes.append(info)
            return jsonify({'nodes': nodes})
        
        @self.app.route('/api/nodes/<int:node_id>', methods=['GET'])
        def get_node(node_id):
            """Get specific node (assumes backplane 0)."""
            node = self.cluster.get_node(0, node_id)
            if not node:
                return jsonify({'error': 'Node not found'}), 404
            return jsonify(node.get_info())
        
        @self.app.route('/api/nodes/<int:node_id>/reset', methods=['POST'])
        def reset_node(node_id):
            """Reset specific node."""
            node = self.cluster.get_node(0, node_id)
            if not node:
                return jsonify({'error': 'Node not found'}), 404
            node.reset()
            return jsonify({'status': 'ok', 'node_id': node_id})
        
        @self.app.route('/api/nodes/<int:node_id>/memory', methods=['GET'])
        def read_memory(node_id):
            """Read node memory."""
            addr = int(request.args.get('addr', 0))
            length = int(request.args.get('length', 256))
            
            node = self.cluster.get_node(0, node_id)
            if not node:
                return jsonify({'error': 'Node not found'}), 404
            
            try:
                data = node.read_memory(addr, length)
                return jsonify({
                    'addr': addr,
                    'length': len(data),
                    'data': base64.b64encode(data).decode('ascii')
                })
            except Exception as e:
                return jsonify({'error': str(e)}), 400
        
        @self.app.route('/api/nodes/<int:node_id>/memory', methods=['POST'])
        def write_memory(node_id):
            """Write node memory."""
            data = request.json
            addr = data.get('addr', 0)
            data_b64 = data.get('data', '')
            
            node = self.cluster.get_node(0, node_id)
            if not node:
                return jsonify({'error': 'Node not found'}), 404
            
            try:
                data_bytes = base64.b64decode(data_b64)
                bytes_written = node.write_memory(addr, data_bytes)
                return jsonify({
                    'status': 'ok',
                    'bytes_written': bytes_written
                })
            except Exception as e:
                return jsonify({'error': str(e)}), 400
        
        @self.app.route('/api/nodes/<int:node_id>/firmware', methods=['GET'])
        def get_firmware_info(node_id):
            """Get firmware information."""
            node = self.cluster.get_node(0, node_id)
            if not node:
                return jsonify({'error': 'Node not found'}), 404
            
            if node.firmware_header:
                return jsonify({
                    'name': node.firmware_header.name,
                    'version': node.firmware_header.version,
                    'size': node.firmware_header.firmware_size,
                    'build_timestamp': node.firmware_header.build_timestamp
                })
            else:
                return jsonify({'name': 'None', 'version': 0})
        
        @self.app.route('/api/nodes/<int:node_id>/firmware', methods=['POST'])
        def flash_firmware(node_id):
            """Flash firmware to node."""
            data = request.json
            firmware_b64 = data.get('firmware', '')
            
            node = self.cluster.get_node(0, node_id)
            if not node:
                return jsonify({'error': 'Node not found'}), 404
            
            try:
                firmware_data = base64.b64decode(firmware_b64)
                success = node.load_firmware(firmware_data)
                if success:
                    return jsonify({'status': 'ok', 'node_id': node_id})
                else:
                    return jsonify({'error': 'Firmware load failed'}), 400
            except Exception as e:
                return jsonify({'error': str(e)}), 400
        
        # SNN management endpoints
        @self.app.route('/api/snn/deploy', methods=['POST'])
        def deploy_snn():
            """Deploy SNN topology."""
            topology = request.json
            self.current_topology = topology
            
            # This would normally be handled by nsnn tool
            # For emulator, we just store the topology
            return jsonify({
                'status': 'ok',
                'message': 'Use nsnn tool to deploy topology'
            })
        
        @self.app.route('/api/snn/topology', methods=['GET'])
        def get_topology():
            """Get current SNN topology."""
            if self.current_topology:
                return jsonify(self.current_topology)
            else:
                return jsonify({'error': 'No topology deployed'}), 404
        
        @self.app.route('/api/snn/start', methods=['POST'])
        def start_snn():
            """Start SNN execution."""
            timestep_us = request.json.get('timestep_us', 1000) if request.json else 1000
            
            # Initialize SNN engines from neuron tables in memory
            self._initialize_snn_engines()
            
            self.snn_coordinator.start_all(timestep_us)
            return jsonify({'status': 'ok'})
        
        @self.app.route('/api/snn/stop', methods=['POST'])
        def stop_snn():
            """Stop SNN execution."""
            self.snn_coordinator.stop_all()
            return jsonify({'status': 'ok'})
        
        @self.app.route('/api/snn/activity', methods=['GET'])
        def get_activity():
            """Get SNN activity."""
            activity = self.snn_coordinator.get_global_activity()
            return jsonify(activity)
        
        @self.app.route('/api/snn/events', methods=['GET'])
        def get_spike_events():
            """Get recent spike events."""
            count = int(request.args.get('count', 100))
            spikes = self.snn_coordinator.get_recent_spikes(count)
            return jsonify({'spikes': spikes, 'count': len(spikes)})
        
        @self.app.route('/api/snn/input', methods=['POST'])
        def inject_spikes():
            """Inject input spikes."""
            data = request.json
            spikes_data = data.get('spikes', [])
            
            injected = 0
            for spike_data in spikes_data:
                neuron_id = spike_data.get('neuron_id', 0)
                value = spike_data.get('value', 1.0)
                
                # Inject into all engines (they will filter based on neuron ID)
                for engine in self.snn_coordinator.engines.values():
                    if neuron_id in engine.neurons:
                        engine.inject_spike(neuron_id, value)
                        injected += 1
                        break
            
            return jsonify({
                'status': 'ok',
                'spikes_injected': injected
            })
        
        # Emulator-specific endpoints
        @self.app.route('/api/emulator/status', methods=['GET'])
        def emulator_status():
            """Get emulator status (identifies as emulator)."""
            return jsonify({
                'emulator': True,
                'version': '1.0.0',
                'cluster_info': self.cluster.get_cluster_info()
            })
        
        @self.app.route('/api/emulator/reset', methods=['POST'])
        def reset_emulator():
            """Reset entire emulator."""
            self.cluster.reset_cluster()
            self.snn_coordinator.stop_all()
            return jsonify({'status': 'ok'})
        
        @self.app.route('/api/emulator/config', methods=['GET'])
        def get_config():
            """Get emulator configuration."""
            return jsonify(self.cluster.config)
        
        @self.app.route('/api/emulator/config', methods=['POST'])
        def set_config():
            """Update emulator configuration."""
            new_config = request.json
            # Update simulation parameters
            if 'simulation' in new_config:
                self.cluster.config['simulation'].update(new_config['simulation'])
            return jsonify({'status': 'ok', 'config': self.cluster.config})
    
    def _initialize_snn_engines(self):
        """Initialize SNN engines from neuron tables in node memory."""
        import sys
        from snn_engine import SNNEngine
        
        print("[SNN] Initializing SNN engines...", file=sys.stderr, flush=True)
        
        for backplane_id, backplane in self.cluster.backplanes.items():
            for node_id, node in backplane.nodes.items():
                try:
                    # Parse neuron table from memory
                    parsed_neurons = node.parse_neuron_table()
                    
                    if parsed_neurons:
                        # Create or get engine
                        key = (backplane_id, node_id)
                        if key not in self.snn_coordinator.engines:
                            engine = SNNEngine(node_id, backplane_id)
                            engine.load_from_parsed_neurons(parsed_neurons)
                            self.snn_coordinator.register_engine(engine)
                            print(f"[SNN] Initialized engine for node {node_id}: {len(parsed_neurons)} neurons", file=sys.stderr, flush=True)
                except Exception as e:
                    print(f"[SNN] Error initializing node {node_id}: {e}", file=sys.stderr, flush=True)
                    import traceback
                    traceback.print_exc()
        
        print(f"[SNN] Total engines initialized: {len(self.snn_coordinator.engines)}", file=sys.stderr, flush=True)
    
    def run(self, debug: bool = False):
        """
        Run Flask server.
        
        Args:
            debug: Enable debug mode
        """
        self.app.run(host=self.host, port=self.port, debug=debug, threaded=True)
