#!/usr/bin/env python3
"""kv_index_split_probe.py — is a KV frame index better off separate from the
memory index? Measured, not argued.

Builds three arrangements of the same data and asks each one the same questions:

  A. KV-only index        — 2,000 real pool messages (own vectors, sid = pool
                            hash), served by its own `ann_search.exe`
  B. memory-only index    — a copy of the live 63,915-row memcore index
  C. mixed                — B plus the 32 KV frame rows (the arrangement proven
                            earlier: rows load and score 1.0 against themselves)

Measurements:
  1. KV retrieval      — 200 KV vectors against A vs C (global): recall@1/@5,
                         items_scored, wall time
  2. memory pollution  — 200 memory vectors against B vs C: how often a KV sid
                         appears in top-5, and whether top-1 moved
  3. separation cost   — boot time, regrow time, files touched

The live index and the live pool log are read only; every write goes to --work.

Usage:
    python kv_index_split_probe.py --log I:/Temp/poolscratch/log.jsonl \
        --index-dir I:/tools/ann/data/memcore --work I:/Temp/kvsplit \
        [--rows 2000] [--queries 200]
"""
from __future__ import annotations

import argparse
import json
import os
import random
import re
import shutil
import subprocess
import sys
import time
import unicodedata
import urllib.request

ANN_REPO = r"I:\tools\ann"
EXE = os.path.join(ANN_REPO, "build", "ann_search.exe")
MEM_FILES = ("ann_index.jsonl", "ann_tags.tsv", "ann_anchors.bin",
             "ann_anchors-roster.bin", "ann_index.order")

ENVELOPE = re.compile(r"^\[[^\]]*\]\s*\([^)]*\)\s*[-0-9T:+.]+\s*")


def norm(t):
    t = unicodedata.normalize("NFKC", t)
    return re.sub(r"\s+", " ", t).strip()


def strip_envelope(t):
    return norm(ENVELOPE.sub("", t, count=1))


def pool_rows(path, limit, min_len=20, max_chars=400):
    """Real messages, body only, ordered as they appear in the log."""
    out = []
    apps = {}
    with open(path, "rb") as f:
        for raw in f:
            if len(out) >= limit:
                break
            line = raw.decode("utf-8", "replace").rstrip("\r\n ")
            if not line.endswith("}"):
                continue
            try:
                d = json.loads(line)
            except Exception:
                continue
            if (d.get("kind") not in ("CHAT", "TLM")):
                continue
            if d.get("app") == "kv-temporal":
                continue
            rec = norm(d.get("text") or "")
            body = strip_envelope(rec)
            if len(rec) < min_len or len(body) < 12:
                continue
            if len(body) > max_chars:
                body = body[:max_chars].rstrip()
            h = d.get("hash") or ""
            if len(h) != 64:
                continue
            apps[d.get("app")] = apps.get(d.get("app"), 0) + 1
            out.append({"turn": len(out) + 1, "app": d.get("app"),
                        "hash": h, "text": body})
    return out, apps


def embed(texts, chunk=64):
    """memcore's e5-small: one big batch asks onnxruntime for a 6.3 GB buffer and
    dies, so the caller's list is fed in chunks."""
    sys.path.insert(0, r"I:\tools\cloud_ws\memcore")
    import v2
    emb = v2._emb()
    out = []
    for i in range(0, len(texts), chunk):
        out.extend([list(map(float, v)) for v in
                    emb.run(["passage: " + t for t in texts[i:i + chunk]])])
    return out


def load_jsonl(path):
    out = []
    with open(path, "rb") as f:
        for raw in f:
            s = raw.decode("utf-8", "replace").rstrip("\r\n ")
            if s.endswith("}"):
                out.append(json.loads(s))
    return out


def post(port, path, body, timeout=900):
    req = urllib.request.Request(
        "http://127.0.0.1:%d%s" % (port, path), data=json.dumps(body).encode(),
        method="POST", headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode())


