# -*- coding: utf-8 -*-
# Recovery step 2: extract gap-day (2026-09-04..15) content from
# cloud-memory-sessions.json + cloud-memory-capture.log -> markdown files.
import json, re, os
from datetime import datetime, timezone

OUT = r"I:\DWGLS-native-fs\_recovery\gap_2026-09-04_to_09-15"
os.makedirs(OUT, exist_ok=True)
GAP_START = datetime(2026, 9, 4, tzinfo=timezone.utc)
GAP_END = datetime(2026, 9, 16, tzinfo=timezone.utc)  # exclusive

def in_gap(dt):
    return dt is not None and GAP_START <= dt < GAP_END

def iso_ms(ts):
    try:
        if ts is None:
            return None
        if isinstance(ts, str):
            ts = float(ts)
        if ts > 1e12:
            ts = ts / 1000.0
        return datetime.fromtimestamp(ts, tz=timezone.utc)
    except Exception:
        return None

# --- source 1: cloud-memory-sessions.json ---
# structure: {_name:{sid:title}, _cp:{sid:ts}, _msgs:{sid:[{text,at}]}, ses_*:uuid}
sp = r"C:\Users\Administrator.AVENTADOR\.config\opencode\cloud-memory-sessions.json"
data = json.load(open(sp, encoding="utf-8"))
names = data.get("_name") or {}
cps = data.get("_cp") or {}
all_msgs = data.get("_msgs") or {}

gap_sids = []
for sid, lst in all_msgs.items():
    hit = False
    for m in lst or []:
        if isinstance(m, dict) and in_gap(iso_ms(m.get("at"))):
            hit = True
            break
    if not hit:
        cp = cps.get(sid)
        if cp is not None and in_gap(iso_ms(cp)):
            hit = True
    if hit:
        gap_sids.append(sid)

# sort by cp time
gap_sids.sort(key=lambda s: cps.get(s) or 0)

lines = []
lines.append("# Recovery: cloud-memory-sessions.json (gap 2026-09-04 .. 2026-09-15)")
lines.append("")
lines.append(f"matched sessions: {len(gap_sids)} / {len(all_msgs)} with _msgs")
lines.append("")
for sid in gap_sids:
    lines.append("---")
    lines.append("")
    title = names.get(sid, "")
    cp = cps.get(sid)
    cp_iso = iso_ms(cp)
    cp_str = cp_iso.isoformat() if cp_iso else "?"
    lines.append(f"## {title or sid}")
    lines.append(f"- id: `{sid}`")
    lines.append(f"- last_cp: {cp_str}")
    lines.append("")
    for m in all_msgs.get(sid) or []:
        if not isinstance(m, dict):
            continue
        dt = iso_ms(m.get("at"))
        text = (m.get("text") or "").strip()
        if not text:
            continue
        tag = "✓gap" if in_gap(dt) else (dt.date().isoformat() if dt else "?")
        dstr = dt.isoformat() if dt else "?"
        lines.append(f"### [{dstr}] ({tag})")
        lines.append("")
        lines.append(text[:6000])
        lines.append("")

out1 = os.path.join(OUT, "01_sessions_json.md")
with open(out1, "w", encoding="utf-8") as f:
    f.write("\n".join(lines))
print(f"sessions.json: {len(gap_sids)} sessions in gap -> 01_sessions_json.md ({os.path.getsize(out1)} bytes)")

# --- source 2: cloud-memory-capture.log ---
logp = r"C:\Users\Administrator.AVENTADOR\.config\opencode\cloud-memory-capture.log"
day_re = re.compile(r"2026-09-(0[4-9]|1[0-5])")
per_day = {}
n_hits = 0
with open(logp, "r", encoding="utf-8", errors="replace") as f:
    for line in f:
        m = day_re.search(line[:400])
        if not m:
            continue
        day = m.group(0)
        per_day.setdefault(day, []).append(line.rstrip("\n"))
        n_hits += 1

idx = ["# Recovery: cloud-memory-capture.log (gap days)", ""]
for day in sorted(per_day):
    rows = per_day[day]
    fn = f"02_capture_{day}.md"
    with open(os.path.join(OUT, fn), "w", encoding="utf-8") as f:
        f.write(f"# capture.log {day} ({len(rows)} lines)")
        f.write("\n\n")
        for ln in rows:
            f.write(ln + "\n")
    idx.append(f"- `{fn}`: {len(rows)} lines")
    print(f"  capture {day}: {len(rows)} lines")
with open(os.path.join(OUT, "02_capture_index.md"), "w", encoding="utf-8") as f:
    f.write("\n".join(idx))
print(f"capture.log: {n_hits} gap lines across {len(per_day)} days")

with open(os.path.join(OUT, "README.md"), "w", encoding="utf-8") as f:
    f.write(f"""# Gap recovery 2026-09-04 .. 2026-09-15
Generated: {datetime.now(timezone.utc).isoformat()}

## Sources in this folder
1. `01_sessions_json.md` — msgs from cloud-memory-sessions.json ({len(gap_sids)} sessions touched gap)
2. `02_capture_*.md` — activity lines from cloud-memory-capture.log ({n_hits} lines, {len(per_day)} days)
3. `02_capture_index.md` — index of capture files

## Sources still to extract (step 3+)
- `I:\\.vault\\exported_md\\` marathon Sep 12-14 (395 files)
- Vectorize day srcs: shf/2026-09-05, dwgls-report-2026-09-06, hm_20260909, hm_20260915/16 marathon findings
""")
print("OUT =", OUT)
