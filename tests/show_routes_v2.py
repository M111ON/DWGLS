#!/usr/bin/env python3
"""Generate 3 example route fingerprints from real weight data."""
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

def rank_order(values, n):
    needed = n * n
    chunk = np.array(values[:needed])
    sorted_idx = np.argsort(chunk)
    ranks = np.empty_like(sorted_idx, dtype=np.float64)
    ranks[sorted_idx] = np.arange(1, needed + 1)
    return ranks.reshape(n, n)

def grid_to_flat(grid):
    return grid.flatten().astype(int).tolist()

def find_descent(grid, mc, steps=2):
    """Find a multi-step descent from current n15 toward 0."""
    n = grid.shape[0]
    current = grid.copy()
    path_grids = [grid.copy()]
    n15_vals = [count_n15(current, mc)]
    
    for step in range(steps):
        current_n15 = count_n15(current, mc)
        if current_n15 == 0:
            break
        
        best_n15 = current_n15
        best_grid = None
        
        # Exhaustive for 3x3: only 36 possible swaps
        positions = [(i, j) for i in range(n) for j in range(n)]
        for a in range(len(positions)):
            for b in range(a+1, len(positions)):
                i1, j1 = positions[a]
                i2, j2 = positions[b]
                g2 = current.copy()
                g2[i1, j1], g2[i2, j2] = g2[i2, j2], g2[i1, j1]
                new_n15 = count_n15(g2, mc)
                if new_n15 < best_n15:
                    best_n15 = new_n15
                    best_grid = g2.copy()
        
        if best_grid is not None:
            current = best_grid
            n15_vals.append(best_n15)
            path_grids.append(current.copy())
        else:
            break  # no improving swap found
    
    return n15_vals, path_grids

def main():
    t1 = parse(open('I:/DWGLS-native-fs/tests/weights_raw.json').read())
    t2 = parse(open('I:/DWGLS-native-fs/tests/kokoro_all.json').read())
    all_tensors = t1 + t2
    
    np.random.seed(42)
    mc = 15  # 3x3 magic constant
    
    # Find tensors with n15 >= 2
    candidates = []
    for t in all_tensors:
        w = t['weights']
        grid = rank_order(w, 3)
        n15 = count_n15(grid, mc)
        if n15 >= 2:
            candidates.append((t['name'], grid, n15))
    
    # Sort by n15 descending
    candidates.sort(key=lambda x: -x[2])
    
    print("=== 3 Route Fingerprint Examples (3x3, 2-level descent) ===")
    print("Format: n15_start > n15_step1[grid] > n15_step2[grid]\n")
    
    for idx, (name, grid, n15_start) in enumerate(candidates[:3]):
        n15_vals, path_grids = find_descent(grid, mc, steps=2)
        
        print(f"Route {idx+1}: '{name}' (starting n15={n15_vals[0]})")
        
        if len(n15_vals) == 1:
            print(f"  Already at n15=0")
        elif len(n15_vals) == 2:
            g1 = grid_to_flat(path_grids[1])
            print(f"  {n15_vals[0]} > {n15_vals[1]}{g1}")
            print(f"  (Hit n15=0 in 1 swap)")
        elif len(n15_vals) >= 3:
            g1 = grid_to_flat(path_grids[1])
            g2 = grid_to_flat(path_grids[2])
            print(f"  {n15_vals[0]} > {n15_vals[1]}{g1} > {n15_vals[2]}{g2}")
        
        # Show compressed format (only n15 sequence + endpoint)
        endpoint = grid_to_flat(path_grids[-1])
        compressed = '>'.join(str(v) for v in n15_vals) + str(endpoint)
        print(f"  Compressed: {compressed}")
        print()

if __name__ == "__main__":
    main()