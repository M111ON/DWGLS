# -*- coding: utf-8 -*-
# Recovery step 3: pull gap sources from Vectorize (via cloud worker /search)
# + index vault exported_md files whose names/content touch gap dates.
import json, os, re, urllib.request, urllib.error
from datetime import datetime, timezone

OUT = r"I:\DWGLS-native-fs\_recovery\gap_2026-09-04_to_09-15"
os.makedirs(OUT, exist_ok=True)

# worker base — try common env/port; fallback localhost defaults
CAND_BASES = []
import os as _os
for k in ("CLOUD_MEMORY_URL", "CLOUD_MEMORY_BASE", "MEMORY_URL", "CLOUD_MEMORY_WORKER"):
    v = _os.environ.get(k)
    if v:
        CAND_BASES.append(v.rstrip("/"))
CAND_BASES += [
    "http://127.0.0.1:8787",
    "http://127.0.0.1:8788",
    "http://127.0.0.1:8390",
    "https://cloud-memory-worker.example.workers.dev",  # placeholder, skipped if fails
]

# queries to recover gap days (each will be written into 03_vectorize_search.md)
QUERIES = [
    "2026-09-04 DWGLS session",
    "2026-09-05 DWGLS session day trail",
    "2026-09-06 dwgls report",
    "2026-09-07 DWGLS work",
    "2026-09-08 DWGLS work",
    "2026-09-09 hermes session",
    "2026-09-10 DWGLS work",
    "2026-09-11 DWGLS work",
    "2026-09-12 DWGLS marathon",
    "2026-09-13 planet-v2 journal BFS commits",
    "2026-09-14 pool session",
    "2026-09-15 restore recovery sync report",
    "marathon Sep 12-14 structured findings inner field KIS codec",
]

def try_search(base, q, k=5):
    body = json.dumps({"q": q, "k": k}).encode("utf-8")
    req = urllib.request.Request(
        base.rstrip("/") + "/search",
        data=body,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=15) as resp:
        return json.loads(resp.read().decode("utf-8"))

# discover a working base
base = None
for b in CAND_BASES:
    if b.startswith("http://example") or "placeholder" in b:
        continue
    try:
        try_search(b, "ping", k=1)
        base = b
        break
    except Exception:
        continue

lines = ["# Recovery: Vectorize cloud search (gap 2026-09-04 .. 2026-09-15)", ""]
if not base:
    lines.append("**No worker base reachable from env/candidates.**")
    lines.append("Tried: " + ", ".join(CAND_BASES))
    lines.append("")
    lines.append("Fall back: search manually via cloud-memory_search_memory tool, or set CLOUD_MEMORY_URL.")
else:
    lines.append(f"Worker: `{base}`")
    lines.append("")
    for q in QUERIES:
        lines.append(f"## Q: {q}")
        try:
            res = try_search(base, q, k=5)
            # res may be list or {results:[]}
            items = res if isinstance(res, list) else res.get("results") or res.get("hits") or []
            lines.append(f"hits: {len(items)}")
            for it in items:
                sid = it.get("source_file") or it.get("source") or "?"
                sc = it.get("score", it.get("similarity"))
                txt = (it.get("text") or it.get("content") or "")[:1500]
                lines.append(f"### source=`{sid}` score={sc}")
                lines.append("```")
                lines.append(txt)
                lines.append("```")
                lines.append("")
        except Exception as e:
            lines.append(f"ERROR: {e}")
            lines.append("")
        lines.append("---")

outp = os.path.join(OUT, "03_vectorize_search.md")
with open(outp, "w", encoding="utf-8") as f:
    f.write("\n".join(lines))
print(f"vectorize -> {outp} ({os.path.getsize(outp)} bytes, base={base})")

# --- vault exported_md: copy/link gap-relevant files list ---
VAULT = r"I:\.vault\exported_md"
if os.path.isdir(VAULT):
    # filename pattern may contain ses_ or date; also check file mtime during gap
    GAP0 = datetime(2026, 9, 4).timestamp()
    GAP1 = datetime(2026, 9, 16).timestamp()
    picked = []
    for fn in os.listdir(VAULT):
        fp = os.path.join(VAULT, fn)
        if not os.path.isfile(fp):
            continue
        try:
            st = os.stat(fp)
        except OSError:
            continue
        # mtime in gap OR name hints
        name_hit = bool(re.search(r"2026-09-(0[4-9]|1[0-5])", fn))
        time_hit = GAP0 <= st.st_mtime < GAP1
        if name_hit or time_hit:
            picked.append((st.st_mtime, fn, st.st_size, "mtime" if time_hit else "name"))
    picked.sort()
    idx = ["# Recovery: I:\\.vault\\exported_md gap-related", "",
           f"files: {len(picked)} (mtime in gap OR name contains gap date)", ""]
    for mt, fn, sz, why in picked:
        d = datetime.fromtimestamp(mt, tz=timezone.utc).isoformat()
        idx.append(f"- `{fn}` ({sz} B, mtime={d}, why={why})")
    with open(os.path.join(OUT, "03_vault_exported_md_index.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(idx))
    print(f"vault exported_md: {len(picked)} gap-related files indexed")
else:
    print("vault exported_md missing:", VAULT)
