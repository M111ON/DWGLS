#!/usr/bin/env python3
"""kv_log_check.py — verify a KV temporal log the way a reader would.

Two jobs, both receipts rather than intentions:

  check <dir>
      Every line must parse, every line's hash must equal
      sha256(prev+app+kind+to+text) (the pool's rule), each line's `prev` must
      be the previous line's hash, every anchor's [off,off+len) must be unique
      and inside the blob file, its fnv1a must match the bytes, and turn numbers
      must be unique and ascending. Exit 1 on any violation.

  noise <dir> <n>
      Append n ordinary pool lines to a SCRATCH dir (pool's globals are
      repointed; the live pool is never touched). This is the second writer used
      to race the C host: the point is that the chain stays one chain while two
      processes append, and that blob offsets stay unique.

Usage:
  python tools/kv_log_check.py check I:\\Temp\\kv9
  python tools/kv_log_check.py noise I:\\Temp\\kv9 300
"""
from __future__ import annotations

import hashlib
import json
import os
import sys

FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
ROUND = 144        # must match KVT_ROUND in core/kv_temporal.h


def fnv1a(data: bytes) -> int:
    h = FNV_OFFSET
    for b in data:
        h ^= b
        h = (h * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return h


def check(directory: str) -> int:
    log = os.path.join(directory, "log.jsonl")
    blob = os.path.join(directory, "kv_blobs.bin")
    if not os.path.exists(log):
        print("no log at %s" % log)
        return 1
    blob_size = os.path.getsize(blob) if os.path.exists(blob) else 0
    bad = []
    prev = None          # nothing to compare the first line against
    first_prev = None
    lines = 0
    turns = []           # delta turns
    anchor_turns = []    # anchor turns (an anchor names a turn that also has a delta)
    deltas = []          # (turn, parent) for the branch check
    regions = []
    with open(log, "r", encoding="utf-8") as f:
        for i, raw in enumerate(f, 1):
            raw = raw.rstrip("\n")
            if not raw:
                continue
            lines += 1
            try:
                m = json.loads(raw)
            except Exception as e:                     # a broken line is the bug
                bad.append("line %d: not JSON (%s)" % (i, e))
                continue
            want = hashlib.sha256(
                (m.get("prev", "") + m.get("app", "") + m.get("kind", "")
                 + m.get("to", "") + m.get("text", "")).encode("utf-8")).hexdigest()
            if m.get("hash") != want:
                bad.append("line %d: hash does not cover its own fields" % i)
            if prev is None:
                # the first line of a real pool log says prev="" (not "GENESIS"):
                # it is the origin of the chain, so there is nothing to match. Any
                # later line MUST point at its predecessor.
                first_prev = m.get("prev")
                if first_prev not in ("", "GENESIS"):
                    bad.append("line %d: first line claims a prev (%r) but nothing precedes it"
                               % (i, first_prev))
            elif m.get("prev") != prev:
                bad.append("line %d: prev %s.. is not the previous hash %s.."
                           % (i, str(m.get("prev"))[:8], prev[:8]))
            prev = m.get("hash") or prev
            if m.get("kind") == "kv-delta":
                d = json.loads(m["text"])
                turns.append(d["turn"])
                deltas.append((d["turn"], d.get("parent")))
            elif m.get("kind") == "kv-anchor":
                a = json.loads(m["text"])
                anchor_turns.append(a["turn"])
                off, ln = a["blob_off"], a["blob_len"]
                if off + ln > blob_size:
                    bad.append("anchor t%d: [%d,%d) outside the blob file (%d B)"
                               % (a["turn"], off, off + ln, blob_size))
                regions.append((off, off + ln, a))
    # overlap: two anchors may not share bytes
    regions.sort(key=lambda r: r[0])
    for (o1, e1, a1), (o2, e2, a2) in zip(regions, regions[1:]):
        if o2 < e1:
            bad.append("anchor t%d [%d,%d) overlaps anchor t%d [%d,%d)"
                       % (a1["turn"], o1, e1, a2["turn"], o2, e2))
    # fnv per anchor
    if regions and blob_size:
        blob_bytes = open(blob, "rb").read(blob_size)
        for o, e, a in regions:
            if fnv1a(blob_bytes[o:e]) != a["blob_fnv"]:
                bad.append("anchor t%d: fnv mismatch at [%d,%d)" % (a["turn"], o, e))
    if turns != sorted(turns):
        bad.append("turn numbers are not ascending")
    if len(set(turns)) != len(turns):
        dup = sorted({t for t in turns if turns.count(t) > 1})[:5]
        bad.append("duplicate delta turn numbers: %s" % dup)
    if len(set(anchor_turns)) != len(anchor_turns):
        bad.append("duplicate anchor turns: %s" % sorted(anchor_turns))
    if anchor_turns and turns and not set(anchor_turns) <= set(turns):
        bad.append("anchor with no matching delta: %s"
                   % sorted(set(anchor_turns) - set(turns)))
    for t in anchor_turns:      # the round rule, checked on the data itself
        if (t - 1) % ROUND != 0:
            bad.append("anchor t%d is not on a round boundary" % t)

    # every delta must hang off the nearest anchor at or behind its turn: a
    # record written under another branch names a different parent and would
    # replay on the wrong base instead of failing
    for turn, parent in deltas:
        best = max([a for a in anchor_turns if a <= turn], default=None)
        if best is None or parent != best:
            bad.append("delta t%d hangs off parent %s, nearest anchor is %s"
                       % (turn, parent, best))

    print("lines %d  (first prev %r)  delta turns %d..%d (unique %d)  anchors %d %s  blob %d B"
          % (lines, first_prev, min(turns) if turns else 0, max(turns) if turns else 0,
             len(set(turns)), len(regions), sorted(anchor_turns), blob_size))
    if bad:
        print("FAIL %d:" % len(bad))
        for b in bad[:20]:
            print("  " + b)
        return 1
    print("CHAIN OK — every line hashes to itself, prev links contiguous, "
          "anchor regions unique and fnv-verified")
    return 0


def noise(directory: str, n: int) -> int:
    """Append n pool lines into a scratch dir (never the live pool)."""
    sys.path.insert(0, r"I:\tools\chat-pool")
    import pool
    pool.DIR = directory
    pool.LOG = os.path.join(directory, "log.jsonl")
    pool.STATE = os.path.join(directory, ".flush_state.json")
    pool._DERIVED = {"key": None}
    for i in range(n):
        pool.append("kv-noise", "chat", "concurrent writer line %d" % i)
    print("noise: appended %d pool lines to %s" % (n, pool.LOG))
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    cmd, d = sys.argv[1], sys.argv[2]
    if cmd == "check":
        sys.exit(check(d))
    elif cmd == "noise":
        sys.exit(noise(d, int(sys.argv[3]) if len(sys.argv) > 3 else 100))
    print("unknown command %r" % cmd)
    sys.exit(2)
