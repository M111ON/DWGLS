#!/usr/bin/env python3
"""bf16_fetch.py — pull ONE tensor row-range from a HF safetensors file by HTTP Range,
then compare BF16 original vs Q8_0 dequantized on the SAME rows, SAME bin edges,
SAME shuffle control.

Why range fetch: model.safetensors is ~988 MB but we need ~2 MB of it.

Pairing note: the Q8_0 on disk is Qwen2.5-0.5B-**Instruct**, so the matching BF16 is
the Instruct repo, not Base. Mixing Base BF16 with Instruct Q8_0 would confound model
identity with quantization.

usage: bf16_fetch.py <repo> <tensor> <row_start> <nrows>
  e.g. bf16_fetch.py Qwen/Qwen2.5-0.5B-Instruct output.weight 29466 960
"""
import json
import struct
import sys
import urllib.request
import numpy as np

UA = {"User-Agent": "curl/8"}


def get_range(url, start, end_incl):
    req = urllib.request.Request(url, headers={**UA, "Range": f"bytes={start}-{end_incl}"})
    with urllib.request.urlopen(req, timeout=120) as r:
        return r.read()


def bf16_to_f32(raw):
    u = np.frombuffer(raw, dtype=np.uint16).astype(np.uint32)
    return (u << 16).view(np.float32)


def H(c):
    n = c.sum()
    p = c[c > 0] / n
    return float(-(p * np.log2(p)).sum())


def MI(a, b, nb):
    j = np.bincount(a * nb + b, minlength=nb * nb).reshape(nb, nb).astype(np.float64)
    n = j.sum()
    px = j.sum(1) / n
    py = j.sum(0) / n
    p = j / n
    nz = p > 0
    return float((p[nz] * np.log2(p[nz] / (px[:, None] * py[None, :])[nz])).sum())


def bias(n, nb):
    return (nb - 1) ** 2 / (2.0 * n * np.log(2))


def main():
    repo, tensor, row0, nrows = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
    url = f"https://huggingface.co/{repo}/resolve/main/model.safetensors"
    print(f"repo: {repo}\ntensor: {tensor}  rows {row0}..{row0+nrows}\n")

    hlen = struct.unpack("<Q", get_range(url, 0, 7))[0]
    hdr = json.loads(get_range(url, 8, 8 + hlen - 1).decode("utf-8"))
    meta = hdr.pop("__metadata__", None)
    print(f"header {hlen} bytes, {len(hdr)} tensors")
    if tensor not in hdr:
        print(f"  `{tensor}` not found. candidates with 896 in shape:")
        for k, v in hdr.items():
            if 896 in v["shape"]:
                print(f"    {k:34s} {v['dtype']:6s} {v['shape']}")
        return 1
    info = hdr[tensor]
    print(f"  {tensor}: dtype={info['dtype']} shape={info['shape']} "
          f"data_offsets={info['data_offsets']}")
    if info["dtype"] != "BF16":
        print(f"  expected BF16, got {info['dtype']}")
        return 1

    shape = info["shape"]
    rowlen = shape[1]
    esz = 2
    base = 8 + hlen
    off0 = base + info["data_offsets"][0] + row0 * rowlen * esz
    off1 = off0 + nrows * rowlen * esz - 1
    print(f"  fetching bytes {off0:,}..{off1:,}  ({(off1-off0+1)/1e6:.2f} MB)")
    raw = get_range(url, off0, off1)
    bf = bf16_to_f32(raw).reshape(nrows, rowlen)
    print(f"  got {bf.size:,} values  range {bf.min():.6g} .. {bf.max():.6g}\n")
    return bf, row0, nrows


if __name__ == "__main__":
    r = main()
    if isinstance(r, tuple):
        bf, row0, nrows = r
        np.save("bf16_slice.npy", bf)
        print("saved bf16_slice.npy")
