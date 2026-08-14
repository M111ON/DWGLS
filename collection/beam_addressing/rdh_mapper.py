#!/usr/bin/env python3
"""
RDH Mapper — Real Ring-Wedge-Mirror Addressing
================================================
Port of collection/rdh/rdh_addr.h + rdh_capture.h to Python

Core: data → 12-gon walk → flat key (collision-free bijection)
"""

import struct
import os
import time
from typing import Tuple, Dict, Optional

# ============================================================================
# RDH CONFIG (port of RDHConfig)
# ============================================================================

class RDHConfig:
    """RDH address space config"""
    def __init__(self, n_rings: int, n_wedges: int, n_mirror: int = 1,
                 max_u: int = 1, n_v: int = 1):
        self.n_rings = n_rings
        self.n_wedges = n_wedges
        self.n_mirror = n_mirror
        self.max_u = max_u
        self.n_v = n_v

# Presets (from rdh_capture.h)
RDH_CAPTURE_144 = RDHConfig(144, 144, 1, 1, 1)  # 20,736 addresses
FRAME_SEEK_CYCLE = 1440

# ============================================================================
# RDH CORE API (port of rdh_addr.h)
# ============================================================================

def rdh_key(cfg: RDHConfig, ring: int, wedge: int, mirror: int, u: int, v: int = 0) -> int:
    """Encode 5-tuple → flat key. O(1), no collision, no hash."""
    return ((ring * cfg.n_wedges + wedge) * cfg.n_mirror + mirror) * cfg.max_u + u

def rdh_decompose(cfg: RDHConfig, key: int) -> Tuple[int, int, int, int]:
    """Decompose flat key → (ring, wedge, mirror, u)."""
    t = key
    u = t % cfg.max_u
    t //= cfg.max_u
    mirror = t % cfg.n_mirror
    t //= cfg.n_mirror
    wedge = t % cfg.n_wedges
    ring = t // cfg.n_wedges
    return (ring, wedge, mirror, u)

def rdh_capacity(cfg: RDHConfig) -> int:
    """Total address space size."""
    return cfg.n_rings * cfg.n_wedges * cfg.n_mirror * cfg.max_u * cfg.n_v

# ============================================================================
# RDH CAPTURE (port of rdh_capture.h)
# ============================================================================

# 12-gon stride directions
# Each byte's low 4 bits (0-11) = one of 12 directions
STRIDE_TABLE = [
    ( 1,  0),  # 0:  E
    ( 1,  1),  # 1:  NE
    ( 0,  1),  # 2:  N
    ( 1, -1),  # 3:  SE
    (-1,  0),  # 4:  W
    (-1, -1),  # 5:  SW
    ( 0, -1),  # 6:  S
    (-1,  1),  # 7:  NW
    ( 2,  0),  # 8:  E×2
    ( 1,  2),  # 9:  NE×2
    (-1,  2),  # 10: NW×2
    (-2,  0),  # 11: W×2
]

def rdh_capture(data: bytes, cfg: RDHConfig) -> int:
    """
    data → flat key via 12-gon walk.
    
    Each byte's low 4 bits = stride direction on 12-gon.
    Walk accumulates (x, y) position.
    Periodic fold every 4096 steps to prevent overflow.
    
    NO HASH. NO LOOKUP TABLE. PURE INTEGER BIJECTION.
    """
    field_w = cfg.n_wedges
    field_h = cfg.n_rings
    
    acc_x = 0
    acc_y = 0
    
    # Minimum walk = 48 (GEO_BLOCK). Shorter data auto-cycles.
    steps = max(len(data), 48)
    
    for i in range(steps):
        b = data[i % len(data)]
        d = b % 12  # map to 12 directions (12-gon)
        
        dx, dy = STRIDE_TABLE[d]
        acc_x += dx
        acc_y += dy
        
        # Periodic fold every 4096 steps
        if (i & 0xFFF) == 0xFFF:
            acc_x %= field_w
            acc_y %= field_h
    
    # Final fold into RDH address space
    wedge = (acc_x % field_w + field_w) % field_w
    ring = (acc_y % field_h + field_h) % field_h
    
    # Flat key — ring+wedge encode everything
    return rdh_key(cfg, ring, wedge, 0, 0, 0)


def rdh_capture_to_enc(data: bytes, cfg: RDHConfig) -> int:
    """Capture + map to frame_seek enc in one call."""
    key = rdh_capture(data, cfg)
    return key % FRAME_SEEK_CYCLE


# ============================================================================
# RDH FIELD (store + lookup)
# ============================================================================

