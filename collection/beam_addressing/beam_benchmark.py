#!/usr/bin/env python3
"""
beam_benchmark.py — Python vs C Benchmark for Beam Addressing
══════════════════════════════════════════════════════════════════

Compares:
  1. Pure Python beam_value.py
  2. C DLL via ctypes (beam_value.dll)

Measures: throughput (ops/sec), speedup factor
"""

import ctypes
import os
import time
import sys
import struct

# ============================================================================
# LOAD C DLL
# ============================================================================

DLL_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "beam_value.dll")

if not os.path.exists(DLL_PATH):
    print(f"[ERROR] DLL not found: {DLL_PATH}")
    print("Compile: gcc -O2 -shared -o beam_value.dll beam_value_dll.c -I../core -I../collection -I../collection/rdh -I../collection/dgls/geo/include")
    sys.exit(1)

dll = ctypes.CDLL(DLL_PATH)

# Define function signatures
dll.beam_store.restype = ctypes.c_uint32
dll.beam_store.argtypes = [
    ctypes.POINTER(ctypes.c_int32),  # weights
    ctypes.c_uint32,                  # count
    ctypes.POINTER(ctypes.c_uint32),  # capo_ids
    ctypes.POINTER(ctypes.c_uint32),  # param_indices
    ctypes.POINTER(ctypes.c_uint32),  # abs_values
    ctypes.POINTER(ctypes.c_uint8),   # signs
]

dll.beam_recover.restype = None
dll.beam_recover.argtypes = [
    ctypes.POINTER(ctypes.c_uint32),  # capo_ids
    ctypes.POINTER(ctypes.c_uint32),  # param_indices
    ctypes.POINTER(ctypes.c_uint32),  # abs_values
    ctypes.POINTER(ctypes.c_uint8),   # signs
    ctypes.c_uint32,                   # count
    ctypes.POINTER(ctypes.c_int32),   # weights
]

dll.beam_verify.restype = ctypes.c_int
dll.beam_verify.argtypes = [
    ctypes.POINTER(ctypes.c_int32),  # weights
    ctypes.c_uint32,                  # count
]

dll.beam_verify_fgls.restype = ctypes.c_int
dll.beam_verify_fgls.argtypes = []

dll.beam_to_fibo_slots.restype = None
dll.beam_to_fibo_slots.argtypes = [
    ctypes.POINTER(ctypes.c_int32),  # weights
    ctypes.c_uint32,                  # count
    ctypes.POINTER(ctypes.c_uint32),  # slots
]

dll.beam_to_frames.restype = None
dll.beam_to_frames.argtypes = [
    ctypes.POINTER(ctypes.c_int32),  # weights
    ctypes.c_uint32,                  # count
    ctypes.POINTER(ctypes.c_uint8),   # faces
    ctypes.POINTER(ctypes.c_uint8),   # frame_slots
]

dll.beam_to_sphericals.restype = None
dll.beam_to_sphericals.argtypes = [
    ctypes.POINTER(ctypes.c_int32),  # weights
    ctypes.c_uint32,                  # count
    ctypes.POINTER(ctypes.c_uint16),  # azimuths
    ctypes.POINTER(ctypes.c_uint16),  # elevations
]

dll.beam_stats.restype = None
dll.beam_stats.argtypes = [
    ctypes.POINTER(ctypes.c_int32),  # weights
    ctypes.c_uint32,                  # count
    ctypes.POINTER(ctypes.c_int32),   # min_val
    ctypes.POINTER(ctypes.c_int32),   # max_val
    ctypes.POINTER(ctypes.c_uint32),  # pos_count
    ctypes.POINTER(ctypes.c_uint32),  # neg_count
]

# ============================================================================
# PYTHON REFERENCE IMPLEMENTATION
# ============================================================================

def python_beam_store(weights):
    """Pure Python: weight → coord"""
    coords = []
    for i, w in enumerate(weights):
        capo_id = i // 1000000
        sign = 1 if w >= 0 else 0
        abs_val = abs(w)
        coords.append((capo_id, i, abs_val, sign))
    return coords

def python_beam_verify(weights, coords):
    """Pure Python: verify roundtrip"""
    for i, (capo_id, param_index, abs_val, sign) in enumerate(coords):
        recovered = abs_val if sign == 1 else -abs_val
        if recovered != weights[i]:
            return False
    return True

# ============================================================================
# BENCHMARK FUNCTIONS
# ============================================================================

