#!/usr/bin/env python3
"""tools/sift1m_prep.py — materialize the SIFT1M forage artifacts as raw .bin.

forage_real.c (item 3) walks the vector DB top->down and must NOT use the
pre-computed lab1/fine_members assignment (verified: fine_members col1 agrees
with a real walk only 9.96%, i.e. it is a different artifact). What it needs is
the raw centroid tables + the leaf slot for every row, all derived here from the
real sift_base.fvecs by walking (PCA-25 -> nearest coarse -> nearest fine ->
nearest L3 sub-leaf).

Outputs (all git-ignored, regenerate with this script):
  <hier>/_base_f32.npy      (1M,128) f32   base vectors unpacked from fvecs
  <hier>/_coarse_assign.npy (1M,)   i32    walk-derived coarse (== lab1, 100%)
  <hier>/_fine_assign.npy   (1M,)   i32    walk-derived fine index in coarse
  <hier>/_l3_assign.npy     (1M,)   i32    walk-derived L3 sub-leaf
  <hier>/fine_cent.bin      (256,10,25) f64   from fine_cent.npz
  <hier>/l3_cent.bin        (256,10,4,25) f64  mini-kmeans(4) inside each fine
  <hier>/leaf_slot3.bin     (1M,) i32   (coarse*10+fine)*4+l3

Usage: python tools/sift1m_prep.py [hier_dir] [sift_dir]
"""
import sys, os, json, time
import numpy as np

hier = sys.argv[1] if len(sys.argv) > 1 else "build/sift1m_hier"
sift = sys.argv[2] if len(sys.argv) > 2 else "build/sift1m/sift"
N, D, K3 = 1_000_000, 128, 4

print(f"[1/6] unpack base fvecs ({N} x {D})")
rec = 4 + D * 4
raw = np.fromfile(os.path.join(sift, "sift_base.fvecs"), dtype=np.uint8).reshape(N, rec)
base = raw[:, 4:].view(np.float32)
np.save(os.path.join(hier, "_base_f32.npy"), base)

C1 = np.load(os.path.join(hier, "C1.npy"))          # (256,25)
comp = np.load(os.path.join(hier, "pca_comp.npy"))
mean = np.load(os.path.join(hier, "pca_mean.npy"))
lab1 = np.load(os.path.join(hier, "lab1.npy"))

print("[2/6] PCA-25 and walk-derived coarse")
q = (base.astype(np.float64) - mean) @ comp.T
C1n = (C1 ** 2).sum(1)
assign = np.empty(N, dtype=np.int32)
t = time.time()
for i in range(0, N, 50_000):
    ch = q[i:i + 50_000]
    d = (ch ** 2).sum(1)[:, None] - 2 * ch @ C1.T + C1n[None, :]
    assign[i:i + 50_000] = d.argmin(1)
print(f"      coarse {time.time()-t:.1f}s, agree with lab1 = {float((assign==lab1).mean()):.4f}")
np.save(os.path.join(hier, "_coarse_assign.npy"), assign)

fc = np.stack([np.load(os.path.join(hier, "fine_cent.npz"))[f"c{c:03d}"] for c in range(256)])
fc.astype("<f8").tofile(os.path.join(hier, "fine_cent.bin"))

print("[3/6] walk-down to fine (10 per coarse)")
t = time.time(); fine = np.empty(N, dtype=np.int32)
order = np.argsort(assign, kind="stable")
starts = np.searchsorted(assign[order], np.arange(257))
for c in range(256):
    idx = order[starts[c]:starts[c + 1]]
    ch = q[idx]
    fine[idx] = ((ch[:, None, :] - fc[c][None, :, :]) ** 2).sum(2).argmin(1)
print(f"      fine {time.time()-t:.1f}s")
np.save(os.path.join(hier, "_fine_assign.npy"), fine)
mm = np.load(os.path.join(hier, "fine_members.npy"))
print(f"      agree with fine_members col1 = {float((fine==mm[:,1]).mean()):.4f}  (expected ~0.10: different artifact)")

print(f"[4/6] walk-down to L3 (mini-kmeans {K3} inside each fine)")
t = time.time(); l3 = np.empty(N, dtype=np.int32); l3c = np.zeros((256, 10, K3, 25))
slot2 = assign.astype(np.int64) * 10 + fine
so = np.argsort(slot2, kind="stable"); ss = np.searchsorted(slot2[so], np.arange(2561))
for s in range(2560):
    idx = so[ss[s]:ss[s + 1]]
    if len(idx) == 0:
        continue
    ch = q[idx]; k = min(K3, len(ch)); cen = ch[:k].copy()
    for _ in range(3):
        a = ((ch[:, None, :] - cen[None, :, :]) ** 2).sum(2).argmin(1)
        for j in range(k):
            m = a == j
            if m.any():
                cen[j] = ch[m].mean(0)
    for j in range(K3):
        l3c[s // 10, s % 10, j] = cen[min(j, k - 1)]
    l3[idx] = a
print(f"      L3 {time.time()-t:.1f}s")
np.save(os.path.join(hier, "_l3_assign.npy"), l3)
l3c.astype("<f8").tofile(os.path.join(hier, "l3_cent.bin"))

print("[5/6] leaf slots")
slot3 = slot2 * K3 + l3
slot3.astype("<i4").tofile(os.path.join(hier, "leaf_slot3.bin"))
print(f"      leaf_slot3 uniq = {len(np.unique(slot3))} (max {slot3.max()})")

print("[6/6] done — run: ./build/forage_real", hier, sift, "<nq> <topb> <topk>")
