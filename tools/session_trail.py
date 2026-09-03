#!/usr/bin/env python3
"""session_trail.py — temporary aggregation of session work, promoted on demand.

Pain point: a short handoff summary is not enough to resume work instantly.
This tool keeps a TEMP pool of everything done in sessions (commits, notes,
conversations, test results) in local JSONL — instantly browsable, no
embedding, works offline — and promotes it into durable memory when the
time comes:

    add             append a note/event to the trail pool
    collect-git     harvest recent commit messages + diff stat into the pool
                    (idempotent: only NEW commits since last collect are added)
    collect-vault   pull the session note from the obsidian vault
                    (update-in-place: one entry per note path, re-promotable)
    query           browse the pool (optional term filter)
    promote         embed unpromoted entries into cloud-memory DB + obsidian
                    vault, then mark them promoted
    nudge           remind to promote when unpromoted trail content is older
                    than N hours (--popup shows a Windows message box)
    auto            scheduled catch-up: collect what a session-end hook missed
                    (new vault note / commits) + nudge
    schedule        register/unregister the hourly nudge task (schtasks /IT)
                    so a Windows popup nudges promote after N hours

Per-session flow (automatic):
  obsidian_mem endsession "summary" --proj <name>
      -> hooks session_trail collect-vault + collect-git automatically
         (guarded: only when tools/session_trail.py exists next to cwd)
  every hour   scheduled task -> `auto` (catch-up collect + nudge popup)

Manual flow:
  python tools/session_trail.py promote --proj <name>   # when nudged
  python tools/session_trail.py query  --proj <name>    # browse anytime
  python tools/session_trail.py schedule --after-hours 6 --proj <name>
  python tools/session_trail.py schedule --uninstall --proj <name>

Env:
  TRAIL_ROOT            temp pool dir (default I:/tools/cloud-workspace/trail)
  OBSIDIAN_VAULT        vault dir (default I:/Vaults/Memory)
  CLOUD_MEMORY_API_KEY  cloud-memory key (defaults like handoff.py)
  SESSION_TRAIL_PYTHON  python.exe for the scheduled task (auto-detected)
  SESSION_TRAIL_TOOL    explicit session_trail.py path for the endsession hook
"""

import argparse
import base64
import json
import os
import re
import shutil
import subprocess
import sys
from datetime import datetime, date, timedelta

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from handoff import load_key, mcp_call  # reuse: key lookup + worker MCP call

TRAIL_ROOT = os.environ.get("TRAIL_ROOT", "I:/tools/cloud-workspace/trail")
VAULT_DIR = os.environ.get("OBSIDIAN_VAULT", "I:/Vaults/Memory")
STATE_FILE = "state.json"
POPUP_PS1 = "nudge_popup.ps1"
POPUP_TITLE = "trail promote due"
SOURCE_SLUG_MAX = 33  # Vectorize id cap 64 - memory_remember id overhead 31

_HASH_RE = re.compile(r"^[0-9a-f]{7,40}\s+\d{4}-\d{2}-\d{2}\s")


def slugify(proj, n=10):
    return proj.lower().replace("-", "").replace("_", "")[:n]


def pool_dir(proj):
    d = os.path.join(TRAIL_ROOT, slugify(proj, 16) or "default")
    os.makedirs(d, exist_ok=True)
    return d


def pool_path(proj):
    return os.path.join(pool_dir(proj), date.today().strftime("%Y-%m") + ".jsonl")


def state_path(proj):
    return os.path.join(pool_dir(proj), STATE_FILE)


def load_entries(path):
    if not os.path.isfile(path):
        return []
    return [json.loads(l) for l in open(path, encoding="utf-8") if l.strip()]


def write_entries(path, entries):
    with open(path, "w", encoding="utf-8") as f:
        for e in entries:
            f.write(json.dumps(e, ensure_ascii=False) + "\n")


def load_state(proj):
    p = state_path(proj)
    if not os.path.isfile(p):
        return {}
    try:
        return json.load(open(p, encoding="utf-8"))
    except Exception:
        return {}


def save_state(proj, st):
    with open(state_path(proj), "w", encoding="utf-8") as f:
        json.dump(st, f, ensure_ascii=False, indent=2)


