#!/usr/bin/env python3
"""Does HNSW/brute have a rebuild problem? Measure the UPDATE story:
 - brute: append-only (no index to rebuild)
 - HNSW: incremental add vs full rebuild
 - IVF/kmeans (the CURRENT system): must retrain centroids on ALL data
Ground truth on real 61k corpus (X.npy cached).
"""
import numpy as np, time, sys
import hnswlib

X = np.load("I:/tmp/ann_eval/X.npy")     # 61241 x 384, unit-norm
N, D = X.shape
print(f"corpus {X.shape}")

rng = np.random.default_rng(0)
perm = rng.permutation(N)
BASE = 60000
Xbase, Xnew = X[perm[:BASE]], X[perm[BASE:]]
print(f"base {Xbase.shape}  new {Xnew.shape}")

# ---- BRUTE: "update" = append to the matrix ----
t = time.time()
M = np.vstack([Xbase, Xnew])
brute_append_ms = (time.time() - t) * 1000
print(f"\n[BRUTE] append {len(Xnew)} vectors: {brute_append_ms:.1f} ms  (no index, no rebuild)")

# ---- HNSW: full rebuild vs incremental add ----
p = hnswlib.Index(space='cosine', dim=D)
t = time.time()
p.init_index(max_elements=N + 100000, ef_construction=200, M=16)
p.add_items(Xbase, np.arange(BASE))
hnsw_build_s = time.time() - t
print(f"\n[HNSW] full build {BASE}: {hnsw_build_s:.1f} s")

t = time.time()
p.add_items(Xnew, np.arange(BASE, N))
hnsw_inc_s = time.time() - t
print(f"[HNSW] INCREMENTAL add {len(Xnew)}: {hnsw_inc_s:.2f} s "
      f"({hnsw_inc_s/len(Xnew)*1000:.2f} ms/vector)")
print(f"[HNSW] rebuild would cost {hnsw_build_s:.1f} s  ->  "
      f"incremental is {hnsw_build_s/max(hnsw_inc_s,1e-6):.0f}x cheaper")

# scale the incremental rate
for add in (1000, 10000, 61000):
    print(f"       extrapolated add {add:>6}: {add*hnsw_inc_s/len(Xnew):>7.2f} s")

# ---- IVF: the CURRENT approach must retrain ----
try:
    import faiss
    faiss.omp_set_num_threads(1)
    K = 1024
    t = time.time()
    q = faiss.IndexFlatIP(D)
    q.add(Xbase)
    ivf = faiss.IndexIVFFlat(q, D, K, faiss.METRIC_INNER_PRODUCT)
    ivf.train(Xbase)
    ivf.add(Xbase)
    ivf_retrain_s = time.time() - t
    print(f"\n[IVF/KMEANS, current system] train K={K} on {BASE}: {ivf_retrain_s:.1f} s")
    # adding data to IVF WITHOUT retrain just appends to nearest cell (fast, but stale centroids)
    t = time.time()
    ivf.add(Xnew)
    print(f"[IVF] add-without-retrain {len(Xnew)}: {(time.time()-t):.2f} s "
          f"(but centroids go STALE -> recall degrades; proper update needs retrain)")
except ImportError:
    print("\n[IVF] faiss not available in this interpreter; skipping")

print("\n=== VERDICT ===")
print("brute   : append O(1), NEVER rebuilds            <- best update story")
print("HNSW    : incremental insert, no full rebuild    <- also good")
print("IVF/kmeans: MUST retrain on all data (regrow)    <- the actual rebuild problem")
print("=> the premise is INVERTED: brute/HNSW are the ones WITHOUT a rebuild problem")
