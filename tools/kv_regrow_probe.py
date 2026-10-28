#!/usr/bin/env python3
"""kv_regrow_probe.py — what does regrow actually cost, on a complete sidecar set?

The 82 s number reported earlier came from a scratch copy that carried only 5 of
the 8 files a routed index needs (no `ann_index.jsonl.stamp`, no
`ann_anchors-state.json`, no `tag_lut.json`). Without the stamp the loader cannot
tell which rows are already routed, so it re-derives every anchor — that is a
first-build cost, not the steady-state cost.

This probe copies the whole sidecar set for the same index and measures:
  boot health (routed?), regrow with nothing changed, regrow after N appended
  rows, and the same again for the index that lacks the stamp.

Usage:
  python kv_regrow_probe.py --index-dir I:/tools/ann/data/memcore \
      --work I:/Temp/annfull --port 8481 [--append-rows I:/Temp/kvreal3/ann_rows32.jsonl]
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.request

EXE = r"I:\tools\ann\build\ann_search.exe"
FULL = ("ann_index.jsonl", "ann_index.jsonl.stamp", "ann_tags.tsv",
        "ann_anchors.bin", "ann_anchors-state.json", "ann_anchors-roster.bin",
        "ann_index.order", "tag_lut.json")
PARTIAL = ("ann_index.jsonl", "ann_tags.tsv", "ann_anchors.bin",
           "ann_anchors-roster.bin", "ann_index.order")


def post(port, path, body, timeout=900):
    req = urllib.request.Request(
        "http://127.0.0.1:%d%s" % (port, path), data=json.dumps(body).encode(),
        method="POST", headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode())


def get(port, path, timeout=60):
    with urllib.request.urlopen("http://127.0.0.1:%d%s" % (port, path),
                                timeout=timeout) as r:
        return json.loads(r.read().decode())


def boot(work, port, limit=600):
    env = dict(os.environ)
    env.update({
        "ANN_PORT": str(port),
        "ANN_INDEX_PATH": os.path.join(work, "ann_index.jsonl"),
        "ANN_TAGS_PATH": os.path.join(work, "ann_tags.tsv"),
        "ANN_ANCH_PATH": os.path.join(work, "ann_anchors.bin"),
        "ANN_ROSTER_PATH": os.path.join(work, "ann_anchors-roster.bin"),
        "ANN_ORDER_PATH": os.path.join(work, "ann_index.order"),
        "ANN_ROUTE_LOG": os.path.join(work, "route.log"),
        "ANN_ROSTER": "1", "ANN_TOTEM_WARM": "1", "ANN_FLOAT_KERNEL": "1",
        "ANN_WALK_PRUNE": "1", "ANN_WALK_FLOOR": "0.5",
        "ANN_DOOR": "0", "ANN_DOOR_EXIT": "0",
    })
    t0 = time.time()
    proc = subprocess.Popen([EXE, "--serve", str(port)], cwd=work, env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    while time.time() - t0 < limit:
        try:
            h = get(port, "/health")
            if h.get("entries"):
                return proc, h, time.time() - t0
        except Exception:
            pass
        time.sleep(0.5)
    proc.terminate()
    raise SystemExit("port %d never healthy" % port)


def stop(proc):
    proc.terminate()
    try:
        proc.wait(timeout=20)
    except Exception:
        proc.kill()


def arm(name, src, work, files, port, append_rows):
    if not os.path.isdir(work):
        os.makedirs(work, exist_ok=True)
    for f in files:
        s = os.path.join(src, f)
        d = os.path.join(work, f)
        if os.path.exists(s) and (not os.path.exists(d)
                                 or os.path.getsize(d) != os.path.getsize(s)):
            shutil.copyfile(s, d)
    rows = []
    if append_rows:
        with open(append_rows, encoding="utf-8") as f:
            rows = [json.loads(l) for l in f if l.strip()]
        with open(os.path.join(work, "ann_index.jsonl"), "ab") as f:
            for r in rows:
                f.write(json.dumps({"sid": r["sid"], "file": r["file"],
                                    "npast": r["npast"], "dim": r["dim"],
                                    "vec": r["vec"]}).encode() + b"\n")
    proc, h, t_boot = boot(work, port)
    try:
        out = {"arm": name, "files": list(files), "appended": len(rows),
               "boot_s": round(t_boot, 1), "entries": h.get("entries"),
               "routed_at_boot": h.get("routed"), "stamps_live": h.get("stamps_live"),
               "order_stale": h.get("order_stale")}
        rg = post(port, "/v1/state/regrow", {})
        inner = rg.get("regrow") or {}
        out["regrow"] = {"mode": rg.get("mode"), "nent": rg.get("nent"),
                         "inner_mode": inner.get("mode"), "ms": inner.get("ms"),
                         "n_prev": inner.get("n_prev"), "n_new": inner.get("n_new"),
                         "load_src": inner.get("load_src"),
                         "phases": inner.get("ms_per_phase")}
        h2 = get(port, "/health")
        out["after"] = {"entries": h2.get("entries"), "routed": h2.get("routed")}
        return out
    finally:
        stop(proc)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--index-dir", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--port", type=int, default=8481)
    ap.add_argument("--append-rows", default=None)
    args = ap.parse_args()

    res = []
    r = arm("full sidecars, nothing appended", args.index_dir,
            os.path.join(args.work, "full"), FULL, args.port, None)
    print("  full  boot %.1f s  routed %s  entries %s"
          % (r["boot_s"], r["routed_at_boot"], r["entries"]))
    print("        regrow mode %s (%s) ms %s  n_prev %s n_new %s"
          % (r["regrow"]["mode"], r["regrow"]["inner_mode"], r["regrow"]["ms"],
             r["regrow"]["n_prev"], r["regrow"]["n_new"]))
    res.append(r)

    if args.append_rows:
        r2 = arm("full sidecars + rows appended", args.index_dir,
                 os.path.join(args.work, "full_add"), FULL, args.port + 1,
                 args.append_rows)
        print("  full+ boot %.1f s  routed %s  entries %s"
              % (r2["boot_s"], r2["routed_at_boot"], r2["entries"]))
        print("        regrow mode %s (%s) ms %s  n_prev %s n_new %s"
              % (r2["regrow"]["mode"], r2["regrow"]["inner_mode"],
                 r2["regrow"]["ms"], r2["regrow"]["n_prev"], r2["regrow"]["n_new"]))
        res.append(r2)

        r3 = arm("partial sidecars + rows appended (no .stamp)", args.index_dir,
                 os.path.join(args.work, "partial"), PARTIAL, args.port + 2,
                 args.append_rows)
        print("  part  boot %.1f s  routed %s  entries %s"
              % (r3["boot_s"], r3["routed_at_boot"], r3["entries"]))
        print("        regrow mode %s (%s) ms %s  n_prev %s n_new %s"
              % (r3["regrow"]["mode"], r3["regrow"]["inner_mode"],
                 r3["regrow"]["ms"], r3["regrow"]["n_prev"], r3["regrow"]["n_new"]))
        res.append(r3)

    dest = os.path.join(args.work, "kv_regrow_probe.json")
    with open(dest, "w", encoding="utf-8") as f:
        json.dump(res, f, indent=1)
    print("  receipt %s" % dest)
    return 0


if __name__ == "__main__":
    sys.exit(main())
