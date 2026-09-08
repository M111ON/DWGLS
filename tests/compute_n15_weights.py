#!/usr/bin/env python3
"""
Compute n15 on model weight grids using rank ordering.
"""
import sys
import json
import re
import numpy as np

def parse_gguf_output(text):
    """Parse the concatenated JSON output from gguf_tool."""
    # Split by } followed by {
    parts = re.split(r'}\s*{', text)
    results = []
    for i, part in enumerate(parts):
        if i == 0:
            part = part + '}'
        elif i == len(parts) - 1:
            part = '{' + part
        else:
            part = '{' + part + '}'
        try:
            results.append(json.loads(part))
        except json.JSONDecodeError as e:
            print(f"Parse error: {e}", file=sys.stderr)
    return results

def count_n15(values, mc):
    """Count n15 for a flat array reshaped to square grid."""
    n = int(np.sqrt(len(values)))
    if n * n != len(values):
        return 0
    grid = np.array(values[:n*n]).reshape(n, n)
    count = 0
    for i in range(n):
        if abs(np.sum(grid[i]) - mc) < 1e-6:
            count += 1
    for j in range(n):
        if abs(np.sum(grid[:, j]) - mc) < 1e-6:
            count += 1
    if abs(np.trace(grid) - mc) < 1e-6:
        count += 1
    if abs(np.trace(np.fliplr(grid)) - mc) < 1e-6:
        count += 1
    return count

def test_grid(values, grid_size):
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
    return count_n15(ranks, mc)

def main():
    # Read from stdin
    text = sys.stdin.read()
    tensors = parse_gguf_output(text)
    
    print(f"Parsed {len(tensors)} tensors")
    print(f"{'='*60}")
    
    results = {}
    for t in tensors:
        name = t['name']
        weights = t['weights']
        
        for grid_size in [3, 5, 9]:
            n15 = test_grid(weights, grid_size)
            if n15 is not None:
                key = f"{grid_size}x{grid_size}"
                if key not in results:
                    results[key] = []
                results[key].append((name, n15, 2*grid_size))
    
    for size_key in sorted(results.keys()):
        items = results[size_key]
        n15_vals = [n for _, n, _ in items]
        print(f"\n{size_key} grid: {len(items)} tensors")
        print(f"  n15 values: {n15_vals}")
        if n15_vals:
            print(f"  Min: {min(n15_vals)}, Max: {max(n15_vals)}, Mean: {sum(n15_vals)/len(n15_vals):.1f}")
            nonzero = sum(1 for n in n15_vals if n > 0)
            print(f"  n15>0: {nonzero}/{len(items)} ({nonzero/len(items)*100:.0f}%)")
    
    # Show one example 9x9 grid
    if tensors:
        t = tensors[0]
        vals = t['weights']
        sorted_idx = np.argsort(vals)
        ranks = np.empty_like(sorted_idx, dtype=np.float64)
        ranks[sorted_idx] = np.arange(1, len(vals) + 1)
        grid_9 = ranks[:81].reshape(9, 9)
        print(f"\nExample 9x9 from '{t['name']}' (rank-ordered):")
        for row in grid_9:
            print(f"  {list(row.astype(int))}")
        mc = 9 * 82 // 2
        print(f"  n15 = {count_n15(ranks[:81], mc)}/{18}")

if __name__ == "__main__":
    main()