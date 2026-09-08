#!/usr/bin/env python3
"""
New tree approach:
- Start with center=5
- Add one value at a time (from 1,2,3,4,6,7,8,9)
- At each step, check if partial grid has n15 > 0
- STOP when n15 = 0

The path = sequence of values added before stopping.
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

def count_n15_from_values(values_list, n):
    """
    Given a list of (position, value) pairs in a n×n grid,
    count how many complete lines sum to magic constant.
    """
    mc = n * (n*n + 1) // 2
    grid = np.zeros((n, n))
    filled = np.zeros((n, n), dtype=bool)
    for pos, val in values_list:
        r, c = divmod(pos, n)
        grid[r, c] = val
        filled[r, c] = True
    
    count = 0
    # Rows
    for i in range(n):
        if filled[i].all() and abs(np.sum(grid[i]) - mc) < 1e-6:
            count += 1
    # Columns
    for j in range(n):
        if filled[:, j].all() and abs(np.sum(grid[:, j]) - mc) < 1e-6:
            count += 1
    # Diagonals
    diag1 = [grid[i, i] for i in range(n)]
    if all(filled[i, i] for i in range(n)) and abs(sum(diag1) - mc) < 1e-6:
        count += 1
    diag2 = [grid[i, n-1-i] for i in range(n)]
    if all(filled[i, n-1-i] for i in range(n)) and abs(sum(diag2) - mc) < 1e-6:
        count += 1
    return count

def compute_path_from_grid(grid, n):
    """
    Compute the value-addition path for a rank-ordered grid.
    Order: add values from largest to smallest weight (rank n*n down to 1),
    but skip 5 (center).
    Path stops when n15 = 0.
    """
    mc = n * (n*n + 1) // 2
    
    # Get all positions with their rank values
    flat = grid.flatten()
    # Sort by rank descending (highest rank = largest weight added first)
    order = np.argsort(-flat)  # descending
    
    path = []
    filled = []
    for pos in order:
        val = int(flat[pos])
        if val == 5:  # skip center (always present)
            continue
        filled.append((pos, val))
        path.append(val)
        
        # Check n15 with current filled positions
        n15 = count_n15_from_values(filled, n)
        if n15 == 0:
            break  # STOP — no lines sum to magic constant
    
    return path, n15

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
        path, final_n15 = compute_path_from_grid(grid, 3)
        key = path_key(path)
        if key not in paths_3:
            paths_3[key] = []
        paths_3[key].append((t['name'], path, final_n15))
    
    print(f"Unique paths: {len(paths_3)}")
    collisions_3 = {k: v for k, v in paths_3.items() if len(v) > 1}
    print(f"Collisions: {len(collisions_3)}")
    
    for t in all_tensors[:5]:
        w = t['weights']
        grid = rank_order(w, 3)
        path, final_n15 = compute_path_from_grid(grid, 3)
        print(f"  {t['name']}: path={path}, final_n15={final_n15}")
    
    # === 9×9 ===
    print("\n=== 9×9 grid ===")
    paths_9 = {}
    for t in all_tensors:
        w = t['weights']
        if len(w) < 81:
            continue
        grid = rank_order(w, 9)
        path, final_n15 = compute_path_from_grid(grid, 9)
        key = path_key(path)
        if key not in paths_9:
            paths_9[key] = []
        paths_9[key].append((t['name'], path, final_n15))
    
    print(f"Unique paths: {len(paths_9)}")
    collisions_9 = {k: v for k, v in paths_9.items() if len(v) > 1}
    print(f"Collisions: {len(collisions_9)}")
    
    for t in all_tensors[:5]:
        w = t['weights']
        if len(w) < 81:
            continue
        grid = rank_order(w, 9)
        path, final_n15 = compute_path_from_grid(grid, 9)
        print(f"  {t['name']}: path_len={len(path)}, final_n15={final_n15}")

if __name__ == "__main__":
    main()