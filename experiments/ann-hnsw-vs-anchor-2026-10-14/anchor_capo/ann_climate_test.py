#!/usr/bin/env python3
"""Why does HNSW die under frequent rebuild? Test the CLIMATE workload, not append.

The earlier test measured pure APPEND (new vectors to a static index) -> cheap.
The project's finding (ANN-CLIMATE-CAMPAIGN §6) is about CLIMATE: the
distribution shifts every ~12h and old data is deleted (session lifecycle), so
you must RE-DERIVE the index. Test both failure modes:

  A) incremental insert of a SHIFTED distribution -> recall vs rebuild
  B) DELETE (session expiry) -> does mark-delete degrade the graph?
  C) the break-even: rebuild cost vs query savings
"""
import numpy as np, time
import hnswlib

X = np.load("I:/tmp/ann_eval/X.npy")   # 61241 x 384 unit-norm
N, D = X.shape
rng = np.random.default_rng(5)

def recall(p, Q, orc):
    r = []
    for i, q in enumerate(Q):
        lab, _ = p.knn_query(q, k=10)
        r.append(len(set(lab[0].tolist()) & orc[i]) / 10.0)
    return np.mean(r)

# ---- CLIMATE simulation: 5 slices, each a shifted sub-distribution ----
# Slice = a contiguous block (different topic mix = shifted distribution)
NS = 5
sl = N // NS
slices = [X[i*sl:(i+1)*sl] for i in range(NS)]
print(f"climate: {NS} slices of ~{sl} vectors each\n")

def make_oracle(data, nq=150):
    qi = rng.choice(data.shape[0], size=nq, replace=False)
    Q = data[qi]
    orc = [set(np.argsort(-(data @ q))[:10].tolist()) for q in Q]
    return Q, orc

# ============ A) incremental insert vs rebuild under climate ============
print("=== A) CLIMATE: incremental insert vs full rebuild ===")
print(f"{'slice':>5} {'incr_recall':>12} {'incr_s':>8} | {'rebuild_recall':>15} {'rebuild_s':>10}")
p_incr = hnswlib.Index(space='cosine', dim=D)
p_incr.init_index(max_elements=N, ef_construction=200, M=16)
p_incr.set_ef(64)
incr_data = None
t_incr_total = 0.0
t_rebuild_total = 0.0
for s in range(NS):
    cur = np.vstack(slices[:s+1])
    if s == 0:
        t = time.time(); p_incr.add_items(slices[0], np.arange(sl)); dt = time.time()-t
    else:
        base = s*sl
        t = time.time(); p_incr.add_items(slices[s], np.arange(base, base+sl)); dt = time.time()-t
    t_incr_total += dt
    Q, orc = make_oracle(cur)
    ri = recall(p_incr, Q, orc)
    # rebuild from scratch
    pr = hnswlib.Index(space='cosine', dim=D)
    pr.init_index(max_elements=cur.shape[0], ef_construction=200, M=16)
    t = time.time(); pr.add_items(cur, np.arange(cur.shape[0])); dtr = time.time()-t
    t_rebuild_total += dtr
    pr.set_ef(64)
    rr = recall(pr, Q, orc)
    print(f"{s:>5} {ri:>12.4f} {dt:>8.2f} | {rr:>15.4f} {dtr:>10.2f}")

print(f"\n  total incremental insert: {t_incr_total:.1f}s")
print(f"  total full rebuild      : {t_rebuild_total:.1f}s")
print(f"  rebuild/insert ratio    : {t_rebuild_total/max(t_incr_total,1e-6):.1f}x")

# ============ B) DELETE (session lifecycle) ============
print("\n=== B) DELETE (mark-delete) — session lifecycle ===")
p = hnswlib.Index(space='cosine', dim=D)
p.init_index(max_elements=N, ef_construction=200, M=16)
p.add_items(X, np.arange(N))
p.set_ef(64)
Q, orc = make_oracle(X)
r_before = recall(p, Q, orc)
# delete 20% (old sessions expire)
del_ids = rng.choice(N, size=N//5, replace=False)
t = time.time()
p.mark_deleted(del_ids)
dt_del = time.time()-t
# recall measured only against surviving items (rebuild oracle without deleted)
keep = np.setdiff1d(np.arange(N), del_ids)
Xk = X[keep]
orc_k = []
for q in Q:
    s = Xk @ q
    orc_k.append(set(np.argsort(-s)[:10].tolist()))
r_after = []
for i, q in enumerate(Q):
    lab, _ = p.knn_query(q, k=10)
    r_after.append(len(set(lab[0].tolist()) & orc_k[i]) / 10.0)
print(f"  recall@10 before delete : {r_before:.4f}")
print(f"  recall@10 after 20% del : {np.mean(r_after):.4f}")
print(f"  mark_deleted cost       : {dt_del*1000:.1f} ms (ids stay in graph, holes)")
print(f"  (HNSW has NO re-link: deleted nodes leave holes; graph quality degrades)")
print(f"  a clean index would need a REBUILD (faiss/hnswlib cannot compact in place)")

# ============ C) break-even ============
print("\n=== C) break-even (project's math, §6) ===")
# measure query savings: HNSW vs brute
t = time.time()
for q in Q[:100]: np.argsort(-(X @ q))[:10]
brute_ms = (time.time()-t)/100*1000
p.set_ef(64)
t = time.time()
for q in Q[:100]: p.knn_query(q, k=10)
hnsw_ms = (time.time()-t)/100*1000
saved = brute_ms - hnsw_ms
print(f"  brute {brute_ms:.2f} ms/q  HNSW {hnsw_ms:.2f} ms/q  saved {saved:.2f} ms/q")
rebuild_s = 85.7   # measured earlier at 61k; project says ~356s at 1M
print(f"  rebuild (61k, measured) = {rebuild_s:.1f} s = {rebuild_s*1000:.0f} ms")
print(f"  break-even = {rebuild_s*1000/saved:.0f} queries/rebuild")
print(f"  => below this many queries between rebuilds, HNSW LOSES")
print(f"     the project measured ~6,800 queries/rebuild at 1M (356s / 0.052ms)")
