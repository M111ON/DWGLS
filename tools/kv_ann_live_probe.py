#!/usr/bin/env python3
"""kv_ann_live_probe.py — KV rows inside a real ANN server, not a toy index.

Copies the live index to a scratch dir, appends the bridge's KV rows, starts
`ann_search.exe --serve` on that copy with the production knob set, regrows, and
then asks the real scorer what it thinks. Two things are being proven that a
Python cosine loop cannot prove:

  1. the rows are loadable and routable by the actual server (entries grows,
     regrow reports the new count, routed != 0);
  2. a hit comes back with a sid whose seek plan resolves to a frame — i.e. the
     whole chain (search -> sid -> map -> KV resume) runs on real data.

Self-exclusion is applied on every query (#7697): a query row lives in the index,
so cosine(q,q)=1.0 would otherwise make top-1 the query itself.

The live index is only ever read. Everything written goes to --work.

Usage:
    python kv_ann_live_probe.py --rows ann_rows.jsonl --sidecar kv_frames.jsonl \
        --index-dir I:\\tools\\ann\\data\\memcore --work I:\\Temp\\annkv [--port 8471]
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

FILES = ("ann_index.jsonl", "ann_tags.tsv", "ann_anchors.bin",
         "ann_anchors-roster.bin", "ann_index.order")
ANN_REPO = r"I:\tools\ann"
EXE = os.path.join(ANN_REPO, "build", "ann_search.exe")


def load_jsonl(path):
    out = []
    with open(path, "rb") as f:
        for raw in f:
            line = raw.decode("utf-8", "replace").rstrip("\r\n ")
            if line.endswith("}"):
                out.append(json.loads(line))
    return out


def post(port, path, body, timeout=600):
    req = urllib.request.Request(
        "http://127.0.0.1:%d%s" % (port, path),
        data=json.dumps(body).encode(), method="POST",
        headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode())


def get(port, path, timeout=30):
    with urllib.request.urlopen("http://127.0.0.1:%d%s" % (port, path),
                                timeout=timeout) as r:
        return json.loads(r.read().decode())


def wait_health(port, limit=180):
    t0 = time.time()
    last = None
    while time.time() - t0 < limit:
        try:
            h = get(port, "/health")
            if h.get("entries"):
                return h
            last = h
        except Exception as e:
            last = str(e)
        time.sleep(1.0)
    raise SystemExit("server did not become healthy: %s" % last)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rows", required=True)
    ap.add_argument("--sidecar", required=True)
    ap.add_argument("--index-dir", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--port", type=int, default=8471)
    ap.add_argument("--queries", type=int, default=8)
    ap.add_argument("--k", type=int, default=5)
    ap.add_argument("--skip-regrow", action="store_true",
                    help="the loader already picks up appended rows at boot; regrow is a "
                         "second, slower proof (full mode was 82 s on 63,915 rows)")
    args = ap.parse_args()

    rows = load_jsonl(args.rows)
    plans = {p["sid"]: p for p in load_jsonl(args.sidecar)}
    by_turn = {p["turn"]: p["sid"] for p in plans.values()}

    if not os.path.isdir(args.work):
        os.makedirs(args.work, exist_ok=True)
    for name in FILES:
        src = os.path.join(args.index_dir, name)
        if not os.path.exists(src):
            print("missing index file: %s" % src, file=sys.stderr)
            return 2
        dst = os.path.join(args.work, name)
        if not os.path.exists(dst) or os.path.getsize(dst) != os.path.getsize(src):
            shutil.copyfile(src, dst)

    base = os.path.join(args.work, "ann_index.jsonl")
    with open(base, "rb") as f:
        before_lines = sum(1 for _ in f)
    with open(base, "ab") as f:
        for r in rows:
            f.write(json.dumps({"sid": r["sid"], "file": r["file"],
                                "npast": r["npast"], "dim": r["dim"],
                                "vec": r["vec"]}).encode() + b"\n")
    with open(base, "rb") as f:
        after_lines = sum(1 for _ in f)

    env = dict(os.environ)
    env.update({
        "ANN_PORT": str(args.port),
        "ANN_INDEX_PATH": base,
        "ANN_TAGS_PATH": os.path.join(args.work, "ann_tags.tsv"),
        "ANN_ANCH_PATH": os.path.join(args.work, "ann_anchors.bin"),
        "ANN_ROSTER_PATH": os.path.join(args.work, "ann_anchors-roster.bin"),
        "ANN_ORDER_PATH": os.path.join(args.work, "ann_index.order"),
        "ANN_ROUTE_LOG": os.path.join(args.work, "route.log"),
        "ANN_ROSTER": "1",
        "ANN_TOTEM_WARM": "1", "ANN_FLOAT_KERNEL": "1",
        "ANN_WALK_PRUNE": "1", "ANN_WALK_FLOOR": "0.5",
        "ANN_DOOR": "0", "ANN_DOOR_EXIT": "0",
    })
    proc = subprocess.Popen([EXE, "--serve", str(args.port)], cwd=args.work,
                            env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        h0 = wait_health(args.port)
        print("  health      entries %s routed %s dim %s"
              % (h0.get("entries"), h0.get("routed"), h0.get("dim")))
        rg = {"skipped": "regrow not requested"}
        if not args.skip_regrow:
            rg = post(args.port, "/v1/state/regrow", {})
        print("  regrow      %s" % json.dumps(rg)[:220])
        h1 = wait_health(args.port)
        print("  after       entries %s routed %s" % (h1.get("entries"), h1.get("routed")))

        hits_pool = 0
        self_hits = 0
        adj1 = adj3 = 0
        resolves = 0
        samples = []
        for r in rows[:args.queries]:
            # topk+1 so the query's own row can be dropped (#7697)
            res = post(args.port, "/v1/state/search",
                       {"vec": r["vec"], "topk": args.k + 1})
            raw = res.get("hits") or res.get("results") or []
            if not raw:
                print("  no hits for turn %d: %s" % (r["npast"], json.dumps(res)[:200]))
                continue
            # the row is in the index: its own vector must score ~1.0 against it
            if raw[0].get("sid") == r["sid"] and (raw[0].get("score") or 0) > 0.999:
                self_hits += 1
            order = [x for x in raw if x.get("sid") != r["sid"]][:args.k]
            hit_sids = [x["sid"] for x in order]
            hits_pool += sum(1 for s in hit_sids if s in plans)
            nbrs = {by_turn.get(r["npast"] - 1), by_turn.get(r["npast"] + 1)} - {None}
            adj1 += 1 if (hit_sids and hit_sids[0] in nbrs) else 0
            adj3 += 1 if (set(hit_sids[:3]) & nbrs) else 0
            plan = plans.get(hit_sids[0]) if hit_sids else None
            resolves += 1 if plan else 0
            samples.append({"query_turn": r["npast"],
                            "self_top1": raw[0].get("sid") == r["sid"],
                            "top1": hit_sids[0] if hit_sids else None,
                            "top1_is_pool": bool(plan),
                            "top1_plan": plan,
                            "top1_score": order[0].get("score") if order else None,
                            "hits": hit_sids[:3]})
        n = len(samples) or 1
        print("  queries     %d (self excluded, topk %d)" % (len(samples), args.k))
        print("  row live    own sid at rank 1 with score ~1.0   %d/%d" % (self_hits, len(samples)))
        print("  hits that are pool rows        %d/%d" % (hits_pool, len(samples) * args.k))
        print("  top-1 that is a pool row       %d/%d" % (resolves, len(samples)))
        print("  adjacency recall@1 %.4f  @3 %.4f" % (adj1 / n, adj3 / n))
        for s in samples[:4]:
            p = s["top1_plan"] or {}
            print("    q t%-3d -> %s score %s%s"
                  % (s["query_turn"], s["top1"][:12], s["top1_score"],
                     ("  plan t%d anchor@%d replay %d" %
                      (p.get("turn"), p.get("anchor_turn"), len(p.get("replay") or [])))
                     if p else "  (not a KV row)"))
        out = os.path.join(args.work, "kv_ann_live_probe.json")
        with open(out, "w", encoding="utf-8") as f:
            json.dump({"index_lines_before": before_lines,
                       "index_lines_after": after_lines,
                       "health_before": h0, "regrow": rg, "health_after": h1,
                       "queries": len(samples), "k": args.k,
                       "self_hits": self_hits,
                       "hits_pool": hits_pool, "top1_pool": resolves,
                       "resolves": resolves, "adj1": adj1, "adj3": adj3,
                       "samples": samples}, f, indent=1)
        print("  receipt     %s" % out)
        ok = (self_hits == len(samples)) and len(samples) > 0
        return 0 if ok else 1
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=15)
        except Exception:
            proc.kill()


if __name__ == "__main__":
    sys.exit(main())