def bump_state(proj, **fields):
    st = load_state(proj)
    st.update(fields)
    save_state(proj, st)


def now_iso():
    return datetime.now().isoformat(timespec="seconds")


def append_entry(proj, kind, text, src=""):
    path = pool_path(proj)
    entry = {"ts": now_iso(), "kind": kind, "text": text.strip(),
             "promoted": False, "src": src}
    with open(path, "a", encoding="utf-8") as f:
        f.write(json.dumps(entry, ensure_ascii=False) + "\n")
    print(f"[add] {kind} entry appended to {os.path.relpath(path)}")
    return True


# ─── state helpers ───────────────────────────────────────────────────────

def last_collect_date(proj):
    """Collect window start date: state.last_collect if set, else None."""
    st = load_state(proj)
    return (st.get("last_collect") or "")[:10] or None


def all_hashes(entries):
    """Every commit hash already present in the pool (any month file)."""
    hashes = set()
    for e in entries:
        if e.get("kind") != "commits":
            continue
        for line in e["text"].splitlines():
            m = _HASH_RE.match(line)
            if m:
                hashes.add(m.group(0).split()[0])
    return hashes


# ─── collect ────────────────────────────────────────────────────────────

def collect_git(proj, days, since, repo):
    repo = repo or os.getcwd()
    st = load_state(proj)
    if not since:
        since = (st.get("last_collect") or "")[:10]
    if not since:
        since = (date.today() - timedelta(days=days)).isoformat()
    since = since + "T00:00:00"
    r = subprocess.run(
        ["git", "-C", repo, "log", f"--since={since}", "--pretty=format:%h %ad %s",
         "--date=short", "--stat"],
        capture_output=True, text=True, timeout=120)
    out = r.stdout.strip()
    if not out:
        print("[collect-git] no commits since", since[:10])
        return False

    # idempotent: keep whole commit blocks whose hash is not yet pooled
    pooldir = pool_dir(proj)
    have = set()
    for fn in os.listdir(pooldir):
        if fn.endswith(".jsonl"):
            have |= all_hashes(load_entries(os.path.join(pooldir, fn)))
    blocks, cur = [], None
    for l in out.splitlines():
        if _HASH_RE.match(l):
            cur = [l]
            blocks.append(cur)
        elif cur is not None:
            cur.append(l)
    new_blocks = [b for b in blocks if b[0].split()[0] not in have]
    if not new_blocks:
        print(f"[collect-git] nothing new since {since[:10]} "
              f"({len(have)} commits already pooled)")
        return False

    header = f"git commits ({since[:10]}..now) in {repo}"
    body = "\n".join("\n".join(b).rstrip() for b in new_blocks)
    append_entry(proj, "commits", header + "\n" + body, src=repo)
    newh = [b[0].split()[0] for b in new_blocks]
    bump_state(proj, last_collect=now_iso())
    print(f"[collect-git] {len(newh)} new commits: {' '.join(newh[:20])}")
    return True


def collect_vault(proj, d):
    """Pull today's session note from the vault. Update-in-place: exactly one
    entry per note path; text changes reset promoted so the delta re-promotes.
    """
    d = d or date.today().strftime("%Y-%m-%d")
    slug = slugify(proj, 20)
    cands = []
    for slug_cand in (slug, proj.lower()):
        for subdir in ("Sessions", "sessions"):
            cands.append(os.path.join(VAULT_DIR, subdir, f"{d}_{slug_cand}.md"))
    for p in cands:
        if os.path.isfile(p):
            text = open(p, encoding="utf-8").read()
            body = f"vault session note {d}\n\n{text}"
            path = pool_path(proj)
            entries = load_entries(path)
            for e in entries:
                if e.get("kind") == "session-note" and e.get("src") == p:
                    if e["text"] == body.strip():
                        print(f"[collect-vault] note unchanged: {p}")
                        return False
                    e["text"] = body.strip()
                    e["promoted"] = False  # content changed -> re-promote later
                    write_entries(path, entries)
                    bump_state(proj, last_collect=now_iso())
                    print(f"[collect-vault] note updated: {p}")
                    return True
            append_entry(proj, "session-note", body, src=p)
            bump_state(proj, last_collect=now_iso())
            return True
    print(f"[collect-vault] no session note for {d} (tried {len(cands)} paths)")
    return False


