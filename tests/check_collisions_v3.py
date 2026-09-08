#!/usr/bin/env python3
"""
New tree approach v3:
- Start at center (position 4, value=5)
- Visit other positions in weight order (largest first)
- Each visited value becomes a "center" branching to remaining values
- Path = sequence of values visited (excluding 5)
- Stop when all 9 values visited (n15 checked at end)
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

def count_n15(grid, mc):
    n = grid.shape[0]
    count = 0
    for i in range(n):
        if abs(np.sum(grid[i]) - mc) < 1e-6: count += 1
    for j in range(n):
        if abs(np.sum(grid[:, j]) - mc) < 1e-6: count += 1
    if abs(np.trace(grid) - mc) < 1e-6: count += 1
    if abs(np.trace(np.fliplr(grid)) - mc) < 1e-6: count += 1
    return count

def compute_path_v3(grid, n):
    """
    Path = values in weight-descending order, excluding center (5).
    Center is always at position n*n//2 (middle of grid).
    """
    mc = n * (n*n + 1) // 2
    center_pos = n * n // 2
    flat = grid.flatten()
    
    # Get all positions except center, sorted by weight descending
    positions = list(range(n*n))
    positions.remove(center_pos)
    positions.sort(key=lambda p: -flat[p])  # descending by weight
    
    path = [int(flat[p]) for p in positions]
    
    # Check n15 on complete grid
    final_n15 = count_n15(grid, mc)
    
    return path, final_n15

def path_key(path):
    return tuple(path)

def main():
    t1 = parse(open('I:/DWGLS-native-fs/tests/weights_raw.json').read())
    t2 = parse(open('I:/DWGLS-native-fs/tests/kokoro_all.json').read())
    all_tensors = t1 + t2
    
    print(f"Total tensors: {len(all_tensors)}\n")
    
    # === 3×3 ===
    print("=== 3×3 grid ===")
    paths_3 = {}
    for t in all_tensors:
        w = t['weights']
        grid = rank_order(w, 3)
        path, final_n15 = compute_path_v3(grid, 3)
        key = path_key(path)
        if key not in paths_3:
            paths_3[key] = []
        paths_3[key].append((t['name'], path, final_n15))
    
    print(f"Unique paths: {len(paths_3)}")
    collisions_3 = {k: v for k, v in paths_3.items() if len(v) > 1}
    print(f"Collisions: {len(collisions_3)}")
    
    if collisions_3:
        print("\nCollision details:")
        for key, items in list(collisions_3.items())[:3]:
            print(f"  Path {list(key)[:5]}...:")
            for name, path, n15 in items:
                print(f"    {name}: n15={n15}")
    
    print("\nExample paths:")
    for t in all_tensors[:5]:
        w = t['weights']
        grid = rank_order(w, 3)
        path, final_n15 = compute_path_v3(grid, 3)
        print(f"  {t['name']}: {path[:5]}..., n15={final_n15}")
    
    # === 9×9 ===
    print("\n=== 9×9 grid ===")
    paths_9 = {}
    for t in all_tensors:
        w = t['weights']
        if len(w) < 81:
            continue
        grid = rank_order(w, 9)
        path, final_n15 = compute_path_v3(grid, 9)
        key = path_key(path)
        if key not in paths_9:
            paths_9[key] = []
        paths_9[key].append((t['name'], path, final_n15))
    
    print(f"Unique paths: {len(paths_9)}")
    collisions_9 = {k: v for k, v in paths_9.items() if len(v) > 1}
    print(f"Collisions: {len(collisions_9)}")

if __name__ == "__main__":
    main()