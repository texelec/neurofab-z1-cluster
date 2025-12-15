"""
SNN Execution Engine Emulator

Simulates Leaky Integrate-and-Fire (LIF) neurons and spike propagation.
"""

import time
import struct
import threading
from typing import Dict, List, Optional, Tuple, Callable
from dataclasses import dataclass
from collections import deque


@dataclass
class Neuron:
    """LIF neuron state."""
    neuron_id: int
    membrane_potential: float = 0.0
    threshold: float = 1.0
    leak_rate: float = 0.95
    refractory_period_us: int = 1000
    last_spike_time_us: int = 0
    synapse_count: int = 0
    flags: int = 0


@dataclass
class Synapse:
    """Synapse connection."""
    source_neuron_global_id: int  # (backplane_id << 24) | (node_id << 16) | local_id
    weight: float
    delay_us: int = 1000


@dataclass
class Spike:
    """Spike event."""
    neuron_id: int
    source_node: int
    source_backplane: int
    timestamp_us: int
    value: float = 1.0


class SNNEngine:
    """Simulates SNN execution on a compute node."""
    
    def __init__(self, node_id: int, backplane_id: int):
        """
        Initialize SNN engine.
        
        Args:
            node_id: Node ID
            backplane_id: Backplane ID
        """
        self.node_id = node_id
        self.backplane_id = backplane_id
        
        # Neuron table
        self.neurons: Dict[int, Neuron] = {}
        self.synapses: Dict[int, List[Synapse]] = {}  # neuron_id -> list of synapses
        
        # Spike queues
        self.incoming_spikes: deque = deque()
        self.outgoing_spikes: deque = deque()
        
        # Simulation state
        self.running = False
        self.current_time_us = 0
        self.timestep_us = 1000  # 1ms default
        
        # Statistics
        self.stats = {
            'total_spikes_received': 0,
            'total_spikes_sent': 0,
            'neurons_spiked': 0,
            'simulation_steps': 0
        }
        
        # Execution thread
        self.exec_thread: Optional[threading.Thread] = None
        self.spike_callback: Optional[Callable] = None
    
    def load_from_parsed_neurons(self, parsed_neurons: List):
        """
        Load neurons and synapses from parsed neuron table.
        
        Args:
            parsed_neurons: List of ParsedNeuron objects from node.py
        """
        self.neurons.clear()
        self.synapses.clear()
        
        for pn in parsed_neurons:
            # Create neuron
            neuron = Neuron(
                neuron_id=pn.neuron_id,
                membrane_potential=pn.membrane_potential,
                threshold=pn.threshold,
                leak_rate=pn.leak_rate,
                refractory_period_us=pn.refractory_period_us,
                last_spike_time_us=pn.last_spike_time,
                synapse_count=pn.synapse_count,
                flags=pn.flags
            )
            self.neurons[pn.neuron_id] = neuron
            
            # Create synapses
            synapses = []
            for source_id, weight_int in pn.synapses:
                # Convert 8-bit weight to float (0-255 -> 0.0-1.0)
                weight_float = weight_int / 255.0
                
                synapse = Synapse(
                    source_neuron_global_id=source_id,
                    weight=weight_float,
                    delay_us=1000  # Default 1ms delay
                )
                synapses.append(synapse)
            
            self.synapses[pn.neuron_id] = synapses
    
    def inject_spike(self, neuron_id: int, value: float = 1.0):
        """
        Inject external spike - directly causes the neuron to fire.
        
        Args:
            neuron_id: Local neuron ID on this node
            value: Spike value (default 1.0)
        """
        self.stats['total_spikes_received'] += 1
        
        # Get the neuron
        neuron = self.neurons.get(neuron_id)
        if not neuron:
            return
        
        # For input neurons, directly generate a spike
        # Input neurons typically have no incoming synapses
        if not self.synapses.get(neuron_id):
            # This is likely an input neuron - make it spike
            self._generate_spike(neuron)
        else:
            # For non-input neurons, add to membrane potential
            neuron.membrane_potential += value
            if neuron.membrane_potential >= neuron.threshold:
                self._generate_spike(neuron)
    
    def start(self, timestep_us: int = 1000):
        """
        Start SNN execution.
        
        Args:
            timestep_us: Simulation timestep in microseconds
        """
        if self.running:
            return
        
        self.timestep_us = timestep_us
        self.running = True
        self.current_time_us = 0
        
        # Start execution thread
        self.exec_thread = threading.Thread(target=self._execution_loop, daemon=True)
        self.exec_thread.start()
    
    def stop(self):
        """Stop SNN execution."""
        self.running = False
        if self.exec_thread:
            self.exec_thread.join(timeout=1.0)
            self.exec_thread = None
    
    def _execution_loop(self):
        """Main execution loop."""
        while self.running:
            self._simulation_step()
            time.sleep(self.timestep_us / 1_000_000.0)  # Convert to seconds
    
    def _simulation_step(self):
        """Execute one simulation step."""
        self.stats['simulation_steps'] += 1
        self.current_time_us += self.timestep_us
        
        # Process incoming spikes
        while self.incoming_spikes:
            spike = self.incoming_spikes.popleft()
            self._process_spike(spike)
        
        # Update all neurons (leak)
        for neuron in self.neurons.values():
            if neuron.membrane_potential > 0:
                neuron.membrane_potential *= neuron.leak_rate
    
    def _process_spike(self, spike: Spike):
        """
        Process incoming spike.
        
        Args:
            spike: Spike to process
        """
        # Calculate global ID of spiking neuron
        spike_global_id = (spike.source_backplane << 24) | (spike.source_node << 16) | spike.neuron_id
        
        # Find all neurons that have synapses from this source
        for target_neuron_id, synapses in self.synapses.items():
            neuron = self.neurons.get(target_neuron_id)
            if not neuron:
                continue
            
            # Check if in refractory period
            if self.current_time_us - neuron.last_spike_time_us < neuron.refractory_period_us:
                continue
            
            # Apply synaptic input
            for synapse in synapses:
                if synapse.source_neuron_global_id == spike_global_id:
                    # Add weighted input to membrane potential
                    neuron.membrane_potential += synapse.weight * spike.value
                    
                    # Check for spike
                    if neuron.membrane_potential >= neuron.threshold:
                        self._generate_spike(neuron)
                        break
    
    def _generate_spike(self, neuron: Neuron):
        """
        Generate output spike from neuron.
        
        Args:
            neuron: Neuron that is spiking
        """
        # Reset neuron
        neuron.membrane_potential = 0.0
        neuron.last_spike_time_us = self.current_time_us
        
        # Create output spike
        spike = Spike(
            neuron_id=neuron.neuron_id,
            source_node=self.node_id,
            source_backplane=self.backplane_id,
            timestamp_us=self.current_time_us,
            value=1.0
        )
        
        self.outgoing_spikes.append(spike)
        self.stats['total_spikes_sent'] += 1
        self.stats['neurons_spiked'] += 1
        
        # Call callback if set
        if self.spike_callback:
            self.spike_callback(spike)
    
    def get_outgoing_spikes(self) -> List[Spike]:
        """Get and clear outgoing spikes."""
        spikes = list(self.outgoing_spikes)
        self.outgoing_spikes.clear()
        return spikes
    
    def get_stats(self) -> Dict:
        """Get engine statistics."""
        return {
            'node_id': self.node_id,
            'backplane_id': self.backplane_id,
            'neuron_count': len(self.neurons),
            'synapse_count': sum(len(syns) for syns in self.synapses.values()),
            'running': self.running,
            'current_time_us': self.current_time_us,
            'timestep_us': self.timestep_us,
            'stats': self.stats.copy()
        }


