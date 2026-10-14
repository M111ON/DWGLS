#!/usr/bin/env python3
"""
ann_eval_multiview.py — does multi-view (2 independent partitions) routing beat
a single partition at EQUAL scan fraction?

This is the DWGLS 'multi-view' idea (octfold/hosoya/limacon = several bijections
over the same carrier). Test: route with partition A (kmeans seed a) AND
partition B (kmeans seed b), then
  - UNION  : score members of A-topb1 OR B-topb1
  - INTERSECT: score members in BOTH A-topb1 and B-topb1 (smaller scan)
and compare recall@10 at matched scan fraction vs a single partition with the
same total scan budget.
"""
import json, time
import numpy as np

IDX = "I:/tools/ann/data/memcore/ann_index.jsonl"

def load_sample(n, seed=1234):
    rng = np.random.default_rng(seed)
    res = []
    with open(IDX, "r", encoding="utf-8", errors="replace") as f:
        for i, line in enumerate(f):
            if i < n: res.append(line)
            else:
                j = rng.integers(0, i + 1)
                if j < n: res[j] = line
    vecs = []
    for line in res:
        try:
            o = json.loads(line); v = o.get("vec")
            if v and len(v) == 384: vecs.append(v)
        except Exception: pass
    X = np.asarray(vecs, dtype=np.float32)
    nrm = np.linalg.norm(X, axis=1, keepdims=True); nrm[nrm==0]=1
    return X / nrm

def kmeans_assign(X, K, seed):
    rng = np.random.default_rng(seed)
    C = X[rng.choice(X.shape[0], size=K, replace=False)].copy()
    for _ in range(25):
        a = np.argmax(X @ C.T, axis=1)
        newC = C.copy()
        for k in range(K):
            m = a == k
            if m.any(): newC[k] = X[m].mean(axis=0)
        nn = np.linalg.norm(newC, axis=1, keepdims=True); nn[nn==0]=1
        newC = newC / nn
        if np.allclose(newC, C, atol=1e-6): C = newC; break
        C = newC
    return C

def buckets_of(X, C, topb=1):
    sims = X @ C.T
    order = np.argsort(-sims, axis=1)[:, :topb]
    members = [set() for _ in range(C.shape[0])]
    for i in range(X.shape[0]):
        for b in order[i]:
            members[b].add(i)
    return members, order

def route(Q, C, members, topb):
    qtop = np.argsort(-(Q @ C.T), axis=1)[:, :topb]
    out = []
    for qi in range(Q.shape[0]):
        s = set()
        for b in qtop[qi]:
            s |= members[b]
        out.append(s)
    return out

def main():
    n = 8000
    X = load_sample(n)
    rng = np.random.default_rng(7)
    qi = rng.choice(X.shape[0], size=60, replace=False)
    Q = X[qi]
    orc = [set(np.argsort(-(X @ q))[:10].tolist()) for q in Q]

    K = 16
    CA = kmeans_assign(X, K, 1)
    CB = kmeans_assign(X, K, 2)
    mA, _ = buckets_of(X, CA, 1)
    mB, _ = buckets_of(X, CB, 1)
    rA = route(Q, CA, mA, 1)
    rB = route(Q, CB, mB, 1)

    def score(cand, q):
        cand = np.array(sorted(cand))
        if cand.size == 0: return set()
        s = X[cand] @ q
        return set(cand[np.argsort(-s)[:10]].tolist())

    print(f"K={K}, single-partition (A) vs multi-view union/intersect")
    print(f"{'mode':>22} {'mean_recall@10':>15} {'mean_frac_scanned':>18}")
    # single A
    for label, getter in [
        ("single A (topb=1)", lambda i: rA[i]),
        ("single A (topb=2)", None),
        ("union A∪B (topb=1)", lambda i: rA[i] | rB[i]),
        ("intersect A∩B (topb=1)", lambda i: rA[i] & rB[i]),
    ]:
        if getter is None: continue
        rec, fr = [], []
        for i in range(Q.shape[0]):
            c = getter(i)
            top = score(c, Q[i])
            rec.append(len(top & orc[i]) / 10.0)
            fr.append(len(c) / X.shape[0])
        print(f"{label:>22} {np.mean(rec):>15.3f} {np.mean(fr):>18.4f}")

    # single A with topb=2 for a matched-budget comparison
    rA2 = route(Q, CA, mA, 2)
    rec, fr = [], []
    for i in range(Q.shape[0]):
        c = rA2[i]; top = score(c, Q[i])
        rec.append(len(top & orc[i]) / 10.0); fr.append(len(c) / X.shape[0])
    print(f"{'single A (topb=2)':>22} {np.mean(rec):>15.3f} {np.mean(fr):>18.4f}")

    print("\n=> Compare 'union A∪B' vs 'single A topb=2' at similar frac_scanned:")
    print("   if union recall <= single recall at same scan => multi-view adds nothing")

if __name__ == "__main__":
    main()
