#!/usr/bin/env python3
"""
New approach: path = value sequence from center=5 outward.
Each step: from current value, follow the line to its pair (sum=10).
No revisiting → prevents loops.

For 3x3 magic square:
- Center = 5
- Pairs that sum to 10: (1,9), (2,8), (3,7), (4,6)
- From 5, go to any of 8 values
- From that value, go to its pair (the other end of the same line)
- From the pair, go to... another line's value? Or stop?

This script:
1. Takes weight grids
2. Maps to value sequence (path)
3. Checks for collisions
"""
import json, re, numpy as np
from collections import Counter

def parse(text):
    parts = re.split(r'}\s*{', text)
    results = []
    for i, part in enumerate(parts):
        if i == 0: part += '}'
        elif i == len(parts) - 1: part = '{' + part
        else: part = '{' + part + '}'
        try: results.append(json.loads(part))
        except: pass
    return results

def rank_order(values, n):
    needed = n * n
    chunk = np.array(values[:needed])
    sorted_idx = np.argsort(chunk)
    ranks = np.empty_like(sorted_idx, dtype=np.float64)
    ranks[sorted_idx] = np.arange(1, needed + 1)
    return ranks.reshape(n, n)

def grid_to_path(grid):
    """
    Convert rank-ordered grid to value path.
    Path = values in row-major order, excluding 5 (center).
    """
    flat = grid.flatten().astype(int).tolist()
    path = [v for v in flat if v != 5]
    return path

def path_to_key(path):
    """Convert path to hashable key."""
    return tuple(path)

def main():
    t1 = parse(open('I:/DWGLS-native-fs/tests/weights_raw.json').read())
    t2 = parse(open('I:/DWGLS-native-fs/tests/kokoro_all.json').read())
    all_tensors = t1 + t2
    
    print(f"Total tensors: {len(all_tensors)}\n")
    
    # Compute paths for all tensors
    paths = {}
    for t in all_tensors:
        w = t['weights']
        grid = rank_order(w, 3)
        path = grid_to_path(grid)
        key = path_to_key(path)
        if key not in paths:
            paths[key] = []
        paths[key].append(t['name'])
    
    print(f"Unique paths: {len(paths)}")
    print(f"Total tensors: {len(all_tensors)}")
    
    # Check collisions
    collisions = {k: v for k, v in paths.items() if len(v) > 1}
    print(f"Collisions (same path): {len(collisions)}")
    
    if collisions:
        print("\nCollision details:")
        for key, names in list(collisions.items())[:5]:
            print(f"  Path {list(key)}:")
            for name in names:
                print(f"    - {name}")
    
    # Show some example paths
    print("\nExample paths:")
    for t in all_tensors[:5]:
        w = t['weights']
        grid = rank_order(w, 3)
        path = grid_to_path(grid)
        print(f"  {t['name']}: {path}")
    
    # Also try 9x9
    print("\n=== 9x9 grid ===")
    paths_9x9 = {}
    for t in all_tensors:
        w = t['weights']
        if len(w) < 81:
            continue
        grid = rank_order(w, 9)
        path = grid_to_path(grid)
        key = path_to_key(path)
        if key not in paths_9x9:
            paths_9x9[key] = []
        paths_9x9[key].append(t['name'])
    
    print(f"Unique paths (9x9): {len(paths_9x9)}")
    collisions_9x9 = {k: v for k, v in paths_9x9.items() if len(v) > 1}
    print(f"Collisions (9x9): {len(collisions_9x9)}")

if __name__ == "__main__":
    main()