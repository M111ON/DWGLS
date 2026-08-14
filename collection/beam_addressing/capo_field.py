#!/usr/bin/env python3
"""
Capo Partitioning System
========================
Clone field → route by hash prefix → parallel processing
Simple partitioning for horizontal scaling
"""

import hashlib
import struct
import os
import time
from typing import Tuple, Dict, List, Optional

# ============================================================================
# SINGLE CAPO (BASE FIELD)
# ============================================================================

class Capo:
    """Single field partition - one capo"""
    
    def __init__(self, capo_id: int):
        self.capo_id = capo_id
        self.data = {}  # coord_key → value
        self.stats = {'reads': 0, 'writes': 0}
    
    def store(self, azimuth: int, elevation: int, radius: int, direction: int, tick: int, value: bytes):
        key = f"{azimuth}:{elevation}:{radius}:{direction}:{tick}"
        self.data[key] = value
        self.stats['writes'] += 1
    
    def lookup(self, azimuth: int, elevation: int, radius: int, direction: int, tick: int) -> Optional[bytes]:
        key = f"{azimuth}:{elevation}:{radius}:{direction}:{tick}"
        self.stats['reads'] += 1
        return self.data.get(key)
    
    def size(self) -> int:
        return len(self.data)


# ============================================================================
# CAPO FIELD (PARTITIONED)
# ============================================================================

class CapoField:
    """
    Multi-partition field using capo system.
    Data routed to correct capo by hash prefix.
    """
    
    # Coordinate limits (fibo_tick integrated)
    AZIMUTH_MAX = 360
    ELEVATION_MAX = 360
    RADIUS_MAX = 100
    DIRECTION_MAX = 6       # 6 directions (fibo_tick full cycle)
    TICK_MAX = 3456         # 20736 / 6 = 3456 ticks per direction
    FIBO_CYCLE = 20736      # full fibo_tick cycle
    
    def __init__(self, num_capos: int = 4):
        self.num_capos = num_capos
        self.capos = [Capo(i) for i in range(num_capos)]
        
        # Total addressable slots per capo
        self.slots_per_capo = (self.AZIMUTH_MAX * self.ELEVATION_MAX * 
                               self.RADIUS_MAX * self.DIRECTION_MAX * self.TICK_MAX)
        self.total_slots = self.slots_per_capo * num_capos
        
        print(f"[CapoField] Initialized: {num_capos} capos")
        print(f"[CapoField] Slots per capo: {self.slots_per_capo:,}")
        print(f"[CapoField] Total slots: {self.total_slots:,}")
    
    def hash_to_coords(self, data: bytes) -> Tuple[int, int, int, int, int, int]:
        """
        Deterministic hash → (capo_id, azimuth, elevation, radius, direction, tick)
        Fibo-time: 20736 cycle = 6 directions × 3456 ticks
        """
        h = hashlib.sha256(data).digest()
        
        # Split 256-bit hash into components
        capo_id   = struct.unpack('<Q', h[0:8])[0]   % self.num_capos
        azimuth   = struct.unpack('<Q', h[8:16])[0]  % self.AZIMUTH_MAX
        elevation = struct.unpack('<Q', h[16:24])[0] % self.ELEVATION_MAX
        radius    = struct.unpack('<Q', h[24:32])[0] % self.RADIUS_MAX
        direction = struct.unpack('<Q', h[32:40])[0] % self.DIRECTION_MAX if len(h) >= 40 else 0
        tick      = struct.unpack('<Q', h[40:48])[0] % self.TICK_MAX if len(h) >= 48 else 0
        
        return (capo_id, azimuth, elevation, radius, direction, tick)
    
    def store(self, data: bytes, metadata: dict = None) -> Tuple[int, int, int, int, int, int]:
        """Store data → get coordinate back"""
        capo_id, az, el, r, d, t = self.hash_to_coords(data)
        self.capos[capo_id].store(az, el, r, d, t, data)
        return (capo_id, az, el, r, d, t)
    
    def lookup(self, capo_id: int, azimuth: int, elevation: int, 
               radius: int, direction: int, tick: int) -> Optional[bytes]:
        """Lookup by coordinate → get data"""
        return self.capos[capo_id].lookup(azimuth, elevation, radius, direction, tick)
    
    def lookup_by_data(self, data: bytes) -> Optional[bytes]:
        """Lookup original data by re-hashing"""
        capo_id, az, el, r, t = self.hash_to_coords(data)
        return self.lookup(capo_id, az, el, r, t)
    
    def verify_deterministic(self, data: bytes) -> bool:
        """Verify same data → same coordinate"""
        c1 = self.hash_to_coords(data)
        c2 = self.hash_to_coords(data)
        c3 = self.hash_to_coords(data)
        return c1 == c2 == c3
    
    def print_stats(self):
        """Print field statistics"""
        print(f"\n{'='*60}")
        print(f"CAPO FIELD STATISTICS")
        print(f"{'='*60}")
        print(f"Num capos:         {self.num_capos}")
        print(f"Total slots:       {self.total_slots:,}")
        print(f"")
        for capo in self.capos:
            print(f"  Capo {capo.capo_id}: {capo.size():>12,} entries "
                  f"(W:{capo.stats['writes']}, R:{capo.stats['reads']})")
        total_entries = sum(c.size() for c in self.capos)
        print(f"  {'─'*40}")
        print(f"  Total:           {total_entries:>12,} entries")
        print(f"  Utilization:     {total_entries / self.total_slots * 100:.6f}%")
        print(f"{'='*60}\n")


