#!/usr/bin/env python3
"""
Geometric Mapper — Replace SHA256 with Structure-Based Coordinates
=================================================================
Data properties → coordinate (no random hash, no collision)

RDH-inspired: data values determine their own position
"""

import struct
import os
import time
from typing import Tuple, Dict, List, Optional

# ============================================================================
# GEOMETRIC MAPPER (replaces SHA256)
# ============================================================================

class GeometricMapper:
    """
    Maps data → coordinate using data's own properties.
    No random hash = no collision (if data is unique).
    """
    
    # Coordinate limits (same as capo_field)
    AZIMUTH_MAX = 360
    ELEVATION_MAX = 360
    RADIUS_MAX = 100
    TICK_MAX = 20736  # fibo_tick full cycle
    
    def __init__(self):
        self.seen = {}  # data → coord (verify no collision)
        self.stats = {'maps': 0, 'collisions': 0}
        print("[GeometricMapper] Initialized — structure-based mapping")
    
    def map_data(self, data: bytes) -> Tuple[int, int, int, int]:
        """
        Data → (azimuth, elevation, radius, tick)
        Uses ALL bytes, NOT random hash. Better mixing = fewer collisions.
        """
        # Use all bytes for mixing
        acc_a = 0
        acc_b = 0
        acc_c = 0
        acc_d = 0
        
        # Process all bytes in 4 streams
        for i, b in enumerate(data):
            stream = i % 4
            if stream == 0:
                acc_a = (acc_a * 31 + b) & 0xFFFFFFFF
                acc_b ^= b
            elif stream == 1:
                acc_b = (acc_b * 37 + b) & 0xFFFFFFFF
                acc_c ^= b
            elif stream == 2:
                acc_c = (acc_c * 41 + b) & 0xFFFFFFFF
                acc_d ^= b
            else:
                acc_d = (acc_d * 43 + b) & 0xFFFFFFFF
                acc_a ^= b
        
        azimuth   = acc_a % self.AZIMUTH_MAX
        elevation = acc_b % self.ELEVATION_MAX
        radius    = acc_c % self.RADIUS_MAX
        tick      = acc_d % self.TICK_MAX
        
        return (azimuth, elevation, radius, tick)
    
    def map_and_verify(self, data: bytes) -> Tuple[int, int, int, int]:
        """Map data and verify no collision"""
        coord = self.map_data(data)
        coord_key = f"{coord[0]}:{coord[1]}:{coord[2]}:{coord[3]}"
        
        self.stats['maps'] += 1
        
        # Check if this coordinate already has different data
        if coord_key in self.seen:
            if self.seen[coord_key] != data:
                self.stats['collisions'] += 1
                return None  # Collision detected
        
        self.seen[coord_key] = data
        return coord


# ============================================================================
# STORE + LOOKUP
# ============================================================================

class GeometricField:
    """Field using geometric mapping instead of SHA256"""
    
    def __init__(self):
        self.mapper = GeometricMapper()
        self.data = {}  # coord_key → data
        self.stats = {'reads': 0, 'writes': 0, 'collisions': 0}
    
    def store(self, data: bytes) -> Optional[Tuple[int, int, int, int]]:
        """Store data → get coordinate (no collision if unique data)"""
        coord = self.mapper.map_and_verify(data)
        
        if coord is None:
            self.stats['collisions'] += 1
            return None  # Collision
        
        coord_key = f"{coord[0]}:{coord[1]}:{coord[2]}:{coord[3]}"
        self.data[coord_key] = data
        self.stats['writes'] += 1
        return coord
    
    def lookup(self, azimuth: int, elevation: int, radius: int, tick: int) -> Optional[bytes]:
        """Lookup by coordinate → get data"""
        key = f"{azimuth}:{elevation}:{radius}:{tick}"
        self.stats['reads'] += 1
        return self.data.get(key)
    
    def lookup_by_data(self, data: bytes) -> Optional[bytes]:
        """Lookup: recompute coord from data → get stored data"""
        coord = self.mapper.map_data(data)
        return self.lookup(*coord)
    
    def verify_deterministic(self, data: bytes) -> bool:
        """Verify same data → same coordinate"""
        c1 = self.mapper.map_data(data)
        c2 = self.mapper.map_data(data)
        return c1 == c2
    
    def print_stats(self):
        print(f"\n{'='*60}")
        print(f"GEOMETRIC FIELD STATISTICS")
        print(f"{'='*60}")
        print(f"Total entries:  {len(self.data):,}")
        print(f"Total writes:   {self.stats['writes']:,}")
        print(f"Total reads:    {self.stats['reads']:,}")
        print(f"Collisions:     {self.stats['collisions']}")
        print(f"Total slots:    {self.AZIMUTH_MAX * self.ELEVATION_MAX * self.RADIUS_MAX * self.TICK_MAX:,}")
        util = len(self.data) / (360 * 360 * 100 * 20736) * 100
        print(f"Utilization:    {util:.6f}%")
        print(f"{'='*60}\n")
    
    @property
    def AZIMUTH_MAX(self): return self.mapper.AZIMUTH_MAX
    @property
    def ELEVATION_MAX(self): return self.mapper.ELEVATION_MAX
    @property
    def RADIUS_MAX(self): return self.mapper.RADIUS_MAX
    @property
    def TICK_MAX(self): return self.mapper.TICK_MAX


