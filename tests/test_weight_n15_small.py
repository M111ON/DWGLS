#!/usr/bin/env python3
"""
Test n15 on model weights - use SmolLM2 (smaller model)
"""
import sys
import struct
import numpy as np

def read_gguf_tensors_simple(path, max_tensors=5):
    """Read first few tensor names and offsets from GGUF."""
    with open(path, 'rb') as f:
        # Header
        magic = f.read(4)
        assert magic == b'GGUF'
        version = struct.unpack('<I', f.read(4))[0]
        n_tensors = struct.unpack('<Q', f.read(8))[0]
        n_kv = struct.unpack('<Q', f.read(8))[0]
        
        print(f"GGUF: version={version}, tensors={n_tensors}, kv={n_kv}")
        
        # Skip KV by seeking - we know typical GGUF KV section isn't huge for small models
        # But let's be safe: read key, then skip value
        for _ in range(n_kv):
            key_len = struct.unpack('<Q', f.read(8))[0]
            key = f.read(key_len)
            val_type = struct.unpack('<I', f.read(4))[0]
            
            # Skip value
            if val_type == 8:  # string
                val_len = struct.unpack('<Q', f.read(8))[0]
                if val_len > 10_000_000:
                    # Too large, seek instead
                    f.seek(val_len, 1)
                else:
                    f.read(val_len)
            elif val_type in (0, 1, 11): f.read(1)
            elif val_type in (2, 3): f.read(2)
            elif val_type in (4, 5, 9): f.read(4)
            elif val_type in (6, 7, 10): f.read(8)
            elif val_type == 12:  # array
                arr_type = struct.unpack('<I', f.read(4))[0]
                arr_len = struct.unpack('<Q', f.read(8))[0]
                if arr_type in (0, 1): f.read(arr_len)
                elif arr_type in (2, 3): f.read(arr_len * 2)
                elif arr_type in (4, 5, 9): f.read(arr_len * 4)
                elif arr_type in (6, 7, 10): f.read(arr_len * 8)
        
        # Tensor infos
        tensor_infos = []
        for i in range(min(n_tensors, max_tensors)):
            name_len = struct.unpack('<Q', f.read(8))[0]
            name = f.read(name_len).decode('utf-8')
            n_dims = struct.unpack('<Q', f.read(8))[0]
            shape = [struct.unpack('<Q', f.read(8))[0] for _ in range(n_dims)]
            total = 1
            for d in shape: total *= d
            dtype_id = struct.unpack('<I', f.read(4))[0]
            offset = struct.unpack('<Q', f.read(8))[0]
            tensor_infos.append({'name': name, 'shape': shape, 'total': total, 'dtype': dtype_id, 'offset': offset})
        
        return tensor_infos

def read_tensor_float32(path, info, max_elements=200):
    """Read tensor data, assuming we can interpret as float32 for testing."""
    # For quantized types, just read raw bytes and reinterpret
    dtype_id = info['dtype']
    size = {0:1, 1:1, 2:2, 3:2, 4:4, 5:4, 6:8, 7:8, 8:4, 9:8, 10:1}.get(dtype_id, 2)
    
    with open(path, 'rb') as f:
        f.seek(info['offset'])
        raw = f.read(min(info['total'] * size, max_elements * size))
        # Interpret as int16 and convert to float
        vals = np.frombuffer(raw, dtype=np.int16)
        return vals.astype(np.float32)[:max_elements]

def count_n15(values, mc):
    n = int(np.sqrt(len(values)))
    if n * n != len(values): return 0
    grid = values[:n*n].reshape(n, n)
    count = 0
    for i in range(n):
        if abs(np.sum(grid[i]) - mc) < 1e-6: count += 1
    for j in range(n):
        if abs(np.sum(grid[:, j]) - mc) < 1e-6: count += 1
    if abs(np.trace(grid) - mc) < 1e-6: count += 1
    if abs(np.trace(np.fliplr(grid)) - mc) < 1e-6: count += 1
    return count

def test_grid(vals, grid_size):
    n = grid_size
    needed = n * n
    if len(vals) < needed: return None
    chunk = vals[:needed]
    sorted_idx = np.argsort(chunk)
    ranks = np.empty_like(sorted_idx, dtype=np.float64)
    ranks[sorted_idx] = np.arange(1, needed + 1)
    mc = n * (needed + 1) // 2
    return count_n15(ranks, mc)

def main():
    path = "I:/model/SmolLM2-360M-Instruct.Q8_0.gguf"
    
    print(f"Reading {path}...")
    tensors = read_gguf_tensors_simple(path, max_tensors=10)
    
    print(f"\nTesting {len(tensors)} tensors:")
    results = {}
    
    for t in tensors:
        vals = read_tensor_float32(path, t, max_elements=200)
        name = t['name']
        
        for grid_size in [3, 5, 9]:
            n15 = test_grid(vals, grid_size)
            if n15 is not None:
                key = f"{grid_size}x{grid_size}"
                if key not in results: results[key] = []
                results[key].append((name, n15, 2*grid_size))
    
    print(f"\n{'='*60}")
    for size_key in sorted(results.keys()):
        items = results[size_key]
        n15_vals = [n for _, n, _ in items]
        print(f"\n{size_key} grid: {len(items)} tensors")
        print(f"  n15: {n15_vals}")
        if n15_vals:
            print(f"  Min: {min(n15_vals)}, Max: {max(n15_vals)}, Mean: {sum(n15_vals)/len(n15_vals):.1f}")
            nonzero = sum(1 for n in n15_vals if n > 0)
            print(f"  n15>0: {nonzero}/{len(items)} ({nonzero/len(items)*100:.0f}%)")
    
    if tensors:
        t = tensors[0]
        vals = read_tensor_float32(path, t, max_elements=81)
        sorted_idx = np.argsort(vals)
        ranks = np.empty_like(sorted_idx, dtype=np.float64)
        ranks[sorted_idx] = np.arange(1, 82)
        grid = ranks[:81].reshape(9, 9)
        print(f"\nExample 9x9 from '{t['name']}':")
        for row in grid:
            print(f"  {list(row.astype(int))}")
        mc = 9 * 82 // 2
        print(f"  n15 = {count_n15(ranks[:81], mc)}/{18}")

if __name__ == "__main__":
    main()