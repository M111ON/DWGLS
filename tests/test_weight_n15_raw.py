#!/usr/bin/env python3
"""
Extract weight samples from Kokoro GGUF directly (no gguf lib) and test n15.
"""
import sys
import struct
import numpy as np

def read_gguf_basic(path):
    """Read GGUF basic info and tensor metadata."""
    with open(path, 'rb') as f:
        # Header
        magic = f.read(4)
        if magic != b'GGUF':
            raise ValueError(f"Not GGUF: {magic}")
        version = struct.unpack('<I', f.read(4))[0]
        n_tensors = struct.unpack('<Q', f.read(8))[0]
        n_kv = struct.unpack('<Q', f.read(8))[0]
        
        print(f"GGUF: version={version}, tensors={n_tensors}, kv={n_kv}")
        
        # Skip KV pairs (we don't need them for this test)
        for _ in range(n_kv):
            # Read key length + key
            key_len = struct.unpack('<Q', f.read(8))[0]
            f.read(key_len)
            # Read value type + value
            val_type = struct.unpack('<I', f.read(4))[0]
            # Skip value based on type
            if val_type == 8:  # string
                val_len = struct.unpack('<Q', f.read(8))[0]
                # Read in chunks to avoid MemoryError
                remaining = val_len
                chunk_size = 1024 * 1024  # 1MB chunks
                while remaining > 0:
                    read_size = min(chunk_size, remaining)
                    f.read(read_size)
                    remaining -= read_size
            elif val_type in (0, 1):  # uint8, int8
                f.read(1)
            elif val_type in (2, 3):  # uint16, int16
                f.read(2)
            elif val_type in (4, 5):  # uint32, int32
                f.read(4)
            elif val_type in (6, 7):  # uint64, int64
                f.read(8)
            elif val_type == 9:  # float32
                f.read(4)
            elif val_type == 10:  # float64
                f.read(8)
            elif val_type == 11:  # bool
                f.read(1)
            elif val_type == 12:  # array
                arr_type = struct.unpack('<I', f.read(4))[0]
                arr_len = struct.unpack('<Q', f.read(8))[0]
                # Skip array elements
                if arr_type in (0, 1): f.read(arr_len)
                elif arr_type in (2, 3): f.read(arr_len * 2)
                elif arr_type in (4, 5): f.read(arr_len * 4)
                elif arr_type in (6, 7): f.read(arr_len * 8)
                elif arr_type == 9: f.read(arr_len * 4)
                elif arr_type == 10: f.read(arr_len * 8)
        
        # Now at tensor infos
        tensor_infos = []
        for i in range(n_tensors):
            # Tensor name
            name_len = struct.unpack('<Q', f.read(8))[0]
            name = f.read(name_len).decode('utf-8')
            
            # Shape: n_dims + dims
            n_dims = struct.unpack('<Q', f.read(8))[0]
            shape = []
            total = 1
            for _ in range(n_dims):
                dim = struct.unpack('<Q', f.read(8))[0]
                shape.append(dim)
                total *= dim
            
            # Type (uint32)
            dtype_id = struct.unpack('<I', f.read(4))[0]
            # Offset (uint64)
            offset = struct.unpack('<Q', f.read(8))[0]
            
            tensor_infos.append({
                'name': name,
                'shape': shape,
                'total': total,
                'dtype': dtype_id,
                'offset': offset
            })
        
        return tensor_infos, f.tell()

def read_tensor_data(path, tensor_info, max_elements=200):
    """Read raw tensor data and convert to float32."""
    # dtype mapping
    dtype_map = {
        0: ('b', 1),   # int8
        1: ('B', 1),   # uint8
        2: ('h', 2),   # int16
        3: ('H', 2),   # uint16
        4: ('i', 4),   # int32
        5: ('I', 4),   # uint32
        6: ('q', 8),   # int64
        7: ('Q', 8),   # uint64
        8: ('f', 4),   # float32
        9: ('d', 8),   # float64
        10: ('?', 1),  # bool
        # Quantized types (need special handling)
        2: ('h', 2),   # placeholder for quantized
    }
    
    dtype_id = tensor_info['dtype']
    if dtype_id > 10:
        # Quantized - just read raw bytes
        with open(path, 'rb') as f:
            f.seek(tensor_info['offset'])
            raw = f.read(min(tensor_info['total'] * 2, max_elements * 2))
            # Interpret as int16
            vals = np.frombuffer(raw, dtype=np.int16)
            return vals.astype(np.float32)[:max_elements]
    
    fmt, size = dtype_map.get(dtype_id, ('f', 4))
    with open(path, 'rb') as f:
        f.seek(tensor_info['offset'])
        raw = f.read(min(tensor_info['total'] * size, max_elements * size))
        if fmt in ('f', 'd'):
            vals = np.frombuffer(raw, dtype=np.dtype(fmt))
        else:
            vals = np.frombuffer(raw, dtype=np.dtype(fmt)).astype(np.float32)
        return vals[:max_elements]

