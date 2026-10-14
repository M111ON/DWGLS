#!/usr/bin/env python3
"""Full-scale (61k) HNSW vs exact, + memory. Decisive check."""
import json, time, os
import numpy as np
import hnswlib

IDX = "I:/tools/ann/data/memcore/ann_index.jsonl"

def load_all():
    vecs = []
    with open(IDX, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            try:
                o = json.loads(line); v = o.get("vec")
                if v and len(v) == 384: vecs.append(v)
            except Exception: pass
    X = np.asarray(vecs, dtype=np.float32)
    nrm = np.linalg.norm(X, axis=1, keepdims=True); nrm[nrm==0]=1
    return X / nrm

def main():
    t0=time.time()
    X = load_all()
    N, D = X.shape
    print(f"loaded {N} x {D} in {time.time()-t0:.1f}s  ({X.nbytes/1e6:.0f} MB in RAM)")
    rng = np.random.default_rng(3)
    qi = rng.choice(N, size=200, replace=False)
    Q = X[qi]
    print("computing oracle ...", flush=True)
    orc = [set(np.argsort(-(X @ q))[:10].tolist()) for q in Q]

    t=time.time()
    for q in Q[:30]: np.argsort(-(X @ q))[:10]
    exact=(time.time()-t)/30*1000
    print(f"\nEXACT numpy scan: {exact:.1f} ms/query  ({N} x {D})")

    p = hnswlib.Index(space='cosine', dim=D)
    t=time.time()
    p.init_index(max_elements=N, ef_construction=200, M=32)
    p.add_items(X, np.arange(N))
    print(f"HNSW build: {time.time()-t:.1f}s")
    p.set_ef(50)
    t=time.time()
    for q in Q[:30]: p.knn_query(q, k=10)
    qms=(time.time()-t)/30*1000
    print(f"HNSW query: {qms:.2f} ms  -> {exact/qms:.1f}x faster than exact")
    print(f"\n{'ef':>5} {'recall@10':>10} {'ms/query':>10} {'speedup':>8}")
    for ef in (10,20,50,100,200):
        p.set_ef(ef)
        t=time.time(); rec=[]
        for i,q in enumerate(Q):
            lab,_=p.knn_query(q,k=10)
            rec.append(len(set(lab[0].tolist())&orc[i])/10.0)
        ms=(time.time()-t)/len(Q)*1000
        print(f"{ef:>5} {np.mean(rec):>10.4f} {ms:>10.3f} {exact/ms:>7.0f}x")
    # memory estimate
    graph_bytes = N*32*2*4  # M*2 neighbors * 4 bytes (approx, both layers)
    print(f"\nmem: vectors {X.nbytes/1e6:.0f}MB + HNSW graph ~{graph_bytes/1e6:.0f}MB = ~{(X.nbytes+graph_bytes)/1e6:.0f}MB")
    print(f"(raw jsonl on disk: {os.path.getsize(IDX)/1e6:.0f}MB)")

if __name__=="__main__": main()
