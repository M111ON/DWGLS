#!/usr/bin/env python3
"""kv_expiry_probe.py — does a reaped (expired) lease invalidate a KV frame?

The lease layer and the KV log are two different things, and it matters which
one owns correctness:

  * the lease owns SPACE (a block returns to the free list when the lease ages
    out), and it is what a subagent reserves before writing;
  * the log owns BYTES (anchors + deltas + blob, append-only).

So the question this probe answers with numbers is: after the lease is reaped,
can the frame still be resumed? The expected answer is yes — reap closes the
block, it does not delete the payload — and that means an expired lease must
NEVER be used as "this frame is gone". The only thing that invalidates a plan
is a byte-level gate (anchor blob FNV / offset / replay turns in the log).

    python kv_expiry_probe.py --kv DIR --map MAP --lease TABLE [--port 8510]
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import kv_ann_bridge as bridge
import kv_lookup as lk
from kv_ann_open import load_map

CLI = os.path.join(ROOT, "build", "kv_lease_cli.exe")


def cli(*args):
    p = subprocess.run([CLI] + [str(a) for a in args], capture_output=True, text=True)
    return p.returncode, ((p.stdout or "") + (p.stderr or "")).strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kv", required=True)
    ap.add_argument("--map", required=True)
    ap.add_argument("--lease", default=None)
    ap.add_argument("--resume", action="store_true", help="actually resume in the C host")
    ap.add_argument("--exe", default=os.path.join(ROOT, "build", "kv_temporal_llama.exe"))
    ap.add_argument("--model", default=r"I:\model\Qwen2.5-0.5B-Instruct-Q8_0.gguf")
    ap.add_argument("--backend", default=r"I:\llama\llama-v040-bin-win-vulkan-x64")
    args = ap.parse_args()

    lease = args.lease or os.path.join(args.kv, "lease.table")
    if os.path.exists(lease):
        os.remove(lease)
    rc, out = cli("init", lease, 1024, 256, 100)     # total, quota, 100 ms lease
    print("init     rc=%d %s" % (rc, out))

    rc, out = cli("reserve", lease, 42, 128)          # owner 42, 128 slots
    print("reserve  rc=%d %s" % (rc, out))
    idx, gen = (out.split() + ["-1", "-1"])[:2]
    rc, out = cli("write", lease, idx, gen, 64)
    print("write    rc=%d %s" % (rc, out))

    plans = load_map(args.map)
    sid = sorted(plans)[0]
    plan = plans[sid]
    kv = lk.load_kv(args.kv)
    ok, why = lk.check_plan(plan, kv[0], kv[1], kv[2], kv[3])
    print("plan     %s (t%s) %s" % (sid[:12], plan["turn"], "PASS " + why if ok else "FAIL " + why))

    time.sleep(0.4)                                   # lease is 100 ms: let it age out
    rc, out = cli("reap", lease)
    print("reap     rc=%d %s" % (rc, out))
    rc, out = cli("status", lease)
    print("status   rc=%d %s" % (rc, out))

    ok2, why2 = lk.check_plan(plan, kv[0], kv[1], kv[2], kv[3])
    print("plan after reap: %s" % ("PASS " + why2 if ok2 else "FAIL " + why2))

    rc, out = cli("write", lease, idx, gen, 64)       # stale handle against a reaped block
    print("stale write rc=%d %s" % (rc, out))

    if args.resume and ok2:
        cmd = [args.exe, args.model, args.kv, args.backend, str(max(1, plan["turn"])),
               "seek=%d" % plan["turn"]]
        env = dict(os.environ)
        env["PATH"] = args.backend + os.pathsep + env.get("PATH", "")
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=900, env=env)
        line = next((l for l in ((p.stdout or "") + (p.stderr or "")).splitlines()
                     if l.startswith("seek t")), None)
        print("resume   %s (exit %d)" % (line or "no output", p.returncode))

    verdict = ok and ok2
    print("\nVERDICT  a reaped lease leaves the frame resumable: %s" % ("YES" if verdict else "NO"))
    return 0 if verdict else 1


if __name__ == "__main__":
    sys.exit(main())
