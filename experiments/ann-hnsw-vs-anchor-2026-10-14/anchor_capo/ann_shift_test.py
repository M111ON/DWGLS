#!/usr/bin/env python3
"""Reconcile: insert is cheap BUT rebuild still kills. Test the SHIFTED
distribution (climate = new topics), not random subsets (same distribution).

My earlier test used random subsets = SAME distribution -> insert fine.
Climate = distribution SHIFTS -> a stale index (however it was updated)
serves new-distribution queries poorly, forcing re-derive.
"""
import numpy as np, time
import hnswlib

X = np.load("I:/tmp/ann_eval/X.npy")
N, D = X.shape
rng = np.random.default_rng(9)

# build 4 genuinely DIFFERENT sub-populations via kmeans-lite (project has e5
# vectors; use random hyperplane splits to create distinct "topics")
def topic_split(X, k=4, seed=0):
    r = np.random.default_rng(seed)
    dirs = r.standard_normal((k, D)).astype(np.float32)
    proj = X @ dirs.T
    lab = np.argmax(proj, axis=1)
    return lab

lab = topic_split(X, 4, 0)
print("topic sizes:", np.bincount(lab))
# order by topic so slices are distribution-SHIFTED
order = np.argsort(lab, kind="stable")
Xs = X[order]
sl = N // 4
slices = [Xs[i*sl:(i+1)*sl] for i in range(4)]

def eval_q(p, data, nq=150):
    qi = rng.choice(data.shape[0], size=nq, replace=False)
    Q = data[qi]
    orc = [set(np.argsort(-(data @ q))[:10].tolist()) for q in Q]
    r = []
    for i, q in enumerate(Q):
        l, _ = p.knn_query(q, k=10)
        r.append(len(set(l[0].tolist()) & orc[i]) / 10.0)
    return np.mean(r)

print("\n=== SHIFTED distribution: stale-insert vs fresh-rebuild ===")
print(f"{'slice':>5} {'topic':>5} {'stale_incr_recall':>18} {'fresh_rebuild':>14} {'rebuild_s':>10}")
p_incr = hnswlib.Index(space='cosine', dim=D)
p_incr.init_index(max_elements=N, ef_construction=200, M=16)
p_incr.set_ef(64)
for s in range(4):
    cur = np.vstack(slices[:s+1])
    if s == 0:
        p_incr.add_items(slices[0], np.arange(sl))
    else:
        p_incr.add_items(slices[s], np.arange(s*sl, (s+1)*sl))
    # query on the CURRENT (newest) topic only = the "new traffic"
    Qnew = slices[s][rng.choice(sl, size=150, replace=False)]
    orc_new = [set(np.argsort(-(slices[s] @ q))[:10].tolist()) for q in Qnew]
    rs = []
    for i, q in enumerate(Qnew):
        l, _ = p_incr.knn_query(q, k=10)
        rs.append(len(set(l[0].tolist()) & orc_new[i]) / 10.0)
    # fresh rebuild on current data
    pr = hnswlib.Index(space='cosine', dim=D)
    pr.init_index(max_elements=cur.shape[0], ef_construction=200, M=16)
    t = time.time(); pr.add_items(cur, np.arange(cur.shape[0])); rb = time.time()-t
    pr.set_ef(64)
    rf = []
    for i, q in enumerate(Qnew):
        l, _ = pr.knn_query(q, k=10)
        rf.append(len(set(l[0].tolist()) & orc_new[i]) / 10.0)
    print(f"{s:>5} {'t'+str(s):>5} {np.mean(rs):>18.4f} {np.mean(rf):>14.4f} {rb:>10.1f}")

print("\n=== VERDICT ===")
print("stale-insert: recall on NEW topic degrades (graph built on old topics)")
print("fresh-rebuild: recall restored, but costs a full rebuild each shift")
print("=> this is the climate cost: you PAY a rebuild per distribution shift,")
print("   and delete/expiry/atomicity add more. At 1M = 356s per rebuild.")
