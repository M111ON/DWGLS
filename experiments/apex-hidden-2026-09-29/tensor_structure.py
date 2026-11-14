#!/usr/bin/env python3
"""tensor_structure.py — is the row-stride signal specific to the embedding, or is it
a property of weight matrices too?

Tests three tensors, each with ITS OWN logical row stride taken from the tensor shape
(not a hard-coded 896):

  embed_tokens  [151936, 896]  rows = token IDs
  q_proj        [896, 896]     rows = output neurons of the projection
  up_proj       [4864, 896]    rows = hidden-expansion neurons

Two axis-specific ablations separate "column profile" from "row order":

  permute rows    -> keeps within-row order, destroys row ordering
  permute columns -> keeps row ordering, destroys column identity

MI is reported raw, with the plug-in bias, and bias-corrected.

usage: tensor_structure.py [layer ...]
"""
import json
import struct
import sys
import urllib.request
import numpy as np

REPO = "Qwen/Qwen2.5-0.5B-Instruct"
URL = f"https://huggingface.co/{REPO}/resolve/main/model.safetensors"
UA = {"User-Agent": "curl/8"}
NB = 16
CACHE = "st_header.json"


def get_range(start, end_incl):
    req = urllib.request.Request(URL, headers={**UA, "Range": f"bytes={start}-{end_incl}"})
    with urllib.request.urlopen(req, timeout=180) as r:
        return r.read()


def header():
    try:
        with open(CACHE) as f:
            return json.load(f)
    except FileNotFoundError:
        pass
    hlen = struct.unpack("<Q", get_range(0, 7))[0]
    hdr = json.loads(get_range(8, 8 + hlen - 1).decode("utf-8"))
    hdr.pop("__metadata__", None)
    with open(CACHE, "w") as f:
        json.dump(hdr, f)
    return hdr


def fetch(hdr, name, row0=0, nrows=None):
    info = hdr[name]
    shp = info["shape"]
    if len(shp) != 2:
        raise ValueError(f"{name} is {shp}, not a matrix")
    rl = shp[1]
    nrows = shp[0] if nrows is None else nrows
    base = 8 + struct.unpack("<Q", get_range(0, 7))[0]
    o0 = base + info["data_offsets"][0] + row0 * rl * 2
    o1 = o0 + nrows * rl * 2 - 1
    raw = get_range(o0, o1)
    u = np.frombuffer(raw, dtype=np.uint16).astype(np.uint32)
    return (u << 16).view(np.float32).reshape(nrows, rl)


def H(c):
    n = c.sum()
    p = c[c > 0] / n
    return float(-(p * np.log2(p)).sum())


def MI(a, b, nb=NB):
    j = np.bincount(a * nb + b, minlength=nb * nb).reshape(nb, nb).astype(np.float64)
    n = j.sum()
    if n == 0:
        return 0.0
    px = j.sum(1) / n
    py = j.sum(0) / n
    p = j / n
    nz = p > 0
    return float((p[nz] * np.log2(p[nz] / (px[:, None] * py[None, :])[nz])).sum())


def bias(n, nb=NB):
    return (nb - 1) ** 2 / (2.0 * n * np.log(2))


