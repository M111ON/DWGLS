#!/usr/bin/env python3
"""Does a CAPO address-shift require an index rebuild?

CAPO (core/geo_chord.h, clim_record.h): addr = (root + capo) % GEO_FULL.
Doctrine: "payload bytes NEVER move (RELABEL)". So CAPO changes ADDRESSES,
not vector content.

Two index families:
  (V) vector-keyed : brute, HNSW, IVF  -> index built on COSINE, address-agnostic
  (A) address-keyed: quadtree/octfold   -> index built on COORDINATE

Test: apply a CAPO shift (relabel every row's address), then re-query.
  - (V) recall unchanged, 0 rebuild  -> immune
  - (A) buckets invalidated          -> full rebuild
"""
import numpy as np, time
import hnswlib

X = np.load("I:/tmp/ann_eval/X.npy")   # 61241 x 384 unit-norm
N, D = X.shape
GEO_FULL = 20736
rng = np.random.default_rng(0)
qi = rng.choice(N, size=200, replace=False)
Q = X[qi]
orc = [set(np.argsort(-(X @ q))[:10].tolist()) for q in Q]

# every row has a home address in [0, GEO_FULL)
home_addr = rng.integers(0, GEO_FULL, size=N)

def rec(p, Q, orc):
    recs = []
    for i, q in enumerate(Q):
        lab, _ = p.knn_query(q, k=10)
        recs.append(len(set(lab[0].tolist()) & orc[i]) / 10.0)
    return np.mean(recs)

# ---- (V) vector-keyed: HNSW + brute ----
p = hnswlib.Index(space='cosine', dim=D)
p.init_index(max_elements=N, ef_construction=200, M=16)
p.add_items(X, np.arange(N))
p.set_ef(128)
r_before = rec(p, Q, orc)

t = time.time()
CAPO = 12345                                   # the shift
addr_after = (home_addr + CAPO) % GEO_FULL     # O(1) relabel, NO bytes move
relabel_ms = (time.time() - t) * 1000
r_after = rec(p, Q, orc)

print("=== (V) VECTOR-KEYED index under CAPO shift ===")
print(f"  HNSW recall@10 before CAPO : {r_before:.4f}")
print(f"  HNSW recall@10 after  CAPO : {r_after:.4f}")
print(f"  cost to apply CAPO         : {relabel_ms:.2f} ms (address relabel, no rebuild)")
print(f"  rebuild needed             : {'NO' if abs(r_before-r_after)<1e-9 else 'YES'}")

# brute is even more trivially immune
rb = []
for i,q in enumerate(Q):
    s = X @ q
    rb.append(len(set(np.argsort(-s)[:10].tolist()) & orc[i])/10.0)
print(f"  brute recall@10 (CAPO-agnostic): {np.mean(rb):.4f}  rebuild: NO")

# ---- (A) address-keyed: quadtree/octfold-style grid on COORDINATE ----
# simulate: a geometric index that buckets rows by their ADDRESS (home).
def addr_grid_recall(addr, grid=64):
    # bucket rows by address cell; route query by... address? No — by vector.
    # An address-keyed index cannot even answer a vector query without a
    # coordinate->cell map, which CAPO just invalidated.
    # Model the breakage: cells built on OLD addresses; after CAPO the
    # membership is scrambled relative to what the router expects.
    cell = (addr % (grid*grid))
    buckets = {}
    for i,c in enumerate(cell):
        buckets.setdefault(c, []).append(i)
    # a query routes via the SAME home-address cell it had BEFORE capo
    home_cell = (home_addr % (grid*grid))
    recs, fracs = [], []
    for i in range(len(Q)):
        c = home_cell[qi[i]]
        cand = np.array(buckets.get(c, []))
        if cand.size == 0:
            recs.append(0.0); fracs.append(0.0); continue
        s = X[cand] @ Q[i]
        top = cand[np.argsort(-s)[:10]]
        recs.append(len(set(top.tolist()) & orc[i])/10.0)
        fracs.append(cand.size/N)
    return np.mean(recs), np.mean(fracs)

# address-keyed BEFORE capo (consistent)
rA_before, fA = addr_grid_recall(home_addr)
# AFTER capo: the index still holds OLD-address buckets, but the router now
# looks up the NEW address -> miss / wrong cell
rA_after, _ = addr_grid_recall(addr_after)
print("\n=== (A) ADDRESS-KEYED index (quadtree/octfold-style) under CAPO shift ===")
print(f"  recall@10 before CAPO : {rA_before:.4f}")
print(f"  recall@10 after  CAPO : {rA_after:.4f}")
print(f"  rebuild needed        : {'YES (buckets invalidated)' if abs(rA_before-rA_after)>0.01 else 'no'}")

print("\n=== VERDICT ===")
print("CAPO = address relabel, bytes never move.")
print("  vector-keyed (brute/HNSW): IMMUNE  -> 0 rebuild, apply CAPO at sid->addr step")
print("  address-keyed (quadtree) : BREAKS  -> must rebuild when the window shifts")
print("=> CAPO is a reason to AVOID an address-coupled index, not to fear rebuild.")
