#!/usr/bin/env python3
"""ann_regrow_upgrade_probe.py — verify the mtime-tolerant unchanged fast path.

`ann_search` used to require `index_mtime` equality for the unchanged path, so a
byte-identical copy or restore (which changes the mtime) fell through to a full
kmeans re-derive: 59 s at 63,915 rows. The fix accepts size + anchors checksum +
the last line still carrying `last_sid` when the mtime differs.

Four cases, each on its own copy, each booted and queried:

  A  copy, mtime dropped          -> must be "unchanged" (this is the fix)
  B  copy, mtime dropped, +32 rows-> must be "incremental"
  C  copy, last line's sid edited -> must NOT be "unchanged" (negative control)
  D  copy with mtime preserved    -> must be "unchanged" (old contract intact)

Usage:
  python ann_regrow_upgrade_probe.py --index-dir I:/tools/ann/data/memcore \
      --work I:/Temp/annup --port 8491 [--append-rows I:/Temp/kvreal3/ann_rows32.jsonl]
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
FILES = ("ann_index.jsonl", "ann_index.jsonl.stamp", "ann_tags.tsv",
         "ann_anchors.bin", "ann_anchors-state.json", "ann_anchors-roster.bin",
         "ann_index.order", "tag_lut.json")


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


def prepare(src, dst, keep_mtime, append_rows=None, tamper_tail=False,
            tamper_head=False):
    os.makedirs(dst, exist_ok=True)
    for f in FILES:
        s, d = os.path.join(src, f), os.path.join(dst, f)
        if not os.path.exists(s):
            continue
        if os.path.exists(d) and os.path.getsize(d) == os.path.getsize(s):
            continue
        if keep_mtime:
            shutil.copy2(s, d)
        else:
            shutil.copyfile(s, d)          # content only: mtime becomes "now"
    idx = os.path.join(dst, "ann_index.jsonl")
    if append_rows:
        with open(append_rows, encoding="utf-8") as f:
            rows = [json.loads(l) for l in f if l.strip()]
        with open(idx, "ab") as f:
            for r in rows:
                f.write(json.dumps({"sid": r["sid"], "file": r["file"],
                                    "npast": r["npast"], "dim": r["dim"],
                                    "vec": r["vec"]}).encode() + b"\n")
    if tamper_tail or tamper_head:
        with open(idx, "rb+") as f:
            if tamper_head:
                blob = f.read(4096)
                pos = blob.find(b'"sid"')
                start = 0
            else:
                f.seek(0, os.SEEK_END)
                end = f.tell()
                start = max(0, end - 65536)
                f.seek(start)
                blob = f.read()
                pos = blob.rfind(b'"sid"')
            if pos < 0:
                raise SystemExit("no sid on the line to tamper with")
            colon = blob.find(b':', pos)
            quote = blob.find(b'"', colon + 1)
            if quote < 0:
                raise SystemExit("sid value is not quoted; cannot tamper in place")
            # change one hex digit in place: same size, different content
            at = start + quote + 1
            f.seek(at)
            ch = f.read(1)
            f.seek(at)
            f.write(b"0" if ch != b"0" else b"1")
    return idx


def run_case(name, work, port, expect, note=""):
    proc, h, t_boot = boot(work, port)
    try:
        rg = post(port, "/v1/state/regrow", {})
        inner = rg.get("regrow") or {}
        mode = inner.get("mode")
        ms = inner.get("ms")
        h2 = get(port, "/health")
        # one query, to prove the server still serves after this path
        served = None
        with open(os.path.join(work, "ann_index.jsonl"), "rb") as f:
            first = json.loads(f.readline())
        if first.get("vec"):
            r = post(port, "/v1/state/search", {"vec": first["vec"], "topk": 2})
            served = len(r.get("hits") or [])
        verdict = "PASS" if mode == expect else "FAIL"
        print("  %-34s mode %-12s ms %-10s boot %.1f s entries %s served %s  [%s]"
              % (name, mode, ("%.1f" % ms) if ms is not None else "-", t_boot,
                 h2.get("entries"), served, verdict))
        return {"case": name, "note": note, "expect": expect, "mode": mode,
                "ms": ms, "boot_s": round(t_boot, 1),
                "entries": h2.get("entries"), "served": served,
                "pass": mode == expect}
    finally:
        stop(proc)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--index-dir", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--port", type=int, default=8491)
    ap.add_argument("--append-rows", default=None)
    ap.add_argument("--only", default="",
                    help="run one case only: A, B, C or D")
    args = ap.parse_args()

    res = []
    only = args.only.upper()
    a = os.path.join(args.work, "a_mtime_dropped")
    if not only or only == "A":
        prepare(args.index_dir, a, keep_mtime=False)
        res.append(run_case("A copy, mtime dropped", a, args.port, "unchanged",
                            "the fix: byte-identical copy must not re-derive"))

    d = os.path.join(args.work, "d_mtime_kept")
    if not only or only == "D":
        prepare(args.index_dir, d, keep_mtime=True)
        res.append(run_case("D copy, mtime kept", d, args.port + 1, "unchanged",
                            "old contract still holds"))

    if args.append_rows and (not only or only == "B"):
        b = os.path.join(args.work, "b_appended")
        prepare(args.index_dir, b, keep_mtime=False, append_rows=args.append_rows)
        res.append(run_case("B copy, mtime dropped, +rows", b, args.port + 2,
                            "incremental", "append path unchanged by the fix"))

    c = os.path.join(args.work, "c_tail_tampered")
    if not only or only == "C":
        prepare(args.index_dir, c, keep_mtime=False, tamper_tail=True)
        res.append(run_case("C copy, last sid edited", c, args.port + 3, "full",
                            "negative control: the tail check must refuse"))

    u = os.path.join(args.work, "u_state_upgrade")
    u_ok = False
    if not only or only == "U":
        prepare(args.index_dir, u, keep_mtime=True)
        r = run_case("U state upgraded in place", u, args.port + 5, "unchanged",
                     "an old state gets index_fp while the mtime still proves it")
        st = os.path.join(u, "ann_anchors-state.json")
        fp = 0
        if os.path.exists(st):
            with open(st, encoding="utf-8") as f:
                fp = json.load(f).get("index_fp", 0)
        r["index_fp"] = fp
        r["pass"] = r["pass"] and fp > 0
        print("    state index_fp %s" % (fp or "ABSENT"))
        res.append(r)
        u_ok = fp > 0

    e = os.path.join(args.work, "e_head_tampered")
    if not only or only == "E":
        src = u if (u_ok or os.path.exists(os.path.join(u, "ann_anchors-state.json"))) \
            else args.index_dir
        prepare(src, e, keep_mtime=True, tamper_head=True)
        res.append(run_case("E copy, FIRST sid edited", e, args.port + 4, "full",
                            "negative control: the fingerprint must see the head"))

    dest = os.path.join(args.work, "ann_regrow_upgrade.json")
    with open(dest, "w", encoding="utf-8") as f:
        json.dump(res, f, indent=1)
    bad = [r for r in res if not r["pass"]]
    print("  receipt %s" % dest)
    print("  %d/%d cases behaved as designed" % (len(res) - len(bad), len(res)))
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
