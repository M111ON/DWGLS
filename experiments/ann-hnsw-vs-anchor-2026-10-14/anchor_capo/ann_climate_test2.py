#!/usr/bin/env python3
"""Part 2: WHY does HNSW die under rebuild? Isolate the real cause.
Hypothesis from ANN-CLIMATE-CAMPAIGN §6: it is NOT insert (cheap) — it is
DELETE (session lifecycle) + the blue-green/atomicity ops, at scale.
"""
import numpy as np, time
import hnswlib

X = np.load("I:/tmp/ann_eval/X.npy")
N, D = X.shape
rng = np.random.default_rng(5)

def recall(p, Q, orc):
    r = []
    for i, q in enumerate(Q):
        lab, _ = p.knn_query(q, k=10)
        r.append(len(set(lab[0].tolist()) & orc[i]) / 10.0)
    return np.mean(r)

qi = rng.choice(N, size=150, replace=False)
Q = X[qi]
orc = [set(np.argsort(-(X @ q))[:10].tolist()) for q in Q]

# ============ B) DELETE: does mark-delete degrade the graph? ============
print("=== B) DELETE (session lifecycle) — HNSW has no clean delete ===")
p = hnswlib.Index(space='cosine', dim=D)
p.init_index(max_elements=N, ef_construction=200, M=16)
p.add_items(X, np.arange(N))
p.set_ef(64)
r_before = recall(p, Q, orc)

for frac in (0.2, 0.5, 0.8):
    p2 = hnswlib.Index(space='cosine', dim=D)
    p2.init_index(max_elements=N, ef_construction=200, M=16)
    p2.add_items(X, np.arange(N))
    p2.set_ef(64)
    del_ids = rng.choice(N, size=int(N*frac), replace=False)
    t = time.time()
    for did in del_ids:
        p2.mark_deleted(int(did))
    dt = time.time() - t
    keep = np.setdiff1d(np.arange(N), del_ids)
    Xk = X[keep]
    orc_k = [set(np.argsort(-(Xk @ q))[:10].tolist()) for q in Q]
    r_after = []
    for i, q in enumerate(Q):
        lab, _ = p2.knn_query(q, k=10)
        r_after.append(len(set(lab[0].tolist()) & orc_k[i]) / 10.0)
    print(f"  delete {frac*100:>3.0f}%: recall(before)={r_before:.4f} "
          f"after={np.mean(r_after):.4f}  mark_deleted={dt*1000:.0f}ms  "
          f"(holes remain; no compaction)")

# ============ does INSERT degrade over many cycles (graph quality)? ============
print("\n=== B2) INSERT-only over 20 cycles: does graph quality drift? ===")
p3 = hnswlib.Index(space='cosine', dim=D)
p3.init_index(max_elements=int(N*1.2), ef_construction=200, M=16)
chunk = N // 20
labels = 0
drift = []
for c in range(20):
    lo, hi = c*chunk, min((c+1)*chunk, N)
    p3.add_items(X[lo:hi], np.arange(labels, labels+(hi-lo)))
    labels += (hi-lo)
    p3.set_ef(64)
    sub = X[:hi]
    orc_c = [set(np.argsort(-(sub @ q))[:10].tolist()) for q in Q]
    r = []
    for i, q in enumerate(Q):
        lab, _ = p3.knn_query(q, k=10)
        r.append(len(set(lab[0].tolist()) & orc_c[i]) / 10.0)
    drift.append(np.mean(r))
    if c in (0, 4, 9, 19):
        print(f"  after cycle {c+1:>2} ({hi:>6} vecs): recall@10 = {np.mean(r):.4f}")
print(f"  drift over 20 cycles: {drift[0]:.4f} -> {drift[-1]:.4f} "
      f"({'STABLE' if abs(drift[0]-drift[-1])<0.01 else 'DEGRADES'})")

# ============ C) the REAL cost: rebuild required by DELETE + scale ============
print("\n=== C) The real rebuild economics (project's §6 math) ===")
print("  HNSW cannot compact deleted ids -> to reclaim memory/quality you REBUILD.")
print("  Rebuild cost scales with N:")
for scale, n in [("61k (this box, measured)", 61241), ("1M (project, measured)", 1_000_000)]:
    if n == 61241:
        t = time.time()
        pp = hnswlib.Index(space='cosine', dim=D)
        pp.init_index(max_elements=n, ef_construction=200, M=16)
        pp.add_items(X, np.arange(N))
        rb = time.time() - t
    else:
        rb = 356.0   # project's faiss HNSW-M32/ef128 @1M, measured
    print(f"    {scale:>24}: rebuild = {rb:>6.1f} s")
print("  project: 12h climate -> 6 min rebuild x2/day (blue-green) = 12 min/day")
print("  anchor regrow: 0.4 s/slice, atomic swap, no 2nd instance")
print("  => at climate frequency, rebuild DOMINATES; insert speed is irrelevant")
print("\n  break-even (project): ~6,800 queries/rebuild @1M")
print("  below that, or shifting distribution -> the rebuild-based system loses")
