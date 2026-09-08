#!/usr/bin/env python3
"""Tantrix-style quantized edges from line-sum fingerprints."""
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
    R = fingerprint[:9]
    C = fingerprint[9:18]
    return (R[0], C[8], R[8], C[0])  # top, right, bottom, left

def quantize(val, min_val, max_val, n_bins=4):
    """Map value to 2-bit bin (0-3)."""
    if max_val == min_val:
        return 0
    return min(int((val - min_val) / (max_val - min_val) * n_bins), n_bins - 1)

# Load all
smollm = parse_concat('I:/DWGLS-native-fs/tests/smollm_all.json')
qwen3 = parse_concat('I:/DWGLS-native-fs/tests/qwen3_all.json')
kokoro = parse_concat('I:/DWGLS-native-fs/tests/kokoro_all.json')
all_data = smollm + qwen3 + kokoro

print(f"Tensors: {len(all_data)}")

# Compute fingerprints + edges + quantized edges
tensors = []
all_edges = []
for obj in all_data:
    name = obj['name']
    vals = obj['weights']
    if len(vals) >= 81:
        fp = fp_rank(vals[:81])
        edges = derive_edges(fp)
        all_edges.append(edges)
        tensors.append((name, fp, edges))

# Find ranges
all_t = [e[0] for e in all_edges]
all_r = [e[1] for e in all_edges]
all_b = [e[2] for e in all_edges]
all_l = [e[3] for e in all_edges]
min_t, max_t = min(all_t), max(all_t)
min_r, max_r = min(all_r), max(all_r)
min_b, max_b = min(all_b), max(all_b)
min_l, max_l = min(all_l), max(all_l)

print(f"Edge ranges: T=[{min_t},{max_t}] R=[{min_r},{max_r}] B=[{min_b},{max_b}] L=[{min_l},{max_l}]")

# Quantize
q_tensors = []
for name, fp, (t, r, b, l) in tensors:
    qt = quantize(t, min_t, max_t)
    qr = quantize(r, min_r, max_r)
    qb = quantize(b, min_b, max_b)
    ql = quantize(l, min_l, max_l)
    q_tensors.append((name, fp, (t, r, b, l), (qt, qr, qb, ql)))

# === Test 1: Quantized edge distribution ===
print(f"\n=== Quantized edge distribution (2-bit = 4 bins) ===")
for i, label in enumerate(['Top', 'Right', 'Bottom', 'Left']):
    dist = Counter(q[3][i] for q in q_tensors)
    total = len(q_tensors)
    parts = [f"  {label}[{k}]: {v} ({v/total*100:.0f}%)" for k, v in sorted(dist.items())]
    print('\n'.join(parts))

# === Test 2: Quantized edge matching ===
q_top = {}
q_right = {}
q_bottom = {}
q_left = {}
for i, (name, fp, edges, (qt, qr, qb, ql)) in enumerate(q_tensors):
    q_top.setdefault(qt, []).append(i)
    q_right.setdefault(qr, []).append(i)
    q_bottom.setdefault(qb, []).append(i)
    q_left.setdefault(ql, []).append(i)

tb_matches = 0
lr_matches = 0
for i, (name, fp, (t,r,b,l), (qt,qr,qb,ql)) in enumerate(q_tensors):
    if qb in q_top:
        tb_matches += len(q_top[qb]) - (1 if qb == qt else 0)
    if qr in q_left:
        lr_matches += len(q_left[qr]) - (1 if qr == ql else 0)

n = len(q_tensors)
print(f"\n=== Quantized matching ===")
print(f"TB-matching pairs (vertical): {tb_matches//2}")
print(f"LR-matching pairs (horizontal): {lr_matches//2}")
print(f"Match rate: {(tb_matches+lr_matches)//2}/{n*(n-1)//2} = {(tb_matches+lr_matches)/2/(n*(n-1)//2)*100:.1f}%")

# === Test 3: Longest chain with quantized edges (greedy BFS) ===
adj_q = {}
for i, (name, fp, edges, (qt, qr, qb, ql)) in enumerate(q_tensors):
    adj_q.setdefault(qb, []).append((i, name))

# Greedy: for each starting tensor, follow chain greedily (first match)
best_chain = []
for si in range(n):
    chain = [si]
    used = {si}
    while True:
        _, _, _, (_, _, qb, _) = q_tensors[chain[-1]]
        if qb not in adj_q:
            break
        found = False
        for ni, nn in adj_q[qb]:
            if ni not in used:
                chain.append(ni)
                used.add(ni)
                found = True
                break
        if not found:
            break
    if len(chain) > len(best_chain):
        best_chain = chain[:]

print(f"\n=== Longest quantized chain ===")
print(f"Chain length: {len(best_chain)} tensors")
for idx in best_chain[:30]:
    name, fp, edges, qe = q_tensors[idx]
    print(f"  {name}  edges={qe}")

# === Test 4: Gate simulation ===
open_q = sum(1 for i in range(n-1) if q_tensors[i][3][2] == q_tensors[i+1][3][0])
print(f"\n=== Gate (quantized, memory order) ===")
print(f"Open: {open_q}/{n-1} ({open_q/(n-1)*100:.1f}%)")
print(f"Closed: {n-1-open_q}/{n-1} ({(n-1-open_q)/(n-1)*100:.1f}%)")

# === Test 5: Wang vs Tantrix comparison ===
print(f"\n=== Wang (raw) vs Tantrix (quantized) ===")
qmatch = (tb_matches+lr_matches)/2/(n*(n-1)//2)*100
gate_pct = open_q/(n-1)*100
print(f"{'Metric':<30} {'Wang (raw)':<20} {'Tantrix (2-bit)':<20}")
print(f"{'Unique edge values':<30} {'237-285':<20} {'4':<20}")
print(f"{'Match rate':<30} {'0.4%':<20} {qmatch:.1f}%")
print(f"{'Longest chain':<30} {'9':<20} {len(best_chain)}")
print(f"{'Gate open rate':<30} {'0.7%':<20} {gate_pct:.1f}%")
