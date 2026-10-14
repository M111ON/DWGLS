#!/usr/bin/env python3
"""
ann_eval_proto.py — empirical test for the ANN indexing question.

Core question: can a 2-D geometric routing structure (quadtree/octfold-style)
prune 384-dim vectors at all, or does the curse of dimensionality kill it?
Compare against plain IVF (kmeans buckets) at rising K.

Ground truth = exact cosine top-10 (numpy brute over the full sample).
Metrics: recall@10 vs FRACTION OF INDEX SCANNED (the only metric that matters
for pruning) and vs cells/buckets touched.

Usage:
  python ann_eval_proto.py --n 8000 --q 50 --k-list 8,32,128,256,512
"""
import argparse, json, sys, time, math
import numpy as np

IDX = "I:/tools/ann/data/memcore/ann_index.jsonl"

def load_sample(n, seed=1234):
    rng = np.random.default_rng(seed)
    vecs, keep = [], []
    # reservoir-ish: read in order but stride to spread across the file
    # first pass: count
    with open(IDX, "r", encoding="utf-8", errors="replace") as f:
        # reservoir sampling of raw lines
        res = []
        for i, line in enumerate(f):
            if i < n:
                res.append(line)
            else:
                j = rng.integers(0, i + 1)
                if j < n:
                    res[j] = line
    for line in res:
        try:
            o = json.loads(line)
            v = o.get("vec")
            if v and len(v) == 384:
                vecs.append(v)
        except Exception:
            pass
    X = np.asarray(vecs, dtype=np.float32)
    # cosine: L2-normalize (e5 vectors are near-unit already)
    nrm = np.linalg.norm(X, axis=1, keepdims=True)
    nrm[nrm == 0] = 1.0
    return X / nrm

def kmeans(X, K, iters=25, seed=0):
    rng = np.random.default_rng(seed)
    n = X.shape[0]
    idx = rng.choice(n, size=K, replace=False)
    C = X[idx].copy()
    for _ in range(iters):
        # assign (n,K) cosine sim = dot since unit-norm
        sims = X @ C.T
        a = np.argmax(sims, axis=1)
        newC = np.zeros_like(C)
        for k in range(K):
            m = a == k
            if m.any():
                newC[k] = X[m].mean(axis=0)
            else:
                newC[k] = C[k]
        # renormalize centroids
        nn = np.linalg.norm(newC, axis=1, keepdims=True); nn[nn==0]=1
        newC = newC / nn
        if np.allclose(newC, C, atol=1e-6):
            C = newC; break
        C = newC
    sims = X @ C.T
    order = np.argsort(-sims, axis=1)          # top-2 assignment
    return C, order

def eval_ivf(X, Q, oracle, C, assign_order, topb):
    """assign_order: (n, K) descending centroid sims. Route query to topb
    nearest centroids; score union of their members; recall@10 vs oracle."""
    qsims = Q @ C.T
    qtop = np.argsort(-qsims, axis=1)[:, :topb]
    K = C.shape[0]
    # bucket members
    members = [[] for _ in range(K)]
    a1 = assign_order[:, 0]
    for i in range(X.shape[0]):
        members[a1[i]].append(i)
    recalls, fracs = [], []
    for qi in range(Q.shape[0]):
        cand = np.unique(np.concatenate([members[b] for b in qtop[qi]]))
        if cand.size == 0:
            recalls.append(0.0); fracs.append(0.0); continue
        s = X[cand] @ Q[qi]
        top = cand[np.argsort(-s)[:10]]
        hit = len(set(top.tolist()) & set(oracle[qi].tolist()))
        recalls.append(hit / 10.0)
        fracs.append(cand.size / X.shape[0])
    return float(np.mean(recalls)), float(np.mean(fracs))

