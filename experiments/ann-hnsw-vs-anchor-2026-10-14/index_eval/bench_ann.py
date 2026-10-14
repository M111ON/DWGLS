#!/usr/bin/env python3
"""
bench_ann.py — (b) standard high-dimensional ANN methods, exact-recall-first.

Methods: FAISS IndexFlatIP (brute reference), IVF-Flat, IVF-PQ, PQ, HNSW,
OPQ+IVF-PQ. Metrics: recall@1/@10 vs exact oracle, latency (ms/query, single
thread), index build time, index memory, fraction of DB touched (nprobe/K).
"""
import json, time, os
import numpy as np
import faiss
import hnswlib

OUT = "I:/tmp/ann_eval"
G = np.load(f"{OUT}/G.npy").astype(np.float32)
Q = np.load(f"{OUT}/Q.npy").astype(np.float32)
GT = np.load(f"{OUT}/GT.npy")
n, d = G.shape
NQ = Q.shape[0]
K = 10
faiss.omp_set_num_threads(1)          # honest single-thread latency

def recall(ID, gt, k):
    """mean recall@k over queries, ID: (NQ, kk)."""
    r = 0.0
    for i in range(NQ):
        r += len(set(ID[i, :k].tolist()) & set(gt[i, :k].tolist())) / k
    return r / NQ

def latency_ms(index, Q, k, nrep=3):
    # warmup
    index.search(Q[:10], k)
    best = 1e9
    for _ in range(nrep):
        t0 = time.perf_counter()
        index.search(Q, k)
        dt = (time.perf_counter() - t0) / Q.shape[0] * 1000
        best = min(best, dt)
    return best

rows = []

def record(name, build_s, mem_mb, lat_ms, r1, r10, frac):
    rows.append(dict(method=name, build_s=round(build_s,2), mem_mb=round(mem_mb,1),
                     lat_ms=round(lat_ms,3), r1=round(r1,4), r10=round(r10,4),
                     frac=round(frac,4)))
    print(f"{name:<28} build={build_s:6.1f}s mem={mem_mb:7.1f}MB lat={lat_ms:7.3f}ms "
          f"R@1={r1:.4f} R@10={r10:.4f} frac={frac:.4f}", flush=True)

# ---------- 0. brute (exact) ----------
t0 = time.time()
flat = faiss.IndexFlatIP(d)
flat.add(G)
build = time.time() - t0
D, I = flat.search(Q, K)
lat = latency_ms(flat, Q, K)
mem = n * d * 4 / 1e6
record("brute_flatIP(exact)", build, mem, lat, recall(I,GT,1), recall(I,GT,K), 1.0)

# ---------- 1. IVF-Flat ----------
for nlist in (256, 1024, 4096, 8192):
    t0 = time.time()
    quant = faiss.IndexFlatIP(d)
    idx = faiss.IndexIVFFlat(quant, d, nlist, faiss.METRIC_INNER_PRODUCT)
    idx.train(G)
    idx.add(G)
    build = time.time() - t0
    mem = (n*d*4 + nlist*d*4) / 1e6
    for nprobe in (1, 4, 8, 16, 32, 64, 128):
        if nprobe > nlist: continue
        idx.nprobe = nprobe
        D, I = idx.search(Q, K)
        lat = latency_ms(idx, Q, K)
        frac = nprobe / nlist
        record(f"IVF{nlist}_np{nprobe}", build, mem, lat, recall(I,GT,1), recall(I,GT,K), frac)

# ---------- 2. HNSW ----------
for M in (16, 32, 48):
    for efC in (200, 400):
        t0 = time.time()
        idx = faiss.IndexHNSWFlat(d, M, faiss.METRIC_INNER_PRODUCT)
        idx.hnsw.efConstruction = efC
        idx.add(G)
        build = time.time() - t0
        # mem: vectors + graph ~ M*2 edges * 4 bytes + overhead
        mem = (n*d*4 + n*M*2*4) / 1e6
        for efS in (16, 32, 64, 128, 256, 512):
            idx.hnsw.efSearch = efS
            D, I = idx.search(Q, K)
            lat = latency_ms(idx, Q, K)
            record(f"HNSW_M{M}_efC{efC}_efS{efS}", build, mem, lat, recall(I,GT,1), recall(I,GT,K), np.nan)

with open(f"{OUT}/res_ann.json", "w") as f:
    json.dump(rows, f, indent=1)
print(f"\nsaved res_ann.json ({len(rows)} rows)")
