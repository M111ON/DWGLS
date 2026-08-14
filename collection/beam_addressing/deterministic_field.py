#!/usr/bin/env python3
"""
Deterministic Geometric Hash
=============================
No real geometry - just deterministic coordinate mapping.
Same data → same coordinate ALWAYS, regardless of source.

INPUT: any data
OUTPUT: (angle, length, tick_steps)
GUARANTEE: deterministic
"""

import hashlib
import struct
import numpy as np
from typing import Tuple, Any

# ============================================================================
# DETERMINISTIC HASH → COORDINATE
# ============================================================================

def data_to_coordinate(data: bytes, 
                       max_angle: int = 360,
                       max_length: int = 100,
                       max_steps: int = 10000) -> Tuple[int, int, int]:
    """
    Deterministic mapping: data → (angle, length, steps)
    
    Same data ALWAYS produces same coordinate.
    No geometry involved - pure hash function.
    """
    # Create deterministic hash
    hash_bytes = hashlib.sha256(data).digest()
    
    # Extract 3 components from hash
    # Use first 8 bytes for angle (0-359)
    angle_raw = struct.unpack('<Q', hash_bytes[0:8])[0]
    angle = angle_raw % max_angle
    
    # Use next 8 bytes for length (0-99)
    length_raw = struct.unpack('<Q', hash_bytes[8:16])[0]
    length = length_raw % max_length
    
    # Use next 8 bytes for steps (0-9999)
    steps_raw = struct.unpack('<Q', hash_bytes[16:24])[0]
    steps = steps_raw % max_steps
    
    return (angle, length, steps)


def coordinate_to_data_key(angle: int, length: int, steps: int) -> str:
    """Create string key from coordinate for indexing"""
    return f"{angle}:{length}:{steps}"


# ============================================================================
# DETERMINISTIC FIELD (Storage + Lookup)
# ============================================================================

class DeterministicField:
    """
    Field where every data point has a deterministic coordinate.
    Same data → same coordinate always.
    """
    
    def __init__(self):
        # Coordinate → data mapping
        self.coordinate_index = {}  # key → {data, metadata}
        
        # Data → coordinate mapping (reverse index)
        self.data_index = {}  # data_hash → coordinate
        
        print("[DeterministicField] Initialized")
    
    def store(self, data: bytes, metadata: dict = None) -> Tuple[int, int, int]:
        """
        Store data with deterministic coordinate.
        Returns coordinate that can be used for instant lookup.
        """
        # Get deterministic coordinate
        coordinate = data_to_coordinate(data)
        key = coordinate_to_data_key(*coordinate)
        
        # Store
        self.coordinate_index[key] = {
            'data': data,
            'metadata': metadata or {},
            'coordinate': coordinate
        }
        
        # Reverse index
        data_hash = hashlib.sha256(data).hexdigest()
        self.data_index[data_hash] = coordinate
        
        print(f"[Store] Coordinate: {coordinate}")
        return coordinate
    
    def lookup(self, angle: int, length: int, steps: int) -> dict:
        """
        O(1) lookup by coordinate.
        Beam pointer: point to coordinate → get data instantly.
        """
        key = coordinate_to_data_key(angle, length, steps)
        return self.coordinate_index.get(key, None)
    
    def get_coordinate(self, data: bytes) -> Tuple[int, int, int]:
        """
        Get deterministic coordinate for data.
        Same data → same coordinate always.
        """
        data_hash = hashlib.sha256(data).hexdigest()
        return self.data_index.get(data_hash, None)
    
    def verify_deterministic(self, data: bytes) -> bool:
        """
        Verify that same data always produces same coordinate.
        """
        coord1 = data_to_coordinate(data)
        coord2 = data_to_coordinate(data)
        coord3 = data_to_coordinate(data)
        
        return coord1 == coord2 == coord3
    
    def print_stats(self):
        print(f"\n{'='*60}")
        print(f"DETERMINISTIC FIELD STATISTICS")
        print(f"{'='*60}")
        print(f"Total entries: {len(self.coordinate_index)}")
        print(f"{'='*60}\n")


# ============================================================================
# DEMO
# ============================================================================

def demo_deterministic():
    print("\n" + "="*60)
    print("DETERMINISTIC GEOMETRIC HASH DEMO")
    print("="*60)
    
    field = DeterministicField()
    
    # Test data
    test_data = [
        b"hello world",
        b"model_weight_0.5",
        b"tensor_data_123",
        b"same_data_again",
    ]
    
    # Store data
    print("\n[Store] Storing test data...")
    coordinates = []
    for data in test_data:
        coord = field.store(data, {'source': 'test'})
        coordinates.append(coord)
    
    field.print_stats()
    
    # Verify determinism
    print("\n[Verify] Testing determinism...")
    for data in test_data:
        is_deterministic = field.verify_deterministic(data)
        print(f"  {data}: deterministic = {is_deterministic}")
    
    # Lookup by coordinate
    print("\n[Lookup] Beam pointer access...")
    for i, (data, coord) in enumerate(zip(test_data, coordinates)):
        result = field.lookup(*coord)
        if result:
            print(f"  Coordinate {coord}: data = {result['data']}")
    
    # Show same data from different "sources"
    print("\n[Demo] Same data, different sources...")
    original = b"important_data"
    coord1 = field.store(original, {'source': 'file_A'})
    coord2 = field.store(original, {'source': 'file_B'})
    coord3 = field.store(original, {'source': 'file_C'})
    
    print(f"  File A: {coord1}")
    print(f"  File B: {coord2}")
    print(f"  File C: {coord3}")
    print(f"  All same? {coord1 == coord2 == coord3}")
    
    print("\n" + "="*60)
    print("DEMO COMPLETE")
    print("="*60)


if __name__ == "__main__":
    demo_deterministic()