def eval_quadtree_2d(X, Q, oracle, grid):
    """The user's idea, tested honestly: project to 2-D (PCA), build a grid
    (quadtree-ish cells = `grid`x`grid`), route each query to the nearest cell
    and its 8 neighbours (a 3x3 window = 'local'), score those members.
    This is the BEST case for 2-D geometric routing (we even let it look at
    neighbours for free). Measures whether 2-D locality preserves 384-dim
    neighbours at all."""
    # PCA to 2-D
    mu = X.mean(axis=0, keepdims=True)
    Xc = X - mu
    # top-2 principal directions via SVD on a subsample for speed
    sub = Xc[::max(1, X.shape[0] // 2000)]
    U, S, Vt = np.linalg.svd(sub, full_matrices=False)
    P = Vt[:2].T                                  # (384,2)
    X2 = Xc @ P
    Q2 = (Q - mu) @ P
    # normalize each axis to [0,1) over data range
    lo = X2.min(axis=0); hi = X2.max(axis=0)
    rng = np.where(hi - lo == 0, 1, hi - lo)
    def cell_of(A):
        t = (A - lo) / rng
        c = np.clip((t * grid).astype(int), 0, grid - 1)
        return c
    Xcell = cell_of(X2)
    buckets = {}
    for i in range(X.shape[0]):
        buckets.setdefault((Xcell[i,0], Xcell[i,1]), []).append(i)
    Qcell = cell_of(Q2)
    recalls, fracs = [], []
    for qi in range(Q.shape[0]):
        cx, cy = Qcell[qi]
        cand = []
        for dx in (-1,0,1):
            for dy in (-1,0,1):
                cand.extend(buckets.get((cx+dx, cy+dy), []))
        cand = np.array(sorted(set(cand)))
        if cand.size == 0:
            recalls.append(0.0); fracs.append(0.0); continue
        s = X[cand] @ Q[qi]
        top = cand[np.argsort(-s)[:10]]
        hit = len(set(top.tolist()) & set(oracle[qi].tolist()))
        recalls.append(hit / 10.0); fracs.append(cand.size / X.shape[0])
    return float(np.mean(recalls)), float(np.mean(fracs))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=8000)
    ap.add_argument("--q", type=int, default=50)
    ap.add_argument("--k-list", type=str, default="8,32,128,256,512")
    ap.add_argument("--seed", type=int, default=7)
    args = ap.parse_args()

    t0 = time.time()
    print(f"loading {args.n} vectors from {IDX} ...", flush=True)
    X = load_sample(args.n, seed=args.seed)
    print(f"  loaded {X.shape} in {time.time()-t0:.1f}s", flush=True)
    n = X.shape[0]

    rng = np.random.default_rng(args.seed)
    qi = rng.choice(n, size=args.q, replace=False)
    Q = X[qi]                                    # queries = real vectors
    # oracle: exact top-10 for each query over the whole sample
    orc = []
    for q in Q:
        s = X @ q
        orc.append(np.argsort(-s)[:10])
    print(f"oracle computed ({args.q} queries)", flush=True)

    print("\n=== IVF (kmeans buckets) recall@10 vs fraction scanned ===")
    print(f"{'K':>5} {'topb':>5} {'recall@10':>10} {'frac_scanned':>13}")
    Ks = [int(x) for x in args.k_list.split(",")]
    ivf_rows = []
    for K in Ks:
        if K >= n: continue
        C, ao = kmeans(X, K, seed=args.seed)
        for topb in (1, 2):
            if topb > K: continue
            r, f = eval_ivf(X, Q, orc, C, ao, topb)
            ivf_rows.append((K, topb, r, f))
            print(f"{K:>5} {topb:>5} {r:>10.3f} {f:>13.4f}", flush=True)

    print("\n=== 2-D geometric routing (PCA + grid, 3x3 window) — the user's idea ===")
    print(f"{'grid':>5} {'cells':>7} {'recall@10':>10} {'frac_scanned':>13}")
    qt_rows = []
    for g in (4, 8, 16, 32, 64):
        r, f = eval_quadtree_2d(X, Q, orc, g)
        qt_rows.append((g, r, f))
        print(f"{g:>5} {g*g:>7} {r:>10.3f} {f:>13.4f}", flush=True)

    print("\n=== VERDICT ===")
    # best IVF row with frac_scanned <= 0.30
    best = [row for row in ivf_rows if row[3] <= 0.30]
    if best:
        b = max(best, key=lambda r: r[2])
        print(f"best IVF @ <=30% scanned: K={b[0]} topb={b[1]} recall={b[2]:.3f} frac={b[3]:.3f}")
    # 2D at similar scan fraction
    for g, r, f in qt_rows:
        if f <= 0.30:
            print(f"2-D PCA grid={g} @ frac={f:.3f}: recall={r:.3f}")
    print("\nNOTE: recall@10 near 1.0 only if frac_scanned is also near 1.0 "
          "=> that means NO pruning (the B1 wall).")

if __name__ == "__main__":
    main()
