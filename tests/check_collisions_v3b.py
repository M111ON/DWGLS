#!/usr/bin/env python3
"""Check if (path, n15) together eliminate all collisions."""
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

def compute_fingerprint(grid, n):
    mc = n * (n*n + 1) // 2
    center_pos = n * n // 2
    flat = grid.flatten()
    
    positions = list(range(n*n))
    positions.remove(center_pos)
    positions.sort(key=lambda p: -flat[p])
    
    path = tuple(int(flat[p]) for p in positions)
    n15 = count_n15(grid, mc)
    
    return (path, n15)

def main():
    t1 = parse(open('I:/DWGLS-native-fs/tests/weights_raw.json').read())
    t2 = parse(open('I:/DWGLS-native-fs/tests/kokoro_all.json').read())
    all_tensors = t1 + t2
    
    print(f"Total tensors: {len(all_tensors)}\n")
    
    # 3×3
    fps_3 = {}
    for t in all_tensors:
        w = t['weights']
        grid = rank_order(w, 3)
        fp = compute_fingerprint(grid, 3)
        if fp not in fps_3:
            fps_3[fp] = []
        fps_3[fp].append(t['name'])
    
    collisions_3 = {k: v for k, v in fps_3.items() if len(v) > 1}
    print(f"3×3: {len(fps_3)} unique fingerprints, {len(collisions_3)} collisions")
    for (path, n15), names in collisions_3.items():
        print(f"  path={list(path)[:5]}..., n15={n15} → {names}")
    
    # 9×9
    fps_9 = {}
    for t in all_tensors:
        w = t['weights']
        if len(w) < 81: continue
        grid = rank_order(w, 9)
        fp = compute_fingerprint(grid, 9)
        if fp not in fps_9:
            fps_9[fp] = []
        fps_9[fp].append(t['name'])
    
    collisions_9 = {k: v for k, v in fps_9.items() if len(v) > 1}
    print(f"9×9: {len(fps_9)} unique fingerprints, {len(collisions_9)} collisions")
    
    # Show some fingerprints
    print("\n3×3 fingerprints:")
    for t in all_tensors[:5]:
        w = t['weights']
        grid = rank_order(w, 3)
        fp = compute_fingerprint(grid, 3)
        path, n15 = fp
        print(f"  {t['name']}: path={list(path)[:5]}..., n15={n15}")

if __name__ == "__main__":
    main()