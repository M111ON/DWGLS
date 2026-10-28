#!/usr/bin/env python3
"""kv_lookup.py — the two-layer lookup the split index implies.

Layer 1 (memory, :8096): the query is embedded and searched against the corpus
index. Every hit sid is checked against `kv_index_map.jsonl`; a hit that is in
the map already names a KV frame, so nothing else is searched.

Layer 2 (KV, its own port): only when layer 1 produced no mapped hit. The KV
index holds the frame rows themselves, which is the only place a frame that is
not in the memory corpus can be found.

Both layers end at the same place: a plan {turn, anchor_turn, anchor_blob,
replay}, verified twice — `map-vs-log` (the map must equal what the append-only
log derives) and the anchor blob's FNV (the bytes the plan names must be the
bytes on disk). A plan that fails either is refused, never served.

    python kv_lookup.py --text "..."                    one query
    python kv_lookup.py --probe N --map DIR\\kv_index_map.jsonl --kv DIR \\
                        --kv-port 8510 [--queries-from DIR\\ann_rows.jsonl]

--probe measures the thing the split was built for: how many queries resolve at
layer 1, how many need layer 2, how many miss, and what each layer costs.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import kv_ann_bridge as bridge
from kv_ann_open import load_map

MEM_PORT = 8096
MEMCORE = r"I:\tools\cloud_ws\memcore"

_emb = None


def embed(text):
    """One embedder, the one the corpus already uses (e5-small, dim 384)."""
    global _emb
    if _emb is None:
        sys.path.insert(0, MEMCORE)
        import v2
        _emb = v2._emb()
    return [float(x) for x in _emb.run(["query: " + text])[0]]


def search(port, vec, topk, timeout=120):
    body = json.dumps({"vec": [float(x) for x in vec], "topk": topk}).encode()
    req = urllib.request.Request("http://127.0.0.1:%d/v1/state/search" % port, data=body,
                                 method="POST", headers={"Content-Type": "application/json"})
    t0 = time.time()
    try:
        r = json.loads(urllib.request.urlopen(req, timeout=timeout).read().decode())
    except urllib.error.URLError as e:
        return None, (time.time() - t0) * 1000.0, "unreachable: %s" % e
    ms = (time.time() - t0) * 1000.0
    return (r.get("hits") or []), ms, None


def check_plan(plan, anchors, deltas, blob_path, blob_size):
    """Two gates. Returns (ok, why)."""
    derived = bridge.seek_plan(anchors, deltas, plan["turn"])
    if derived is None:
        return False, "log cannot derive turn %s" % plan["turn"]
    if (derived["anchor_turn"] != plan["anchor_turn"]
            or list(derived["replay"]) != list(plan["replay"])):
        return False, "map-vs-log: log derives anchor t%d replay %s" % (
            derived["anchor_turn"], derived["replay"])
    off, ln, fnv = plan["anchor_blob"]
    if off < 0 or ln <= 0 or off + ln > blob_size:
        return False, "anchor blob [%d,%d) is outside the blob file (%d B)" % (off, off + ln, blob_size)
    try:
        with open(blob_path, "rb") as f:
            f.seek(off)
            data = f.read(ln)
    except OSError as e:
        return False, "blob unreadable: %s" % e
    got = bridge.fnv1a64(data) if hasattr(bridge, "fnv1a64") else _fnv(data)
    if got != fnv:
        return False, "anchor blob fnv %d != recorded %d" % (got, fnv)
    return True, "ok"


def _fnv(data):
    h = 1469598103934665603
    for b in data:
        h ^= b
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def resolve(sid, plans, anchors, deltas, blob_path, blob_size):
    plan = plans.get(sid)
    if plan is None:
        return None, "sid not in the map"
    ok, why = check_plan(plan, anchors, deltas, blob_path, blob_size)
    return (plan if ok else None), why


def load_kv(kvdir):
    anchors, deltas = bridge.read_kv_log(os.path.join(kvdir, "log.jsonl"))
    blob = os.path.join(kvdir, "kv_blobs.bin")
    size = os.path.getsize(blob) if os.path.exists(blob) else 0
    return anchors, deltas, blob, size


def lookup(vec, plans, kv, kv_port, topk=5, exclude_sid=None):
    """Layer 1 then layer 2. Returns a dict describing where the answer came from."""
    anchors, deltas, blob, size = kv
    out = {"layer": None, "plan": None, "sid": None, "why": None, "ms": {}}
    ask = topk + 1 if exclude_sid else topk

    hits, ms, err = search(MEM_PORT, vec, ask)
    out["ms"]["memory"] = round(ms, 2)
    if err:
        out["why"] = err
    else:
        hits = [h for h in hits if h.get("sid") != exclude_sid]
        for h in hits:
            sid = h.get("sid")
            if sid in plans:
                plan, why = resolve(sid, plans, anchors, deltas, blob, size)
                out["ms"]["kv_check"] = 0.0
                if plan is not None:
                    out.update(layer="memory", plan=plan, sid=sid, why="hit at memory rank %s" % (h.get("rank") or "?"))
                    return out
                out["why"] = "memory hit %s but %s" % (str(sid)[:12], why)
                break
        if not out["why"]:
            out["why"] = "no mapped hit in memory top-%d" % topk

    if kv_port:
        hits, ms, err = search(kv_port, vec, ask)
        out["ms"]["kv_index"] = round(ms, 2)
        if err:
            out["why"] += " | " + err
        else:
            hits = [h for h in hits if h.get("sid") != exclude_sid]
            found = False
            for h in hits:
                sid = h.get("sid")
                if sid in plans:
                    plan, why = resolve(sid, plans, anchors, deltas, blob, size)
                    if plan is not None:
                        out.update(layer="kv_index", plan=plan, sid=sid,
                                   why="hit at KV rank %s" % (h.get("rank") or "?"))
                        return out
                    out["why"] += " | kv hit %s but %s" % (str(sid)[:12], why)
                    found = True
                    break
            if not found:
                out["why"] += " | no mapped hit in KV top-%d" % topk
    return out


def build_map(frames_path, out_path, kv):
    """frames sidecar -> {sid: plan}, every plan gated against the log + blob."""
    anchors, deltas, blob, size = kv
    ok, bad = [], []
    with open(frames_path, "rb") as f:
        for raw in f:
            line = raw.decode("utf-8", "replace").strip()
            if not line.endswith("}"):
                continue
            try:
                fr = json.loads(line)
            except Exception:
                continue
            sid = (fr.get("pool_hash") or fr.get("sid") or "")[:63]
            if not sid:
                continue
            plan = {"turn": fr["turn"], "frame_sid": fr.get("frame_sid"),
                    "anchor_turn": fr["anchor_turn"],
                    "anchor_blob": list(fr["anchor_blob"]),
                    "replay": list(fr["replay"])}
            good, why = check_plan(plan, anchors, deltas, blob, size)
            (ok if good else bad).append({"sid": sid, "plan": plan, "why": why})
    with open(out_path, "w", encoding="utf-8") as f:
        for r in ok:
            f.write(json.dumps({"sid": r["sid"], "kv": r["plan"]}, ensure_ascii=False) + "\n")
    for r in bad:
        print("  REFUSED %s t%s: %s" % (r["sid"][:12], r["plan"]["turn"], r["why"]))
    print("map %d plan(s) written to %s, %d refused" % (len(ok), out_path, len(bad)))
    return 0 if ok and not bad else 1


def show(res):
    if res["plan"] is None:
        print("MISS  %s" % res["why"])
        return 1
    p = res["plan"]
    print("HIT   layer=%s sid=%s" % (res["layer"], str(res["sid"])[:16]))
    print("      frame t%d anchor t%d blob[%d,%d) fnv %d replay %s"
          % (p["turn"], p["anchor_turn"], p["anchor_blob"][0], p["anchor_blob"][1],
             p["anchor_blob"][2], p["replay"]))
    print("      %s | ms %s" % (res["why"], res["ms"]))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--text", default=None)
    ap.add_argument("--frames", default=None, help="frames sidecar -> build the map")
    ap.add_argument("--build-map", default=None)
    ap.add_argument("--no-self-exclude", action="store_true",
                    help="keep the query's own row (tautology arm, labelled in the receipt)")
    ap.add_argument("--probe", type=int, default=0, help="run N queries from --queries-from")
    ap.add_argument("--queries-from", default=None, help="jsonl with {text} rows")
    ap.add_argument("--map", default=None)
    ap.add_argument("--kv", default=None)
    ap.add_argument("--kv-port", type=int, default=0)
    ap.add_argument("--topk", type=int, default=5)
    ap.add_argument("--receipt", default=None)
    args = ap.parse_args()

    if args.frames and args.build_map:
        kvdir = args.kv or os.path.dirname(os.path.abspath(args.frames))
        return build_map(args.frames, args.build_map, load_kv(kvdir))

    if not args.map:
        return print("need --map") or 2
    plans = load_map(args.map)
    kvdir = args.kv or os.path.dirname(os.path.abspath(args.map))
    kv = load_kv(kvdir)
    print("map %d plan(s) | log %d anchor(s) %d delta(s) | blob %d B"
          % (len(plans), len(kv[0]), len(kv[1]), kv[3]))

    if args.text:
        return show(lookup(embed(args.text), plans, kv, args.kv_port, args.topk))

    if not args.probe:
        return print("need --text or --probe") or 2
    rows = []
    with open(args.queries_from, "rb") as f:
        for raw in f:
            line = raw.decode("utf-8", "replace").strip()
            if line.endswith("}"):
                try:
                    d = json.loads(line)
                except Exception:
                    continue
                if d.get("text") or d.get("vec"):
                    rows.append(d)
    rows = rows[: args.probe]
    tally = {"memory": 0, "kv_index": 0, "miss": 0}
    dist = {"exact": 0, "near1": 0, "far": 0}
    ms_mem, ms_kv = [], []
    rec = []
    for i, r in enumerate(rows, 1):
        vec = r.get("vec") or embed(r["text"])
        exclude = None if args.no_self_exclude else r.get("sid")
        res = lookup(vec, plans, kv, args.kv_port, args.topk, exclude_sid=exclude)
        got_turn = (res["plan"] or {}).get("turn")
        own = r.get("npast")
        if got_turn is not None and own is not None:
            d = abs(int(got_turn) - int(own))
            dist["exact" if d == 0 else ("near1" if d <= 1 else "far")] += 1
        tally[res["layer"] or "miss"] += 1
        if "memory" in res["ms"]:
            ms_mem.append(res["ms"]["memory"])
        if "kv_index" in res["ms"]:
            ms_kv.append(res["ms"]["kv_index"])
        rec.append({"i": i, "layer": res["layer"], "sid": res["sid"],
                    "turn": (res["plan"] or {}).get("turn"), "why": res["why"],
                    "ms": res["ms"]})
        print("  %2d %-8s t%-4s %s" % (i, res["layer"] or "MISS",
                                       (res["plan"] or {}).get("turn", "-"), res["why"]))

    def p50(xs):
        xs = sorted(xs)
        return round(xs[len(xs) // 2], 2) if xs else None

    print("\nqueries %d | layer1 %d | layer2 %d | miss %d | self-excluded %s"
          % (len(rows), tally["memory"], tally["kv_index"], tally["miss"],
             not args.no_self_exclude))
    print("turn match vs the query's own turn: exact %d | +-1 %d | farther %d"
          % (dist["exact"], dist["near1"], dist["far"]))
    print("ms p50 memory %s | kv index %s" % (p50(ms_mem), p50(ms_kv)))
    if args.receipt:
        with open(args.receipt, "w", encoding="utf-8") as f:
            json.dump({"tally": tally, "dist": dist, "ms_p50": {"memory": p50(ms_mem), "kv": p50(ms_kv)},
                       "self_excluded": not args.no_self_exclude,
                       "queries": rec}, f, indent=1)
        print("receipt %s" % args.receipt)
    return 0 if tally["miss"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
