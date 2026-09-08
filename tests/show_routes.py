#!/usr/bin/env python3
"""Generate 3 example route fingerprints from real weight data."""
import json, re, numpy as np

def parse_gguf_output(text):
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

def grid_to_list(grid):
    return grid.flatten().astype(int).tolist()

def find_descent(grid, mc, steps=2):
    """Find a path that reduces n15 by steps."""
    n = grid.shape[0]
    current = grid.copy()
    path = [grid_to_list(current)]
    n15_values = [count_n15(current, mc)]
    
    for step in range(steps):
        current_n15 = count_n15(current, mc)
        best_n15 = current_n15
        best_grid = None
        best_swap = None
        
        # Try many random swaps
        for _ in range(100):
            i1, j1 = np.random.randint(0, n, 2)
            i2, j2 = np.random.randint(0, n, 2)
            if (i1, j1) == (i2, j2): continue
            g2 = current.copy()
            g2[i1, j1], g2[i2, j2] = g2[i2, j2], g2[i1, j1]
            new_n15 = count_n15(g2, mc)
            if new_n15 < best_n15:
                best_n15 = new_n15
                best_grid = g2.copy()
                best_swap = (i1, j1, i2, j2)
        
        if best_grid is not None:
            current = best_grid
            n15_values.append(best_n15)
            path.append(grid_to_list(current))
    
    return n15_values, path

def main():
    # Use both SmolLM and Kokoro data
    text1 = open("I:/DWGLS-native-fs/tests/weights_raw.json").read()
    text2 = open("I:/DWGLS-native-fs/tests/kokoro_all.json").read()
    tensors = parse_gguf_output(text1) + parse_gguf_output(text2)
    
    np.random.seed(42)
    
    examples = []
    for t in tensors:
        w = t['weights']
        name = t['name']
        grid_3 = rank_order(w, 3)
        n15 = count_n15(grid_3, 15)
        if n15 >= 2 and len(examples) < 5:
            examples.append((name, w, grid_3, n15))
    
    print("=== 3 Route Fingerprint Examples (3x3, 2-level descent) ===\n")
    print("Format: n15_start > n15_step1[grid] > n15_step2[grid]\n")
    
    for idx, (name, weights, grid, n15_start) in enumerate(examples[:3]):
        mc = 15
        n15_vals, path = find_descent(grid, mc, steps=2)
        
        print(f"Route {idx+1}: '{name}'")
        if len(n15_vals) >= 3:
            g1 = path[1]
            g2 = path[2]
            route_str = f"{n15_vals[0]}>{n15_vals[1]}{g1}>{n15_vals[2]}{g2}"
            print(f"  Full:  {route_str}")
            compressed = f"{n15_vals[0]}>{n15_vals[1]}>{n15_vals[2]}{g2}"
            print(f"  Compressed: {compressed}")
        elif len(n15_vals) == 2:
            g1 = path[1]
            route_str = f"{n15_vals[0]}>{n15_vals[1]}{g1}"
            print(f"  Full:  {route_str}")
            print(f"  (Hit n15=0 in 1 swap — only 2 levels)")
        print()

if __name__ == "__main__":
    main()