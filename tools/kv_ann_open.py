#!/usr/bin/env python3
"""kv_ann_open.py — the consumer end: ANN hit -> sid -> KV frame -> resumed.

The bridge writes two things: rows ANN can rank, and `kv_index_map.jsonl` mapping a
base sid to a KV frame plan. This tool is the other half of that contract — give it
a sid, it prints the plan, and with --run it makes the C host actually resume that
frame and checks the position the host reports against the position the log
implies.

Why the position check: a plan that merely *looks* right is what silently rots.
`anchor.base_n_tokens + sum(tokens of the replay turns)` is derivable from the log
alone, so agreement between it and the host's `pos` means the anchor that was
resumed and the deltas that were replayed are the ones the map named.

    python kv_ann_open.py --map DIR\\kv_index_map.jsonl --sid <sid> [--run]
    python kv_ann_open.py --map ... --turn 3 --run        (skip the lookup)
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import kv_ann_bridge as bridge            # read_kv_log: same reader, same tolerances

DEFAULT_EXE = os.path.join(ROOT, "build", "kv_temporal_llama.exe")
DEFAULT_MODEL = r"I:\model\Qwen2.5-0.5B-Instruct-Q8_0.gguf"
DEFAULT_BACKEND = r"I:\llama\llama-v040-bin-win-vulkan-x64"


def load_map(path):
    out = {}
    with open(path, "rb") as f:
        for raw in f:
            line = raw.decode("utf-8", "replace").rstrip("\r\n ")
            if not line.endswith("}"):
                continue
            try:
                m = json.loads(line)
            except Exception:
                continue
            if m.get("sid") and isinstance(m.get("kv"), dict):
                out[m["sid"]] = m["kv"]
    return out


def expected_pos(anchors, deltas, plan):
    """Where the host's context must be after resuming this plan, from the log."""
    a = next((x for x in anchors if int(x["turn"]) == plan["anchor_turn"]), None)
    if a is None:
        return None
    pos = int(a.get("base_n_tokens", 0))
    for t in plan["replay"]:
        d = next((x for x in deltas if int(x["turn"]) == t), None)
        if d is None:
            return None
        pos += len(d.get("_tokens") or d.get("tokens") or [])
    return pos


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--map", required=True)
    ap.add_argument("--sid", default=None)
    ap.add_argument("--turn", type=int, default=None, help="skip the sid lookup")
    ap.add_argument("--kv", default=None, help="KV dir (default: the map's dir)")
    ap.add_argument("--run", action="store_true", help="actually resume in the C host")
    ap.add_argument("--exe", default=DEFAULT_EXE)
    ap.add_argument("--model", default=DEFAULT_MODEL)
    ap.add_argument("--backend", default=DEFAULT_BACKEND)
    args = ap.parse_args()

    kvdir = args.kv or os.path.dirname(os.path.abspath(args.map))
    plan = None
    if args.turn is not None:
        plans = load_map(args.map)
        plan = next((p for p in plans.values() if p["turn"] == args.turn), None)
        src = "turn %d" % args.turn
    elif args.sid:
        plan = load_map(args.map).get(args.sid)
        src = args.sid
    else:
        return print("need --sid or --turn") or 2
    if plan is None:
        print("no KV frame for %s" % src)
        return 1

    anchors, deltas = bridge.read_kv_log(os.path.join(kvdir, "log.jsonl"))
    want = expected_pos(anchors, deltas, plan)
    # The host re-derives the plan from the log, so the map's own fields must agree
    # with that derivation or the map is the thing that is wrong (a stale map is the
    # only failure mode here — the log is append-only, blobs never move).
    derived = bridge.seek_plan(anchors, deltas, plan["turn"])
    agree = (derived is not None
             and derived["anchor_turn"] == plan["anchor_turn"]
             and list(derived["replay"]) == list(plan["replay"])
             and list(derived["anchor_blob"]) == list(plan["anchor_blob"]))
    print("sid   %s" % (args.sid or "-"))
    print("frame turn %d (frame_sid %s), anchor t%d blob[%d,%d) fnv %d, replay %s"
          % (plan["turn"], plan.get("frame_sid"), plan["anchor_turn"],
             plan["anchor_blob"][0], plan["anchor_blob"][1], plan["anchor_blob"][2],
             plan["replay"]))
    print("map-vs-log %s%s" % ("PASS" if agree else "FAIL",
                               "" if agree or derived is None else
                               " (log derives anchor t%d replay %s)"
                               % (derived["anchor_turn"], derived["replay"])))
    print("expect pos %s from the log (anchor base_n_tokens + replay tokens)" % want)

    if not args.run:
        return 0 if agree else 1
    if not os.path.exists(args.exe):
        print("host exe missing: %s" % args.exe)
        return 1
    cmd = [args.exe, args.model, kvdir, args.backend, str(max(1, plan["turn"])),
           "seek=%d" % plan["turn"]]
    env = dict(os.environ)
    env["PATH"] = args.backend + os.pathsep + env.get("PATH", "")
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=900, env=env)
    out = (p.stdout or "") + (p.stderr or "")
    line = next((l for l in out.splitlines() if l.startswith("seek t")), None)
    print("host  %s" % (line or out.strip().splitlines()[-1:] or "no output"))
    if p.returncode != 0 or not line:
        print("resume FAILED (exit %d)" % p.returncode)
        return 1
    got = int(line.split("pos=")[1].split()[0])
    ok = (want is None or got == want) and agree
    print("pos   %d %s%s" % (got, "MATCH" if (want is None or got == want) else
                             "MISMATCH (log implies %s)" % want,
                             "" if agree else " | map-vs-log FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
