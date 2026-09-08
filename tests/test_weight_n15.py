#!/usr/bin/env python3
"""
Extract weight samples from Kokoro GGUF and test n15 on small grids.
"""
import sys
import struct
import numpy as np

def read_gguf_header(path):
    """Read GGUF file header to find tensor data offset."""
    with open(path, 'rb') as f:
        magic = f.read(4)
        assert magic == b'GGUF', f"Not GGUF: {magic}"
        version = struct.unpack('<I', f.read(4))[0]
        n_tensors = struct.unpack('<Q', f.read(8))[0]
        n_kv = struct.unpack('<Q', f.read(8))[0]
        return version, n_tensors, n_kv

def read_tensors(path, max_tensors=20):
    """Read tensor names and shapes from GGUF."""
    from gguf import GGUFReader
    reader = GGUFReader(path, 'r')
    tensors = []
    for i, tensor in enumerate(reader.tensors):
        tensors.append({
            'name': tensor.name,
            'shape': tensor.data.shape,
            'dtype': tensor.data.dtype,
            'data': tensor.data.flatten()[:1000].astype(np.float32)
        })
        if i >= max_tensors - 1:
            break
    return tensors

def count_n15_flat(values, mc):
    """Count how many contiguous groups of len(values) sum to mc.
    For a 1D array, count groups of N consecutive values summing to mc."""
    n = int(np.sqrt(len(values)))
    if n * n != len(values):
        return 0
    grid = values[:n*n].reshape(n, n)
    count = 0
    for i in range(n):
        if np.sum(grid[i]) == mc: count += 1
    for j in range(n):
        if np.sum(grid[:, j]) == mc: count += 1
    if np.trace(grid) == mc: count += 1
    if np.trace(np.fliplr(grid)) == mc: count += 1
    return count

def test_weight_grid(name, values, grid_size):
    """Test n15 on a weight grid of given size."""
    n = grid_size
    needed = n * n
    if len(values) < needed:
        return None
    chunk = values[:needed]
    # Scale to integer for n15 testing
    # Use rank-order: sort values, assign 1..n*n
    sorted_idx = np.argsort(chunk)
    ranks = np.empty_like(sorted_idx)
    ranks[sorted_idx] = np.arange(1, needed + 1)
    mc = n * (needed + 1) // 2
    n15 = count_n15_flat(ranks.astype(np.float64), mc)
    return n15

def main():
    path = "I:/model/Kokoro_no_espeak_Q8.gguf"
    
    version, n_tensors, n_kv = read_gguf_header(path)
    print(f"GGUF: version={version}, tensors={n_tensors}, kv={n_kv}")
    
    print(f"\nReading tensor data...")
    tensors = read_tensors(path, max_tensors=20)
    
    print(f"\nFound {len(tensors)} tensors:")
    for t in tensors:
        print(f"  {t['name']}: shape={t['shape']} dtype={t['dtype']}")
    
    # Test n15 on weight samples
    print(f"\n{'='*60}")
    print(f"TEST: Weight Grid n15")
    print(f"{'='*60}")
    
    results = {}
    for t in tensors:
        vals = t['data']
        name = t['name']
        
        # Test 3x3, 5x5, 9x9
        for grid_size in [3, 5, 9]:
            n15 = test_weight_grid(name, vals, grid_size)
            if n15 is not None:
                max_n15 = 2 * grid_size  # rows + cols + 2 diags
                key = f"{grid_size}x{grid_size}"
                if key not in results:
                    results[key] = []
                results[key].append((name, n15, max_n15))
    
    for size_key in sorted(results.keys()):
        items = results[size_key]
        n15_vals = [n for _, n, _ in items]
        print(f"\n{size_key} grid:")
        print(f"  Tensors tested: {len(items)}")
        print(f"  n15 values: {n15_vals}")
        print(f"  Min: {min(n15_vals)}, Max: {max(n15_vals)}, Mean: {sum(n15_vals)/len(n15_vals):.1f}")
        # How many have n15 > 0?
        nonzero = sum(1 for n in n15_vals if n > 0)
        print(f"  With structure (n15>0): {nonzero}/{len(items)} ({nonzero/len(items)*100:.0f}%)")
    
    # Show one example grid
    if tensors:
        t = tensors[0]
        vals = t['data'][:81]
        sorted_idx = np.argsort(vals)
        ranks = np.empty_like(sorted_idx)
        ranks[sorted_idx] = np.arange(1, len(vals) + 1)
        grid_9 = ranks[:81].reshape(9, 9)
        print(f"\nExample 9x9 grid from '{t['name']}' (rank-ordered):")
        for row in grid_9:
            print(f"  {list(row)}")
        mc = 9 * 82 // 2
        n15 = count_n15_flat(ranks[:81].astype(np.float64), mc)
        print(f"  n15 = {n15}/{2*9}")

if __name__ == "__main__":
    main()
