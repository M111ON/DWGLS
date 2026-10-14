#!/usr/bin/env python3
"""
bench_pq.py — compression + two-stage (prefilter -> exact rerank) methods.
  * PQ / OPQ+IVF-PQ : product quantization
  * IVF+SQ8         : scalar quantized
  * binary-sign prefilter + exact rerank (Hamming over 384-bit codes)
  * PCA-truncate prefilter + exact rerank
All measured against the exact cosine oracle with single-thread latency.
"""
import json, time
import numpy as np
import faiss

OUT = "I:/tmp/ann_eval"
G = np.load(f"{OUT}/G.npy").astype(np.float32)
Q = np.load(f"{OUT}/Q.npy").astype(np.float32)
GT = np.load(f"{OUT}/GT.npy")
n, d = G.shape
NQ = Q.shape[0]
K = 10
faiss.omp_set_num_threads(1)

def recall(ID, gt, k):
    return np.mean([len(set(ID[i,:k].tolist()) & set(gt[i,:k].tolist()))/k for i in range(NQ)])

def latency_ms(fn, nrep=3):
    fn()
    best = 1e9
    for _ in range(nrep):
        t0 = time.perf_counter(); fn(); best = min(best, (time.perf_counter()-t0)/NQ*1000)
    return best

rows = []
def record(name, build, mem, lat, r1, r10, note=""):
    rows.append(dict(method=name, build_s=round(build,2), mem_mb=round(mem,1),
                     lat_ms=round(lat,3), r1=round(float(r1),4), r10=round(float(r10),4), note=note))
    print(f"{name:<30} build={build:6.1f}s mem={mem:7.1f}MB lat={lat:7.3f}ms R@1={r1:.4f} R@10={r10:.4f} {note}", flush=True)

# ---------- PQ (no IVF, exhaustive ADC) ----------
for m in (48, 96, 192):
    for nbits in (8,):
        t0=time.time()
        pq = faiss.IndexPQ(d, m, nbits, faiss.METRIC_INNER_PRODUCT)
        pq.train(G); pq.add(G); build=time.time()-t0
        mem = n*(m*nbits/8)/1e6
        D,I = pq.search(Q,K)
        lat = latency_ms(lambda: pq.search(Q,K))
        record(f"PQ_m{m}_b{nbits}", build, mem, lat, recall(I,GT,1), recall(I,GT,K))

# ---------- OPQ + IVF-PQ ----------
for nlist in (1024, 4096):
    for m in (48, 96):
        t0=time.time()
        opq = faiss.OPQMatrix(d, m)
        opq.train(G)
        Gq = opq.apply_py(G)
        Qq = opq.apply_py(Q)
        quant = faiss.IndexFlatIP(m)
        idx = faiss.IndexIVFPQ(quant, m, nlist, m, 8, faiss.METRIC_INNER_PRODUCT)
        idx.train(Gq); idx.add(Gq); build=time.time()-t0
        mem = (n*m*8/8 + nlist*m*4)/1e6
        for nprobe in (16, 64, 128):
            idx.nprobe=nprobe
            D,I = idx.search(Qq,K)
            lat = latency_ms(lambda: idx.search(Qq,K))
            record(f"OPQ{m}_IVF{nlist}_np{nprobe}", build, mem, lat, recall(I,GT,1), recall(I,GT,K))

# ---------- IVF + SQ8 ----------
for nlist in (1024, 4096):
    t0=time.time()
    quant = faiss.IndexFlatIP(d)
    idx = faiss.IndexIVFScalarQuantizer(quant, d, nlist, faiss.ScalarQuantizer.QT_8bit, faiss.METRIC_INNER_PRODUCT)
    idx.train(G); idx.add(G); build=time.time()-t0
    mem = (n*d*1 + nlist*d*4)/1e6
    for nprobe in (16, 64, 128):
        idx.nprobe=nprobe
        D,I = idx.search(Q,K)
        lat = latency_ms(lambda: idx.search(Q,K))
        record(f"IVF{nlist}_SQ8_np{nprobe}", build, mem, lat, recall(I,GT,1), recall(I,GT,K))

# ---------- Two-stage: binary sign codes (Hamming) prefilter -> exact rerank ----------
def bench_binary(cand_frac_list):
    codes = np.packbits((G > 0).astype(np.uint8), axis=1)   # (n, 48) uint8
    qcodes = np.packbits((Q > 0).astype(np.uint8), axis=1)
    # popcount table
    pop = np.unpackbits(np.arange(256, dtype=np.uint8)[:,None], axis=1).sum(1).astype(np.uint8)
    mem = codes.nbytes/1e6
    for frac in cand_frac_list:
        nprobe = max(K, int(frac*n))
        def run():
            outs = np.empty((NQ,K), dtype=np.int64)
            for i in range(NQ):
                x = np.bitwise_xor(codes, qcodes[i])
                ham = pop[x].sum(1)
                cand = np.argpartition(ham, nprobe)[:nprobe]
                s = G[cand] @ Q[i]
                outs[i] = cand[np.argpartition(-s,K)[:K]]
            return outs
        t0=time.perf_counter(); I = run(); lat=(time.perf_counter()-t0)/NQ*1000
        record(f"BIN_prefilter_{frac:.3f}", 0.0, mem, lat, recall(I,GT,1), recall(I,GT,K))

bench_binary([0.01, 0.02, 0.05, 0.10, 0.20, 0.40])

# ---------- Two-stage: PCA-truncate prefilter -> exact rerank ----------
mu = G.mean(0, keepdims=True)
sub = (G-mu)[::max(1,n//8000)]
_,_,Vt = np.linalg.svd(sub, full_matrices=False)
for pdim in (64, 128, 192):
    P = Vt[:pdim].T
    Gt = (G-mu) @ P; Qt = (Q-mu) @ P
    for frac in (0.02, 0.05, 0.10, 0.20):
        nprobe = max(K, int(frac*n))
        def run():
            outs = np.empty((NQ,K), dtype=np.int64)
            for i in range(NQ):
                s = Gt @ Qt[i]
                cand = np.argpartition(-s, nprobe)[:nprobe]
                s2 = G[cand] @ Q[i]
                outs[i] = cand[np.argpartition(-s2,K)[:K]]
            return outs
        t0=time.perf_counter(); I = run(); lat=(time.perf_counter()-t0)/NQ*1000
        record(f"PCA{pdim}_prefilter_{frac:.2f}", 0.0, n*pdim*4/1e6, lat, recall(I,GT,1), recall(I,GT,K))

with open(f"{OUT}/res_pq.json","w") as f:
    json.dump(rows, f, indent=1)
print(f"\nsaved res_pq.json ({len(rows)} rows)")
