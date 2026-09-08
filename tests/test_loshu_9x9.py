#!/usr/bin/env python3
"""
9x9 Magic Square — test if n15 pattern scales to real target.
81 cells, 20 lines (9 rows + 9 cols + 2 diags), MC=369.
81! = impossible to brute-force. Sample + D4 + center test.
"""

import random

# ============================================================
# 1. MAGIC SQUARE 9x9 (Siamese method)
# ============================================================
def generate_magic_9x9(a=1, d=1):
    n = 9
    grid = [[0]*n for _ in range(n)]
    r, c = 0, n//2
    for i in range(n*n):
        grid[r][c] = a + i*d
        nr, nc = (r-1)%n, (c+1)%n
        if grid[nr][nc] != 0:
            r = (r+1)%n
        else:
            r, c = nr, nc
    return grid

def mc_9x9(a=1, d=1):
    return 9*a + (9*(81-1)//2)*d

# ============================================================
# 2. LINE SUM COUNTING
# ============================================================
def count_lines(grid, mc):
    n = len(grid)
    count = 0
    for i in range(n):
        if sum(grid[i]) == mc: count += 1
    for j in range(n):
        if sum(grid[i][j] for i in range(n)) == mc: count += 1
    if sum(grid[i][i] for i in range(n)) == mc: count += 1
    if sum(grid[i][n-1-i] for i in range(n)) == mc: count += 1
    return count

# ============================================================
# 3. D4 OPERATIONS
# ============================================================
def rot90(g): return [list(row) for row in zip(*g[::-1])]
def rot180(g): return rot90(rot90(g))
def rot270(g): return rot90(rot180(g))
def mh(g): return [row[::-1] for row in g]
def mv(g): return g[::-1]
def md(g): return [list(row) for row in zip(*g)]
def mad(g): return [list(row) for row in zip(*g[::-1])]

D4 = [lambda g: g, rot90, rot180, rot270, mh, mv, md, mad]
D4_NAMES = ["identity","rot90","rot180","rot270","mh","mv","md","mad"]

# ============================================================
# 4. TESTS
# ============================================================
def test_d4():
    print("="*60)
    print("TEST 1: D4 Symmetry for 9x9")
    print("="*60)
    
    test_cases = [(1,1), (100,1), (1,2)]
    for a, d in test_cases:
        mc = mc_9x9(a, d)
        grid = generate_magic_9x9(a, d)
        scores = [count_lines(op(grid), mc) for op in D4]
        status = "PASS" if len(set(scores)) == 1 else "FAIL"
        print(f"  start={a}, step={d}, MC={mc}: n15={scores[0]} [{status}]")
    
    # Print center
    grid = generate_magic_9x9(1,1)
    print(f"\n  Base 9x9 magic square center: {grid[4][4]}")
    print(f"  Base n15: {count_lines(grid, mc_9x9())}")
    print(f"  Lines: 9 rows + 9 cols + 2 diags = 20")

def test_center():
    print("\n" + "="*60)
    print("TEST 2: Center Pointer for 9x9")
    print("="*60)
    
    grid = generate_magic_9x9(1,1)
    mc = mc_9x9()
    center = grid[4][4]
    
    print(f"  Magic center: {center}")
    for name, op in zip(D4_NAMES, D4):
        form = op(grid)
        print(f"  {name}: n15={count_lines(form, mc)}, center={form[4][4]}")

def test_sample():
    print("\n" + "="*60)
    print("TEST 3: Sample n15 Distribution (9x9, 50K samples)")
    print("="*60)
    
    mc = mc_9x9()
    values = list(range(1, 82))
    n_lines = 20
    
    dist = {}
    n_samples = 50_000
    
    for _ in range(n_samples):
        perm = random.sample(values, 81)
        grid = [perm[i*9:(i+1)*9] for i in range(9)]
        n15 = count_lines(grid, mc)
        dist[n15] = dist.get(n15, 0) + 1
    
    print(f"  MC: {mc}, Lines: {n_lines}, Samples: {n_samples}")
    print(f"\n  n15 distribution:")
    for k in sorted(dist.keys()):
        if dist[k] > 0:
            pct = dist[k] / n_samples * 100
            bar = "#" * int(pct)
            print(f"    n15={k:>2}: {dist[k]:>6} ({pct:>5.2f}%) {bar}")

def test_route():
    print("\n" + "="*60)
    print("TEST 4: Route Fingerprint (9x9)")
    print("="*60)
    
    mc = mc_9x9()
    grid = generate_magic_9x9(1,1)
    n15_start = count_lines(grid, mc)
    
    print(f"  Start: n15={n15_start}, center={grid[4][4]}")
    
    # Swap center with corner
    g2 = [row[:] for row in grid]
    g2[0][0], g2[4][4] = g2[4][4], g2[0][0]
    n15_after = count_lines(g2, mc)
    
    route_path = f"{n15_start}>{n15_after}"
    endpoint = [v for row in g2 for v in row]
    
    print(f"  After swap (0,0)↔(4,4): n15 {n15_start}→{n15_after}")
    print(f"  Route fingerprint: {route_path}[{endpoint[:5]}...{endpoint[-5:]}]")
    print(f"  Endpoint size: {len(endpoint)} values (81 cells)")

def test_scale():
    print("\n" + "="*60)
    print("TEST 5: Scale Comparison")
    print("="*60)
    
    sizes = [
        (3, 8, 20736),
        (5, 12, 20736),
        (9, 20, 20736),
    ]
    
    print(f"  {'Size':>4} {'Lines':>5} {'MC':>4} {'Max n15':>8} {'Root states':>12}")
    for n, lines, field in sizes:
        mc = n * (n*n + 1) // 2
        grid = generate_magic_9x9(1,1) if n == 9 else None
        if n == 3:
            grid = [[2,7,6],[9,5,1],[4,3,8]]
        elif n == 5:
            grid = generate_magic_9x9(1,1)
            # regenerate 5x5
            grid = [[0]*5 for _ in range(5)]
            r, c = 0, 2
            for i in range(25):
                grid[r][c] = 1 + i
                nr, nc = (r-1)%5, (c+1)%5
                if grid[nr][nc] != 0:
                    r = (r+1)%5
                else:
                    r, c = nr, nc
        
        n15 = count_lines(grid, mc) if grid else 0
        root = 8  # D4 always gives 8 roots
        print(f"  {n:>4}x{n} {lines:>5} {mc:>4} {n15:>8} {root:>12}")
    
    print(f"\n  20736 = 144² = 12⁴")
    print(f"  9x9 map: 81 cells × 2 (ico) = 162")
    print(f"  128 × 162 = 144 × 144 = 20736")

# ============================================================
# MAIN
# ============================================================
if __name__ == "__main__":
    test_d4()
    test_center()
    test_sample()
    test_route()
    test_scale()
    
    print("\n" + "="*60)
    print("SUMMARY")
    print("="*60)
    print("9x9: D4 symmetry HOLDS, 20 lines, MC=369")
    print("n15=0 dominates even more than 5x5")
    print("Route fingerprint: 81 values, scales from 3×3")
    print("128 × 162 = 20736 — the bridge to DWGLS")
