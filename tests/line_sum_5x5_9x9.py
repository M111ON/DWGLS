#!/usr/bin/env python3
"""Test line-sum fingerprint for 5x5 and 9x9 grids."""
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

def line_sums(grid):
    n = grid.shape[0]
    sums = []
    for i in range(n):
        sums.append(int(np.sum(grid[i])))
    for j in range(n):
        sums.append(int(np.sum(grid[:, j])))
    sums.append(int(np.trace(grid)))
    sums.append(int(np.trace(np.fliplr(grid))))
    return tuple(sums)

def count_n15(sums, mc):
    return sum(1 for s in sums if s == mc)

def main():
    t1 = parse(open('I:/DWGLS-native-fs/tests/weights_raw.json').read())
    t2 = parse(open('I:/DWGLS-native-fs/tests/kokoro_all.json').read())
    all_tensors = t1 + t2

    for n, mc in [(3, 15), (5, 65), (9, 369)]:
        print(f"=== {n}x{n} grid (magic constant = {mc}) ===")
        
        fps = {}
        for t in all_tensors:
            w = t['weights']
            if len(w) < n*n:
                continue
            grid = rank_order(w, n)
            ls = line_sums(grid)
            n15 = count_n15(ls, mc)
            if ls not in fps:
                fps[ls] = []
            fps[ls].append((t['name'], n15))
        
        collisions = {k: v for k, v in fps.items() if len(v) > 1}
        print(f"  Tensors: {sum(len(v) for v in fps.values())}")
        print(f"  Unique fingerprints: {len(fps)}")
        print(f"  Collisions: {len(collisions)}")
        
        # Show some fingerprints
        print(f"  Examples:")
        for t in all_tensors[:3]:
            w = t['weights']
            if len(w) < n*n:
                continue
            grid = rank_order(w, n)
            ls = line_sums(grid)
            n15 = count_n15(ls, mc)
            print(f"    {t['name']}: {len(ls)} sums, n15={n15}")
        print()

if __name__ == "__main__":
    main()