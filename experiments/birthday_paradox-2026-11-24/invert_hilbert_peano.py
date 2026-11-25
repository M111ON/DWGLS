"""Experiment v2: invert-Hilbert (horizontal) XOR Peano (vertical) on an empty grid field.

Fixes from v1: combined test now uses sparse subset + measures distribution quality
of XOR as a hash on occupied cells, plus interleaving comparison.
"""
import numpy as np
import json, os

# ---------- Hilbert (2D, power of 2) ----------
def hilbert_xy2d(order, x, y):
    n = 1 << order
    d = 0
    s = n >> 1
    while s > 0:
        rx = 1 if (x & s) else 0
        ry = 1 if (y & s) else 0
        d += s * s * ((3 * rx) ^ ry)
        if ry == 0:
            if rx == 1:
                x = n - 1 - x
                y = n - 1 - y
            x, y = y, x
        s >>= 1
    return d

def hilbert_d2xy(order, d):
    n = 1 << order
    x = y = 0
    s = 1
    while s < n:
        rx = 1 & (d >> 1)
        ry = 1 & (d ^ rx)
        if ry == 0:
            if rx == 1:
                x = s - 1 - x
                y = s - 1 - y
            x, y = y, x
        x += s * rx
        y += s * ry
        d >>= 2
        s <<= 1
    return x, y

# ---------- Peano (2D, power of 3) ----------
def peano_curve(order):
    if order == 0:
        return [(0, 0)]
    prev = peano_curve(order - 1)
    n = 3 ** (order - 1)
    result = []
    for by in range(3):
        cols = range(3) if (by % 2 == 0) else range(2, -1, -1)
        for bx in cols:
            flip_x = (by % 2 == 1)
            flip_y = (bx % 2 == 1) and (by % 2 == 0)
            for (px, py) in prev:
                x = bx * n + (n - 1 - px if flip_x else px)
                y = by * n + (n - 1 - py if flip_y else py)
                result.append((x, y))
    return result

def peano_xy2d(order, x, y):
    n = 3 ** order
    curve = peano_curve(order)
    idx_map = {}
    for i, (cx, cy) in enumerate(curve):
        idx_map[(cx, cy)] = i
    return idx_map[(x, y)]

def peano_d2xy(order, d):
    return peano_curve(order)[d]

# ---------- helpers ----------
def chi2_uniform(values, n_bins):
    hist, _ = np.histogram(values, bins=n_bins, range=(0, n_bins))
    expected = len(values) / n_bins
    if expected == 0:
        return float('inf')
    return float(np.sum((hist - expected) ** 2 / expected))