# ─── query ──────────────────────────────────────────────────────────────

def query(proj, term, unpromoted, limit, kinds):
    path = pool_path(proj)
    entries = load_entries(path)
    n = 0
    for e in reversed(entries):  # newest first
        if unpromoted and e.get("promoted"):
            continue
        if kinds and e.get("kind") not in kinds:
            continue
        if term and term.lower() not in e["text"].lower():
            continue
        flag = "" if e.get("promoted") else " (unpromoted)"
        print(f"--- [{e['ts']}] {e.get('kind','?')}{flag}")
        print(e["text"][:600])
        n += 1
        if limit and n >= limit:
            break
    print(f"[query] {n} entries shown of {len(entries)} total")


# ─── promote ────────────────────────────────────────────────────────────

def chunk_text(text, size=1800):
    """Split on newlines into <=size chunks (safe for bge-m3 + 64B id rules)."""
    chunks, cur = [], ""
    for line in text.splitlines():
        if len(cur) + len(line) + 1 > size:
            if cur:
                chunks.append(cur)
            cur = line[:size]
        else:
            cur = cur + "\n" + line if cur else line
    if cur:
        chunks.append(cur)
    return chunks or [""]


def promote(proj, all_entries, since_days, do_cloud, do_vault):
    path = pool_path(proj)
    entries = load_entries(path)
    if since_days:
        cutoff = datetime.now() - timedelta(days=since_days)
        pending = [e for e in entries if not e.get("promoted") and
                   datetime.fromisoformat(e["ts"]) >= cutoff]
    else:
        pending = [e for e in entries if not e.get("promoted")]
    if all_entries:
        pending = entries
    if not pending:
        print("[promote] nothing to promote")
        # keep the clock honest so nudge doesn't nag about old promoted work
        bump_state(proj, last_promote=now_iso())
        return

    slug = slugify(proj)
    date_s = date.today().strftime("%Y-%m-%d")
    source = f"trail/{slug}/{date_s}"  # <=33 bytes: id fits Vectorize cap
    full_text = "\n\n---\n\n".join(f"[{e['ts']}] {e.get('kind','?')}: {e['text']}"
                                   for e in pending)

    if do_cloud:
        key = load_key()
        chunks = chunk_text(full_text)
        pushed = 0
        for i, c in enumerate(chunks):
            out = mcp_call(key, "memory_remember",
                           {"text": c, "source": f"{source}/{i}", "api_key": key})
            if '"ok": true' in out:
                pushed += 1
        print(f"[promote] cloud-memory: {pushed}/{len(chunks)} chunks -> '{source}'")

    if do_vault:
        vdir = os.path.join(VAULT_DIR, "Trail", slug)
        os.makedirs(vdir, exist_ok=True)
        vpath = os.path.join(vdir, f"{date_s}-trail.md")
        block = (f"\n\n## {date_s} trail\n\n" + full_text)
        with open(vpath, "a", encoding="utf-8") as f:
            f.write(block)
        print(f"[promote] vault note appended -> {vpath}")

    # mark promoted (keep entries as history, flag flips)
    changed = False
    for e in entries:
        if (all_entries or not e.get("promoted")) and e in pending:
            e["promoted"] = True
            changed = True
    if changed:
        write_entries(path, entries)
        bump_state(proj, last_promote=now_iso())
    print(f"[promote] {len(pending)} entries marked promoted")


# ─── nudge / popup ──────────────────────────────────────────────────────

