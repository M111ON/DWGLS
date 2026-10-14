#!/usr/bin/env python3
"""
bench_geo.py — (a) the proposed quadtree x octfold geometric hybrid.

Faithful, BEST-CASE implementation of the user's idea:
  * quadtree  = 2-D recursive grid routing (PCA -> 2 principal axes -> cells)
  * octfold   = multi-view: several independent 2-D projections ("folds"),
                union/intersect of their cells.

Metrics: recall@1/@10 vs FRACTION OF INDEX SCANNED (pruning power), + cells.
Ground truth = exact cosine top-100 (held-out protocol).
"""
import json, time, sys
import numpy as np

OUT = "I:/tmp/ann_eval"
G = np.load(f"{OUT}/G.npy")
Q = np.load(f"{OUT}/Q.npy")
GT = np.load(f"{OUT}/GT.npy")
GTs = np.load(f"{OUT}/GTs.npy")
n, d = G.shape
NQ = Q.shape[0]
K = 10

def recall_at(cand_idx, gt, k):
    """cand_idx: array of candidate db indices (unsorted). Return recall@k."""
    return len(set(cand_idx[:k].tolist()) & set(gt[:k].tolist())) / k

def score_candidates(cand, q):
    if len(cand) == 0:
        return np.array([], dtype=np.int64)
    c = np.asarray(cand)
    s = G[c] @ q
    return c[np.argsort(-s)]

# ---------- projections (views) ----------
# Precompute PCA directions on db (this is the training step).
mu = G.mean(0, keepdims=True)
Gc = G - mu
# full SVD directions on a subsample for speed
sub = Gc[::max(1, n // 8000)]
_, _, Vt = np.linalg.svd(sub, full_matrices=False)
# view = pair of principal directions
views = {
    "PC0-1": Vt[0:2],   # dominant 2-D
    "PC2-3": Vt[2:4],
    "PC4-5": Vt[4:6],
    "PC6-7": Vt[6:8],
}

def build_grid(proj, grid):
    """Return (cell_of function params, buckets dict)."""
    P = proj.T                       # (d,2)
    X2 = Gc @ P
    Q2 = (Q - mu) @ P
    lo = X2.min(0); hi = X2.max(0)
    rng = np.where(hi - lo == 0, 1, hi - lo)
    def cell(A):
        t = (A - lo) / rng
        return np.clip((t * grid).astype(np.int32), 0, grid - 1)
    Xc = cell(X2); Qc = cell(Q2)
    buckets = {}
    for i in range(n):
        buckets.setdefault((int(Xc[i,0]), int(Xc[i,1])), []).append(i)
    return buckets, Qc

def route_cells(buckets, Qc, grid, win):
    """For each query, union of cells in (2win+1)^2 window."""
    cands = []
    for qi in range(Qc.shape[0]):
        cx, cy = int(Qc[qi,0]), int(Qc[qi,1])
        s = []
        for dx in range(-win, win+1):
            for dy in range(-win, win+1):
                s.extend(buckets.get((cx+dx, cy+dy), []))
        cands.append(s)
    return cands

def eval_cands(cands, tag):
    r1 = r10 = 0.0; frac = 0.0
    for qi in range(NQ):
        c = cands[qi]
        frac += len(c) / n
        if len(c):
            top = score_candidates(c, Q[qi])[:K]
            r1 += recall_at(top, GT[qi], 1)
            r10 += recall_at(top, GT[qi], K)
    r1 /= NQ; r10 /= NQ; frac /= NQ
    return dict(tag=tag, r1=r1, r10=r10, frac=frac)

results = []

print("=== (a) SINGLE-VIEW quadtree (PC0-1), 3x3 and 5x5 windows ===")
print(f"{'grid':>5} {'win':>4} {'R@1':>7} {'R@10':>7} {'frac':>7}")
for grid in (8, 16, 32, 64, 128, 256):
    buckets, Qc = build_grid(views["PC0-1"], grid)
    for win in (1, 2, 3):
        cands = route_cells(buckets, Qc, grid, win)
        r = eval_cands(cands, f"quad_g{grid}_w{win}")
        results.append(r)
        print(f"{grid:>5} {win:>4} {r['r1']:>7.3f} {r['r10']:>7.3f} {r['frac']:>7.4f}", flush=True)

print("\n=== (a2) OCTFOLD multi-view: union of 4 view-pairs (PC0-1,2-3,4-5,6-7) ===")
print(f"{'grid':>5} {'win':>4} {'mode':>8} {'R@1':>7} {'R@10':>7} {'frac':>7}")
for grid in (16, 32, 64, 128):
    for win in (1, 2):
        view_cands = []
        for vname, P in views.items():
            b, Qc = build_grid(P, grid)
            view_cands.append(route_cells(b, Qc, grid, win))
        for mode in ("union", "intersect"):
            cands = []
            for qi in range(NQ):
                sets = [set(vc[qi]) for vc in view_cands]
                if mode == "union":
                    s = set().union(*sets)
                else:
                    s = set.intersection(*sets)
                cands.append(list(s))
            r = eval_cands(cands, f"oct_{grid}_w{win}_{mode}")
            results.append(r)
            print(f"{grid:>5} {win:>4} {mode:>8} {r['r1']:>7.3f} {r['r10']:>7.3f} {r['frac']:>7.4f}", flush=True)

with open(f"{OUT}/res_geo.json", "w") as f:
    json.dump(results, f, indent=1)
print("\nsaved res_geo.json", len(results), "rows")
