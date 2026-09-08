#!/usr/bin/env python3
"""
Line-sum fingerprint: 8 values = sum of each line.
Positions: [1,2,3],[4,5,6],[7,8,9],[1,4,7],[2,5,8],[3,6,9],[1,5,9],[3,5,7]
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

def line_sums(grid):
    """Compute 8 line sums: 3 rows + 3 cols + 2 diags."""
    n = grid.shape[0]
    sums = []
    for i in range(n):
        sums.append(int(np.sum(grid[i])))  # rows
    for j in range(n):
        sums.append(int(np.sum(grid[:, j])))  # cols
    sums.append(int(np.trace(grid)))  # diag1
    sums.append(int(np.trace(np.fliplr(grid))))  # diag2
    return tuple(sums)

def count_n15_from_sums(sums):
    """Count how many lines sum to magic constant."""
    mc = 15  # for 3x3
    return sum(1 for s in sums if s == mc)

def main():
    t1 = parse(open('I:/DWGLS-native-fs/tests/smollm_all.json').read())
    t2 = parse(open('I:/DWGLS-native-fs/tests/kokoro_all.json').read())
    t3 = parse(open('I:/DWGLS-native-fs/tests/qwen3_all.json').read())
    all_tensors = t1 + t2 + t3

    print(f"Total tensors: {len(all_tensors)}\n")

    # Line-sum fingerprint
    fps = {}
    for t in all_tensors:
        w = t['weights']
        grid = rank_order(w, 3)
        ls = line_sums(grid)
        if ls not in fps:
            fps[ls] = []
        fps[ls].append(t['name'])

    collisions = {k: v for k, v in fps.items() if len(v) > 1}
    print(f"Unique line-sum fingerprints: {len(fps)}")
    print(f"Collisions: {len(collisions)}")

    if collisions:
        print("\nCollision details:")
        for sums, names in collisions.items():
            n15 = count_n15_from_sums(sums)
            print(f"  sums={sums}, n15={n15} → {names}")
    else:
        print("\nNO COLLISIONS! Every tensor has unique line-sum fingerprint.")

    # Show examples
    print("\nExamples:")
    for t in all_tensors[:5]:
        w = t['weights']
        grid = rank_order(w, 3)
        ls = line_sums(grid)
        n15 = count_n15_from_sums(ls)
        print(f"  {t['name']}: sums={ls}, n15={n15}")

if __name__ == "__main__":
    main()