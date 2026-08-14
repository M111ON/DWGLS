#!/usr/bin/env python3
"""
Beam Addressing System for Entropy Management
==============================================
Every value gets coordinates: (angle, length, step)
Beam pointer moves to coordinate → instant access

Test with structured weights mimicking real model layers
"""

import numpy as np
import json
import time
import struct
from dataclasses import dataclass, field
from typing import Dict, List, Tuple, Optional

# ============================================================================
# CORE: Beam Coordinate System
# ============================================================================

@dataclass
class BeamCoordinate:
    """3D coordinate in beam space"""
    angle: int      # direction (0-359 degrees)
    length: int     # radius from center
    step: int       # time/tick index
    
    def to_tuple(self) -> Tuple[int, int, int]:
        return (self.angle, self.length, self.step)
    
    def __hash__(self):
        return hash((self.angle, self.length, self.step))
    
    def __eq__(self, other):
        return self.to_tuple() == other.to_tuple()

@dataclass
class WeightEntry:
    """A single weight with its coordinate and metadata"""
    coordinate: BeamCoordinate
    value: float
    polarity: int  # +1 or -1 (top/bottom surface)
    layer_id: int = 0
    param_index: int = 0
    
    def to_dict(self):
        return {
            'coordinate': self.coordinate.to_tuple(),
            'value': float(self.value),
            'polarity': self.polarity,
            'layer_id': self.layer_id,
            'param_index': self.param_index
        }

