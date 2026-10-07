"""Is the pruning failure the ARCHITECTURE, or just poor centroids?

kmeans_prune.py used random-init k-means, 40 iters, no restarts, and measured
recall@8 at b=1 of only 0.28 for K=8 — the single nearest bucket holding under
a third of a query's true neighbours. That is suspiciously bad for embedding
space. Two readings:

  (a) the space is genuinely diffuse for this corpus/query set;
  (b) the centroids are bad (random init, few iters like ANCHR_ITERS=25).

This script re-fits with k-means++ seeding and restarts, and reports the
within-cluster cosine energy (fit quality) alongside the recall curve, so the
two readings can be told apart. If good centroids move b@r=1.0 down sharply,
the fix is the regrow quality, not the routing design.
"""
import json, math, urllib.request, numpy as np

X, SID = [], []
for line in open('build/kv_index.jsonl', encoding='utf-8'):
    d = json.loads(line)
    X.append(d['vec'])
    SID.append(d['sid'])
X = np.asarray(X, dtype=np.float64)
N, D = X.shape
Xn = X / np.linalg.norm(X, axis=1, keepdims=True)

QS = ['tesspack format spec', 'kv remap hybrid rail', 'geofs multivolume',
      'kiss timeline scale', 'moe expert routing', 'wang latch fog',
      'platonic field 20736', 'wang latch fog gate']


def embed(q):
    b = json.dumps({'content': q}).encode()
    r = urllib.request.Request('http://127.0.0.1:18095/embedding', data=b,
                               headers={'Content-Type': 'application/json'})
    e = json.loads(urllib.request.urlopen(r, timeout=30).read().decode())[0]['embedding']
    while e and isinstance(e[0], list):
        e = e[0]
    return e


Q = np.asarray([embed(q) for q in QS], dtype=np.float64)
Qn = Q / np.linalg.norm(Q, axis=1, keepdims=True)
SIM = Qn @ Xn.T
GT8 = [set(np.argsort(-SIM[i])[:8].tolist()) for i in range(len(QS))]
GT3 = [set(np.argsort(-SIM[i])[:3].tolist()) for i in range(len(QS))]


def kmeans_pp(Xn, K, restarts=10, iters=100, seed=0):
    """k-means++ seeding + several restarts, keep the best inertia."""
    rng = np.random.default_rng(seed)
    best_C, best_e = None, -np.inf
    for r in range(restarts):
        # k-means++ seed
        C = [Xn[rng.integers(len(Xn))]]
        d2 = 1.0 - Xn @ C[0]
        for _ in range(K - 1):
            p = np.clip(d2, 0, None)
            s = p.sum()
            if s <= 0:
                C.append(Xn[rng.integers(len(Xn))])
            else:
                C.append(Xn[rng.choice(len(Xn), p=p / s)])
            d2 = np.minimum(d2, 1.0 - Xn @ C[-1])
        C = np.asarray(C)
        for _ in range(iters):
            a = np.argmax(Xn @ C.T, axis=1)
            newC = C.copy()
            for k in range(K):
                m = a == k
                if m.any():
                    v = Xn[m].mean(0)
                    nn = np.linalg.norm(v)
                    if nn > 0:
                        newC[k] = v / nn
            if np.allclose(newC, C, atol=1e-9):
                break
            C = newC
        energy = float((Xn @ C.T).max(axis=1).mean())   # mean cosine to own centroid
        if energy > best_e:
            best_e, best_C = energy, C
    return best_C, best_e


print('index %d x %d, %d queries' % (N, D, len(QS)))
print()
print('%-5s %-7s %-9s %-10s %-9s %s'
      % ('K', 'fit', 'b@r8=1.0', 'b/N', 'b@r3=1.0', 'recall@8 b=1,2,4'))
for K in (8, 32, 64, 128):
    C, fit = kmeans_pp(Xn, K)
    assign = np.argmax(Xn @ C.T, axis=1)
    rank = np.argsort(-(Qn @ C.T), axis=1)
    curve, need8, need3 = [], None, None
    for b in range(1, K + 1):
        r8 = r3 = 0.0
        for i in range(len(QS)):
            inb = np.isin(assign, rank[i, :b])
            idx = np.where(inb)[0]
            if len(idx) == 0:
                continue
            order = idx[np.argsort(-SIM[i, idx])]
            sel8 = set(order[:8].tolist())
            r8 += len(sel8 & GT8[i]) / 8.0
            r3 += len(set(order[:3].tolist()) & GT3[i]) / 3.0
        r8 /= len(QS)
        r3 /= len(QS)
        curve.append(r8)
        if need8 is None and r8 >= 0.999:
            need8 = b
        if need3 is None and r3 >= 0.999:
            need3 = b
    print('%-5d %-7.3f %-9s %-10s %-9s %s'
          % (K, fit, str(need8), '%.3f' % (need8 / K) if need8 else 'never',
             str(need3),
             ' '.join('%.2f' % curve[min(c - 1, len(curve) - 1)] for c in (1, 2, 4))))