#!/usr/bin/env python3
"""handoff.py — cross-session / cross-platform session handoff.

Push a session summary to BOTH channels:
  1. cloud-memory worker (remote): POST /mcp memory_remember — server-side
     embedding (Workers AI bge-m3) -> D1 + Vectorize. Searchable from ANY
     platform/client that has the API key (opencode MCP: search_memory).
  2. Obsidian vault (local): obsidian_mem endsession — project-convention
     note under [[Memory/Sessions/...]] on this machine.

Usage:
  python tools/handoff.py --summary "done X, next Y"
  python tools/handoff.py --file handoff.md
  python tools/handoff.py --summary "..." --proj DWGLS-native-fs --vault-only
  python tools/handoff.py --summary "..." --proj DWGLS-native-fs --cloud-only

Env:
  CLOUD_MEMORY_API_KEY — cloud-memory key (defaults to
  I:/tools/cloud-workspace/.cloud_memory_key)
"""

import argparse
import json
import os
import subprocess
import sys
import urllib.request
from datetime import datetime, timezone

WORKER_URL = "https://cloud-memory-worker.aexid03.workers.dev"
DEFAULT_PROJ = "DWGLS-native-fs"
KEY_CANDIDATES = [
    "I:/tools/cloud-workspace/.cloud_memory_key",
    r"C:\Users\Administrator.AVENTADOR\.config\opencode\tools\.env",
]
OBSIDIAN_VENV = "I:/tools/obsidian-memory/.venv/Scripts/python.exe"
OBSIDIAN_PY = "I:/tools/obsidian-memory/obsidian_mem.py"


def load_key():
    k = os.environ.get("CLOUD_MEMORY_API_KEY", "").strip()
    if k:
        return k
    for p in KEY_CANDIDATES:
        if os.path.isfile(p):
            for line in open(p, encoding="utf-8"):
                line = line.strip()
                if line.startswith("CLOUD_MEMORY_API_KEY="):
                    return line.split("=", 1)[1].strip().strip('"').strip("'")
                if len(line) == 32 and "=" not in line:  # raw key file
                    return line
    raise RuntimeError("CLOUD_MEMORY_API_KEY not found (env or key files)")


def mcp_call(key, name, args):
    body = {"jsonrpc": "2.0", "method": "tools/call", "id": 1,
            "params": {"name": name, "arguments": args}}
    req = urllib.request.Request(f"{WORKER_URL}/mcp",
                                 data=json.dumps(body).encode(), method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("X-API-Key", key)
    req.add_header("User-Agent", "Handoff/1.0")
    with urllib.request.urlopen(req, timeout=30) as resp:
        j = json.loads(resp.read())
    err = j.get("error")
    if err:
        raise RuntimeError(f"MCP {name}: {err}")
    content = j.get("result", {}).get("content", [{}])
    return content[0].get("text", "") if content else ""


def push_cloud(key, summary, proj):
    # Vectorize caps chunk ids at 64 bytes and memory_remember builds
    # id = f"{source}/{ts}_{hash16}" (ts=13 + '_' + 16 hex + '/' = 31 bytes),
    # so source must stay well under ~33 bytes.
    slug = proj.lower().replace("-", "")[:12]
    source = f"shf/{slug}/{datetime.now(timezone.utc).strftime('%Y-%m-%d')}"
    out = mcp_call(key, "memory_remember",
                   {"text": summary, "source": source, "api_key": key})
    print(f"[cloud-memory] pushed to '{source}'")
    print(f"               {out.strip()[:300]}")
    return source


def push_obsidian(summary, proj):
    if not (os.path.isfile(OBSIDIAN_VENV) and os.path.isfile(OBSIDIAN_PY)):
        print("[obsidian] skipped: obsidian_mem not found at expected paths")
        return False
    cmd = [OBSIDIAN_VENV, OBSIDIAN_PY, "endsession", summary, "--proj", proj]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
    if r.returncode != 0:
        print(f"[obsidian] FAILED rc={r.returncode}: {r.stderr[-400:]}")
        return False
    print(f"[obsidian] note written: {r.stdout.strip()[-300:]}")
    return True


def verify(key, term):
    out = mcp_call(key, "search_memory", {"q": term, "k": 3})
    try:
        data = json.loads(out)
    except json.JSONDecodeError:
        print(f"[verify] raw: {out[:300]}")
        return
    hits = data.get("results", [])
    print(f"[verify] search '{term}' -> {len(hits)} hits")
    for h in hits[:3]:
        print(f"         {h.get('score', 0):.3f}  {h.get('source_file')}: {h.get('text', '')[:90]}")


def main():
    ap = argparse.ArgumentParser(description="cross-session/cross-platform handoff")
    ap.add_argument("--summary", default="", help="handoff summary text")
    ap.add_argument("--file", default=None, help="read summary from file")
    ap.add_argument("--proj", default=DEFAULT_PROJ)
    ap.add_argument("--cloud-only", action="store_true")
    ap.add_argument("--vault-only", action="store_true")
    ap.add_argument("--no-verify", action="store_true")
    args = ap.parse_args()

    summary = args.summary
    if args.file:
        summary = open(args.file, encoding="utf-8").read().strip()
    if not summary.strip():
        ap.error("need --summary or --file")

    key = load_key()
    if not args.vault_only:
        push_cloud(key, summary, args.proj)
    if not args.cloud_only:
        push_obsidian(summary, args.proj)

    if not args.no_verify and not args.vault_only:
        term = summary.strip().splitlines()[0][:60].strip() or "handoff"
        verify(key, term)
    print("[done]")


if __name__ == "__main__":
    main()