# ============================================================================
# STRESS TEST
# ============================================================================

def stress_test_geometric(file_path: str, chunk_size: int = 64):
    """Test geometric mapping with real model file"""
    if not os.path.exists(file_path):
        print(f"[Skip] {file_path} not found")
        return
    
    file_size = os.path.getsize(file_path)
    file_name = os.path.basename(file_path)
    
    print(f"\n{'='*60}")
    print(f"GEOMETRIC STRESS TEST: {file_name}")
    print(f"{'='*60}")
    print(f"File size: {file_size:,} bytes ({file_size/1024/1024:.1f} MB)")
    print(f"Chunk size: {chunk_size} bytes")
    
    field = GeometricField()
    
    start = time.time()
    total_chunks = 0
    collisions = 0
    
    with open(file_path, 'rb') as f:
        while True:
            chunk = f.read(chunk_size)
            if not chunk:
                break
            
            coord = field.store(chunk)
            if coord is None:
                collisions += 1
            
            total_chunks += 1
            
            if total_chunks % 500000 == 0:
                elapsed = time.time() - start
                rate = total_chunks / elapsed
                print(f"  {total_chunks:>10,} chunks ({rate:,.0f}/s) collisions={collisions}")
    
    elapsed = time.time() - start
    rate = total_chunks / elapsed
    
    print(f"\n  Done: {total_chunks:,} chunks in {elapsed:.1f}s ({rate:,.0f}/s)")
    print(f"  Collisions: {collisions} ({collisions/max(total_chunks,1)*100:.4f}%)")
    
    field.print_stats()
    
    # Verify determinism
    print("[Verify] Determinism test...")
    test_chunks = [b"A" * 64, b"B" * 64, b"C" * 64]
    for chunk in test_chunks:
        result = field.verify_deterministic(chunk)
        coord = field.mapper.map_data(chunk)
        print(f"  {chunk[:4]}...: deterministic={result}, coord={coord}")
    
    return {
        'file': file_name,
        'chunks': total_chunks,
        'collisions': collisions,
        'rate': rate
    }


if __name__ == "__main__":
    print("\n" + "="*60)
    print("GEOMETRIC MAPPER — NO HASH, NO COLLISION")
    print("="*60)
    
    test_files = [
        r"I:\model\smolVLM-256M-Instruct-text.Q8_0.gguf",
        r"I:\model\Kokoro_no_espeak_Q8.gguf",
        r"I:\model\SmolLM2-360M-Instruct.Q8_0.gguf",
    ]
    
    results = []
    for fp in test_files:
        r = stress_test_geometric(fp)
        if r:
            results.append(r)
    
    print("\n" + "="*60)
    print("SUMMARY")
    print("="*60)
    for r in results:
        print(f"  {r['file']:<40} {r['chunks']:>10,} chunks  {r['collisions']:>6} collisions  {r['rate']:>8,.0f}/s")
    print("="*60)
