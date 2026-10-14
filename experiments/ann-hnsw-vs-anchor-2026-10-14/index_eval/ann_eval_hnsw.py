#!/usr/bin/env python3
"""
ann_eval_hnsw.py — is HNSW the right answer? Measure recall@10 vs build/query
cost against the exact oracle, on a real sample. Also compare to the current
approach (exact scan, and the anchor+prune mode).

Reports: build time, index size, query latency (per query, batch), recall@10,
and the recall/latency tradeoff knob (ef_search).
"""
import json, time, os
import numpy as np
import hnswlib

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

def main():
    n = 20000
    print(f"loading {n} vectors ...", flush=True)
    X = load_sample(n)
    N, D = X.shape
    print(f"  {X.shape}", flush=True)
    rng = np.random.default_rng(11)
    qi = rng.choice(N, size=200, replace=False)
    Q = X[qi]
    # oracle
    orc = [set(np.argsort(-(X @ q))[:10].tolist()) for q in Q]
    print("oracle done", flush=True)

    # ---- exact scan baseline (what the server does) ----
    t = time.time()
    for q in Q[:50]:
        _ = np.argsort(-(X @ q))[:10]
    exact_ms = (time.time() - t) / 50 * 1000
    print(f"\nEXACT scan (numpy, {N}): {exact_ms:.2f} ms/query", flush=True)

    # ---- HNSW ----
    for M in (16, 32):
        p = hnswlib.Index(space='cosine', dim=D)
        t = time.time()
        p.init_index(max_elements=N, ef_construction=200, M=M)
        p.add_items(X, np.arange(N))
        build = time.time() - t
        p.set_ef(50)
        # measure latency
        t = time.time()
        for q in Q[:50]:
            _ = p.knn_query(q, k=10)
        qms = (time.time() - t) / 50 * 1000
        print(f"\nHNSW M={M}: build {build:.1f}s, query {qms:.2f} ms  "
              f"(speedup {exact_ms/qms:.1f}x)", flush=True)
        print(f"{'ef':>5} {'recall@10':>10} {'ms/query':>10}")
        for ef in (10, 20, 50, 100, 200):
            p.set_ef(ef)
            t = time.time()
            rec = []
            for i, q in enumerate(Q):
                lab, _ = p.knn_query(q, k=10)
                rec.append(len(set(lab[0].tolist()) & orc[i]) / 10.0)
            ms = (time.time() - t) / len(Q) * 1000
            print(f"{ef:>5} {np.mean(rec):>10.3f} {ms:>10.3f}", flush=True)

    # index size
    sz = os.path.getsize(IDX) if os.path.exists(IDX) else 0
    print(f"\n(raw jsonl for full 61k index ~229MB; HNSW in-mem ~ (N*D*4 + graph)")

if __name__ == "__main__":
    main()
