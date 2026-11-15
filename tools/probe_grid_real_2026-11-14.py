"""Z2-a GATE — does the 144x144 (20736) cell grid preserve the REAL corpus
neighbourhood, i.e. is grid/address routing worth wiring?

Real data only: data/memcore/ann_index.jsonl.V.npy  (54,373 x 768 float32,
the served nomic embeddings). No synthetic rows.

Three placements are reported SEPARATELY and labelled, because they answer
different questions:

  A  PCA-2D -> 144x144 grid   : does a 20,736-cell geometric grid carry the
                                embedding neighbourhood at all?
  B  production anchors (K=56): what the served anchor route already achieves
                                (the baseline a grid would have to beat)
  R  random placement          : control — structure-free routing lower bound

Metric: retrieval recall@10 vs the exact cosine top-10 over all rows, reported
against SCAN% (fraction of rows examined). This is NOT an embedding-index recall
claim about the field (#6365) — the grid arm is measured only to decide whether
Z2-b/Z2-c wiring is worth spending on.
"""
import sys, time
from pathlib import Path
import numpy as np

ANN = Path(r"I:\tools\ann")
V = np.load(ANN / "data/memcore/ann_index.jsonl.V.npy", mmap_mode="r")
N, D = V.shape
CH = 8192
TOPK = 10
QN = 200
rng = np.random.default_rng(11)
print(f"corpus: {N} x {D}", flush=True)


def load_norm(idx):
    a = np.asarray(V[idx], dtype=np.float32)
    n = np.linalg.norm(a, axis=1, keepdims=True)
    n[n == 0] = 1.0
    return a / n


t0 = time.time()
R = load_norm(np.arange(N))                     # normalized corpus, 167 MB
print(f"normalized corpus in {time.time()-t0:.1f}s", flush=True)

qi = np.sort(rng.choice(N, size=QN, replace=False))
Vq = R[qi]
S = Vq @ R.T                                    # (QN, N)
for i, qidx in enumerate(qi):
    S[i, qidx] = -2.0                           # exclude self
truth = np.argsort(-S, axis=1)[:, :TOPK]
print("truth top-10 done", flush=True)


def recall_from_mask(mask):
    got = np.argpartition(-np.where(mask, S, -2.0), TOPK - 1, axis=1)[:, :TOPK]
    hit = 0
    for i in range(QN):
        hit += len(set(got[i].tolist()) & set(truth[i].tolist()))
    return hit / (QN * TOPK)


def report(name, cell_of, G):
    cell_of = np.asarray(cell_of)
    o = np.argsort(cell_of, kind="stable")
    cs = cell_of[o]
    starts = np.searchsorted(cs, np.arange(G * G), side="left")
    ends = np.searchsorted(cs, np.arange(G * G), side="right")
    rows_of_cell = [o[starts[c]:ends[c]] for c in range(G * G)]
    qcell = cell_of[qi]
    used = int(np.count_nonzero(starts != ends))
    print(f"\n--- {name} ---  cells used {used}/{G*G}", flush=True)
    for rad in [0, 1, 2, 3, 5, 8, 12, 20, 40, G - 1]:
        mask = np.zeros((QN, N), dtype=bool)
        for i in range(QN):
            r0, c0 = divmod(int(qcell[i]), G)
            for r in range(max(0, r0 - rad), min(G, r0 + rad + 1)):
                base = r * G
                for c in range(max(0, c0 - rad), min(G, c0 + rad + 1)):
                    rr = rows_of_cell[base + c]
                    if rr.size:
                        mask[i, rr] = True
        rec = recall_from_mask(mask)
        scan = mask.sum(axis=1).mean() / N * 100.0
        cells = min((2 * rad + 1) ** 2, G * G)
        print(f"  ring<={rad:>3}  cells={cells:>6}  scan={scan:6.2f}%  recall@10={rec:.4f}", flush=True)


# --- A: PCA-2D -> 144x144
t0 = time.time()
samp = R[rng.choice(N, size=4000, replace=False)]
samp = samp - samp.mean(axis=0)
_, _, Vt = np.linalg.svd(samp, full_matrices=False)
P = Vt[:2]
proj = R @ P.T
lo, hi = proj.min(axis=0), proj.max(axis=0)
span = np.where(hi - lo == 0, 1.0, hi - lo)
g = np.clip(((proj - lo) / span * 143.999).astype(np.int64), 0, 143)
cellA = g[:, 0] * 144 + g[:, 1]
cnt = np.bincount(cellA, minlength=20736)
print(f"PCA-2D built in {time.time()-t0:.1f}s  rows/cell p50={int(np.median(cnt))} "
      f"max={int(cnt.max())}", flush=True)
report("A  PCA-2D -> 144x144 grid", cellA, 144)

# --- B: production anchors K=56
anch = np.fromfile(ANN / "data/memcore/kv_anchors.bin", dtype=np.float32)
K = 56
# 43,013 floats = 5 (header) + 56*768. Reading from offset 0 gives one inf-norm
# "anchor" and a 16,170-row bucket — the numbers below were first produced that
# way and were VOID; see experiments/z2a_armB_recheck_2026-11-14.py.
anch = anch[anch.size - K * D:].reshape(K, D)
an = np.linalg.norm(anch, axis=1, keepdims=True); an[an == 0] = 1
anch = anch / an
bucket = np.empty(N, dtype=np.int64)
for s in range(0, N, CH):
    e = min(s + CH, N)
    bucket[s:e] = np.argmax(R[s:e] @ anch.T, axis=1)
bc = np.bincount(bucket, minlength=56)
print(f"\nproduction anchors K=56: rows/bucket p50={int(np.median(bc))} "
      f"min={int(bc.min())} max={int(bc.max())}", flush=True)
report("B  production anchors, cell=bucket (1-D of 144)", bucket, 144)

# --- R: random control
report("R  random placement (control)", rng.integers(0, 20736, size=N), 144)