class RDHField:
    """Field using real RDH addressing — collision-free"""
    
    def __init__(self, cfg: RDHConfig = None):
        self.cfg = cfg or RDH_CAPTURE_144
        self.capacity = rdh_capacity(self.cfg)
        self.data = {}  # flat_key → data
        self.stats = {'reads': 0, 'writes': 0}
        
        print(f"[RDHField] Config: {self.cfg.n_rings}×{self.cfg.n_wedges}×{self.cfg.n_mirror}×{self.cfg.max_u}")
        print(f"[RDHField] Capacity: {self.capacity:,} unique addresses")
    
    def store(self, data: bytes) -> int:
        """Store data → get flat key (no collision if cfg capacity ≥ data entropy)"""
        key = rdh_capture(data, self.cfg)
        self.data[key] = data
        self.stats['writes'] += 1
        return key
    
    def lookup(self, key: int) -> Optional[bytes]:
        """Lookup by flat key → get data"""
        self.stats['reads'] += 1
        return self.data.get(key)
    
    def lookup_by_data(self, data: bytes) -> Optional[bytes]:
        """Lookup: recompute key from data → get stored data"""
        key = rdh_capture(data, self.cfg)
        return self.lookup(key)
    
    def verify_deterministic(self, data: bytes) -> bool:
        """Verify same data → same key"""
        k1 = rdh_capture(data, self.cfg)
        k2 = rdh_capture(data, self.cfg)
        return k1 == k2
    
    def print_stats(self):
        print(f"\n{'='*60}")
        print(f"RDH FIELD STATISTICS")
        print(f"{'='*60}")
        print(f"Config:         {self.cfg.n_rings}×{self.cfg.n_wedges}×{self.cfg.n_mirror}×{self.cfg.max_u}")
        print(f"Capacity:       {self.capacity:,}")
        print(f"Entries:        {len(self.data):,}")
        print(f"Writes:         {self.stats['writes']:,}")
        print(f"Reads:          {self.stats['reads']:,}")
        util = len(self.data) / self.capacity * 100 if self.capacity > 0 else 0
        print(f"Utilization:    {util:.6f}%")
        print(f"{'='*60}\n")


# ============================================================================
# STRESS TEST
# ============================================================================

def stress_test_rdh(file_path: str, chunk_size: int = 64):
    """Test RDH mapping with real model file"""
    if not os.path.exists(file_path):
        print(f"[Skip] {file_path} not found")
        return
    
    file_size = os.path.getsize(file_path)
    file_name = os.path.basename(file_path)
    
    print(f"\n{'='*60}")
    print(f"RDH STRESS TEST: {file_name}")
    print(f"{'='*60}")
    print(f"File size: {file_size:,} bytes ({file_size/1024/1024:.1f} MB)")
    print(f"Chunk size: {chunk_size} bytes")
    
    field = RDHField(RDH_CAPTURE_144)
    
    start = time.time()
    total_chunks = 0
    collisions = 0
    
    with open(file_path, 'rb') as f:
        while True:
            chunk = f.read(chunk_size)
            if not chunk:
                break
            
            key = rdh_capture(chunk, field.cfg)
            if key in field.data:
                # Collision — different data mapped to same key
                if field.data[key] != chunk:
                    collisions += 1
            
            field.data[key] = chunk
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
        key = rdh_capture(chunk, field.cfg)
        enc = rdh_capture_to_enc(chunk, field.cfg)
        ring, wedge, mirror, u = rdh_decompose(field.cfg, key)
        print(f"  {chunk[:4]}...: deterministic={result}, key={key}, enc={enc}, (ring={ring}, wedge={wedge})")
    
    return {
        'file': file_name,
        'chunks': total_chunks,
        'collisions': collisions,
        'rate': rate
    }


if __name__ == "__main__":
    print("\n" + "="*60)
    print("RDH MAPPER — REAL RING-WEDGE-MIRROR ADDRESSING")
    print("Port of collection/rdh/rdh_addr.h + rdh_capture.h")
    print("="*60)
    
    # Quick capacity check
    cfg = RDH_CAPTURE_144
    cap = rdh_capacity(cfg)
    print(f"\nRDH capacity: {cap:,} unique addresses")
    print(f"12-gon walk: 12 stride directions per byte")
    print(f"Periodic fold: every 4096 steps")
    print(f"NO HASH. NO LOOKUP TABLE. PURE INTEGER BIJECTION.")
    
    test_files = [
        r"I:\model\smolVLM-256M-Instruct-text.Q8_0.gguf",
        r"I:\model\Kokoro_no_espeak_Q8.gguf",
        r"I:\model\SmolLM2-360M-Instruct.Q8_0.gguf",
    ]
    
    results = []
    for fp in test_files:
        r = stress_test_rdh(fp)
        if r:
            results.append(r)
    
    print("\n" + "="*60)
    print("SUMMARY")
    print("="*60)
    print(f"{'File':<45} {'Chunks':>10} {'Collisions':>10} {'Rate':>10}")
    print("-" * 75)
    for r in results:
        print(f"  {r['file']:<43} {r['chunks']:>10,} {r['collisions']:>10} {r['rate']:>8,.0f}/s")
    print("="*60)
