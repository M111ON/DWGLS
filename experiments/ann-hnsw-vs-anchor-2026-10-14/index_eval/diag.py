#!/usr/bin/env python3
"""Diagnostics on the 61k x 384 corpus + held-out oracle construction.
Writes: Q.npy (queries), G.npy (db without queries), GT.npy (exact top-100 idx into G).
"""
import time
import numpy as np

OUT = "I:/tmp/ann_eval"
X = np.load(f"{OUT}/X.npy")
n, d = X.shape
print(f"X: {X.shape} dtype={X.dtype}")

# norms
nrm = np.linalg.norm(X, axis=1)
print(f"row norms: min={nrm.min():.4f} max={nrm.max():.4f} mean={nrm.mean():.4f} "
      f"frac_unit(>0.999)={np.mean(nrm>0.999):.3f}")

# global stats
print(f"global mean abs={np.abs(X).mean():.5f}  std={X.std():.5f}")
print(f"per-dim std: min={X.std(0).min():.5f} max={X.std(0).max():.5f} mean={X.std(0).mean():.5f}")

# intrinsic dimensionality: participation ratio of eigenvalues
mu = X.mean(0, keepdims=True)
Xc = X - mu
# covariance eigenvalues via SVD on centered data (economy)
t0 = time.time()
cov = (Xc.T @ Xc) / (n - 1)
w = np.linalg.eigvalsh(cov)[::-1]
w = np.clip(w, 0, None)
print(f"cov eig computed in {time.time()-t0:.1f}s")
pr = (w.sum() ** 2) / (w ** 2).sum()
print(f"participation ratio (effective dim) = {pr:.1f}")
cum = np.cumsum(w) / w.sum()
for frac in (0.5, 0.9, 0.95, 0.99):
    k = int(np.searchsorted(cum, frac)) + 1
    print(f"  dims for {frac*100:.0f}% variance: {k}")

# variance explained by top-2 (the quadtree/2D projection ceiling)
print(f"top-2 PCA variance explained = {cum[1]:.4f}")
print(f"top-8  PCA variance explained = {cum[7]:.4f}")

# Held-out query protocol: 300 random rows are queries, removed from DB.
rng = np.random.default_rng(20261014)
NQ = 300
qi = np.sort(rng.choice(n, size=NQ, replace=False))
mask = np.ones(n, dtype=bool); mask[qi] = False
G = X[mask]                       # db
Q = X[qi]                         # queries (held out of db)
print(f"db G: {G.shape}  queries Q: {Q.shape}")

# exact ground truth top-100 (cosine == dot on unit vectors)
K = 100
t0 = time.time()
GT = np.empty((Q.shape[0], K), dtype=np.int32)
GTs = np.empty((Q.shape[0], K), dtype=np.float32)
for i in range(Q.shape[0]):
    s = G @ Q[i]
    idx = np.argpartition(-s, K)[:K]
    idx = idx[np.argsort(-s[idx])]
    GT[i] = idx
    GTs[i] = s[idx]
print(f"oracle top-{K} in {time.time()-t0:.1f}s")
print(f"top-1 sim: mean={GTs[:,0].mean():.4f} min={GTs[:,0].min():.4f}")
print(f"top-10 sim: mean={GTs[:,9].mean():.4f}")

np.save(f"{OUT}/G.npy", G)
np.save(f"{OUT}/Q.npy", Q)
np.save(f"{OUT}/GT.npy", GT)
np.save(f"{OUT}/GTs.npy", GTs)
np.save(f"{OUT}/qi.npy", qi)
print("saved G,Q,GT,GTs,qi")