POPUP_PS1_SRC = r"""param([string]$msgFile)
$ErrorActionPreference = 'SilentlyContinue'
Add-Type -AssemblyName System.Windows.Forms | Out-Null
$body = Get-Content -Raw -Encoding UTF8 -LiteralPath $msgFile

$f = New-Object System.Windows.Forms.Form
$f.Text = 'trail promote due'
$f.StartPosition = 'CenterScreen'
$f.TopMost = $true
$f.Width = 560
$f.Height = 200
$f.FormBorderStyle = 'FixedDialog'
$f.MaximizeBox = $false
$f.MinimizeBox = $false

$l = New-Object System.Windows.Forms.Label
$l.Text = $body
$l.Left = 14; $l.Top = 12; $l.Width = 516; $l.Height = 120
$l.TextAlign = 'TopLeft'

$btn = New-Object System.Windows.Forms.Button
$btn.Text = 'OK'
$btn.Left = 220; $btn.Top = 138; $btn.Width = 120; $btn.Height = 26
$btn.Add_Click({ $f.Close() })

$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 45000
$timer.Add_Tick({ $timer.Stop(); $f.Close() })

$f.Controls.Add($l)
$f.Controls.Add($btn)
$timer.Start()
[void]$f.ShowDialog()
try { Remove-Item -LiteralPath $msgFile -Force -ErrorAction SilentlyContinue } catch {}
"""


def ensure_popup_ps1(proj):
    """(Re)write the popup helper so stale copies from older versions are
    refreshed, not kept forever."""
    p = os.path.join(pool_dir(proj), POPUP_PS1)
    try:
        old = open(p, encoding="utf-8").read()
    except OSError:
        old = None
    if old != POPUP_PS1_SRC:
        with open(p, "w", encoding="utf-8") as f:
            f.write(POPUP_PS1_SRC)
        print(f"[nudge] popup helper written -> {p}")
    return p


def show_popup(proj, body):
    ps1 = ensure_popup_ps1(proj)
    msg = os.path.join(pool_dir(proj), "nudge_msg.txt")
    with open(msg, "w", encoding="utf-8") as f:
        f.write(body)
    try:
        subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                        "-File", ps1, msg], timeout=180)
    except Exception as exc:
        print(f"[nudge] popup error: {exc}")


def nudge(proj, after_hours, popup):
    entries = load_entries(pool_path(proj))
    pending = [e for e in entries if not e.get("promoted")]
    if not pending:
        print("[nudge] nothing unpromoted — all trail content is promoted")
        bump_state(proj, last_promote=now_iso())  # no history to nag about
        return False
    newest = max(datetime.fromisoformat(e["ts"]) for e in pending)
    hours = (datetime.now() - newest).total_seconds() / 3600.0
    if hours < after_hours:
        print(f"[nudge] {len(pending)} unpromoted entries, newest {hours:.1f}h old "
              f"(nudge after {after_hours}h)")
        return False
    body = (f"Trail '{proj}': {len(pending)} unpromoted entries since "
            f"{newest:%Y-%m-%d %H:%M} ({hours:.0f}h ago).\n\n"
            f"Promote into cloud-memory DB + vault:\n"
            f"  python tools/session_trail.py promote --proj {proj}")
    print(f"[nudge] DUE: {len(pending)} unpromoted entries, oldest {hours:.0f}h")
    print(body)
    if popup:
        show_popup(proj, body)
    return True


# ─── scheduled auto + task registration ─────────────────────────────────

def auto(proj, after_hours, popup):
    """Hourly catch-up: collect anything a session-end hook missed, then nudge.
    Both collects are idempotent — safe to run every hour."""
    collect_vault(proj, date.today().isoformat())
    collect_git(proj, days=1, since=None, repo=None)
    nudge(proj, after_hours, popup)


def detect_python():
    env = os.environ.get("SESSION_TRAIL_PYTHON")
    if env and os.path.isfile(env):
        return env
    for cand in ("I:/tools/cloud-workspace/.venv/Scripts/python.exe",
                 "I:/tools/obsidian-memory/.venv/Scripts/python.exe"):
        if os.path.isfile(cand):
            return cand
    return shutil.which("python") or sys.executable


def task_name(proj):
    return "session-trail-" + (slugify(proj, 16) or "default")