def run():
    results = {}

    # Hilbert 8x8
    oh, nh = 3, 8
    cells_h = nh * nh
    h_idx = np.array([hilbert_xy2d(oh, x, y) for y in range(nh) for x in range(nh)])
    rt_h = all(hilbert_d2xy(oh, hilbert_xy2d(oh, x, y)) == (x, y)
               for y in range(nh) for x in range(nh))
    results["hilbert_8x8"] = {
        "cells": cells_h, "chi2": chi2_uniform(h_idx, cells_h),
        "unique": int(len(np.unique(h_idx))), "roundtrip": bool(rt_h),
    }

    # Peano 9x9
    op, np_ = 2, 9
    cells_p = np_ * np_
    p_idx = np.array([peano_xy2d(op, x, y) for y in range(np_) for x in range(np_)])
    rt_p = all(peano_d2xy(op, peano_xy2d(op, x, y)) == (x, y)
               for y in range(np_) for x in range(np_))
    results["peano_9x9"] = {
        "cells": cells_p, "chi2": chi2_uniform(p_idx, cells_p),
        "unique": int(len(np.unique(p_idx))), "roundtrip": bool(rt_p),
    }

    # --- Combined test on sparse subset ---
    # Simulate an empty field with 5% occupancy on a 72x72 grid
    rng = np.random.default_rng(7)
    GRID = 72
    n_cells = GRID * GRID
    occ_frac = 0.05
    n_occ = int(n_cells * occ_frac)
    occ_indices = rng.choice(n_cells, size=n_occ, replace=False)
    occ_xs = occ_indices % GRID
    occ_ys = occ_indices // GRID

    # Method A: XOR of Hilbert(x%8) and Peano(y%9) -- per-axis
    xor_vals = np.array([hilbert_xy2d(3, int(x) % 8, int(y) % 8) ^
                         peano_xy2d(2, int(x) % 9, int(y) % 9)
                         for x, y in zip(occ_xs, occ_ys)])
    # Method B: interleaved bits (Hilbert 6 bits + Peano 7 bits -> 13 bits)
    interleave = np.array([(hilbert_xy2d(3, int(x) % 8, int(y) % 8) |
                            (peano_xy2d(2, int(x) % 9, int(y) % 9) << 6))
                           for x, y in zip(occ_xs, occ_ys)])
    # Method C: Hilbert-only on 8x8 (mod 64) -- baseline
    h_only = np.array([hilbert_xy2d(3, int(x) % 8, int(y) % 8) for x, y in zip(occ_xs, occ_ys)])
    # Method D: Peano-only on 9x9 (mod 81) -- baseline
    p_only = np.array([peano_xy2d(2, int(x) % 9, int(y) % 9) for x, y in zip(occ_xs, occ_ys)])

    n_bins_xor = 128  # 7 bits
    n_bins_inter = 8192  # 13 bits
    n_bins_h = 64
    n_bins_p = 81

    results["sparse_combined"] = {
        "grid": f"{GRID}x{GRID}",
        "occupied": n_occ,
        "occupancy_pct": occ_frac * 100,
        "xor_unique": int(len(np.unique(xor_vals))),
        "xor_chi2 (128 bins)": chi2_uniform(xor_vals, n_bins_xor),
        "interleave_unique": int(len(np.unique(interleave))),
        "interleave_chi2 (8192 bins)": chi2_uniform(interleave, n_bins_inter),
        "hilbert_only_unique": int(len(np.unique(h_only))),
        "hilbert_only_chi2 (64 bins)": chi2_uniform(h_only, n_bins_h),
        "peano_only_unique": int(len(np.unique(p_only))),
        "peano_only_chi2 (81 bins)": chi2_uniform(p_only, n_bins_p),
    }

    # --- Cross-dimensional adjacency: Hilbert neighbors vs Peano neighbors ---
    n = 8
    h_adj_total = 0
    h_adj_peano_close = 0
    for y in range(n):
        for x in range(n):
            if x + 1 < n:
                h1 = hilbert_xy2d(3, x, y)
                h2 = hilbert_xy2d(3, x + 1, y)
                p1 = peano_xy2d(2, x % 9, y % 9)
                p2 = peano_xy2d(2, (x + 1) % 9, y % 9)
                h_adj_total += 1
                if abs(p1 - p2) <= 3:
                    h_adj_peano_close += 1
    results["cross_dim_adjacency"] = {
        "hilbert_adjacent_pairs": h_adj_total,
        "also_peano_close (|diff|<=3)": h_adj_peano_close,
        "fraction": round(h_adj_peano_close / h_adj_total, 4),
    }

    # --- Storage: sparse inverse vs full array ---
    storage = {}
    for occ_frac in (0.01, 0.05, 0.10, 0.50):
        n_occ = int(cells_h * occ_frac) if occ_frac * cells_h >= 1 else 1
        # sparse: 8 bytes per occupied cell (4 idx + 2 x + 2 y)
        sparse_b = n_occ * 8
        # full array: 4 bytes per cell
        full_b = cells_h * 4
        storage[f"occ_{int(occ_frac*100)}pct"] = {
            "occupied": n_occ,
            "sparse_bytes": sparse_b,
            "full_bytes": full_b,
            "ratio": round(sparse_b / full_b, 4),
            "savings_pct": round((1 - sparse_b / full_b) * 100, 1),
        }
    results["storage"] = storage

    # --- Inverse roundtrip timing (simulate O(1) lookup) ---
    # Verify that inverse is truly O(1) (array lookup) vs forward (computation)
    import time
    # forward: compute hilbert for all cells
    t0 = time.perf_counter()
    for _ in range(100):
        [hilbert_xy2d(3, x, y) for y in range(8) for x in range(8)]
    t_fwd = (time.perf_counter() - t0) / 100
    # inverse: array lookup
    inv_table = [hilbert_d2xy(3, d) for d in range(64)]
    t0 = time.perf_counter()
    for _ in range(100):
        [inv_table[d] for d in range(64)]
    t_inv = (time.perf_counter() - t0) / 100
    results["timing"] = {
        "forward_64cells_us": round(t_fwd * 1e6, 2),
        "inverse_64cells_us": round(t_inv * 1e6, 2),
        "speedup": round(t_fwd / t_inv, 2) if t_inv > 0 else float('inf'),
    }

    out_path = os.path.join(os.path.dirname(__file__), "invert_hilbert_peano_results.json")
    with open(out_path, "w") as f:
        json.dump(results, f, indent=2)
    print(json.dumps(results, indent=2))

if __name__ == "__main__":
    run()
