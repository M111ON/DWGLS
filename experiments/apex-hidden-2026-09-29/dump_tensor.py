#!/usr/bin/env python3
"""dump_tensor.py — write one safetensors tensor as raw little-endian float32.

usage: dump_tensor.py <tensor-name> <out.f32>

Fetches only the byte range the tensor needs (HTTP Range), dequantises BF16 to f32,
and writes it row-major so a C test can mmap/read it directly.
"""
import json
import struct
import sys
import urllib.request

import numpy as np

REPO = "Qwen/Qwen2.5-0.5B-Instruct"
URL = f"https://huggingface.co/{REPO}/resolve/main/model.safetensors"
UA = {"User-Agent": "curl/8"}


def get_range(a, b):
    r = urllib.request.Request(URL, headers={**UA, "Range": f"bytes={a}-{b}"})
    with urllib.request.urlopen(r, timeout=300) as f:
        return f.read()


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    name, out = sys.argv[1], sys.argv[2]

    hdr = json.load(open("st_header.json"))
    if name not in hdr:
        print(f"no tensor {name!r}")
        return 1
    base = 8 + struct.unpack("<Q", get_range(0, 7))[0]
    info = hdr[name]
    nr, nc = info["shape"]
    o0 = base + info["data_offsets"][0]
    raw = get_range(o0, o0 + nr * nc * 2 - 1)
    u = np.frombuffer(raw, dtype=np.uint16).astype(np.uint32)
    m = (u << 16).view(np.float32).reshape(nr, nc)
    m.astype("<f4").tofile(out)
    print(f"{name}  {nr}x{nc}  -> {out}  ({m.nbytes} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
