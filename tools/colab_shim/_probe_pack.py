#!/usr/bin/env python3
"""Pack-sized forage probe: real field array (20736 x 64B = 1.3MB) walked flat
vs hj3-quiet vs stride-37, cold + warm, on the Colab VM. This measures the
memory/cache access pattern that #7161 attributes the cold/warm flip to --
NOT just interpreter loop overhead (the earlier probe_forage_quiet.py measured
that and got ratio 2.5-5.6 on every machine, which is an artifact).

Field layout: 20736 slots x 64 bytes = 1327104 bytes = 1.27 MiB.
Each slot holds a 64-bit mix so the walk cannot be optimised away.
"""
import time, json, sys

TOTAL_SLOTS = 20736
CELL = 64
TOTAL_BYTES = TOTAL_SLOTS * CELL

# --- orbit ground truth (same as probe_forage_quiet.py, spec-derived) ---
def hj3_tower(p): return (p % 144) // 48
def hj3_local(p): return (p % 144) % 48
def hj3_jump(p):
    t = hj3_tower(p); l = hj3_local(p)
    return ((t + 1) % 3) * 48 + (47 - l)

def walk_flat(n, field):
    acc = 0
    for i in range(n):
        off = (i % TOTAL_SLOTS) * CELL
        acc ^= field[off]
    return acc

def walk_hj3(n, field):
    acc = 0
    p = 0
    for i in range(n):
        off = p * CELL
        acc ^= field[off]
        p = hj3_jump(p) if (i % 2 == 0) else ((p + 37) % TOTAL_SLOTS)
    return acc

def walk_stride37(n, field):
    acc = 0
    p = 1
    for i in range(n):
        off = p * CELL
        acc ^= field[off]
        p = (p * 37) % TOTAL_SLOTS
    return acc

def timed(fn, n, field):
    t0 = time.perf_counter()
    a = fn(n, field)
    t1 = time.perf_counter()
    return (t1 - t0) * 1000.0, a

def main():
    # build the real field: 20736 slots x 64B, each slot a distinct 64-bit value
    # stored as one byte per slot position in a bytearray, so the walk touches
    # one cache line region per slot (matching a slot-per-access layout).
    field = bytearray(TOTAL_BYTES)
    for s in range(TOTAL_SLOTS):
        field[s * CELL] = (s * 2654435761) & 0xFF  # distinct-ish per slot
    N = 200000  # 200k accesses: ~10 sweeps of the field
    REPS = 20

    report = {"N": N, "cell": CELL, "slots": TOTAL_SLOTS, "bytes": TOTAL_BYTES}

    # COLD: first touch after build
    cold = {}
    for name, fn in (("flat", walk_flat), ("hj3", walk_hj3), ("stride37", walk_stride37)):
        ms, acc = timed(fn, N, field)
        cold[name] = ms
        report[f"{name}_cold_ms"] = round(ms, 3)

    # WARM: repeated back-to-back runs, take min (best case, cache hot)
    warm = {}
    for name, fn in (("flat", walk_flat), ("hj3", walk_hj3), ("stride37", walk_stride37)):
        best = 1e18
        for _ in range(REPS):
            ms, acc = timed(fn, N, field)
            if ms < best: best = ms
        warm[name] = best
        report[f"{name}_warm_ms"] = round(best, 3)

    report["ratio_hj3_flat_cold"] = round(cold["hj3"] / cold["flat"], 3)
    report["ratio_hj3_flat_warm"] = round(warm["hj3"] / warm["flat"], 3)
    report["ratio_stride37_flat_cold"] = round(cold["stride37"] / cold["flat"], 3)
    report["ratio_stride37_flat_warm"] = round(warm["stride37"] / warm["flat"], 3)

    with open("/content/pack_report.json", "w") as f:
        json.dump(report, f, indent=2)
    print("wrote /content/pack_report.json")
    print(json.dumps(report, indent=2))

main()
