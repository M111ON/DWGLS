#!/usr/bin/env python3
"""
LUT-based n15 checker using parametric magic square form.

Parametric form (center c=5):
  c-b     c+a+b   c-a
  c-a+b   c       c+a-b
  c+a     c-a-b   c+b

For c=5: all lines sum to 3c = 15.

LUT: 64 entries (8×8 for a,b ∈ {1,2,3,4,6,7,8,9})
Each entry checks if grid matches parametric form for given (a,b).
"""
import numpy as np

def build_lut():
    """Build 64-entry LUT for (a,b) pairs."""
    vals = [1,2,3,4,6,7,8,9]  # possible a,b values (not 5)
    c = 5
    lut = {}
    for a in vals:
        for b in vals:
            # Expected grid for this (a,b)
            expected = np.array([
                [c-b, c+a+b, c-a],
                [c-a+b, c, c+a-b],
                [c+a, c-a-b, c+b]
            ])
            lut[(a, b)] = expected
    return lut

def check_n15_lut(grid, lut):
    """
    Check if grid matches any parametric form.
    Returns n15 (number of lines summing to 15).
    """
    c = 5
    n15 = 0
    
    # Check all 8 lines directly
    for i in range(3):
        if abs(np.sum(grid[i]) - 15) < 1e-6: n15 += 1  # rows
        if abs(np.sum(grid[:, i]) - 15) < 1e-6: n15 += 1  # cols
    if abs(np.trace(grid) - 15) < 1e-6: n15 += 1  # diag1
    if abs(np.trace(np.fliplr(grid)) - 15) < 1e-6: n15 += 1  # diag2
    
    return n15

def check_parametric(grid, lut):
    """
    Check if grid matches any parametric (a,b) form.
    Returns (a, b) if match, None otherwise.
    """
    for (a, b), expected in lut.items():
        if np.array_equal(grid, expected):
            return (a, b)
    return None

def rank_order(values, n):
    needed = n * n
    chunk = np.array(values[:needed])
    sorted_idx = np.argsort(chunk)
    ranks = np.empty_like(sorted_idx, dtype=np.float64)
    ranks[sorted_idx] = np.arange(1, needed + 1)
    return ranks.reshape(n, n)

def main():
    # Build LUT
    lut = build_lut()
    print(f"LUT size: {len(lut)} entries (8×8)")
    
    # Test with known magic square
    c = 5
    a, b = 1, 2
    magic = np.array([
        [c-b, c+a+b, c-a],
        [c-a+b, c, c+a-b],
        [c+a, c-a-b, c+b]
    ])
    print(f"\nTest magic square (a={a}, b={b}):")
    print(magic)
    print(f"n15 = {check_n15_lut(magic, lut)}")
    match = check_parametric(magic, lut)
    print(f"Parametric match: {match}")
    
    # Test with Lo Shu
    loshu = np.array([
        [2, 7, 6],
        [9, 5, 1],
        [4, 3, 8]
    ])
    print(f"\nLo Shu:")
    print(loshu)
    print(f"n15 = {check_n15_lut(loshu, lut)}")
    match = check_parametric(loshu, lut)
    print(f"Parametric match: {match}")
    
    # Test with random grid
    random_grid = np.array([
        [1, 2, 3],
        [4, 5, 6],
        [7, 8, 9]
    ])
    print(f"\nRandom grid:")
    print(random_grid)
    print(f"n15 = {check_n15_lut(random_grid, lut)}")
    match = check_parametric(random_grid, lut)
    print(f"Parametric match: {match}")
    
    # Test with weight data
    import json, re
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
    
    t1 = parse(open('I:/DWGLS-native-fs/tests/weights_raw.json').read())
    t2 = parse(open('I:/DWGLS-native-fs/tests/kokoro_all.json').read())
    all_tensors = t1 + t2
    
    print(f"\n=== Weight data ({len(all_tensors)} tensors) ===")
    for t in all_tensors[:5]:
        w = t['weights']
        grid = rank_order(w, 3)
        n15 = check_n15_lut(grid, lut)
        match = check_parametric(grid, lut)
        print(f"  {t['name']}: n15={n15}, parametric={match}")

if __name__ == "__main__":
    main()