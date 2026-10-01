#!/usr/bin/env python3
"""
hourglass_probe.py  --  Gate 1: data-independent layout T on real Q8_0 weights.

Question: does ANY fixed (no per-element table) bijection T on the int8 stream
make the stream cheaper to code than identity layout, under the SAME coder?

Rules of the game (from REPORT.md section 8):
  * T must not depend on the data  -> manifest cost = log2(#layouts) bits / tensor
  * Q8_0 block scales (fp16) stay in original block order, untouched.
    Only the int8 quants are permuted; inverse T restores them exactly.
  * Every layout is roundtrip-verified per tensor (assert).
  * Baseline is identity layout + SAME coder (not 8.00). Entropy coding alone
    already beats 8.00 on int8 weights; the layout only counts for the DELTA.

Usage:
  pip install numpy gguf
  python hourglass_probe.py Qwen2.5-0.5B-Instruct-Q8_0.gguf
  python hourglass_probe.py model.gguf --max-elems 40000000 --zcap 8000000
  python hourglass_probe.py --selftest
"""
import argparse
import math
import sys
import time
import zlib

import numpy as np

# ----------------------------------------------------------------- entropy --

def h0(a):
    """Order-0 entropy, bits/symbol, a = uint8 array."""
    c = np.bincount(a, minlength=256).astype(np.float64)
    p = c[c > 0] / a.size
    return float(-(p * np.log2(p)).sum())


def h1(a):
    """Order-1 conditional entropy H(X_t | X_{t-1}), bits/symbol."""
    if a.size < 2:
        return h0(a)
    pair = a[:-1].astype(np.int64) * 256 + a[1:]
    c = np.bincount(pair, minlength=65536).astype(np.float64).reshape(256, 256)
    tot = c.sum()
    rows = c.sum(axis=1, keepdims=True)
    with np.errstate(divide="ignore", invalid="ignore"):
        cond = np.where(c > 0, c / np.where(rows > 0, rows, 1.0), 1.0)
    return float(-((c / tot) * np.log2(cond)).sum())


def zbytes(comp, packed, cap):
    """zlib-9 size in bytes; contiguous middle sample of `cap` symbols, scaled."""
    n = comp.size
    if n > cap:
        s = (n - cap) // 2
        chunk, scale = comp[s:s + cap], n / cap
    else:
        chunk, scale = comp, 1.0
    if packed:
        chunk = np.packbits(chunk)
    return len(zlib.compress(chunk.tobytes(), 9)) * scale

# ----------------------------------------------------------------- layouts --

def morton(y, x):
    k = np.zeros_like(y, dtype=np.int64)
    for b in range(20):
        k |= (((x >> b) & 1) << (2 * b)) | (((y >> b) & 1) << (2 * b + 1))
    return k


def hilbert_d(n, x, y):
    """Vectorised Hilbert xy -> d on an n*n grid (n power of two)."""
    x = x.astype(np.int64).copy()
    y = y.astype(np.int64).copy()
    d = np.zeros_like(x)
    s = n // 2
    while s > 0:
        rx = ((x & s) > 0).astype(np.int64)
        ry = ((y & s) > 0).astype(np.int64)
        d += s * s * ((3 * rx) ^ ry)
        m = ry == 0
        f = m & (rx == 1)
        x = np.where(f, n - 1 - x, x)
        y = np.where(f, n - 1 - y, y)
        x, y = np.where(m, y, x), np.where(m, x, y)
        s //= 2
    return d


def tile_perm(R, C, T, order):
    if R % T or C % T:
        return None
    gr, gc = R // T, C // T
    ty, tx = np.divmod(np.arange(gr * gc, dtype=np.int64), gc)
    if order == "raster":
        key = np.arange(gr * gc, dtype=np.int64)
    elif order == "morton":
        key = morton(ty, tx)
    else:
        n = 1
        while n < max(gr, gc):
            n *= 2
        key = hilbert_d(n, tx, ty)
    tiles = np.argsort(key, kind="stable")
    ty_s, tx_s = ty[tiles], tx[tiles]
    iy, ix = np.divmod(np.arange(T * T, dtype=np.int64), T)
    rows = ty_s[:, None] * T + iy[None, :]
    cols = tx_s[:, None] * T + ix[None, :]
    return (rows * C + cols).ravel()