class ClusterSNNCoordinator:
    """Coordinates SNN execution across multiple nodes."""
    
    def __init__(self):
        """Initialize coordinator."""
        self.engines: Dict[Tuple[int, int], SNNEngine] = {}  # (backplane_id, node_id) -> engine
        self.spike_routing_active = False
        self.routing_thread: Optional[threading.Thread] = None
        
        # Global spike buffer
        self.global_spike_buffer: deque = deque(maxlen=10000)
        self.buffer_lock = threading.Lock()
    
    def register_engine(self, engine: SNNEngine):
        """Register an SNN engine."""
        key = (engine.backplane_id, engine.node_id)
        self.engines[key] = engine
        
        # Set spike callback to route spikes
        engine.spike_callback = self._route_spike
    
    def unregister_engine(self, backplane_id: int, node_id: int):
        """Unregister an SNN engine."""
        key = (backplane_id, node_id)
        if key in self.engines:
            engine = self.engines[key]
            engine.stop()
            del self.engines[key]
    
    def start_all(self, timestep_us: int = 1000):
        """Start all engines."""
        for engine in self.engines.values():
            engine.start(timestep_us)
        
        # Start spike routing
        self.spike_routing_active = True
        self.routing_thread = threading.Thread(target=self._routing_loop, daemon=True)
        self.routing_thread.start()
    
    def stop_all(self):
        """Stop all engines."""
        self.spike_routing_active = False
        if self.routing_thread:
            self.routing_thread.join(timeout=1.0)
            self.routing_thread = None
        
        for engine in self.engines.values():
            engine.stop()
    
    def inject_spike(self, backplane_id: int, node_id: int, neuron_id: int, value: float = 1.0):
        """Inject spike into specific neuron."""
        key = (backplane_id, node_id)
        engine = self.engines.get(key)
        if engine:
            engine.inject_spike(neuron_id, value)
    
    def _route_spike(self, spike: Spike):
        """Route spike to appropriate engines."""
        # Add to global buffer
        with self.buffer_lock:
            self.global_spike_buffer.append(spike)
        
        # Broadcast to all engines (they will filter based on synapses)
        for engine in self.engines.values():
            engine.incoming_spikes.append(spike)
    
    def _routing_loop(self):
        """Spike routing loop."""
        while self.spike_routing_active:
            # Collect spikes from all engines
            for engine in self.engines.values():
                spikes = engine.get_outgoing_spikes()
                for spike in spikes:
                    self._route_spike(spike)
            
            time.sleep(0.001)  # 1ms
    
    def get_global_activity(self) -> Dict:
        """Get global cluster activity."""
        total_neurons = sum(len(e.neurons) for e in self.engines.values())
        total_spikes_sent = sum(e.stats['total_spikes_sent'] for e in self.engines.values())
        total_spikes_received = sum(e.stats['total_spikes_received'] for e in self.engines.values())
        
        return {
            'total_engines': len(self.engines),
            'total_neurons': total_neurons,
            'total_spikes_sent': total_spikes_sent,
            'total_spikes_received': total_spikes_received,
            'routing_active': self.spike_routing_active
        }
    
    def get_recent_spikes(self, count: int = 100) -> List[Dict]:
        """Get recent spikes from buffer."""
        with self.buffer_lock:
            spikes = list(self.global_spike_buffer)[-count:]
        
        return [
            {
                'neuron_id': s.neuron_id,
                'node_id': s.source_node,
                'backplane_id': s.source_backplane,
                'timestamp_us': s.timestamp_us,
                'value': s.value
            }
            for s in spikes
        ]
