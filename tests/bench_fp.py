#!/usr/bin/env python3
import json, re, time

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

# Load all
smollm = parse_concat('I:/DWGLS-native-fs/tests/smollm_all.json')
qwen3 = parse_concat('I:/DWGLS-native-fs/tests/qwen3_all.json')
all_data = smollm + qwen3
print(f"Tensors: {len(all_data)}")

# Benchmark: single tensor
vals = all_data[0]['weights'][:81]
t0 = time.perf_counter()
for _ in range(10000):
    fp_rank(vals)
t1 = time.perf_counter()
print(f"Single tensor (9x9): {(t1-t0)/10000*1e6:.1f} µs")

# Benchmark: all tensors
t0 = time.perf_counter()
for _ in range(1000):
    for obj in all_data:
        fp_rank(obj['weights'][:81])
t1 = time.perf_counter()
total = len(all_data) * 1000
print(f"All {len(all_data)} tensors x1000: {(t1-t0):.3f}s total, {(t1-t0)/total*1e6:.1f} µs/tensor")

# Throughput
print(f"Throughput: {total/(t1-t0):.0f} tensors/sec")
