#!/usr/bin/env python3
import json, re
from collections import Counter

def parse(path):
    with open(path) as f:
        raw = f.read()
    return [json.loads(m) for m in re.findall(r'\{[^{}]+\}', raw)]

LINES_9x9 = [list(range(i*9,(i+1)*9)) for i in range(9)] + \
             [list(range(i,81,9)) for i in range(9)] + \
             [list(range(0,81,10)), list(range(8,81,8))]

def fp(vals):
    g = sorted(range(len(vals)), key=lambda i: -vals[i])
    grid = [0]*81
    for pos,rank in enumerate(g): grid[rank] = pos+1
    return tuple(sum(grid[p] for p in line) for line in LINES_9x9)

t1 = parse('I:/DWGLS-native-fs/tests/smollm_all.json')
t2 = parse('I:/DWGLS-native-fs/tests/kokoro_all.json')
t3 = parse('I:/DWGLS-native-fs/tests/qwen3_all.json')
all_tensors = t1 + t2 + t3

fps = []
for obj in all_tensors:
    name = obj['name']
    vals = obj['weights']
    if len(vals) >= 81:
        fps.append((fp(vals[:81]), name))

groups = {}
for f, n in fps:
    groups.setdefault(f, []).append(n)

sizes = Counter(len(v) for v in groups.values())
print(f"Total tensors: {len(fps)}")
print(f"Unique fingerprints: {len(groups)}")
print()
print("Collision group size distribution:")
for s, c in sorted(sizes.items()):
    tag = " <-- collision" if s > 1 else ""
    print(f"  {s} tensor(s): {c} groups{tag}")
print()

collisions = [(f, names) for f, names in groups.items() if len(names) >= 2]
collisions.sort(key=lambda x: -len(x[1]))
print(f"Total collision groups: {len(collisions)}")
print(f"Total tensors in collisions: {sum(len(n) for _,n in collisions)} / {len(fps)}")
print()
for f, names in collisions:
    print(f"  {len(names)}x collision: {names}")