def get(port, path, timeout=30):
    with urllib.request.urlopen("http://127.0.0.1:%d%s" % (port, path),
                                timeout=timeout) as r:
        return json.loads(r.read().decode())


def boot(work, port, extra_env=None, limit=300):
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
    if extra_env:
        env.update(extra_env)
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
    raise SystemExit("port %d never became healthy" % port)


def stop(proc):
    proc.terminate()
    try:
        proc.wait(timeout=20)
    except Exception:
        proc.kill()


def ask(port, vec, k):
    t0 = time.time()
    res = post(port, "/v1/state/search", {"vec": vec, "topk": k + 1})
    ms = (time.time() - t0) * 1000.0
    hits = res.get("hits") or []
    route = res.get("route") or {}
    return hits, route, ms


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", required=True)
    ap.add_argument("--index-dir", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--rows", type=int, default=2000)
    ap.add_argument("--queries", type=int, default=200)
    ap.add_argument("--frame-rows", default=r"I:\Temp\kvreal3\ann_rows32.jsonl",
                    help="KV frame rows to mix into the memory index (arrangement C)")
    ap.add_argument("--plans", default=r"I:\Temp\kvreal3\kv_frames32.jsonl",
                    help="seek plans, used to prove the resume rows survive separation")
    ap.add_argument("--kv-port", type=int, default=8474)
    ap.add_argument("--mem-port", type=int, default=8475)
    ap.add_argument("--mixed-port", type=int, default=8477)
    args = ap.parse_args()

    kvdir = os.path.join(args.work, "kv")
    memdir = os.path.join(args.work, "mem")
    mixdir = os.path.join(args.work, "mix")
    for d in (kvdir, memdir, mixdir):
        os.makedirs(d, exist_ok=True)

    # ---- 1. the KV side: real messages, real vectors, own index -------------
    rows, apps = pool_rows(args.log, args.rows)
    print("  kv rows     %d real messages (%d apps, top %s)"
          % (len(rows), len(apps), sorted(apps.items(), key=lambda x: -x[1])[:2]))
    t0 = time.time()
    vecpath = os.path.join(args.work, "kvvec.jsonl")
    if os.path.exists(vecpath):
        cached = load_jsonl(vecpath)
        if len(cached) == len(rows) and all(
                c["hash"] == rows[i]["hash"] for i, c in enumerate(cached)):
            vecs = [c["vec"] for c in cached]
            print("  embed       %d x %d from cache (%s)"
                  % (len(vecs), len(vecs[0]), vecpath))
        else:
            cached = None
    else:
        cached = None
    if cached is None:
        vecs = embed([r["text"] for r in rows])
        with open(vecpath, "w", encoding="utf-8", newline="\n") as f:
            for r, v in zip(rows, vecs):
                f.write(json.dumps({"hash": r["hash"], "vec": v}) + "\n")
        print("  embed       %d x %d in %.1f s"
              % (len(vecs), len(vecs[0]), time.time() - t0))
    kv32 = load_jsonl(args.frame_rows)
    kvidx = os.path.join(kvdir, "ann_index.jsonl")
    have = {r["hash"][:63] for r in rows}
    with open(kvidx, "w", encoding="utf-8", newline="\n") as f:
        for r, v in zip(rows, vecs):
            f.write(json.dumps({"sid": r["hash"][:63], "file": "kv/frames",
                                "npast": r["turn"], "dim": len(v),
                                "vec": v}) + "\n")
        # the frame rows are the point of the KV index: they carry the seek plans
        added = 0
        for r in kv32:
            if r["sid"] in have:
                continue
            f.write(json.dumps({"sid": r["sid"], "file": r["file"],
                                "npast": r["npast"], "dim": r["dim"],
                                "vec": r["vec"]}) + "\n")
            added += 1
    print("  kv index    %d messages + %d frame rows carrying seek plans"
          % (len(rows), added))
    open(os.path.join(kvdir, "ann_tags.tsv"), "w").close()

    # ---- 2. memory-only copy, and the mixed copy ---------------------------
    for name in MEM_FILES:
        src = os.path.join(args.index_dir, name)
        if not os.path.exists(src):
            continue
        for dst_dir in (memdir, mixdir):
            dst = os.path.join(dst_dir, name)
            if not os.path.exists(dst) or os.path.getsize(dst) != os.path.getsize(src):
                shutil.copyfile(src, dst)
    # the mixed index must start from the pristine memory index on every run, or
    # the frame rows from a previous run are appended twice
    shutil.copyfile(os.path.join(args.index_dir, "ann_index.jsonl"),
                    os.path.join(mixdir, "ann_index.jsonl"))
    with open(os.path.join(mixdir, "ann_index.jsonl"), "ab") as f:
        for r in kv32:
            f.write(json.dumps({"sid": r["sid"], "file": r["file"],
                                "npast": r["npast"], "dim": r["dim"],
                                "vec": r["vec"]}).encode() + b"\n")

    kv_sids = {r["hash"][:63] for r in rows}
    out = {"kv_rows": len(rows), "kv_sids": len(kv_sids), "apps": apps}

    # ---- 3. serve the three arrangements -----------------------------------
    p_kv, h_kv, t_kv = boot(kvdir, args.kv_port)
    print("  kv server   entries %s routed %s boot %.1f s"
          % (h_kv.get("entries"), h_kv.get("routed"), t_kv))
    rg_kv = post(args.kv_port, "/v1/state/regrow", {})
    print("  kv regrow   %s" % json.dumps(rg_kv)[:150])
    p_mem, h_mem, t_mem = boot(memdir, args.mem_port)
    print("  mem server  entries %s routed %s boot %.1f s"
          % (h_mem.get("entries"), h_mem.get("routed"), t_mem))
    p_mix, h_mix, t_mix = boot(mixdir, args.mixed_port)
    print("  mix server  entries %s routed %s boot %.1f s"
          % (h_mix.get("entries"), h_mix.get("routed"), t_mix))
    out.update({"boot_s": {"kv": round(t_kv, 1), "mem": round(t_mem, 1),
                           "mix": round(t_mix, 1)},
                "entries": {"kv": h_kv.get("entries"), "mem": h_mem.get("entries"),
                            "mix": h_mix.get("entries")},
                "regrow_kv": rg_kv})

    try:
        # ---- 4. KV retrieval: own index vs the shared one -------------------
        qn = min(args.queries, len(rows))
        idx = list(range(len(rows)))
        random.Random(7).shuffle(idx)
        picks = sorted(idx[:qn])
        by_turn = {r["turn"]: r["hash"][:63] for r in rows}
        arms = {}
        for name, port in (("kv_only", args.kv_port), ("mixed", args.mixed_port)):
            hit1 = hit5 = 0
            scored = []
            times = []
            top1_kvrow = 0
            for i in picks:
                hits, route, ms = ask(port, vecs[i], 5)
                times.append(ms)
                scored.append(route.get("items_scored") or 0)
                order = [h["sid"] for h in hits if h["sid"] != rows[i]["hash"][:63]][:5]
                if order and order[0] in kv_sids:
                    top1_kvrow += 1
                nbrs = {by_turn.get(rows[i]["turn"] - 1),
                        by_turn.get(rows[i]["turn"] + 1)} - {None}
                hit1 += 1 if (order and order[0] in nbrs) else 0
                hit5 += 1 if (set(order) & nbrs) else 0
            scored = sorted(s for s in scored if s)
            times.sort()
            arms[name] = {"n": qn, "adj@1": hit1 / qn, "adj@5": hit5 / qn,
                          "top1_is_kv_row": top1_kvrow,
                          "items_scored_p50": scored[len(scored) // 2] if scored else None,
                          "ms_p50": round(times[len(times) // 2], 2)}
        out["kv_retrieval"] = arms

        # ---- 5. memory pollution: does a KV row leak into memory results? --
        mem_rows = []
        with open(os.path.join(memdir, "ann_index.jsonl"), "rb") as f:
            for raw in f:
                if len(mem_rows) >= 4000:
                    break
                s = raw.decode("utf-8", "replace").rstrip("\r\n ")
                if s.endswith("}"):
                    d = json.loads(s)
                    if d.get("dim") and d.get("vec"):
                        mem_rows.append(d)
        random.Random(11).shuffle(mem_rows)
        mem_picks = mem_rows[:qn]
        pol = {}
        for name, port in (("mem_only", args.mem_port), ("mixed", args.mixed_port)):
            kv_in_top5 = 0
            changed_top1 = 0
            times = []
            tops = []
            for d in mem_picks:
                hits, route, ms = ask(port, d["vec"], 5)
                times.append(ms)
                order = [h["sid"] for h in hits if h["sid"] != d["sid"]][:5]
                tops.append(order)
                kv_in_top5 += sum(1 for s in order if s in kv_sids)
            times.sort()
            pol[name] = {"n": len(mem_picks),
                         "kv_rows_in_top5": kv_in_top5,
                         "top5_slots": len(mem_picks) * 5,
                         "ms_p50": round(times[len(times) // 2], 2)}
            pol[name + "_tops"] = tops
        a, b = pol["mem_only_tops"], pol["mixed_tops"]
        pol["top1_changed"] = sum(1 for x, y in zip(a, b)
                                  if (x[:1] or [None]) != (y[:1] or [None]))
        pol["top5_set_changed"] = sum(1 for x, y in zip(a, b) if set(x) != set(y))
        out["memory_pollution"] = pol

        # ---- 6. the resume path still resolves on the separated index ------
        plans = {p["sid"]: p for p in load_jsonl(args.plans)}
        found = rank1 = with_plan = 0
        for r in kv32:
            hits, _, _ = ask(args.kv_port, r["vec"], 3)
            own = next((h for h in hits if h["sid"] == r["sid"]), None)
            if own:
                found += 1
                rank1 += 1 if hits[0]["sid"] == r["sid"] else 0
                with_plan += 1 if own["sid"] in plans else 0
        out["resume_rows_present"] = {"frames": len(kv32), "found_own_sid": found,
                                      "rank1": rank1, "with_plan": with_plan}
        print("  resume      %d frame rows in the KV index: own sid found %d, rank 1 %d, "
              "plan attached %d" % (len(kv32), found, rank1, with_plan))

        print("\n  KV retrieval (200 queries, self excluded)")
        for name, v in arms.items():
            print("    %-8s @1 %.4f  @5 %.4f  top1-is-kv %d/%d  items_scored p50 %s  ms p50 %s"
                  % (name, v["adj@1"], v["adj@5"], v["top1_is_kv_row"], v["n"],
                     v["items_scored_p50"], v["ms_p50"]))
        print("\n  memory pollution (200 memory queries)")
        for name in ("mem_only", "mixed"):
            v = pol[name]
            print("    %-8s kv rows in top-5 %d/%d  ms p50 %s"
                  % (name, v["kv_rows_in_top5"], v["top5_slots"], v["ms_p50"]))
        print("    top-1 changed by adding KV rows: %d/%d   top-5 set changed: %d/%d"
              % (pol["top1_changed"], qn, pol["top5_set_changed"], qn))
        print("\n  cost        boot %s s" % out["boot_s"])
        dest = os.path.join(args.work, "kv_index_split.json")
        with open(dest, "w", encoding="utf-8") as f:
            json.dump(out, f, indent=1)
        print("  receipt     %s" % dest)
        return 0
    finally:
        for p in (p_kv, p_mem, p_mix):
            stop(p)


if __name__ == "__main__":
    sys.exit(main())
