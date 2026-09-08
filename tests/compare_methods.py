#!/usr/bin/env python3
"""Compare two fingerprint methods to find the source of 'collisions'."""
import json, re
from collections import Counter

def parse_concat(path):
    with open(path) as f:
        raw = f.read()
    return [json.loads(m) for m in re.findall(r'\{[^{}]*"weights"\s*:\s*\[[^\]]*\][^{}]*\}', raw)]

LINES_9x9 = [list(range(i*9,(i+1)*9)) for i in range(9)] + \
             [list(range(i,81,9)) for i in range(9)] + \
             [list(range(0,81,10)), list(range(8,81,8))]

# Method A: rank-ordering (assign 1-81 based on sorted position)
def fp_rank(vals):
    g = sorted(range(len(vals)), key=lambda i: -vals[i])
    grid = [0]*81
    for pos,rank in enumerate(g): grid[rank] = pos+1
    return tuple(sum(grid[p] for p in line) for line in LINES_9x9)

# Method B: modulo-based (what the earlier test likely used)
def fp_mod(vals):
    grid = [0]*81
    for i,v in enumerate(vals[:81]):
        grid[i] = int(v*1000) % 81 + 1
    return tuple(sum(grid[p] for p in line) for line in LINES_9x9)

smollm = parse_concat('I:/DWGLS-native-fs/tests/smollm_all.json')
kokoro = parse_concat('I:/DWGLS-native-fs/tests/kokoro_all.json')
qwen3  = parse_concat('I:/DWGLS-native-fs/tests/qwen3_all.json')
all_data = smollm + kokoro + qwen3

print(f"Total tensors: {len(all_data)}\n")

for method_name, fp_func in [("Rank-order (correct)", fp_rank), ("Modulo (buggy?)", fp_mod)]:
    fps = []
    for obj in all_data:
        name = obj['name']
        vals = obj['weights']
        if len(vals) >= 81:
            fps.append((fp_func(vals[:81]), name))
    
    groups = {}
    for f, n in fps:
        groups.setdefault(f, []).append(n)
    
    n_collisions = sum(1 for v in groups.values() if len(v) > 1)
    n_in_collisions = sum(len(v) for v in groups.values() if len(v) > 1)
    
    print(f"=== {method_name} ===")
    print(f"  Unique: {len(groups)}, Collisions: {n_collisions} groups, {n_in_collisions} tensors")
    if n_collisions > 0:
        for f, names in sorted(groups.items(), key=lambda x: -len(x[1])):
            if len(names) >= 2:
                print(f"    {len(names)}x: {names}")
    print()
