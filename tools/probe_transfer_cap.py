"""tools/probe_transfer_cap.py — ITEM 7.3 transfer proof (proper method).

PURPOSE
Every number in the FORAGE-GATE round (baseline, cap curve, OVERLAP) came
from ONE density: the 8x16 hierarchy config with 2560 used buckets over 1M
vectors = nent mean 390 per bucket. The real field is 20736 slots, so
entries-per-bucket there is a different number. Item 7.3 asks: does the
per-bucket cap curve SHAPE transfer, or is cap=512 covering everything a
SIFT1M artifact?

WHY THIS METHOD (not subsampling)
Subsampling base vectors loses ground-truth neighbours, so recall falls for
the wrong reason (probe_transfer_proof.c did this -- f<1 drops recall to
0.04 and conflates density with GT loss). The clean variable is the
hierarchy scan width: keeping base, query, GT, PCA and coarse centroids
fixed, the (topC, topF) routing config changes how many members land in the
scan -- 4x8 scans 3535 members, 8x32 scans 13887, a 4x increase in
entries-per-bucket with GT untouched.

METHOD
For each routing config (a,b) in the 5 from hier2_artifacts.py, and each cap
in {0(unbounded),32,64,128,256,512,1024,2048}, run the shipped route (top-a
coarse -> top-b fine) and scan members in the routed buckets, stopping each
bucket at `cap` members. Report recall@10 and members scored. The transfer
criterion: the smallest cap whose recall is within 0.005 of that config's
unbounded recall should track that config's entries-per-bucket, not sit at a
constant 512.

RUN (repo root): python tools/probe_transfer_cap.py
"""
import struct, time

OUT = 'I:/DWGLS-native-fs/build/sift1m_hier/'
D = 'I:/DWGLS-native-fs/build/sift1m/sift/'


def read_fvecs(p):
    with open(p, 'rb') as f:
        d = f.read()
    off, n, rows = 0, len(d), []
    while off < n:
        (dim,) = struct.unpack('<i', d[off:off + 4])
        rows.append(np.frombuffer(d[off + 4:off + 4 + dim * 4], dtype=np.float32).copy())
        off += 4 + dim * 4
    return np.array(rows)


def read_ivecs(p):
    with open(p, 'rb') as f:
        d = f.read()
    off, n, rows = 0, len(d), []
    while off < n:
        (dim,) = struct.unpack('<i', d[off:off + 4])
        rows.append(np.frombuffer(d[off + 4:off + 4 + dim * 4], dtype=np.int32).copy())
        off += 4 + dim * 4
    return np.array(rows)


import numpy as np  # noqa: E402

t0 = time.perf_counter()
B = read_fvecs(D + 'sift_base.fvecs')
Q = read_fvecs(D + 'sift_query.fvecs')
GT = read_ivecs(D + 'sift_groundtruth.ivecs')
n = len(B)
NQ = 1000
Q, GT = Q[:NQ], GT[:NQ]
print(f'loaded base {B.shape} query {Q.shape} in {time.perf_counter()-t0:.0f}s', flush=True)

# reuse the trained artifacts exactly (same files F.bin/off.bin/mem.bin came from)
pca_comp = np.load(OUT + 'pca_comp.npy')
pca_mean = np.load(OUT + 'pca_mean.npy')
C1c = np.load(OUT + 'C1.npy')
fine_npz = np.load(OUT + 'fine_cent.npz')
fm_rows = np.load(OUT + 'fine_members.npy')  # rows (coarse, fine, member_idx)
print(f'fine_members rows={len(fm_rows)}', flush=True)

Zq = (Q.astype(np.float64) - pca_mean) @ pca_comp.T

# rebuild per-coarse fine-centroid list and per-(c,j) member arrays from fm_rows
C1 = C1c.shape[0]
fcent = [[] for _ in range(C1)]
fmem = [[] for _ in range(C1)]
for c, j, v in fm_rows:
    if j >= len(fcent[c]):
        fcent[c].append(None)
    while len(fcent[c]) <= j:
        fcent[c].append(None)
# member arrays
tmp = {}
for c, j, v in fm_rows:
    tmp.setdefault((c, j), []).append(int(v))
for c in range(C1):
    key = f'c{c:03d}'
    cent = fine_npz[key]
    fmem[c] = [np.array(tmp[(c, j)], dtype=np.int64) if (c, j) in tmp else np.array([], dtype=np.int64)
               for j in range(cent.shape[0])]
    for j in range(cent.shape[0]):
        fcent[c][j] = cent[j]
print('rebuilt fcent/fmem', flush=True)


def eval_cap(a, b, cap):
    """Ship route: top-a coarse, top-b fine, scan routed members with per-bucket cap."""
    r = 0.0
    sc = 0
    for qi in range(NQ):
        dd1 = ((C1c - Zq[qi]) ** 2).sum(axis=1)
        top_c = np.argpartition(dd1, a)[:a]
        fcs, owners = [], []
        for c in top_c:
            for j, fc in enumerate(fcent[c]):
                if fc is None:
                    continue
                fcs.append(fc)
                owners.append((c, j))
        if not fcs:
            continue
        fcs = np.array(fcs)
        dd2 = ((fcs - Zq[qi]) ** 2).sum(axis=1)
        take = min(b, len(dd2))
        sel = np.argpartition(dd2, take - 1)[:take]
        # candidates = union of routed buckets, each bucket capped at `cap`
        cand = set()
        for s in sel:
            c, j = owners[s]
            mem = fmem[c][j]
            if cap > 0 and len(mem) > cap:
                mem = mem[:cap]
            sc += len(mem)
            cand.update(mem.tolist())
        # recall against true top-10 (candidates are the ONLY thing ranked)
        hit = len(cand & set(int(x) for x in GT[qi][:10]))
        r += hit / 10.0
    return r / NQ, sc / NQ


configs = [(4, 8), (4, 16), (8, 16), (4, 32), (8, 32)]
caps = [0, 32, 64, 128, 256, 512, 1024, 2048]

print(f"\n{'cfg':>5} {'cap':>6} {'recall':>9} {'scored':>8}")
print('-' * 34)
summary = {}
for (a, b) in configs:
    unb = None
    rows = []
    for cap in caps:
        rec, sc = eval_cap(a, b, cap)
        if cap == 0:
            unb = rec
        rows.append((cap, rec, sc))
    # entries-per-bucket proxy = scored at unbounded / #buckets touched
    print(f'{a}x{b:>2} unbounded recall={unb:.4f} scored={rows[0][2]:.0f}')
    sat = None
    for cap, rec, sc in rows:
        flag = ''
        if sat is None and abs(rec - unb) <= 0.005 and sc <= rows[0][2]:
            sat = cap
            flag = '  <-- saturates'
        print(f'{a}x{b:>2} {cap:>6} {rec:>9.4f} {sc:>8.0f}{flag}')
    summary[f'{a}x{b}'] = {'unbounded_recall': round(unb, 4), 'scored': int(rows[0][2]), 'sat_cap': sat}
    print('-' * 34)

print('\nTRANSFER SUMMARY (config -> entries scanned -> smallest cap within 0.005):')
for k, v in summary.items():
    print(f"  {k:>5}: scored={v['scored']:>6}  sat_cap={v['sat_cap']}  recall={v['unbounded_recall']}")
print('done', flush=True)
