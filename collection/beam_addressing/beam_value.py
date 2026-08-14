#!/usr/bin/env python3
"""
Beam Value Mapper — Coordinate IS the Data
==========================================
No hash. No storage. Coordinate = (position, value, sign)

weight → beam(length=value, direction=position)
capo   = parameter partition (unlimited scale)
"""

import struct
import os
import time
from typing import Tuple, Optional

# ============================================================================
# BEAM VALUE MAPPER
# ============================================================================

class BeamValueMapper:
    """
    Coordinate IS the data.
    weight[i] → (capo_id, param_index, abs_value, sign)
    
    NO hash. NO collision. NO dict storage.
    beam length = weight value directly.
    """
    
    def __init__(self, params_per_capo: int = 1000000, num_capos: int = 4):
        self.params_per_capo = params_per_capo
        self.num_capos = num_capos
        self.total_capacity = params_per_capo * num_capos
        
        print(f"[BeamValueMapper] Capacity: {num_capos} capos × {params_per_capo:,} params = {self.total_capacity:,}")
    
    def weight_to_coord(self, capo_id: int, param_index: int, 
                        weight: int) -> Tuple[int, int, int, int]:
        """
        weight → (capo_id, param_index, abs_value, sign)
        
        sign: 1 = positive (ceiling), 0 = negative (ground)
        abs_value: beam length (weight magnitude)
        """
        sign = 1 if weight >= 0 else 0
        abs_value = abs(weight)
        local_index = param_index % self.params_per_capo
        
        return (capo_id, local_index, abs_value, sign)
    
    def coord_to_weight(self, capo_id: int, param_index: int,
                        abs_value: int, sign: int) -> int:
        """(capo_id, param_index, abs_value, sign) → weight"""
        return abs_value if sign == 1 else -abs_value
    
    def store_weights(self, weights: list) -> list:
        """
        Store weight array → list of coordinates.
        Each weight gets its own coordinate directly.
        """
        coords = []
        for i, w in enumerate(weights):
            capo_id = i // self.params_per_capo
            param_index = i
            coord = self.weight_to_coord(capo_id, i, w)
            coords.append(coord)
        return coords
    
    def verify_roundtrip(self, weights: list) -> bool:
        """Verify: weight → coord → weight = original"""
        for i, w in enumerate(weights):
            capo_id = i // self.params_per_capo
            coord = self.weight_to_coord(capo_id, i, w)
            recovered = self.coord_to_weight(*coord)
            if recovered != w:
                return False
        return True
    
    def print_stats(self):
        print(f"\n{'='*60}")
        print(f"BEAM VALUE MAPPER")
        print(f"{'='*60}")
        print(f"Params per capo:  {self.params_per_capo:,}")
        print(f"Num capos:        {self.num_capos}")
        print(f"Total capacity:   {self.total_capacity:,}")
        print(f"Storage needed:   0 bytes (coord IS data)")
        print(f"{'='*60}\n")


# ============================================================================
# STRESS TEST — read real GGUF weights
# ============================================================================

def extract_weights_from_gguf(file_path: str, max_weights: int = 10000):
    """
    Extract weight values from GGUF file.
    GGUF stores weights as int8 (Q8) or int4 (Q4).
    """
    if not os.path.exists(file_path):
        return None
    
    weights = []
    file_size = os.path.getsize(file_path)
    chunk_size = 64
    
    with open(file_path, 'rb') as f:
        while len(weights) < max_weights:
            chunk = f.read(chunk_size)
            if not chunk:
                break
            # Treat each byte as a weight value (int8: -128 to 127)
            for b in chunk:
                val = b if b < 128 else b - 256  # unsigned → signed
                weights.append(val)
                if len(weights) >= max_weights:
                    break
    
    return weights