class BeamEntropySystem:
    """
    Geometric addressing system for entropy management
    
    Coordinates: (angle, length, step)
    - angle: direction from center (0-359)
    - length: distance from center (radius) 
    - step: time/tick index
    
    Polarity: even length = top surface (+), odd length = bottom surface (-)
    """
    
    def __init__(self, 
                 max_angle: int = 360,
                 max_length: int = 10,
                 max_steps: int = 100):
        self.max_angle = max_angle
        self.max_length = max_length
        self.max_steps = max_steps
        
        # Coordinate → WeightEntry mapping (dict for O(1) access)
        self.coordinates: Dict[Tuple[int,int,int], WeightEntry] = {}
        
        # Layer index: layer_id → list of coordinates
        self.layer_index: Dict[int, List[BeamCoordinate]] = {}
        
        # Statistics
        self.total_writes = 0
        self.total_reads = 0
        self.clock_tick = 0
        
        print(f"[BeamSystem] Initialized: {max_angle} angles × {max_length} lengths × {max_steps} steps")
        print(f"[BeamSystem] Total addressable slots: {max_angle * max_length * max_steps:,}")
    
    def snap_calibrate(self):
        """Lock geometry before data enters"""
        self.clock_tick += 1
        return self.clock_tick
    
    def write(self, value: float, angle: int, length: int, step: int,
              layer_id: int = 0, param_index: int = 0) -> BeamCoordinate:
        """Write a value to beam coordinate"""
        # Determine polarity: top surface (+) or bottom surface (-)
        polarity = 1 if length % 2 == 0 else -1
        
        coord = BeamCoordinate(
            angle=angle % self.max_angle,
            length=length % self.max_length,
            step=step % self.max_steps
        )
        
        entry = WeightEntry(
            coordinate=coord,
            value=value,
            polarity=polarity,
            layer_id=layer_id,
            param_index=param_index
        )
        
        self.coordinates[coord.to_tuple()] = entry
        
        # Update layer index
        if layer_id not in self.layer_index:
            self.layer_index[layer_id] = []
        self.layer_index[layer_id].append(coord)
        
        self.total_writes += 1
        return coord
    
    def read(self, angle: int, length: int, step: int) -> Optional[WeightEntry]:
        """Read value by moving beam pointer to coordinate - O(1)"""
        coord_key = (angle % self.max_angle, 
                     length % self.max_length, 
                     step % self.max_steps)
        
        self.total_reads += 1
        return self.coordinates.get(coord_key)
    
    def read_by_layer(self, layer_id: int) -> List[WeightEntry]:
        """Read all weights in a layer"""
        coords = self.layer_index.get(layer_id, [])
        return [self.coordinates[c.to_tuple()] for c in coords if c.to_tuple() in self.coordinates]
    
    def beam_scan(self, angle_range: Tuple[int,int], 
                  length_range: Tuple[int,int],
                  step_range: Tuple[int,int]) -> List[WeightEntry]:
        """Scan a region of beam space"""
        results = []
        for a in range(angle_range[0], angle_range[1]):
            for l in range(length_range[0], length_range[1]):
                for s in range(step_range[0], step_range[1]):
                    entry = self.read(a, l, s)
                    if entry:
                        results.append(entry)
        return results
    
    def snapshot(self) -> bytes:
        """Capture full state as compact binary"""
        # Header: tick, count, max_angle, max_length, max_steps
        header = struct.pack('IIIii', 
                            self.clock_tick,
                            len(self.coordinates),
                            self.max_angle,
                            self.max_length,
                            self.max_steps)
        
        # Entries: angle, length, step, value (float32), polarity, layer_id
        entries = []
        for coord, entry in self.coordinates.items():
            entries.append(struct.pack('iiifii',
                                      coord[0], coord[1], coord[2],
                                      entry.value,
                                      entry.polarity,
                                      entry.layer_id))
        
        return header + b''.join(entries)
    
    def restore(self, data: bytes):
        """Restore from snapshot"""
        # Parse header: tick, count, max_angle, max_length, max_steps (5 x 4 bytes = 20 bytes)
        tick, count, max_a, max_l, max_s = struct.unpack('IIIii', data[:20])
        
        self.clock_tick = tick
        self.max_angle = max_a
        self.max_length = max_l
        self.max_steps = max_s
        self.coordinates.clear()
        self.layer_index.clear()
        
        # Parse entries (24 bytes each)
        offset = 20  # After 20-byte header
        for i in range(count):
            a, l, s, val, pol, layer = struct.unpack('iiifii', data[offset:offset+24])
            offset += 24
            
            coord = BeamCoordinate(a, l, s)
            entry = WeightEntry(coord, val, pol, layer)
            self.coordinates[coord.to_tuple()] = entry
            
            if layer not in self.layer_index:
                self.layer_index[layer] = []
            self.layer_index[layer].append(coord)
        
        self.total_writes = count
        self.total_reads = 0
    
    def print_stats(self):
        """Print system statistics"""
        total_slots = self.max_angle * self.max_length * self.max_steps
        print(f"\n{'='*60}")
        print(f"BEAM ADDRESSING SYSTEM STATISTICS")
        print(f"{'='*60}")
        print(f"Clock tick:        {self.clock_tick}")
        print(f"Total entries:     {len(self.coordinates):,}")
        print(f"Total writes:      {self.total_writes:,}")
        print(f"Total reads:       {self.total_reads:,}")
        print(f"Layers:            {len(self.layer_index)}")
        print(f"Addressable slots: {total_slots:,}")
        print(f"Utilization:       {len(self.coordinates) / total_slots * 100:.2f}%")
        print(f"{'='*60}\n")


# ============================================================================
# MODEL: Structured weights mimicking real neural network
# ============================================================================

def create_structured_model(layers_config: List[Tuple[str, int]]) -> List[Tuple[str, np.ndarray]]:
    """
    Create structured model weights mimicking real architecture
    
    layers_config: [(layer_name, num_params), ...]
    Returns: [(layer_name, weights_array), ...]
    """
    print(f"\n[Model] Creating structured model...")
    print(f"[Model] Architecture: {len(layers_config)} layers")
    
    model = []
    total_params = 0
    
    for layer_name, num_params in layers_config:
        # Different distributions for different layer types
        if 'embedding' in layer_name:
            weights = np.random.randn(num_params).astype(np.float32) * 0.1
        elif 'attention' in layer_name:
            weights = np.random.randn(num_params).astype(np.float32) * 0.02
        elif 'ffn' in layer_name or 'mlp' in layer_name:
            weights = np.random.randn(num_params).astype(np.float32) * 0.02
        elif 'norm' in layer_name:
            weights = np.random.randn(num_params).astype(np.float32) * 0.01
        else:
            weights = np.random.randn(num_params).astype(np.float32) * 0.02
        
        model.append((layer_name, weights))
        total_params += num_params
        print(f"  {layer_name}: {num_params:,} params")
    
    print(f"[Model] Total params: {total_params:,}")
    return model