def schedule(proj, after_hours, python_exe, uninstall):
    name = task_name(proj)
    if uninstall:
        r = subprocess.run(["schtasks", "/Delete", "/F", "/TN", name],
                           capture_output=True, text=True)
        print("[schedule] " + ((r.stdout or r.stderr).strip() or "task removed"))
        return
    tool = os.path.abspath(__file__)
    py = python_exe or detect_python()
    if not py or not os.path.isfile(py):
        print("[schedule] no python.exe found — pass --python PATH")
        return
    cmdline = (f'"{py}" "{tool}" auto --proj "{proj}" '
               f'--after-hours {after_hours} --popup')
    r = subprocess.run(["schtasks", "/Create", "/F", "/SC", "HOURLY", "/MO", "1",
                        "/TN", name, "/TR", cmdline, "/IT"],
                       capture_output=True, text=True)
    print("[schedule] " + ((r.stdout or r.stderr).strip()))
    q = subprocess.run(["schtasks", "/Query", "/TN", name, "/FO", "LIST"],
                       capture_output=True, text=True)
    ok = "SUCCESS" in (r.stdout + r.stderr) or r.returncode == 0
    print(f"[schedule] task '{name}' runs hourly: auto (collect + nudge "
          f"popup after {after_hours}h) — uninstall: "
          f"python tools/session_trail.py schedule --uninstall --proj {proj}")


def main():
    ap = argparse.ArgumentParser(description="temp session-trail pool + promote")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("add", help="append an entry")
    p.add_argument("--proj", default="DWGLS-native-fs")
    p.add_argument("--kind", default="note", choices=["note", "commit", "test",
                                                      "conversation", "handoff", "session-note"])
    p.add_argument("--text", default="")
    p.add_argument("--file", default=None)
    p.add_argument("--src", default="")

    p = sub.add_parser("collect-git")
    p.add_argument("--proj", default="DWGLS-native-fs")
    p.add_argument("--days", type=int, default=1)
    p.add_argument("--since", default=None)
    p.add_argument("--repo", default=None)

    p = sub.add_parser("collect-vault")
    p.add_argument("--proj", default="DWGLS-native-fs")
    p.add_argument("--date", default=None)

    p = sub.add_parser("query")
    p.add_argument("--proj", default="DWGLS-native-fs")
    p.add_argument("--term", default=None)
    p.add_argument("--unpromoted", action="store_true")
    p.add_argument("--limit", type=int, default=10)
    p.add_argument("--kinds", default="")

    p = sub.add_parser("promote")
    p.add_argument("--proj", default="DWGLS-native-fs")
    p.add_argument("--all", action="store_true", dest="all_entries")
    p.add_argument("--since-days", type=int, default=0)
    p.add_argument("--cloud-only", action="store_true")
    p.add_argument("--vault-only", action="store_true")

    p = sub.add_parser("nudge")
    p.add_argument("--proj", default="DWGLS-native-fs")
    p.add_argument("--after-hours", type=float, default=6.0)
    p.add_argument("--popup", action="store_true")

    p = sub.add_parser("auto", help="scheduled catch-up collect + nudge")
    p.add_argument("--proj", default="DWGLS-native-fs")
    p.add_argument("--after-hours", type=float, default=6.0)
    p.add_argument("--popup", action="store_true")

    p = sub.add_parser("schedule", help="register/unregister hourly nudge task")
    p.add_argument("--proj", default="DWGLS-native-fs")
    p.add_argument("--after-hours", type=float, default=6.0)
    p.add_argument("--python", default=None, dest="python_exe")
    p.add_argument("--uninstall", action="store_true")

    args = ap.parse_args()

    if args.cmd == "add":
        text = args.text
        if args.file:
            text = open(args.file, encoding="utf-8").read()
        if not text.strip():
            print("need --text or --file"); return
        append_entry(args.proj, args.kind, text, args.src)
    elif args.cmd == "collect-git":
        collect_git(args.proj, args.days, args.since, args.repo)
    elif args.cmd == "collect-vault":
        collect_vault(args.proj, args.date)
    elif args.cmd == "query":
        query(args.proj, args.term, args.unpromoted, args.limit,
              set(args.kinds.split(",")) if args.kinds else None)
    elif args.cmd == "promote":
        promote(args.proj, args.all_entries, args.since_days,
                not args.vault_only, not args.cloud_only)
    elif args.cmd == "nudge":
        nudge(args.proj, args.after_hours, args.popup)
    elif args.cmd == "auto":
        auto(args.proj, args.after_hours, args.popup)
    elif args.cmd == "schedule":
        schedule(args.proj, args.after_hours, args.python_exe, args.uninstall)


if __name__ == "__main__":
    main()