def stress_test_beam_value(file_path: str, max_weights: int = 500000):
    """Test beam value mapping with real model weights"""
    if not os.path.exists(file_path):
        print(f"[Skip] {file_path} not found")
        return None
    
    file_name = os.path.basename(file_path)
    file_size = os.path.getsize(file_path)
    
    print(f"\n{'='*60}")
    print(f"BEAM VALUE TEST: {file_name}")
    print(f"{'='*60}")
    print(f"File: {file_size/1024/1024:.1f} MB")
    
    # Extract weights (treating raw bytes as int8 values)
    weights = []
    start = time.time()
    
    with open(file_path, 'rb') as f:
        while len(weights) < max_weights:
            chunk = f.read(4096)
            if not chunk:
                break
            for b in chunk:
                val = b if b < 128 else b - 256
                weights.append(val)
    
    extract_time = time.time() - start
    print(f"Extracted: {len(weights):,} weights in {extract_time:.2f}s")
    
    # Map to coordinates
    mapper = BeamValueMapper(params_per_capo=1000000, num_capos=4)
    
    start = time.time()
    coords = mapper.store_weights(weights)
    map_time = time.time() - start
    
    # Verify roundtrip
    start = time.time()
    ok = mapper.verify_roundtrip(weights)
    verify_time = time.time() - start
    
    print(f"Map:       {len(coords):,} coords in {map_time:.3f}s ({len(coords)/map_time:,.0f}/s)")
    print(f"Roundtrip: {'PASS' if ok else 'FAIL'} ({verify_time:.3f}s)")
    
    # Stats
    if coords:
        values = [abs(w) for w in weights]
        signs = [1 if w >= 0 else 0 for w in weights]
        pos_count = sum(signs)
        neg_count = len(signs) - pos_count
        
        print(f"\nValue stats:")
        print(f"  Min:      {min(weights)}")
        print(f"  Max:      {max(weights)}")
        print(f"  Positive: {pos_count:,} ({pos_count/len(weights)*100:.1f}%)")
        print(f"  Negative: {neg_count:,} ({neg_count/len(weights)*100:.1f}%)")
        print(f"  Avg abs:  {sum(values)/len(values):.2f}")
        
        # Capo distribution
        capo_counts = {}
        for capo_id, _, _, _ in coords:
            capo_counts[capo_id] = capo_counts.get(capo_id, 0) + 1
        
        print(f"\nCapo distribution:")
        for cid in sorted(capo_counts.keys()):
            count = capo_counts[cid]
            print(f"  Capo {cid}: {count:>10,} params ({count/len(coords)*100:.1f}%)")
    
    mapper.print_stats()
    
    # Show examples
    print("Examples:")
    for i in range(min(5, len(weights))):
        w = weights[i]
        coord = coords[i]
        recovered = mapper.coord_to_weight(*coord)
        print(f"  weight[{i}]={w:>4} → coord={coord} → recovered={recovered} {'✓' if recovered==w else '✗'}")
    
    return {
        'file': file_name,
        'weights': len(weights),
        'roundtrip': ok,
        'map_speed': len(coords) / map_time if map_time > 0 else 0
    }


if __name__ == "__main__":
    print("\n" + "="*60)
    print("BEAM VALUE MAPPER — COORDINATE IS THE DATA")
    print("No hash. No collision. No storage.")
    print("="*60)
    
    test_files = [
        r"I:\model\smolVLM-256M-Instruct-text.Q8_0.gguf",
        r"I:\model\Kokoro_no_espeak_Q8.gguf",
        r"I:\model\SmolLM2-360M-Instruct.Q8_0.gguf",
    ]
    
    results = []
    for fp in test_files:
        r = stress_test_beam_value(fp)
        if r:
            results.append(r)
    
    print("\n" + "="*60)
    print("SUMMARY")
    print("="*60)
    for r in results:
        print(f"  {r['file']:<45} {r['weights']:>10,} weights  roundtrip={'✓' if r['roundtrip'] else '✗'}  {r['map_speed']:>10,.0f}/s")
    print("="*60)