# ============================================================================
# DEMO
# ============================================================================

def demo_capo_field():
    """Demonstrate capo partitioning"""
    print("\n" + "="*60)
    print("CAPO PARTITIONING SYSTEM DEMO")
    print("="*60)
    
    # Create 4-capo field
    field = CapoField(num_capos=4)
    
    # Test data
    test_files = [
        (r"I:\model\smolVLM-256M-Instruct-text.Q8_0.gguf", "smolVLM-256M"),
        (r"I:\model\Kokoro_no_espeak_Q8.gguf", "Kokoro"),
        (r"I:\model\SmolLM2-360M-Instruct.Q8_0.gguf", "SmolLM2-360M"),
    ]
    
    for file_path, name in test_files:
        if not os.path.exists(file_path):
            print(f"\n[Skip] {file_path} not found")
            continue
        
        file_size = os.path.getsize(file_path)
        chunk_size = 64
        
        print(f"\n[Stress] {name} ({file_size/1024/1024:.1f} MB)")
        
        start = time.time()
        total_chunks = 0
        capo_counts = [0] * field.num_capos
        
        with open(file_path, 'rb') as f:
            while True:
                chunk = f.read(chunk_size)
                if not chunk:
                    break
                
                coords = field.store(chunk)
                capo_counts[coords[0]] += 1
                total_chunks += 1
                
                if total_chunks % 500000 == 0:
                    elapsed = time.time() - start
                    rate = total_chunks / elapsed
                    print(f"  {total_chunks:>10,} chunks ({rate:,.0f}/s)")
        
        elapsed = time.time() - start
        rate = total_chunks / elapsed
        
        print(f"  Done: {total_chunks:,} chunks in {elapsed:.1f}s ({rate:,.0f}/s)")
        print(f"  Distribution:")
        for i, count in enumerate(capo_counts):
            pct = count / total_chunks * 100
            bar = "█" * int(pct / 2)
            print(f"    Capo {i}: {count:>10,} ({pct:>5.1f}%) {bar}")
    
    field.print_stats()
    
    # Verify determinism
    print("\n[Verify] Determinism test...")
    test_data = [b"test_chunk_1", b"test_chunk_2", b"test_chunk_3"]
    for data in test_data:
        result = field.verify_deterministic(data)
        coord = field.hash_to_coords(data)
        print(f"  {data}: deterministic={result}, coord={coord}")
    
    print("\n" + "="*60)
    print("DEMO COMPLETE")
    print("="*60)


if __name__ == "__main__":
    demo_capo_field()
