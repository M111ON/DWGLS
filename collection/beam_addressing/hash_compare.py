#!/usr/bin/env python3
"""
xxHash Mapper — Fast Non-Cryptographic Hash for Beam Addressing
================================================================
xxHash: 5-10x faster than SHA256, excellent distribution

Compare: SHA256 vs xxHash64 vs xxHash3
"""

import hashlib
import xxhash
import struct
import os
import time
from typing import Tuple, Optional

# ============================================================================
# HASH MAPPERS
# ============================================================================

FRAME_SEEK_CYCLE = 20736  # fibo_tick

def sha256_to_coord(data: bytes) -> Tuple[int, int, int, int]:
    """SHA256 → 4D coordinate"""
    h = hashlib.sha256(data).digest()
    azimuth   = struct.unpack('<Q', h[0:8])[0]   % 360
    elevation = struct.unpack('<Q', h[8:16])[0]  % 360
    radius    = struct.unpack('<Q', h[16:24])[0] % 100
    tick      = struct.unpack('<Q', h[24:32])[0] % FRAME_SEEK_CYCLE
    return (azimuth, elevation, radius, tick)

def xxhash64_to_coord(data: bytes) -> Tuple[int, int, int, int]:
    """xxHash64 → 4D coordinate (8-byte hash, split into 4 components)"""
    h = xxhash.xxh64(data).digest()
    val = struct.unpack('<Q', h[0:8])[0]
    azimuth   = val % 360
    elevation = (val // 360) % 360
    radius    = (val // 129600) % 100
    tick      = (val // 12960000) % FRAME_SEEK_CYCLE
    return (azimuth, elevation, radius, tick)

def xxhash3_to_coord(data: bytes) -> Tuple[int, int, int, int]:
    """xxHash3 → 4D coordinate"""
    h = xxhash.xxh3_64(data).digest()
    val = struct.unpack('<Q', h[0:8])[0]
    azimuth   = val % 360
    elevation = (val // 360) % 360
    radius    = (val // 129600) % 100
    tick      = (val // 12960000) % FRAME_SEEK_CYCLE
    return (azimuth, elevation, radius, tick)

def xxhash64_1_to_coord(data: bytes) -> Tuple[int, int, int, int]:
    """xxHash64 → 4D coordinate (full 64-bit split)"""
    h = xxhash.xxh64(data).digest()
    val = struct.unpack('<Q', h[0:8])[0]
    azimuth   = val % 360
    elevation = (val // 360) % 360
    radius    = (val // 129600) % 100
    tick      = (val // 12960000) % FRAME_SEEK_CYCLE
    return (azimuth, elevation, radius, tick)


# ============================================================================
# FIELD
# ============================================================================

class HashField:
    def __init__(self, name: str, hash_func):
        self.name = name
        self.hash_func = hash_func
        self.data = {}
        self.stats = {'collisions': 0}
    
    def store(self, data: bytes) -> Tuple[int, int, int, int]:
        coord = self.hash_func(data)
        key = f"{coord[0]}:{coord[1]}:{coord[2]}:{coord[3]}"
        if key in self.data and self.data[key] != data:
            self.stats['collisions'] += 1
        self.data[key] = data
        return coord
    
    def verify_deterministic(self, data: bytes) -> bool:
        c1 = self.hash_func(data)
        c2 = self.hash_func(data)
        return c1 == c2


# ============================================================================
# STRESS TEST
# ============================================================================

def stress_test(file_path: str, chunk_size: int = 64):
    if not os.path.exists(file_path):
        return None
    
    file_size = os.path.getsize(file_path)
    file_name = os.path.basename(file_path)
    
    print(f"\n  {file_name} ({file_size/1024/1024:.1f} MB)")
    
    # Test all hash functions
    mappers = [
        ("SHA256", sha256_to_coord),
        ("xxHash64", xxhash64_to_coord),
        ("xxHash3", xxhash3_to_coord),
        ("xxHash64-split", xxhash64_1_to_coord),
    ]
    
    results = {}
    for name, func in mappers:
        field = HashField(name, func)
        
        start = time.time()
        total = 0
        
        with open(file_path, 'rb') as f:
            while True:
                chunk = f.read(chunk_size)
                if not chunk:
                    break
                field.store(chunk)
                total += 1
        
        elapsed = time.time() - start
        rate = total / elapsed
        
        # Determinism check
        test = b"A" * 64
        det = field.verify_deterministic(test)
        
        results[name] = {
            'collisions': field.stats['collisions'],
            'rate': rate,
            'deterministic': det
        }
    
    return {'file': file_name, 'results': results}


if __name__ == "__main__":
    print("=" * 70)
    print("HASH COMPARISON: SHA256 vs xxHash (Beam Addressing)")
    print("=" * 70)
    
    test_files = [
        r"I:\model\smolVLM-256M-Instruct-text.Q8_0.gguf",
        r"I:\model\Kokoro_no_espeak_Q8.gguf",
        r"I:\model\SmolLM2-360M-Instruct.Q8_0.gguf",
    ]
    
    all_results = []
    for fp in test_files:
        r = stress_test(fp)
        if r:
            all_results.append(r)
    
    # Print comparison table
    print("\n" + "=" * 70)
    print("COMPARISON TABLE")
    print("=" * 70)
    print(f"{'Hash':<15} {'smolVLM':>15} {'Kokoro':>15} {'SmolLM2':>15} {'Avg Rate':>15}")
    print(f"{'':15} {'(chunks/s)':>15} {'(chunks/s)':>15} {'(chunks/s)':>15} {'':>15}")
    print("-" * 70)
    
    for mapper_name in ["SHA256", "xxHash64", "xxHash3", "xxHash64-split"]:
        rates = []
        for r in all_results:
            rate = r['results'][mapper_name]['rate']
            rates.append(rate)
        avg = sum(rates) / len(rates)
        print(f"{mapper_name:<15} {rates[0]:>15,.0f} {rates[1]:>15,.0f} {rates[2]:>15,.0f} {avg:>15,.0f}")
    
    print("\n" + "-" * 70)
    print(f"{'Hash':<15} {'smolVLM':>15} {'Kokoro':>15} {'SmolLM2':>15}")
    print(f"{'':15} {'(collisions)':>15} {'(collisions)':>15} {'(collisions)':>15}")
    print("-" * 70)
    
    for mapper_name in ["SHA256", "xxHash64", "xxHash3", "xxHash64-split"]:
        cols = []
        for r in all_results:
            col = r['results'][mapper_name]['collisions']
            cols.append(col)
        print(f"{mapper_name:<15} {cols[0]:>15,} {cols[1]:>15,} {cols[2]:>15,}")
    
    # Speedup calculation
    sha_rate = sum(r['results']['SHA256']['rate'] for r in all_results) / len(all_results)
    for name in ["xxHash64", "xxHash3", "xxHash64-split"]:
        xx_rate = sum(r['results'][name]['rate'] for r in all_results) / len(all_results)
        speedup = xx_rate / sha_rate
        print(f"\n{name} vs SHA256: {speedup:.2f}x faster")
    
    print("\n" + "=" * 70)