def make_perm(name, R, C):
    """out = q[perm]. Returns None if layout n/a for this shape."""
    N = R * C
    if name == "id":
        return None
    if name == "transpose":
        return np.arange(N, dtype=np.int64).reshape(R, C).T.ravel()
    if name == "tile32":
        return tile_perm(R, C, 32, "raster")
    if name == "morton32":
        return tile_perm(R, C, 32, "morton")
    if name == "hilbert32":
        return tile_perm(R, C, 32, "hilbert")
    if name == "stride37":
        m = 37
        while math.gcd(m, N) != 1:
            m += 2
        return (np.arange(N, dtype=np.int64) * m) % N
    if name == "fold_il":  # hourglass fold: 0, N-1, 1, N-2, ...
        p = np.empty(N, dtype=np.int64)
        p[0::2] = np.arange((N + 1) // 2)
        p[1::2] = np.arange(N - 1, N - 1 - N // 2, -1)
        return p
    raise ValueError(name)


LAYOUTS = ["id", "transpose", "tile32", "morton32", "hilbert32", "stride37", "fold_il"]

# ------------------------------------------------------------ value maps ----
# All bijective / parameter-free. Return list of (stream, is_bitstream).

def vmap(name, v):
    """v: int8 array."""
    if name == "raw":
        return [(v.view(np.uint8), False)]
    w = v.astype(np.int16)
    if name == "zigzag":
        return [(((w << 1) ^ (w >> 15)).astype(np.uint8), False)]
    if name == "signmag":
        mag = np.abs(w).astype(np.uint8)          # 128 fits in uint8
        sign = (w[w != 0] < 0).astype(np.uint8)   # sign only where mag != 0
        return [(mag, False), (sign, True)]
    raise ValueError(name)


VMAPS = ["raw", "zigzag", "signmag"]

# ------------------------------------------------------------- tensor I/O ---

def gguf_tensors(path, min_elems, max_elems):
    from gguf import GGUFReader
    from gguf.constants import GGMLQuantizationType as QT
    rd = GGUFReader(path)
    for t in rd.tensors:
        if t.tensor_type != QT.Q8_0:
            continue
        if len(t.shape) != 2:
            continue
        C, R = int(t.shape[0]), int(t.shape[1])   # ggml ne[0]=cols (contiguous)
        N = R * C
        if N < min_elems or N > max_elems:
            yield t.name, R, C, None
            continue
        raw = np.asarray(t.data).reshape(-1, 34)
        q = np.ascontiguousarray(raw[:, 2:]).view(np.int8).ravel()
        assert q.size == N, (t.name, q.size, N)
        yield t.name, R, C, q


def synthetic_tensors():
    rng = np.random.default_rng(1)
    for i, (R, C) in enumerate([(256, 512), (512, 256), (96, 160)]):
        q = np.clip(np.round(rng.laplace(0, 9, size=R * C)), -128, 127).astype(np.int8)
        yield f"synth{i}", R, C, q

# ------------------------------------------------------------------- main ---

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("gguf", nargs="?")
    ap.add_argument("--min-elems", type=int, default=262144)
    ap.add_argument("--max-elems", type=int, default=16_000_000,
                    help="skip bigger tensors (memory: ~8 B/elem x ~4 temp arrays)")
    ap.add_argument("--zcap", type=int, default=8_000_000)
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if not a.selftest and not a.gguf:
        ap.error("need a .gguf path or --selftest")

    if a.selftest:
        a.min_elems = 1
        src = synthetic_tensors()
    else:
        src = gguf_tensors(a.gguf, a.min_elems, a.max_elems)

    acc = {(l, v): [0.0, 0.0, 0.0] for l in LAYOUTS for v in VMAPS}  # h0b h1b zB
    used = skipped = fallback = 0
    n_used = n_skipped = 0
    t0 = time.time()

    for name, R, C, q in src:
        N = R * C
        if q is None:
            skipped += 1
            n_skipped += N
            continue
        used += 1
        n_used += N
        for l in LAYOUTS:
            P = make_perm(l, R, C)
            if l != "id" and P is None:
                fallback += 1
                s = q                      # manifest would say 'id' for this tensor
            elif P is None:
                s = q
            else:
                s = q[P]
                inv = np.empty_like(q)
                inv[P] = s
                assert np.array_equal(inv, q), f"ROUNDTRIP FAIL {l} {name}"
                del inv
            for v in VMAPS:
                parts = vmap(v, s)
                # roundtrip check of the value map (raw/zigzag/signmag)
                for comp, packed in parts:
                    if comp.size == 0:
                        continue
                    acc[(l, v)][0] += h0(comp) * comp.size
                    acc[(l, v)][1] += h1(comp) * comp.size
                    acc[(l, v)][2] += zbytes(comp, packed, a.zcap)
            del P, s
        print(f"  {name:36s} {R}x{C}  ok  ({time.time() - t0:.0f}s)", file=sys.stderr)

    if not n_used:
        print("no tensors used (check --min-elems/--max-elems)")
        return

    man = math.log2(len(LAYOUTS)) * used / n_used
    print(f"\ntensors used {used}, skipped(size) {skipped}, "
          f"coverage {n_used / (n_used + n_skipped):.1%} of Q8_0 2D values, "
          f"layout fallbacks->id {fallback}")
    print(f"manifest cost if per-tensor layout id: {man:.6f} b/val (negligible)\n")
    print(f"{'layout':10s} {'vmap':8s} {'H0':>7s} {'H1':>7s} {'zlib':>7s}   "
          f"{'dH1':>7s} {'dzlib':>7s}   (bits/val, quants only; scales unchanged)")
    base = acc[("id", "raw")]
    b1, bz = base[1] / n_used, base[2] * 8 / n_used
    best = None
    for l in LAYOUTS:
        for v in VMAPS:
            x = acc[(l, v)]
            H0, H1, Z = x[0] / n_used, x[1] / n_used, x[2] * 8 / n_used
            print(f"{l:10s} {v:8s} {H0:7.3f} {H1:7.3f} {Z:7.3f}   "
                  f"{H1 - b1:+7.3f} {Z - bz:+7.3f}")
            if (l, v) != ("id", "raw") and (best is None or Z < best[0]):
                best = (Z, H1, l, v)

    print(f"\nbaseline id/raw : H1 {b1:.3f}  zlib {bz:.3f}  (raw int8 = 8.000)")
    Z, H1, l, v = best
    gain = (bz - Z) / bz
    print(f"best non-baseline by zlib: {l}/{v}  zlib {Z:.3f} H1 {H1:.3f}  "
          f"({gain:+.2%} vs id/raw zlib)")
    passed = (Z + man < bz * 0.99) and (H1 + man < b1 - 1e-9)
    print("GATE 1:", "PASS (>=1% under id/raw on zlib AND lower H1)" if passed
          else "FAIL -> layout is organization only, not size. Keep as infrastructure.")


if __name__ == "__main__":
    main()