def load_model_to_beam(model: List[Tuple[str, np.ndarray]], system: BeamEntropySystem):
    """
    Load model weights into beam coordinate system
    Each layer gets its own step range for temporal separation
    """
    print(f"\n[Loader] Loading model into beam space...")
    
    # Calculate total params
    total_params = sum(len(w) for _, w in model)
    
    # Calculate grid dimensions
    # We want each layer to have its own time range
    num_layers = len(model)
    steps_per_layer = max(1, 100 // num_layers)  # Divide 100 steps among layers
    
    print(f"[Loader] Grid: 360 angles × {system.max_length} lengths × {system.max_steps} steps")
    print(f"[Loader] Steps per layer: {steps_per_layer}")
    
    start_time = time.time()
    
    param_counter = 0
    for layer_idx, (layer_name, weights) in enumerate(model):
        layer_start_step = layer_idx * steps_per_layer
        
        for param_idx, weight in enumerate(weights):
            # Map to 3D coordinate
            angle = param_counter % 360
            length = (param_counter // 360) % system.max_length
            step = layer_start_step + (param_counter // (360 * system.max_length)) % steps_per_layer
            
            system.write(weight, angle, length, step,
                        layer_id=layer_idx,
                        param_index=param_idx)
            
            param_counter += 1
    
    elapsed = time.time() - start_time
    print(f"[Loader] Loaded {param_counter:,} params in {elapsed:.4f} seconds")
    print(f"[Loader] Rate: {param_counter / elapsed:,.0f} params/second")
    
    return num_layers, steps_per_layer


# ============================================================================
# DEMO: Beam pointer access patterns
# ============================================================================

def demo_beam_access_patterns(system: BeamEntropySystem, 
                               model: List[Tuple[str, np.ndarray]],
                               num_layers: int,
                               steps_per_layer: int):
    """Demonstrate different beam access patterns"""
    print(f"\n{'='*60}")
    print(f"BEAM POINTER ACCESS PATTERNS")
    print(f"{'='*60}")
    
    # Pattern 1: Direct coordinate access
    print(f"\n[Pattern 1] Direct coordinate access:")
    test_coords = [(0, 0, 0), (180, 5, 5), (90, 3, 10)]
    for angle, length, step in test_coords:
        entry = system.read(angle, length, step)
        if entry:
            print(f"  ({angle}°, len={length}, step={step}): value={entry.value:.6f}, layer={entry.layer_id}")
    
    # Pattern 2: Layer-wise access (all weights in a layer)
    print(f"\n[Pattern 2] Layer-wise access:")
    for layer_idx in range(min(3, num_layers)):
        layer_weights = system.read_by_layer(layer_idx)
        print(f"  Layer {layer_idx}: {len(layer_weights)} weights")
        if layer_weights:
            vals = [w.value for w in layer_weights]
            print(f"    Range: [{min(vals):.4f}, {max(vals):.4f}]")
    
    # Pattern 3: Angular sweep (same radius, different angles)
    print(f"\n[Pattern 3] Angular sweep at length=0, step=0:")
    sweep_results = system.beam_scan((0, 10), (0, 1), (0, 1))
    for entry in sweep_results[:5]:
        print(f"  Angle {entry.coordinate.angle}°: value={entry.value:.4f}, polarity={entry.polarity:+d}")
    
    # Pattern 4: Radial scan (different radii, same angle)
    print(f"\n[Pattern 4] Radial scan at angle=0°, step=0:")
    for length in range(min(5, system.max_length)):
        entry = system.read(0, length, 0)
        if entry:
            print(f"  Length {length}: value={entry.value:.4f}, polarity={entry.polarity:+d}")
    
    print(f"\n{'='*60}\n")


def demo_performance_benchmark(system: BeamEntropySystem):
    """Benchmark beam pointer performance"""
    print(f"{'='*60}")
    print(f"PERFORMANCE BENCHMARK")
    print(f"{'='*60}")
    
    # Random reads
    num_reads = 10000
    print(f"\n[Benchmark] {num_reads:,} random reads...")
    
    start = time.time()
    for _ in range(num_reads):
        angle = np.random.randint(0, 360)
        length = np.random.randint(0, system.max_length)
        step = np.random.randint(0, system.max_steps)
        system.read(angle, length, step)
    elapsed = time.time() - start
    
    print(f"  Time: {elapsed:.4f} seconds")
    print(f"  Rate: {num_reads / elapsed:,.0f} reads/second")
    print(f"  Per read: {elapsed / num_reads * 1e6:.2f} µs")
    
    # Sequential scan
    scan_size = 1000
    print(f"\n[Benchmark] Sequential scan of {scan_size} coordinates...")
    
    start = time.time()
    results = system.beam_scan((0, 10), (0, 10), (0, 10))
    elapsed = time.time() - start
    
    print(f"  Found: {len(results)} entries")
    print(f"  Time: {elapsed*1000:.2f} ms")
    if elapsed > 0:
        print(f"  Rate: {len(results) / elapsed:,.0f} entries/second")
    else:
        print(f"  Rate: < 1 µs per entry (too fast to measure)")
    
    print(f"\n{'='*60}\n")


def demo_snapshot_replay(system: BeamEntropySystem):
    """Demonstrate snapshot and replay with binary format"""
    print(f"{'='*60}")
    print(f"SNAPSHOT & REPLAY DEMO (Binary Format)")
    print(f"{'='*60}")
    
    # Take snapshot
    print(f"\n[Snapshot] Capturing state...")
    snapshot_data = system.snapshot()
    snapshot_size = len(snapshot_data)
    print(f"[Snapshot] Size: {snapshot_size:,} bytes ({snapshot_size/1024:.2f} KB)")
    print(f"[Snapshot] Entries: {len(system.coordinates):,}")
    
    # Modify system
    print(f"\n[Modify] Writing 1000 new values...")
    for i in range(1000):
        system.write(float(i * 1.1), 
                    angle=i % 360, 
                    length=(i // 360) % system.max_length, 
                    step=(i // (360 * system.max_length)) % system.max_steps)
    print(f"[Modify] Total entries now: {len(system.coordinates):,}")
    
    # Restore from snapshot
    print(f"\n[Restore] Restoring from snapshot...")
    original_entries = len(system.coordinates)
    system.restore(snapshot_data)
    print(f"[Restore] Entries: {len(system.coordinates):,} (was {original_entries:,})")
    
    # Verify
    entry = system.read(0, 0, 0)
    if entry:
        print(f"[Verify] Entry (0,0,0): {entry.value:.6f}")
    else:
        print(f"[Verify] Entry (0,0,0): NOT FOUND")
    
    print(f"\n{'='*60}\n")


# ============================================================================
# MAIN
# ============================================================================

if __name__ == "__main__":
    print("\n" + "="*60)
    print("BEAM ADDRESSING SYSTEM FOR ENTROPY MANAGEMENT")
    print("Structured Model Test")
    print("="*60)
    
    # Create system with appropriate dimensions
    system = BeamEntropySystem(
        max_angle=360,
        max_length=10,
        max_steps=100
    )
    
    # Create structured model (mimicking small transformer)
    model_config = [
        ("embedding", 1000),
        ("attention_q", 500),
        ("attention_k", 500),
        ("attention_v", 500),
        ("attention_out", 500),
        ("ffn_up", 2000),
        ("ffn_down", 2000),
        ("norm_1", 100),
        ("norm_2", 100),
    ]
    
    model = create_structured_model(model_config)
    
    # Load into beam space
    num_layers, steps_per_layer = load_model_to_beam(model, system)
    
    # Print stats
    system.print_stats()
    
    # Demo access patterns
    demo_beam_access_patterns(system, model, num_layers, steps_per_layer)
    
    # Performance benchmark
    demo_performance_benchmark(system)
    
    # Demo snapshot/replay
    demo_snapshot_replay(system)
    
    # Final stats
    system.print_stats()
    
    print("Demo complete!")
