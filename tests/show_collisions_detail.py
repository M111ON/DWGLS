#!/usr/bin/env python3
"""Show exactly where collisions happen."""
import json, re, numpy as np

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

def compute_fp(grid, n):
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

    fps = {}
    for t in all_tensors:
        w = t['weights']
        grid = rank_order(w, 3)
        fp = compute_fp(grid, 3)
        if fp not in fps:
            fps[fp] = []
        fps[fp].append((t['name'], grid))

    collisions = {k: v for k, v in fps.items() if len(v) > 1}

    print(f"=== {len(collisions)} collision groups ===\n")

    for (path, n15), items in collisions.items():
        print(f"Path: {list(path)[:5]}..., n15={n15}")
        for name, grid in items:
            print(f"  {name}:")
            print(f"    {grid[0]}")
            print(f"    {grid[1]}")
            print(f"    {grid[2]}")
        # Show which lines are same/different
        if len(items) == 2:
            g1, g2 = items[0][1], items[1][1]
            diffs = []
            for i in range(3):
                for j in range(3):
                    if g1[i,j] != g2[i,j]:
                        diffs.append(f"  pos({i},{j}): {int(g1[i,j])} vs {int(g2[i,j])}")
            print(f"  Position differences:")
            for d in diffs:
                print(f"    {d}")
        print()

if __name__ == "__main__":
    main()