def bench_python(N):
    """Benchmark pure Python implementation"""
    import random
    random.seed(42)
    weights = [random.randint(-128, 127) for _ in range(N)]
    
    # Store
    start = time.perf_counter()
    coords = python_beam_store(weights)
    t_store = time.perf_counter() - start
    
    # Verify
    start = time.perf_counter()
    ok = python_beam_verify(weights, coords)
    t_verify = time.perf_counter() - start
    
    return {
        'store': t_store,
        'verify': t_verify,
        'ops_store': N / t_store if t_store > 0 else 0,
        'ops_verify': N / t_verify if t_verify > 0 else 0,
        'pass': ok
    }

def bench_c(N):
    """Benchmark C DLL implementation"""
    import random
    random.seed(42)
    weights = [random.randint(-128, 127) for _ in range(N)]
    
    # Convert to C arrays
    c_weights = (ctypes.c_int32 * N)(*weights)
    c_capo_ids = (ctypes.c_uint32 * N)()
    c_param_indices = (ctypes.c_uint32 * N)()
    c_abs_values = (ctypes.c_uint32 * N)()
    c_signs = (ctypes.c_uint8 * N)()
    
    # Store
    start = time.perf_counter()
    stored = dll.beam_store(c_weights, N, c_capo_ids, c_param_indices, c_abs_values, c_signs)
    t_store = time.perf_counter() - start
    
    # Verify
    start = time.perf_counter()
    ok = dll.beam_verify(c_weights, N)
    t_verify = time.perf_counter() - start
    
    # Fibo slots
    c_slots = (ctypes.c_uint32 * N)()
    start = time.perf_counter()
    dll.beam_to_fibo_slots(c_weights, N, c_slots)
    t_fibo = time.perf_counter() - start
    
    # Frames
    c_faces = (ctypes.c_uint8 * N)()
    c_frame_slots = (ctypes.c_uint8 * N)()
    start = time.perf_counter()
    dll.beam_to_frames(c_weights, N, c_faces, c_frame_slots)
    t_frame = time.perf_counter() - start
    
    # Spherical
    c_az = (ctypes.c_uint16 * N)()
    c_el = (ctypes.c_uint16 * N)()
    start = time.perf_counter()
    dll.beam_to_sphericals(c_weights, N, c_az, c_el)
    t_spherical = time.perf_counter() - start
    
    return {
        'store': t_store,
        'verify': t_verify,
        'fibo': t_fibo,
        'frame': t_frame,
        'spherical': t_spherical,
        'ops_store': N / t_store if t_store > 0 else 0,
        'ops_verify': N / t_verify if t_verify > 0 else 0,
        'ops_fibo': N / t_fibo if t_fibo > 0 else 0,
        'ops_frame': N / t_frame if t_frame > 0 else 0,
        'ops_spherical': N / t_spherical if t_spherical > 0 else 0,
        'pass': ok == 1
    }

# ============================================================================
# MAIN
# ============================================================================

if __name__ == "__main__":
    print("╔══════════════════════════════════════════════════════════╗")
    print("║  BEAM ADDRESSING: Python vs C Benchmark                 ║")
    print("║  Coordinate IS the data. No hash. No collision.        ║")
    print("╚══════════════════════════════════════════════════════════╝")
    print()
    
    # Verify FGLS core
    fgls_ok = dll.beam_verify_fgls()
    print(f"FGLS Core Verify: {'PASS' if fgls_ok == 0 else 'FAIL (code={})'.format(fgls_ok)}")
    print()
    
    # Run benchmarks
    for N in [10000, 100000, 1000000]:
        print(f"{'='*60}")
        print(f"N = {N:,}")
        print(f"{'='*60}")
        
        py = bench_python(N)
        c = bench_c(N)
        
        print(f"\n{'Operation':<20} {'Python':<15} {'C':<15} {'Speedup':<10}")
        print(f"{'-'*60}")
        
        for op in ['store', 'verify']:
            py_t = py[op]
            c_t = c[op]
            speedup = py_t / c_t if c_t > 0 else 0
            py_ops = py[f'ops_{op}']
            c_ops = c[f'ops_{op}']
            print(f"{op:<20} {py_ops:>10,.0f}/s  {c_ops:>10,.0f}/s  {speedup:>7.1f}x")
        
        for op in ['fibo', 'frame', 'spherical']:
            c_t = c[op]
            c_ops = c[f'ops_{op}']
            print(f"{op:<20} {'N/A':<15} {c_ops:>10,.0f}/s  {'C only':>10}")
        
        print(f"\nRoundtrip: Python={'✓' if py['pass'] else '✗'}  C={'✓' if c['pass'] else '✗'}")
        print()
    
    print("Done.")
