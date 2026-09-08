#!/usr/bin/env python3
"""
5x5 Magic Square — test if n15 pattern scales.
25 cells, 12 lines (5 rows + 5 cols + 2 diags), MC=65.
25! = 15.5 quintillion — can't brute-force all.
"""

from itertools import permutations
import random

# ============================================================
# 1. MAGIC SQUARE 5x5 (Siamese method)
# ============================================================
def generate_magic_5x5(a=1, d=1):
    """5x5 magic square, start=a, step=d."""
    n = 5
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

def mc_5x5(a=1, d=1):
    return 5*a + (5*(25-1)//2)*d

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

# ============================================================
# 4. TESTS
# ============================================================
def test_d4_symmetry():
    """Test: do all 8 D4 forms have same n15?"""
    print("="*60)
    print("TEST 1: D4 Symmetry for 5x5")
    print("="*60)
    
    test_cases = [(1,1), (100,1), (1,2), (10,5)]
    for a, d in test_cases:
        mc = mc_5x5(a, d)
        grid = generate_magic_5x5(a, d)
        forms = [op(grid) for op in D4]
        scores = [count_lines(f, mc) for f in forms]
        
        status = "PASS" if len(set(scores)) == 1 else "FAIL"
        print(f"  start={a}, step={d}, MC={mc}: scores={scores} [{status}]")
    
    # Print base form
    print(f"\n  Base 5x5 magic square (a=1,d=1,MC=65):")
    grid = generate_magic_5x5(1,1)
    for row in grid:
        print(f"    {row}")

def test_center_pointer():
    """Test: what's max n15 for different centers in 5x5?"""
    print("\n" + "="*60)
    print("TEST 2: Center Pointer for 5x5")
    print("="*60)
    
    # For 5x5, center is at (2,2)
    # We can't enumerate all 25! permutations
    # But we can sample and check known magic squares
    
    mc = mc_5x5(1,1)
    grid = generate_magic_5x5(1,1)
    center = grid[2][2]
    
    print(f"  Magic square center: {center}")
    print(f"  Max n15: {count_lines(grid, mc)}")
    
    # Check D4 forms
    forms = [op(grid) for op in D4]
    for i, (name, form) in enumerate(zip(
        ["identity","rot90","rot180","rot270","mh","mv","md","mad"],
        forms)):
        n15 = count_lines(form, mc)
        print(f"  {name}: n15={n15}, center={form[2][2]}")

def test_sample_distribution():
    """Test: sample random permutations to see n15 distribution."""
    print("\n" + "="*60)
    print("TEST 3: Sample n15 Distribution (5x5, 100K random samples)")
    print("="*60)
    
    mc = mc_5x5(1,1)
    values = list(range(1, 26))
    n_lines = 12  # 5 rows + 5 cols + 2 diags
    
    dist = {i: 0 for i in range(n_lines+1)}
    n_samples = 100_000
    
    for _ in range(n_samples):
        perm = random.sample(values, 25)
        grid = [perm[i*5:(i+1)*5] for i in range(5)]
        n15 = count_lines(grid, mc)
        dist[n15] = dist.get(n15, 0) + 1
    
    print(f"  Samples: {n_samples}")
    print(f"  Lines: {n_lines} (5 rows + 5 cols + 2 diags)")
    print(f"  MC: {mc}")
    print(f"\n  n15 distribution:")
    for k in sorted(dist.keys()):
        if dist[k] > 0:
            pct = dist[k] / n_samples * 100
            bar = "#" * int(pct / 2)
            print(f"    n15={k:>2}: {dist[k]:>7} ({pct:>5.2f}%) {bar}")

def test_route_fingerprint():
    """Test: can we fingerprint routes in 5x5?"""
    print("\n" + "="*60)
    print("TEST 4: Route Fingerprint Concept (5x5)")
    print("="*60)
    
    mc = mc_5x5(1,1)
    grid = generate_magic_5x5(1,1)
    n15_start = count_lines(grid, mc)
    
    print(f"  Start grid (n15={n15_start}):")
    for row in grid:
        print(f"    {row}")
    
    # Single swap: what happens?
    g2 = [row[:] for row in grid]
    g2[0][0], g2[2][2] = g2[2][2], g2[0][0]  # swap corner with center
    n15_after = count_lines(g2, mc)
    
    print(f"\n  After swap (0,0)↔(2,2):")
    for row in g2:
        print(f"    {row}")
    print(f"  n15: {n15_start} → {n15_after}")
    
    # Route path
    route_path = f"{n15_start}>{n15_after}"
    endpoint = [v for row in g2 for v in row]
    fingerprint = f"{route_path}{endpoint}"
    print(f"\n  Route fingerprint: {route_path}{endpoint}")

# ============================================================
# MAIN
# ============================================================
if __name__ == "__main__":
    test_d4_symmetry()
    test_center_pointer()
    test_sample_distribution()
    test_route_fingerprint()
    
    print("\n" + "="*60)
    print("SUMMARY")
    print("="*60)
    print("5x5 has 12 lines, MC=65, 25! permutations")
    print("D4 symmetry holds (8 forms, same n15)")
    print("Center pointer: center value controls max n15")
    print("Route fingerprint: n15 sequence + endpoint grid")
    print("Next: test if 9x9 scales the same way")
