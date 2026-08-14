#!/usr/bin/env python3
"""
Stress Test: Deterministic Field with Real Models
===================================================
Test capacity, collision rate, memory usage with actual .gguf files
"""

import hashlib
import struct
import os
import sys
import time
from typing import Tuple, Dict

# ============================================================================
# DETERMINISTIC HASH → COORDINATE
# ============================================================================

def data_to_coordinate(data: bytes,
                       max_angle: int = 360,
                       max_length: int = 100,
                       max_steps: int = 10000) -> Tuple[int, int, int]:
    hash_bytes = hashlib.sha256(data).digest()
    angle = struct.unpack('<Q', hash_bytes[0:8])[0] % max_angle
    length = struct.unpack('<Q', hash_bytes[8:16])[0] % max_length
    steps = struct.unpack('<Q', hash_bytes[16:24])[0] % max_steps
    return (angle, length, steps)

# ============================================================================
# STRESS TEST
# ============================================================================

def stress_test(file_path: str, chunk_size: int = 64):
    """Test with real model file"""
    file_size = os.path.getsize(file_path)
    file_name = os.path.basename(file_path)
    
    print(f"\n{'='*60}")
    print(f"STRESS TEST: {file_name}")
    print(f"{'='*60}")
    print(f"File size: {file_size:,} bytes ({file_size/1024/1024:.1f} MB)")
    print(f"Chunk size: {chunk_size} bytes")
    print(f"Expected chunks: {file_size // chunk_size:,}")
    
    # Storage
    coordinate_index = {}  # coord_key → chunk_data
    collisions = 0
    total_chunks = 0
    
    # Memory tracking
    start_mem = sys.getsizeof(coordinate_index)
    
    # Read and process
    print(f"\n[Processing] Reading file...")
    start_time = time.time()
    
    with open(file_path, 'rb') as f:
        while True:
            chunk = f.read(chunk_size)
            if not chunk:
                break
            
            # Get deterministic coordinate
            coord = data_to_coordinate(chunk)
            coord_key = f"{coord[0]}:{coord[1]}:{coord[2]}"
            
            # Check collision
            if coord_key in coordinate_index:
                collisions += 1
                old_chunk = coordinate_index[coord_key]['data']
                if old_chunk != chunk:
                    # Real collision: same coordinate, different data
                    pass
            
            # Store
            coordinate_index[coord_key] = {
                'data': chunk,
                'offset': total_chunks * chunk_size
            }
            
            total_chunks += 1
            
            # Progress
            if total_chunks % 100000 == 0:
                elapsed = time.time() - start_time
                rate = total_chunks / elapsed
                print(f"  {total_chunks:,} chunks processed ({rate:,.0f} chunks/sec)")
    
    elapsed = time.time() - start_time
    
    # Memory
    end_mem = sys.getsizeof(coordinate_index)
    
    # Calculate actual memory per entry
    total_data_size = sum(len(v['data']) for v in coordinate_index.values())
    
    print(f"\n{'='*60}")
    print(f"RESULTS: {file_name}")
    print(f"{'='*60}")
    print(f"Total chunks:      {total_chunks:,}")
    print(f"Unique coordinates: {len(coordinate_index):,}")
    print(f"Collisions:        {collisions:,} ({collisions/max(total_chunks,1)*100:.4f}%)")
    print(f"Processing time:   {elapsed:.2f} seconds")
    print(f"Processing rate:   {total_chunks/elapsed:,.0f} chunks/sec")
    print(f"")
    print(f"MEMORY:")
    print(f"  Index size:      {end_mem:,} bytes")
    print(f"  Data stored:     {total_data_size:,} bytes ({total_data_size/1024/1024:.1f} MB)")
    print(f"  Per entry:       {(end_mem + total_data_size) / max(len(coordinate_index),1):.1f} bytes")
    print(f"")
    print(f"COORDINATE SPACE:")
    print(f"  Used slots:      {len(coordinate_index):,}")
    print(f"  Total slots:     {360 * 100 * 10000:,}")
    print(f"  Utilization:     {len(coordinate_index) / (360 * 100 * 10000) * 100:.6f}%")
    print(f"{'='*60}")
    
    return {
        'file': file_name,
        'total_chunks': total_chunks,
        'unique': len(coordinate_index),
        'collisions': collisions,
        'elapsed': elapsed,
        'rate': total_chunks / elapsed
    }


def main():
    """Run stress test on all models"""
    
    # Test with smallest first
    test_files = [
        r"I:\model\Kokoro_no_espeak_Q8.gguf",          # 197MB
        r"I:\model\smolVLM-256M-Instruct-text.Q8_0.gguf", # 167MB
        r"I:\model\SmolLM2-360M-Instruct.Q8_0.gguf",   # 369MB
    ]
    
    print("\n" + "="*60)
    print("DETERMINISTIC FIELD - CAPACITY STRESS TEST")
    print("="*60)
    
    results = []
    
    for file_path in test_files:
        if os.path.exists(file_path):
            result = stress_test(file_path, chunk_size=64)
            results.append(result)
        else:
            print(f"\n[Skip] {file_path} not found")
    
    # Summary
    print("\n" + "="*60)
    print("SUMMARY")
    print("="*60)
    print(f"{'File':<40} {'Chunks':>10} {'Unique':>10} {'Collisions':>10} {'Rate':>12}")
    print("-"*85)
    for r in results:
        print(f"{r['file']:<40} {r['total_chunks']:>10,} {r['unique']:>10,} {r['collisions']:>10,} {r['rate']:>10,.0f}/s")
    print("="*60)


if __name__ == "__main__":
    main()