def count_n15_flat(values, mc):
    """Count n15 for a flat array reshaped to square grid."""
    n = int(np.sqrt(len(values)))
    if n * n != len(values):
        return 0
    grid = values[:n*n].reshape(n, n)
    count = 0
    for i in range(n):
        if abs(np.sum(grid[i]) - mc) < 1e-6: count += 1
    for j in range(n):
        if abs(np.sum(grid[:, j]) - mc) < 1e-6: count += 1
    if abs(np.trace(grid) - mc) < 1e-6: count += 1
    if abs(np.trace(np.fliplr(grid)) - mc) < 1e-6: count += 1
    return count

def test_weight_grid(name, values, grid_size):
    """Test n15 on a weight grid of given size."""
    n = grid_size
    needed = n * n
    if len(values) < needed:
        return None
    chunk = values[:needed]
    # Rank order: sort, assign 1..n*n
    sorted_idx = np.argsort(chunk)
    ranks = np.empty_like(sorted_idx, dtype=np.float64)
    ranks[sorted_idx] = np.arange(1, needed + 1)
    mc = n * (needed + 1) // 2
    n15 = count_n15_flat(ranks, mc)
    return n15

def main():
    path = "I:/model/Kokoro_no_espeak_Q8.gguf"
    
    print(f"Reading {path}...")
    tensor_infos, data_start = read_gguf_basic(path)
    print(f"Data starts at offset: {data_start}")
    
    print(f"\nTesting first 30 tensors...")
    results = {}
    
    for i, t in enumerate(tensor_infos[:30]):
        vals = read_tensor_data(path, t, max_elements=200)
        name = t['name']
        
        for grid_size in [3, 5, 9]:
            n15 = test_weight_grid(name, vals, grid_size)
            if n15 is not None:
                key = f"{grid_size}x{grid_size}"
                if key not in results:
                    results[key] = []
                max_n15 = 2 * grid_size
                results[key].append((name, n15, max_n15))
        
        if i % 10 == 0:
            print(f"  Processed {i+1} tensors...")
    
    # Report
    print(f"\n{'='*60}")
    print(f"RESULTS: Weight Grid n15")
    print(f"{'='*60}")
    
    for size_key in sorted(results.keys()):
        items = results[size_key]
        n15_vals = [n for _, n, _ in items]
        print(f"\n{size_key} grid:")
        print(f"  Tensors tested: {len(items)}")
        print(f"  n15 values: {n15_vals}")
        if n15_vals:
            print(f"  Min: {min(n15_vals)}, Max: {max(n15_vals)}, Mean: {sum(n15_vals)/len(n15_vals):.1f}")
            nonzero = sum(1 for n in n15_vals if n > 0)
            print(f"  With structure (n15>0): {nonzero}/{len(items)} ({nonzero/len(items)*100:.0f}%)")
    
    # Show one example
    if tensor_infos:
        t = tensor_infos[0]
        vals = read_tensor_data(path, t, max_elements=81)
        sorted_idx = np.argsort(vals)
        ranks = np.empty_like(sorted_idx, dtype=np.float64)
        ranks[sorted_idx] = np.arange(1, len(vals) + 1)
        grid_9 = ranks[:81].reshape(9, 9)
        print(f"\nExample 9x9 from '{t['name']}' (rank-ordered):")
        for row in grid_9:
            print(f"  {list(row.astype(int))}")
        mc = 9 * 82 // 2
        n15 = count_n15_flat(ranks[:81], mc)
        print(f"  n15 = {n15}/{2*9}")

if __name__ == "__main__":
    main()