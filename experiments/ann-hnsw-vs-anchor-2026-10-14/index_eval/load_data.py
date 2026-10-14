#!/usr/bin/env python3
"""Load the real 61k x 384 memcore index -> cached .npy, L2-normalized.
Also build the exact oracle for a fixed query set (held-out protocol)."""
import json, time, os
import numpy as np

IDX = "I:/tools/ann/data/memcore/ann_index.jsonl"
OUT = "I:/tmp/ann_eval"
N_DIM = 384

def load_all():
    vecs = []
    sids = []
    t0 = time.time()
    with open(IDX, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                o = json.loads(line)
            except Exception:
                continue
            v = o.get("vec")
            if v and len(v) == N_DIM:
                vecs.append(v)
                sids.append(o.get("sid", ""))
    X = np.asarray(vecs, dtype=np.float32)
    nrm = np.linalg.norm(X, axis=1, keepdims=True)
    nrm[nrm == 0] = 1.0
    X /= nrm
    print(f"loaded {X.shape} in {time.time()-t0:.1f}s", flush=True)
    return X, sids

if __name__ == "__main__":
    X, sids = load_all()
    np.save(f"{OUT}/X.npy", X)
    with open(f"{OUT}/sids.txt", "w", encoding="utf-8") as f:
        f.write("\n".join(sids))
    print("saved", X.shape, "dtype", X.dtype)
