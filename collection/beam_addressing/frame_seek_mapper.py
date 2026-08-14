#!/usr/bin/env python3
"""
Frame Seek Mapper — 1440 Cycle + Round Counting
================================================
Data → enc (0-1439) → face/slot/phase/ico_idx
Collision → increment round
Capacity = 1440 × unlimited rounds
"""

import struct
import os
import time
from typing import Tuple, Dict, Optional

# ============================================================================
# FRAME SEEK CONSTANTS (from FGLS)
# ============================================================================

FRAME_SEEK_CYCLE = 1440
N_FACES = 12
SLOTS_PER_FACE = 120
N_PHASES = 12
N_ICO = 162

# ============================================================================
# FRAME SEEK DECOMPOSITION (port of frame_at)
# ============================================================================

def frame_at(enc: int) -> Tuple[int, int, int, int]:
    """enc → (face, slot, phase, ico_idx)"""
    face = enc // SLOTS_PER_FACE      # 0-11
    slot = enc % SLOTS_PER_FACE       # 0-119
    phase = (enc // N_PHASES) % N_PHASES  # 0-11
    ico_idx = enc % N_ICO             # 0-161
    return (face, slot, phase, ico_idx)


# ============================================================================
# FRAME SEEK MAPPER
# ============================================================================

class FrameSeekMapper:
    """
    Data → enc (0-1439) using data bytes directly.
    No SHA256, no RDH — pure byte arithmetic.
    """
    
    def __init__(self):
        self.stats = {'maps': 0}
        print("[FrameSeekMapper] Initialized — 1440 cycle + rounds")
    
    def data_to_enc(self, data: bytes) -> int:
        """
        Data → enc (0-1439)
        Uses ALL bytes with rolling multiply + add.
        """
        enc = 0
        for i, b in enumerate(data):
            # Rolling multiply with prime, add byte value
            enc = (enc * 31 + b) % FRAME_SEEK_CYCLE
        self.stats['maps'] += 1
        return enc
    
    def data_to_coords(self, data: bytes) -> Tuple[int, int, int, int]:
        """Data → (face, slot, phase, ico_idx)"""
        enc = self.data_to_enc(data)
        return frame_at(enc)


# ============================================================================
# FRAME SEEK FIELD (with round counting)
# ============================================================================

class FrameSeekField:
    """
    Frame Seek field with round counting for collision resolution.
    
    Coordinate: (face, slot, phase, ico_idx, round)
    Capacity: 1440 positions × unlimited rounds
    """
    
    def __init__(self):
        self.mapper = FrameSeekMapper()
        # Store: (face, slot, phase, ico_idx) → list of (round, data)
        self.data = {}
        self.stats = {'reads': 0, 'writes': 0, 'collisions': 0}
        print(f"[FrameSeekField] Capacity: 1,440 base positions × unlimited rounds")
    
    def store(self, data: bytes) -> Tuple[int, int, int, int, int]:
        """
        Store data → get coordinate (face, slot, phase, ico_idx, round)
        Collision → increment round
        """
        face, slot, phase, ico_idx = self.mapper.data_to_coords(data)
        key = (face, slot, phase, ico_idx)
        
        if key not in self.data:
            self.data[key] = [(0, data)]
            self.stats['writes'] += 1
            return (face, slot, phase, ico_idx, 0)
        
        # Check for existing data
        entries = self.data[key]
        for rnd, existing in entries:
            if existing == data:
                # Already stored
                return (face, slot, phase, ico_idx, rnd)
        
        # New data at same position → new round
        new_round = len(entries)
        entries.append((new_round, data))
        self.stats['collisions'] += 1
        self.stats['writes'] += 1
        return (face, slot, phase, ico_idx, new_round)
    
    def lookup(self, face: int, slot: int, phase: int, ico_idx: int, 
               round: int) -> Optional[bytes]:
        """Lookup by coordinate → get data"""
        key = (face, slot, phase, ico_idx)
        self.stats['reads'] += 1
        
        if key not in self.data:
            return None
        
        entries = self.data[key]
        for rnd, data in entries:
            if rnd == round:
                return data
        return None
    
    def lookup_by_data(self, data: bytes) -> Optional[bytes]:
        """Lookup: recompute coord from data → find stored data"""
        face, slot, phase, ico_idx = self.mapper.data_to_coords(data)
        key = (face, slot, phase, ico_idx)
        
        if key not in self.data:
            return None
        
        for rnd, existing in self.data[key]:
            if existing == data:
                return existing
        return None
    
    def verify_deterministic(self, data: bytes) -> bool:
        """Verify same data → same coordinate"""
        c1 = self.mapper.data_to_coords(data)
        c2 = self.mapper.data_to_coords(data)
        return c1 == c2
    
    def print_stats(self):
        print(f"\n{'='*60}")
        print(f"FRAME SEEK FIELD STATISTICS")
        print(f"{'='*60}")
        print(f"Base positions:  1,440")
        print(f"Unique positions: {len(self.data):,}")
        print(f"Total entries:    {sum(len(v) for v in self.data.values()):,}")
        print(f"Writes:           {self.stats['writes']:,}")
        print(f"Reads:            {self.stats['reads']:,}")
        print(f"Collisions:       {self.stats['collisions']:,}")
        
        # Max rounds per position
        if self.data:
            max_rounds = max(len(v) for v in self.data.values())
            avg_rounds = sum(len(v) for v in self.data.values()) / len(self.data)
            print(f"Max rounds/pos:   {max_rounds}")
            print(f"Avg rounds/pos:   {avg_rounds:.2f}")
        print(f"{'='*60}\n")


# ============================================================================
# STRESS TEST
# ============================================================================

def stress_test_frame_seek(file_path: str, chunk_size: int = 64):
    """Test frame seek mapping with real model file"""
    if not os.path.exists(file_path):
        print(f"[Skip] {file_path} not found")
        return
    
    file_size = os.path.getsize(file_path)
    file_name = os.path.basename(file_path)
    
    print(f"\n{'='*60}")
    print(f"FRAME SEEK STRESS TEST: {file_name}")
    print(f"{'='*60}")
    print(f"File size: {file_size:,} bytes ({file_size/1024/1024:.1f} MB)")
    print(f"Chunk size: {chunk_size} bytes")
    
    field = FrameSeekField()
    
    start = time.time()
    total_chunks = 0
    
    with open(file_path, 'rb') as f:
        while True:
            chunk = f.read(chunk_size)
            if not chunk:
                break
            
            coord = field.store(chunk)
            total_chunks += 1
            
            if total_chunks % 500000 == 0:
                elapsed = time.time() - start
                rate = total_chunks / elapsed
                print(f"  {total_chunks:>10,} chunks ({rate:,.0f}/s) collisions={field.stats['collisions']}")
    
    elapsed = time.time() - start
    rate = total_chunks / elapsed
    
    print(f"\n  Done: {total_chunks:,} chunks in {elapsed:.1f}s ({rate:,.0f}/s)")
    
    field.print_stats()
    
    # Verify determinism
    print("[Verify] Determinism test...")
    test_chunks = [b"A" * 64, b"B" * 64, b"C" * 64]
    for chunk in test_chunks:
        result = field.verify_deterministic(chunk)
        enc = field.mapper.data_to_enc(chunk)
        face, slot, phase, ico_idx = frame_at(enc)
        coord = field.store(chunk)
        print(f"  {chunk[:4]}...: deterministic={result}, enc={enc}, face={face}, slot={slot}, coord={coord}")
    
    return {
        'file': file_name,
        'chunks': total_chunks,
        'collisions': field.stats['collisions'],
        'rate': rate
    }


if __name__ == "__main__":
    print("\n" + "="*60)
    print("FRAME SEEK MAPPER — 1440 CYCLE + ROUND COUNTING")
    print("="*60)
    
    test_files = [
        r"I:\model\smolVLM-256M-Instruct-text.Q8_0.gguf",
        r"I:\model\Kokoro_no_espeak_Q8.gguf",
        r"I:\model\SmolLM2-360M-Instruct.Q8_0.gguf",
    ]
    
    results = []
    for fp in test_files:
        r = stress_test_frame_seek(fp)
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
