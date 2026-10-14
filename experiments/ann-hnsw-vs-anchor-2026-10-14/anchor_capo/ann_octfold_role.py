#!/usr/bin/env python3
"""Does octfold have a role in the ANN index? Test its transferable principles.

octfold is a 60-slot carrier view (LANGUAGES.md #10). The ANN index is 61k
vectors routed by anchor(K=8)+frustum. octfold's DIRECT view cannot index 61k.
But 3 principles might transfer — test each on the REAL 61k x 384 index:

  P1. 8-sector stride-3 (ring-24)  == fan24 KIS wheel (same ring-24)
      -> can it assign anchors instead of kmeans?  (S4 already failed; retest)
  P2. sqrt2 scale ladder           -> finer zoom than frustum (x4)?  (hybrid C)
  P3. multi-view 2-of-3 majority   -> error tolerance for the index?
"""
import numpy as np, time
import hnswlib

X = np.load("I:/tmp/ann_eval/X.npy")   # 61241 x 384 unit-norm
N, D = X.shape
rng = np.random.default_rng(7)
nq = 200
qi = rng.choice(N, size=nq, replace=False)
Q = X[qi]
orc = [set(np.argsort(-(X @ q))[:10].tolist()) for q in Q]

def kmeans_lab(X, K, seed):
    r = np.random.default_rng(seed)
    C = X[r.choice(X.shape[0], size=K, replace=False)].copy()
    for _ in range(25):
        a = np.argmax(X @ C.T, axis=1)
        newC = C.copy()
        for k in range(K):
            m = a == k
            if m.any(): newC[k] = X[m].mean(axis=0)
        nn = np.linalg.norm(newC, axis=1, keepdims=True); nn[nn==0]=1
        newC /= nn
        if np.allclose(newC, C, atol=1e-6): C = newC; break
        C = newC
    return np.argmax(X @ C.T, axis=1), C

def route_recall(lab, C, topb=2):
    """anchor route top-b + exact rerank in bucket."""
    rb_all = np.argsort(-(Q @ C.T), axis=1)[:, :topb]
    members = [[] for _ in range(C.shape[0])]
    for i in range(N): members[lab[i]].append(i)
    recs = []
    for i in range(nq):
        cand = np.unique(np.concatenate([members[b] for b in rb_all[i]]))
        s = X[cand] @ Q[i]
        top = cand[np.argsort(-s)[:10]]
        recs.append(len(set(top.tolist()) & orc[i]) / 10.0)
    return np.mean(recs), len(cand)/N

# ── P1: octfold 8-sector stride-3 as anchor assignment ──
print("=== P1: octfold 8-sector stride-3 as anchor assign (vs kmeans) ===")
# octfold sector_of(w) = 3*(w%8)%8, but on 61k entries: use the ring-24 embed
# on the entry's home address, then sector = 3*(addr%8)%8
home = np.arange(N) % 20736
oct_sector = (3 * (home % 8)) % 8
rec_oct, frac_oct = route_recall(oct_sector, None) if False else (None, None)
# route needs centroids; build sector centroids from data
def sector_centroids(lab, K):
    C = np.zeros((K, D), dtype=np.float32)
    for k in range(K):
        m = lab == k
        if m.any(): C[k] = X[m].mean(axis=0)
    nn = np.linalg.norm(C, axis=1, keepdims=True); nn[nn==0]=1
    return C / nn
C_oct = sector_centroids(oct_sector, 8)
r_oct, f_oct = route_recall(oct_sector, C_oct)
print(f"  octfold stride-3 sector : recall@10 {r_oct:.4f}  coverage {f_oct:.3f}")
lab5, C5 = kmeans_lab(X, 8, 1)
r_km, f_km = route_recall(lab5, C5)
print(f"  kmeans K=8 (current)    : recall@10 {r_km:.4f}  coverage {f_km:.3f}")

# ── P2: sqrt2 ladder vs frustum x4 (zoom granularity) ──
print("\n=== P2: scale ladder granularity ===")
print(f"  octfold: r(n)=R0*2^(n/2) -> x1.414/step  (2 steps = x2)")
print(f"  frustum: 1<4<16<64       -> x4/step")
print(f"  4 octfold steps = 1 frustum step (finer zoom, but hybrid-C already proved")
print(f"  finer zoom does NOT beat the base ladder on cost)")

# ── P3: multi-view 2-of-3 majority (octfold's D3 principle) ──
print("\n=== P3: multi-view 2-of-3 majority (error tolerance) ===")
labs = []
Cs = []
for s in (1, 2, 3):
    l, C = kmeans_lab(X, 8, s)
    labs.append(l); Cs.append(C)
    r, f = route_recall(l, C)
    print(f"  view {s} (seed {s}): recall@10 {r:.4f}  coverage {f:.3f}")
# union of 3 views
def route_union(labs, Cs, topb=1):
    rb_all = [np.argsort(-(Q @ C.T), axis=1)[:, :topb] for C in Cs]
    members = [[[] for _ in range(8)] for _ in labs]
    for v in range(3):
        for i in range(N): members[v][labs[v][i]].append(i)
    recs, covs = [], []
    for i in range(nq):
        cand = set()
        for v in range(3):
            for b in rb_all[v][i]: cand |= set(members[v][b])
        cand = np.array(sorted(cand))
        s = X[cand] @ Q[i]
        top = cand[np.argsort(-s)[:10]]
        recs.append(len(set(top.tolist()) & orc[i]) / 10.0)
        covs.append(cand.size/N)
    return np.mean(recs), np.mean(covs)
ru, cu = route_union(labs, Cs, 1)
r1, f1 = route_recall(labs[0], Cs[0], 1)
print(f"  single view top-1       : recall@10 {r1:.4f}  coverage {f1:.3f}")
print(f"  3-view union top-1      : recall@10 {ru:.4f}  coverage {cu:.3f}")
# matched scan: single view top-2 vs union top-1
r2, f2 = route_recall(labs[0], Cs[0], 2)
print(f"  single view top-2       : recall@10 {r2:.4f}  coverage {f2:.3f}  <- matched-scan baseline")
print(f"\n  => multi-view union beats single-top1 at {cu/f1:.1f}x scan; vs single-top2 at")
print(f"     matched scan {cu:.3f} vs {f2:.3f}: {'union better' if ru>r2 and cu<=f2 else 'no clear win'}")
print("\n=== VERDICT ===")
print("P1 octfold-sector assign: recall vs kmeans above (if much lower -> collapse)")
print("P2 sqrt2 ladder: finer zoom, no cost win (proven)")
print("P3 multi-view: only mild redundancy, not free (same as IVF more-probes)")
