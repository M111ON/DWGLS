#!/usr/bin/env python3
"""Wang tile edges derived from line-sum fingerprints."""
import json, re
from collections import Counter

def parse_concat(path):
    with open(path) as f:
        raw = f.read()
    return [json.loads(m) for m in re.findall(r'\{[^{}]*"weights"\s*:\s*\[[^\]]*\][^{}]*\}', raw)]

LINES_9x9 = [list(range(i*9,(i+1)*9)) for i in range(9)] + \
             [list(range(i,81,9)) for i in range(9)] + \
             [list(range(0,81,10)), list(range(8,81,8))]

def fp_rank(vals):
    g = sorted(range(len(vals)), key=lambda i: -vals[i])
    grid = [0]*81
    for pos,rank in enumerate(g): grid[rank] = pos+1
    return tuple(sum(grid[p] for p in line) for line in LINES_9x9)

def derive_edges(fingerprint):
    """Map 20 line-sums → 4 Wang edges.
    
    Layout of 9x9 fingerprint:
      [R0,R1,R2,R3,R4,R5,R6,R7,R8, C0,C1,C2,C3,C4,C5,C6,C7,C8, D0,D1]
    
    Edge assignment:
      top    = R0 (first row)
      bottom = R8 (last row)
      left   = C0 (first column)
      right  = C8 (last column)
    """
    R = fingerprint[:9]
    C = fingerprint[9:18]
    return (R[0], C[8], R[8], C[0])  # top, right, bottom, left

# Load all models
smollm = parse_concat('I:/DWGLS-native-fs/tests/smollm_all.json')
qwen3 = parse_concat('I:/DWGLS-native-fs/tests/qwen3_all.json')
kokoro = parse_concat('I:/DWGLS-native-fs/tests/kokoro_all.json')
all_data = smollm + qwen3 + kokoro

print(f"Tensors: {len(all_data)}")

# Compute fingerprints + edges
tensors = []
for obj in all_data:
    name = obj['name']
    vals = obj['weights']
    if len(vals) >= 81:
        fp = fp_rank(vals[:81])
        edges = derive_edges(fp)
        tensors.append((name, fp, edges))

# === Test 1: Edge distribution ===
edge_sets = [e for _, _, e in tensors]
top_vals = Counter(e[0] for e in edge_sets)
right_vals = Counter(e[1] for e in edge_sets)
bottom_vals = Counter(e[2] for e in edge_sets)
left_vals = Counter(e[3] for e in edge_sets)

print(f"\n=== Edge value distribution ===")
print(f"Top:    {len(top_vals)} unique values, range [{min(top_vals)}, {max(top_vals)}]")
print(f"Right:  {len(right_vals)} unique values, range [{min(right_vals)}, {max(right_vals)}]")
print(f"Bottom: {len(bottom_vals)} unique values, range [{min(bottom_vals)}, {max(bottom_vals)}]")
print(f"Left:   {len(left_vals)} unique values, range [{min(left_vals)}, {max(left_vals)}]")

# === Test 2: Edge matching (Wang constraint) ===
# For each pair of tensors, check if they can be placed adjacent
# top-bottom match: t1.bottom == t2.top
# left-right match: t1.right == t2.left
print(f"\n=== Edge matching (Wang constraint) ===")

# Build edge index for fast lookup
top_index = {}  # top_value → [tensor indices]
bottom_index = {}
left_index = {}
right_index = {}
for i, (name, fp, edges) in enumerate(tensors):
    top_val, right_val, bottom_val, left_val = edges
    top_index.setdefault(top_val, []).append(i)
    bottom_index.setdefault(bottom_val, []).append(i)
    left_index.setdefault(left_val, []).append(i)
    right_index.setdefault(right_val, []).append(i)

# Count matching pairs
tb_matches = 0  # top-bottom (vertical adjacency)
lr_matches = 0  # left-right (horizontal adjacency)
for i, (n1, fp1, (t1, r1, b1, l1)) in enumerate(tensors):
    # How many tensors match bottom of i as their top?
    if b1 in top_index:
        tb_matches += len(top_index[b1]) - (1 if b1 == t1 else 0)
    # How many tensors match right of i as their left?
    if r1 in left_index:
        lr_matches += len(left_index[r1]) - (1 if r1 == l1 else 0)

n = len(tensors)
print(f"Total possible pairs: {n*(n-1)//2}")
print(f"TB-matching pairs (vertical): {tb_matches//2} (after dedup)")
print(f"LR-matching pairs (horizontal): {lr_matches//2} (after dedup)")

# === Test 3: Can we build a valid 2D grid? ===
# Place tensors on a 2D grid where Wang edges must match
# Try to find a chain of tensors where bottom matches next top
print(f"\n=== Wang chain test (bottom→top matching) ===")
# Build adjacency: bottom_val → list of tensor names
adj = {}
for i, (name, fp, (t, r, b, l)) in enumerate(tensors):
    adj.setdefault(b, []).append((i, name))

# DFS: find longest chain where each tensor's bottom matches next tensor's top
visited = set()
best_chain = []

def dfs(current_idx, chain):
    global best_chain
    if len(chain) > len(best_chain):
        best_chain = chain[:]
    _, _, (_, _, bottom, _) = tensors[current_idx]
    if bottom not in adj:
        return
    for next_idx, next_name in adj[bottom]:
        if next_idx not in visited:
            visited.add(next_idx)
            chain.append(next_name)
            dfs(next_idx, chain)
            chain.pop()
            visited.remove(next_idx)

# Try from each tensor as starting point
for start_idx in range(min(50, n)):  # limit for speed
    visited.clear()
    visited.add(start_idx)
    dfs(start_idx, [tensors[start_idx][0]])

print(f"Longest Wang chain: {len(best_chain)} tensors")
if len(best_chain) <= 20:
    for name in best_chain:
        print(f"  {name}")

# === Test 4: Gate open/close simulation ===
# If we place tensors in memory order, how many paths are "open"?
print(f"\n=== Gate simulation (memory order) ===")
open_count = 0
closed_count = 0
for i in range(n - 1):
    _, _, (_, _, b1, _) = tensors[i]
    _, _, (t2, _, _, _) = tensors[i+1]
    if b1 == t2:
        open_count += 1
    else:
        closed_count += 1
print(f"Open (match): {open_count}/{n-1} ({open_count/(n-1)*100:.1f}%)")
print(f"Closed (no match): {closed_count}/{n-1} ({closed_count/(n-1)*100:.1f}%)")

# With optimal ordering (sort by edges)
sorted_tensors = sorted(tensors, key=lambda x: (x[2][3], x[2][0]))  # sort by left, then top
open_opt = 0
closed_opt = 0
for i in range(n - 1):
    _, _, (_, _, b1, _) = sorted_tensors[i]
    _, _, (t2, _, _, _) = sorted_tensors[i+1]
    if b1 == t2:
        open_opt += 1
    else:
        closed_opt += 1
print(f"\nWith edge-sorted ordering:")
print(f"Open (match): {open_opt}/{n-1} ({open_opt/(n-1)*100:.1f}%)")
print(f"Closed (no match): {closed_opt}/{n-1} ({closed_opt/(n-1)*100:.1f}%)")
