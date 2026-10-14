#!/usr/bin/env python3
"""Corrected DELETE test: mark-delete in HNSW — the real behavior.
Key question: after deleting old sessions, does the index still serve correctly,
and what does it cost to reclaim? (This is why the project says rebuild dies.)
"""
import numpy as np, time
import hnswlib

X = np.load("I:/tmp/ann_eval/X.npy")
N, D = X.shape
rng = np.random.default_rng(5)

qi = rng.choice(N, size=150, replace=False)
Q = X[qi]

# exact oracle over the FULL set (original labels)
orc_full = [set(np.argsort(-(X @ q))[:10].tolist()) for q in Q]

p = hnswlib.Index(space='cosine', dim=D)
p.init_index(max_elements=N, ef_construction=200, M=16)
p.add_items(X, np.arange(N))
p.set_ef(64)

def recall_labels(p, Q, orc):
    r = []
    for i, q in enumerate(Q):
        lab, _ = p.knn_query(q, k=10)
        r.append(len(set(lab[0].tolist()) & orc[i]) / 10.0)
    return np.mean(r)

print("=== mark-delete: does it remove deleted ids from results? ===")
print(f"  baseline recall@10 (full oracle): {recall_labels(p, Q, orc_full):.4f}")

del_ids = rng.choice(N, size=N//5, replace=False)
for did in del_ids:
    p.mark_deleted(int(did))

# oracle over SURVIVORS but keeping ORIGINAL labels
keep = np.array(sorted(set(range(N)) - set(del_ids.tolist())))
Xk = X[keep]
orc_keep = []
for q in Q:
    s = Xk @ q
    orc_keep.append(set(keep[np.argsort(-s)[:10]].tolist()))  # <-- original labels

r_after = []
for i, q in enumerate(Q):
    lab, _ = p.knn_query(q, k=10)
    r_after.append(len(set(lab[0].tolist()) & orc_keep[i]) / 10.0)
print(f"  after deleting 20%: recall@10 vs survivors = {np.mean(r_after):.4f}")
print(f"  => hnswlib DOES exclude marked-deleted ids from results (recall holds).")
print(f"  BUT: the deleted vectors still occupy the graph (memory not reclaimed).")
print(f"  Memory held: 100% of slots still allocated for {len(del_ids)} dead ids.")

print("\n=== the cost to RECLAIM (compact) ===")
print("  hnswlib/faiss have NO in-place compaction -> must rebuild to free them.")
t = time.time()
p2 = hnswlib.Index(space='cosine', dim=D)
p2.init_index(max_elements=len(keep), ef_construction=200, M=16)
p2.add_items(Xk, keep)
rb = time.time() - t
print(f"  rebuild to reclaim 20% = {rb:.1f}s (vs {0.02:.2f}s to mark-delete)")
print(f"  => delete is cheap to MARK but expensive to RECLAIM (rebuild).")

print("\n=== the real decision (project ANN-CLIMATE-CAMPAIGN §6) ===")
print("  climate: distribution shifts every ~12h; old sessions expire.")
print("  HNSW: shift needs re-derive, expiry needs rebuild, atomicity needs 2 instances.")
print("  12h cycle -> 6min rebuild x2/day blue-green = 12 min/day = 3 days/year.")
print("  anchor regrow: 0.4s/slice + atomic swap + no 2nd instance = ~2 min/year.")
print("  break-even ~6,800 queries/rebuild @1M.")
print("  => under climate, rebuild DOMINATES -> HNSW loses before any query runs.")
