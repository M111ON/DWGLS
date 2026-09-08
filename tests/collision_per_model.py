#!/usr/bin/env python3
"""Reproduce the exact earlier collision test but with corrected parser."""
import json, re
from collections import Counter

def parse_concat(path):
    """Parse concatenated JSON objects (not newline-delimited)."""
    with open(path) as f:
        raw = f.read()
    return [json.loads(m) for m in re.findall(r'\{[^{}]*"weights"\s*:\s*\[[^\]]*\][^{}]*\}', raw)]

LINES_9x9 = [list(range(i*9,(i+1)*9)) for i in range(9)] + \
             [list(range(i,81,9)) for i in range(9)] + \
             [list(range(0,81,10)), list(range(8,81,8))]

def fp(vals):
    g = sorted(range(len(vals)), key=lambda i: -vals[i])
    grid = [0]*81
    for pos,rank in enumerate(g): grid[rank] = pos+1
    return tuple(sum(grid[p] for p in line) for line in LINES_9x9)

# Load all models
smollm = parse_concat('I:/DWGLS-native-fs/tests/smollm_all.json')
kokoro = parse_concat('I:/DWGLS-native-fs/tests/kokoro_all.json')
qwen3  = parse_concat('I:/DWGLS-native-fs/tests/qwen3_all.json')

print(f"Loaded: SmolLM={len(smollm)}, Kokoro={len(kokoro)}, Qwen3={len(qwen3)}")

# Per-model collision check
for model_name, data in [("SmolLM", smollm), ("Kokoro", kokoro), ("Qwen3", qwen3)]:
    fps = []
    for obj in data:
        name = obj['name']
        vals = obj['weights']
        if len(vals) >= 81:
            fps.append((fp(vals[:81]), name))
    
    groups = {}
    for f, n in fps:
        groups.setdefault(f, []).append(n)
    
    n_collisions = sum(1 for v in groups.values() if len(v) > 1)
    n_in_collisions = sum(len(v) for v in groups.values() if len(v) > 1)
    print(f"\n{model_name}: {len(fps)} tensors, {len(groups)} unique, {n_collisions} collision groups, {n_in_collisions} tensors in collisions")
    
    if n_collisions > 0:
        for f, names in sorted(groups.items(), key=lambda x: -len(x[1])):
            if len(names) >= 2:
                print(f"  {len(names)}x: {names}")

# Combined
all_data = smollm + kokoro + qwen3
fps = []
for obj in all_data:
    name = obj['name']
    vals = obj['weights']
    if len(vals) >= 81:
        fps.append((fp(vals[:81]), name))

groups = {}
for f, n in fps:
    groups.setdefault(f, []).append(n)

n_collisions = sum(1 for v in groups.values() if len(v) > 1)
print(f"\nCombined: {len(fps)} tensors, {len(groups)} unique, {n_collisions} collision groups")
if n_collisions > 0:
    for f, names in sorted(groups.items(), key=lambda x: -len(x[1])):
        if len(names) >= 2:
            print(f"  {len(names)}x: {names}")