def analyse(label, mat, rng):
    nr, nc = mat.shape
    lo, hi = float(mat.min()), float(mat.max())
    edges = np.linspace(lo, hi, NB + 1)[1:-1]
    q = np.searchsorted(edges, mat.ravel().astype(np.float64)).astype(np.int32)
    flat = q
    n = flat.size
    print(f"\n### {label}   shape {nr}x{nc}   row stride {nc}   n={n:,}")
    print(f"    range [{lo:.6g}, {hi:.6g}]  equal-width {NB} bins")
    print(f"    H = {H(np.bincount(flat, minlength=NB)):.6f} bits (max {np.log2(NB):.3f})"
          f"   bias floor(lag1) = {bias(n-1):.6f}")

    print(f"\n    {'lag':>7s} {'raw MI':>9s} {'bias':>9s} {'corrected':>10s}  {'pearson':>9s}")
    lags = [1, 2, 8, 32, nc, 2 * nc]
    for lag in lags:
        if lag >= n // 4:
            continue
        a, b = flat[:-lag], flat[lag:]
        raw = MI(a, b)
        bi = bias(a.size)
        r = np.corrcoef(mat.ravel()[:-lag].astype(np.float64),
                        mat.ravel()[lag:].astype(np.float64))[0, 1]
        print(f"    {lag:7d} {raw:9.6f} {bi:9.6f} {raw-bi:+10.6f}  {r:+9.6f}")

    lag = nc
    print(f"\n    axis ablations at lag = row stride ({lag}):")
    base = MI(flat[:-lag], flat[lag:])
    bi = bias(n - lag)

    rows = rng.permutation(nr)
    qr = q.reshape(nr, nc)[rows].ravel()
    mi_rows = MI(qr[:-lag], qr[lag:])

    # columns permuted INDEPENDENTLY per row: breaks the column pairing.
    # (one shared permutation would preserve it and prove nothing)
    m = q.reshape(nr, nc).copy()
    for i in range(nr):
        m[i] = m[i][rng.permutation(nc)]
    qc = m.ravel()
    mi_cols = MI(qc[:-lag], qc[lag:])

    qflat = flat.copy()
    rng.shuffle(qflat)
    mi_flat = MI(qflat[:-lag], qflat[lag:])

    print(f"      original                    {base:9.6f}  corrected {base-bi:+.6f}")
    print(f"      permute ROWS                {mi_rows:9.6f}  corrected {mi_rows-bi:+.6f}")
    print(f"      permute COLS per row        {mi_cols:9.6f}  corrected {mi_cols-bi:+.6f}")
    print(f"      shuffle all values          {mi_flat:9.6f}  corrected {mi_flat-bi:+.6f}")

    # row-scale hypothesis: transformer rows differ in magnitude
    sd = mat.std(axis=1)
    rsd = np.corrcoef(sd[:-1], sd[1:])[0, 1]
    print(f"\n    row std: min {sd.min():.5g} max {sd.max():.5g}"
          f" ratio {sd.max()/max(sd.min(),1e-12):.1f}x"
          f"  pearson(adjacent rows) = {rsd:+.6f}")
    norm = (mat - mat.mean(1, keepdims=True)) / np.maximum(sd[:, None], 1e-12)
    qn = np.searchsorted(np.linspace(norm.min(), norm.max(), NB + 1)[1:-1],
                         norm.ravel().astype(np.float64)).astype(np.int32)
    mi_norm = MI(qn[:-lag], qn[lag:])
    print(f"    after row standardisation:  MI(lag {lag}) = {mi_norm:.6f}"
          f"  corrected {mi_norm-bi:+.6f}")

    verdict = []
    if base - bi < 0.002:
        verdict.append("no row-stride structure")
    else:
        if mi_rows - bi > 0.5 * (base - bi):
            verdict.append("survives row permutation => COLUMN PROFILE")
        else:
            verdict.append("dies under row permutation => real ROW-to-ROW dependence")
        if mi_cols - bi < 0.5 * (base - bi):
            verdict.append("dies when columns are unpaired => needs COLUMN IDENTITY")
        if mi_norm < 0.5 * base:
            verdict.append("collapses after row standardisation => carried by ROW SCALE")
    print("    -> " + "; ".join(verdict))
    return base - bi, mi_rows - bi, mi_cols - bi, mi_norm


def main():
    layers = [int(x) for x in sys.argv[1:]] or [0, 12]
    hdr = header()
    rng = np.random.default_rng(31)
    print(f"repo {REPO}   {len(hdr)} tensors   NB={NB} bins")

    e = fetch(hdr, "model.embed_tokens.weight", 29466, 960)
    analyse("embed_tokens rows 29466..30426 (token IDs)", e, rng)

    for L in layers:
        for nm, tag in (("self_attn.q_proj.weight", "q_proj (out-neuron rows)"),
                        ("mlp.up_proj.weight", "up_proj / ffn_up (expansion-neuron rows)")):
            key = f"model.layers.{L}.{nm}"
            m = fetch(hdr, key)
            analyse(f"{key}\n      {tag}", m, rng)
    return 0


if __name__ == "__main__":
    sys.exit(main())
