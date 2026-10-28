#!/usr/bin/env python3
"""kv_index_supervisor.py — keep the KV index server alive without touching :8096.

The memory index has `ann_keepalive.py`, which is pinned to :8096 and prefers
`build/ann_search_batch.exe`. A second index needs its own keeper, or it becomes
a silent single point of failure: when the process dies nothing restarts it and
lookups fall back to "no mapped hit" while `/health` on the memory side still
says ok.

Contract:
  * never touches the memory port (refuses if --port equals it)
  * one supervisor per index dir (pid file, --take to replace a live one)
  * health = GET /health on --port, `entries > 0` and `routed == 1` optional
  * restart with backoff, and every start/stop/exit is appended to --log

    python kv_index_supervisor.py --dir I:\\Temp\\kvsplit\\kv --port 8510 \\
        --once            # single check (cron/task friendly)
    python kv_index_supervisor.py --dir ... --port 8510 --serve   # loop
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request

ANN_REPO = r"I:\tools\ann"
EXE = os.path.join(ANN_REPO, "build", "ann_search.exe")
MEM_PORT = 8096
FILES = ("ann_index.jsonl", "ann_tags.tsv", "ann_anchors.bin",
         "ann_anchors-roster.bin", "ann_index.order")


def health(port, timeout=8):
    try:
        with urllib.request.urlopen("http://127.0.0.1:%d/health" % port, timeout=timeout) as r:
            return json.loads(r.read().decode())
    except (urllib.error.URLError, OSError, ValueError):
        return None


def env_for(d):
    e = dict(os.environ)
    e.update({
        "ANN_PORT": str(d["port"]),
        "ANN_INDEX_PATH": os.path.join(d["dir"], "ann_index.jsonl"),
        "ANN_TAGS_PATH": os.path.join(d["dir"], "ann_tags.tsv"),
        "ANN_ANCH_PATH": os.path.join(d["dir"], "ann_anchors.bin"),
        "ANN_ROSTER_PATH": os.path.join(d["dir"], "ann_anchors-roster.bin"),
        "ANN_ORDER_PATH": os.path.join(d["dir"], "ann_index.order"),
        "ANN_ROUTE_LOG": os.path.join(d["dir"], "route.log"),
        "ANN_ROSTER": "1", "ANN_TOTEM_WARM": "1", "ANN_FLOAT_KERNEL": "1",
        "ANN_WALK_PRUNE": "1", "ANN_WALK_FLOOR": "0.5",
        "ANN_DOOR": "0", "ANN_DOOR_EXIT": "0",
    })
    return e


def logline(path, msg):
    line = "%s %s\n" % (time.strftime("%Y-%m-%dT%H:%M:%S"), msg)
    try:
        with open(path, "a", encoding="utf-8") as f:
            f.write(line)
    except OSError:
        pass
    print(line.rstrip())


def start(d):
    """Start the server detached; returns the pid or None."""
    missing = [f for f in FILES if not os.path.exists(os.path.join(d["dir"], f))]
    if missing:
        logline(d["log"], "REFUSED start: missing %s" % ",".join(missing))
        return None
    out = open(os.path.join(d["dir"], "supervisor.out"), "ab")
    p = subprocess.Popen([EXE, "--serve", str(d["port"])], cwd=d["dir"],
                         env=env_for(d), stdout=out, stderr=subprocess.STDOUT,
                         creationflags=getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0))
    with open(d["pid"], "w", encoding="utf-8") as f:
        f.write(str(p.pid))
    logline(d["log"], "start pid %d port %d dir %s" % (p.pid, d["port"], d["dir"]))
    return p.pid


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True)
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--log", default=None)
    ap.add_argument("--pid", default=None)
    ap.add_argument("--once", action="store_true", help="one check, then exit")
    ap.add_argument("--serve", action="store_true", help="loop")
    ap.add_argument("--interval", type=int, default=15)
    ap.add_argument("--require-routed", action="store_true",
                    help="also require routed==1 (a fresh index boots routed 0)")
    args = ap.parse_args()

    if args.port == MEM_PORT:
        print("refusing: %d is the memory index port" % MEM_PORT)
        return 2
    if not os.path.isdir(args.dir):
        print("no such index dir: %s" % args.dir)
        return 2
    d = {"dir": os.path.abspath(args.dir), "port": args.port,
         "log": args.log or os.path.join(args.dir, "supervisor.log"),
         "pid": args.pid or os.path.join(args.dir, "supervisor.pid")}

    def check():
        h = health(d["port"])
        if h is None:
            return False, "down"
        if h.get("entries", 0) <= 0:
            return False, "up but entries %s" % h.get("entries")
        if args.require_routed and h.get("routed") != 1:
            return False, "up but routed %s" % h.get("routed")
        return True, "ok entries %d routed %s" % (h.get("entries"), h.get("routed"))

    def ensure():
        ok, why = check()
        if ok:
            return True, why
        logline(d["log"], "unhealthy: %s" % why)
        if start(d) is None:
            return False, "start refused"
        for _ in range(20):
            time.sleep(1.5)
            ok, why = check()
            if ok:
                logline(d["log"], "recovered %s" % why)
                return True, why
        logline(d["log"], "FAILED to recover: %s" % why)
        return False, why

    if not args.serve:
        ok, why = ensure()
        if ok:
            logline(d["log"], "healthy %s" % why)
        return 0 if ok else 1

    logline(d["log"], "supervising %s on :%d every %ds" % (d["dir"], d["port"], args.interval))
    while True:
        ensure()
        time.sleep(args.interval)


if __name__ == "__main__":
    sys.exit(main())
