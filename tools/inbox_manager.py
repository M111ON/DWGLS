#!/usr/bin/env python3
"""
inbox_manager.py — Standalone Inbox Manager (no MCP dependency)

CLI + HTTP dashboard for file management, versioning, board tracking.
Ported from inbox_mcp_server.py for local standalone use.

Usage:
  python tools/inbox_manager.py scan              # rescan workspace
  python tools/inbox_manager.py index             # show cached index
  python tools/inbox_manager.py put <file>        # ingest file
  python tools/inbox_manager.py put-zip <file>    # ingest ZIP
  python tools/inbox_manager.py incoming          # list pending
  python tools/inbox_manager.py apply             # apply all safe
  python tools/inbox_manager.py board list        # list cards
  python tools/inbox_manager.py board post <title> [--tags x,y]  # add card
  python tools/inbox_manager.py board update <id> <status>       # change status
  python tools/inbox_manager.py board tag <id> <tags>            # add tags
  python tools/inbox_manager.py board edit <id> <body>           # set body
  python tools/inbox_manager.py board claim <id> <owner>         # lock (owner working)
  python tools/inbox_manager.py board release <id> [--force]     # unlock
  python tools/inbox_manager.py dashboard         # start web dashboard
"""

import os, re, sys, json, hashlib, zipfile, io, time, csv
from datetime import datetime
from pathlib import Path
from http.server import HTTPServer, BaseHTTPRequestHandler
import threading, urllib.parse

# ── globals ──────────────────────────────────────────────────────────
SCRIPT_DIR = Path(__file__).resolve().parent
WORKSPACE = SCRIPT_DIR.parent  # FGLS_new root
VAULT_DIR = WORKSPACE / ".vault"
STATE_FILE = VAULT_DIR / ".inbox_state.json"
ZIP_ARCHIVE = VAULT_DIR / "zips"
MANIFEST_FILE = VAULT_DIR / "manifest.csv"
BLOB_DIR = VAULT_DIR / "blobs"
DASHBOARD_HTML = SCRIPT_DIR / "inbox_dashboard.html"

MAX_VAULT_PER_FILE = 10
VAULT_SIZE_LIMIT = 3 * 1024 * 1024
VAULT_EXTS = {".c", ".h", ".py", ".md", ".json", ".txt"}
TEXT_EXTS = {".h", ".c", ".cpp", ".cc", ".py", ".js", ".ts", ".json",
             ".yaml", ".yml", ".md", ".txt", ".rs", ".go", ".java", ".toml"}

DEP_PATTERNS = [
    (re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.MULTILINE), "c"),
    (re.compile(r'^\s*(?:import|from)\s+([\w.]+)', re.MULTILINE), "py"),
    (re.compile(r'(?:import|require)\s*\(?[\'"]([^\'"]+)[\'"]\)?', re.MULTILINE), "js"),
]

VER_RE = re.compile(r"^(.+?)_v(\d+(?:[._]\d+)*)(\.[a-z0-9]+)$", re.IGNORECASE)

# ── state helpers ────────────────────────────────────────────────────
def _load_state() -> dict:
    if STATE_FILE.exists():
        try:
            return json.loads(STATE_FILE.read_text("utf-8"))
        except Exception:
            pass
    return {"roots": [], "file_index": {}, "vault": {}, "incoming": [],
            "project_name": WORKSPACE.name, "last_scan_ts": 0.0,
            "folder_groups": {}, "board": [], "sources": {}}

def _save_state(state: dict):
    VAULT_DIR.mkdir(parents=True, exist_ok=True)
    STATE_FILE.write_text(json.dumps(state, indent=2, default=str), "utf-8")

def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()[:16]

def _b64(data: bytes) -> str:
    import base64
    return base64.b64encode(data).decode()

def _parse_ver(name: str):
    m = VER_RE.match(name)
    if not m:
        return None
    parts = [int(x) for x in re.split(r"[._]", m.group(2))]
    return {"base": m.group(1), "ver": m.group(2), "ver_parts": parts, "ext": m.group(3)}

def _ver_gt(a, b):
    for i in range(max(len(a), len(b))):
        ai = a[i] if i < len(a) else 0
        bi = b[i] if i < len(b) else 0
        if ai != bi:
            return ai > bi
    return False

# ── file scanning ────────────────────────────────────────────────────
def _walk(root, prefix="", root_idx=0):
    out = {}
    if not root.exists():
        return out
    for entry in sorted(root.iterdir(), key=lambda p: p.name):
        name = entry.name
        if name.startswith(".") or name in ("__pycache__", "node_modules"):
            continue
        rel = f"{prefix}/{name}" if prefix else name
        if entry.is_dir():
            out.update(_walk(entry, rel, root_idx))
        else:
            pv = _parse_ver(name)
            try:
                st = entry.stat()
                h = _sha256(entry.read_bytes()) if st.st_size < 10_000_000 else ""
            except Exception:
                h = ""
            out[rel] = {"name": name, "size": entry.stat().st_size,
                        "mtime": entry.stat().st_mtime, "hash": h,
                        "parsed": pv, "root_idx": root_idx, "rel": rel}
    return out

def _scan_imports(text):
    imports = set()
    for pat, lang in DEP_PATTERNS:
        for m in pat.finditer(text):
            raw = m.group(1)
            fname = raw.split("/")[-1] if "/" in raw else raw
            imports.add(fname)
    return imports

def _build_dep_graph(state):
    graph = {}
    for rel, fi in state["file_index"].items():
        name = fi["name"]
        if name not in graph:
            graph[name] = {"imports": set(), "imported_by": set()}
        ext = Path(name).suffix.lower()
        if ext in {e.lower() for e in TEXT_EXTS}:
            try:
                p = WORKSPACE / rel
                if p.exists() and p.stat().st_size < 500_000:
                    deps = _scan_imports(p.read_text("utf-8", errors="replace"))
                    for d in deps:
                        if d != name:
                            graph[name]["imports"].add(d)
                            if d not in graph:
                                graph[d] = {"imports": set(), "imported_by": set()}
                            graph[d]["imported_by"].add(name)
            except Exception:
                pass
    return graph

def _compute_folder_groups(file_index):
    groups = {}
    for rel in file_index:
        top = rel.split("/")[0]
        groups[top] = groups.get(top, 0) + 1
    return dict(sorted(groups.items(), key=lambda x: -x[1]))

def _auto_place(item, state):
    name = item["name"]
    pv = item.get("parsed")
    if item.get("match_key"):
        return item["match_key"]
    ext = Path(name).suffix.lower()
    base = pv["base"] if pv else Path(name).stem
    siblings = [r for r, fi in state["file_index"].items()
                if isinstance(fi.get("parsed"), dict) and fi["parsed"].get("base") == base]
    if siblings:
        return siblings[0]
    dep_graph = state.get("_dep_graph", {})
    importers = dep_graph.get(name, {}).get("imported_by", [])
    if importers:
        importer_rels = [r for r, fi in state["file_index"].items() if fi["name"] in importers]
        if importer_rels:
            dir_part = str(Path(importer_rels[0]).parent)
            return f"{dir_part}/{name}" if dir_part != "." else name
    conv_map = {".h": "include/", ".c": "src/", ".py": "python_client/"}
    return f"{conv_map.get(ext, '')}{name}"

# ── vault ────────────────────────────────────────────────────────────
def _vault_file(rel, state):
    ext = Path(rel).suffix.lower()
    if ext not in VAULT_EXTS:
        return
    src = WORKSPACE / rel
    if not src.exists():
        return
    try:
        st = src.stat()
        if st.st_size > VAULT_SIZE_LIMIT:
            return
        h = _sha256(src.read_bytes())
    except Exception:
        return
    VAULT_DIR.mkdir(parents=True, exist_ok=True)
    manifest = _load_manifest()
    existing = manifest.get(rel, [])
    latest = existing[0] if existing else None
    if latest and latest.get("hash") == h:
        return
    new_ver = (latest["version"] + 1) if latest else 1
    record = {"rel": rel, "version": new_ver, "hash": h, "size": st.st_size,
              "mtime": st.st_mtime, "ts": int(datetime.now().timestamp())}
    BLOB_DIR.mkdir(parents=True, exist_ok=True)
    blob_path = BLOB_DIR / h
    if not blob_path.exists():
        blob_path.write_bytes(src.read_bytes())
    manifest.setdefault(rel, []).insert(0, record)
    manifest[rel] = sorted(manifest[rel], key=lambda x: x["version"])[:MAX_VAULT_PER_FILE]
    _save_manifest(manifest)

def _load_manifest():
    if not MANIFEST_FILE.exists():
        return {}
    result = {}
    with open(MANIFEST_FILE, "r", newline="") as f:
        for row in csv.DictReader(f):
            rel = row.get("rel", "")
            if not rel:
                continue
            row["size"] = int(row.get("size", 0))
            row["mtime"] = float(row.get("mtime", 0))
            row["version"] = int(row.get("version", 0))
            row["ts"] = int(row.get("ts", 0))
            result.setdefault(rel, []).append(row)
    for rel in result:
        result[rel].sort(key=lambda x: x["version"], reverse=True)
    return result

def _save_manifest(manifest):
    VAULT_DIR.mkdir(parents=True, exist_ok=True)
    rows = []
    for rel, vers in manifest.items():
        rows.extend(vers)
    rows.sort(key=lambda r: (r["rel"], -r["version"]))
    with open(MANIFEST_FILE, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=["rel", "version", "hash", "size", "mtime", "ts"])
        writer.writeheader()
        writer.writerows(rows)

# ── CLI commands ─────────────────────────────────────────────────────
def cmd_scan():
    state = _load_state()
    if not state["roots"]:
        state["roots"] = ["."]
    state["file_index"] = {}
    for ri, root_rel in enumerate(state["roots"]):
        root_path = WORKSPACE / root_rel
        if root_path.exists():
            state["file_index"].update(_walk(root_path, "", ri))
    graph = _build_dep_graph(state)
    state["_dep_graph"] = {k: {"imports": list(v["imports"]), "imported_by": list(v["imported_by"])}
                           for k, v in graph.items()}
    state["project_name"] = WORKSPACE.name
    state["last_scan_ts"] = time.time()
    state["folder_groups"] = _compute_folder_groups(state["file_index"])
    _save_state(state)
    total = len(state["file_index"])
    vaulted = sum(len(v) for v in state.get("vault", {}).values())
    missing = sum(1 for n in state.get("_dep_graph", {})
                  if n not in {fi["name"] for fi in state["file_index"].values()}
                  and any(state["_dep_graph"][n].get("imported_by")))
    print(f"ok  scanned {total} files, {vaulted} vaulted, {missing} missing deps")
    # write .inbox_index.md
    _write_index(state)

def _write_index(state):
    ts = state.get("last_scan_ts", 0)
    when = datetime.fromtimestamp(ts).strftime("%Y-%m-%d %H:%M:%S") if ts else "never"
    total = len(state.get("file_index", {}))
    groups = state.get("folder_groups", {})
    vaulted = sum(len(v) for v in state.get("vault", {}).values())
    lines = [f"# {state.get('project_name', '?')} — Index",
             f"Last scan: {when}  |  Files: {total}  |  Vault: {vaulted}", "", "## Folder groups"]
    for folder, count in groups.items():
        lines.append(f"  {folder}/  ({count} files)")
    lines.append("")
    (WORKSPACE / ".inbox_index.md").write_text("\n".join(lines), "utf-8")

def cmd_index():
    state = _load_state()
    ts = state.get("last_scan_ts", 0)
    if ts == 0 and not state.get("file_index"):
        print("no scan data — run: python tools/inbox_manager.py scan")
        return
    when = datetime.fromtimestamp(ts).strftime("%Y-%m-%d %H:%M") if ts else "never"
    total = len(state.get("file_index", {}))
    groups = state.get("folder_groups", {})
    print(f"# {state.get('project_name', '?')}  (last scan: {when})")
    print(f"Files: {total}  |  Vault: {sum(len(v) for v in state.get('vault', {}).values())}")
    print(f"\n## Folder groups ({len(groups)})")
    for folder, count in list(groups.items())[:30]:
        print(f"  {folder}/  ({count})")

def cmd_put(path):
    state = _load_state()
    p = Path(path)
    if not p.exists():
        print(f"err  not found: {path}")
        return
    raw = p.read_bytes()
    fname = p.name
    h = _sha256(raw)
    pv = _parse_ver(fname)
    match_key = match_fi = None
    for r, fi in state["file_index"].items():
        fp = fi.get("parsed") or _parse_ver(fi["name"])
        if fp and pv and fp["base"] == pv["base"] and fp["ext"] == pv["ext"]:
            match_key, match_fi = r, fi
            break
    if match_fi:
        status = "same" if match_fi.get("hash") == h else "update" if pv and match_fi.get("parsed") and _ver_gt(pv["ver_parts"], match_fi["parsed"]["ver_parts"]) else "conflict"
    else:
        status = "new"
    item = {"name": fname, "zip": "direct", "hash": h, "parsed": pv, "status": status,
            "match_key": match_key, "content_len": len(raw), "content_b64": _b64(raw)}
    state.setdefault("incoming", []).append(item)
    _save_state(state)
    print(f"ok  {status}: {fname}" + (f" → {match_key}" if match_key else ""))

def cmd_incoming():
    state = _load_state()
    items = state.get("incoming", [])
    pending = [i for i in items if i["status"] in ("new", "update", "conflict", "downgrade")]
    if not pending:
        print("no incoming files")
        return
    print(f"{'status':10s} {'name':30s} {'size':>8s}  match")
    print("-" * 70)
    for i in pending:
        print(f"{i['status']:10s} {i['name']:30s} {i.get('content_len', 0):>8d}  {i.get('match_key', '-')}")

def cmd_apply():
    state = _load_state()
    to_apply = [i for i in state.get("incoming", [])
                if i["status"] in ("new", "update") and i.get("content_b64")]
    if not to_apply:
        print("nothing to apply")
        return
    import base64
    for item in to_apply:
        try:
            target_rel = _auto_place(item, state)
            target_path = WORKSPACE / target_rel
            target_path.parent.mkdir(parents=True, exist_ok=True)
            if item.get("match_key") and item["match_key"] in state["file_index"]:
                _vault_file(item["match_key"], state)
            raw = base64.b64decode(item["content_b64"])
            target_path.write_bytes(raw)
            print(f"  {item['name']} → {target_rel}")
            item["status"] = "done"
        except Exception as e:
            print(f"  {item['name']} FAIL: {e}")
    state["incoming"] = [i for i in state["incoming"] if i["status"] != "done"]
    _save_state(state)
    cmd_scan()

# ── board commands ───────────────────────────────────────────────────
BOARD_STATUSES = ["todo", "in_progress", "done", "blocked", "cancelled"]

def _card_fmt(c):
    """Human-readable one-line summary for a card."""
    tags = f" [{','.join(c.get('tags', []))}]" if c.get("tags") else ""
    owner = f" @{c['owner']}" if c.get("owner") else ""
    return f"  #{c['id']} [{c.get('status','todo')}] {c.get('title','')}{owner}{tags}"

def cmd_board(args):
    state = _load_state()
    if not args or args[0] == "list":
        board = state.get("board", [])
        if not board:
            print("board is empty")
            return
        for c in reversed(board):
            print(_card_fmt(c))
            if c.get("body"):
                print(f"       {c['body'][:120]}")
    elif args[0] == "post":
        title_parts = list(args[1:])
        tag_words = []
        # extract --tags x,y  (position insensitive)
        rest = []
        i = 0
        while i < len(title_parts):
            if title_parts[i] in ("--tags", "--tag") and i + 1 < len(title_parts):
                tag_words = [t.strip() for t in title_parts[i+1].split(",") if t.strip()]
                i += 2
            else:
                rest.append(title_parts[i])
                i += 1
        title = " ".join(rest) if rest else "untitled"
        board = state.setdefault("board", [])
        card_id = (board[-1]["id"] + 1) if board else 1
        ts = datetime.now().isoformat()
        board.append({"id": card_id, "title": title, "body": "", "status": "todo",
                       "tags": tag_words, "owner": "", "lock_ts": "",
                       "created_at": ts, "updated_at": ts})
        _save_state(state)
        tags_str = f" [tags:{','.join(tag_words)}]" if tag_words else ""
        print(f"ok  card #{card_id}: {title} [todo]{tags_str}")
    elif args[0] == "update" and len(args) >= 3:
        card_id = int(args[1])
        new_status = args[2]
        if new_status not in BOARD_STATUSES:
            print(f"err  invalid status '{new_status}' — use: {', '.join(BOARD_STATUSES)}")
            return
        for c in state.get("board", []):
            if c["id"] == card_id:
                c["status"] = new_status
                c["updated_at"] = datetime.now().isoformat()
                _save_state(state)
                print(f"ok  card #{card_id} → [{new_status}]")
                return
        print(f"err  card #{card_id} not found")
    elif args[0] == "tag" and len(args) >= 3:
        card_id = int(args[1])
        tag_words = [t.strip() for t in " ".join(args[2:]).split(",") if t.strip()]
        for c in state.get("board", []):
            if c["id"] == card_id:
                existing = set(c.get("tags", []))
                existing.update(tag_words)
                c["tags"] = sorted(existing)
                c["updated_at"] = datetime.now().isoformat()
                _save_state(state)
                print(f"ok  card #{card_id} tagged: {','.join(c['tags'])}")
                return
        print(f"err  card #{card_id} not found")
    elif args[0] == "edit" and len(args) >= 3:
        card_id = int(args[1])
        body = " ".join(args[2:])
        for c in state.get("board", []):
            if c["id"] == card_id:
                c["body"] = body
                c["updated_at"] = datetime.now().isoformat()
                _save_state(state)
                print(f"ok  card #{card_id} body updated ({len(body)} ch)")
                return
        print(f"err  card #{card_id} not found")
    elif args[0] == "claim" and len(args) >= 3:
        card_id = int(args[1])
        owner = " ".join(args[2:])
        force = False
        if owner.rstrip().endswith("--force"):
            force = True
            owner = owner.rstrip()[:-7].strip()
        if not owner:
            owner = "ai"
        for c in state.get("board", []):
            if c["id"] == card_id:
                if c.get("status") == "in_progress" and c.get("owner") and c["owner"] != owner and not force:
                    print(f"err  card #{card_id} LOCKED by {c['owner']} — use --force to override")
                    return
                c["status"] = "in_progress"
                c["owner"] = owner
                c["lock_ts"] = datetime.now().isoformat()
                c["updated_at"] = datetime.now().isoformat()
                _save_state(state)
                print(f"ok  card #{card_id} CLAIMED by {owner} → [in_progress] (locked)")
                return
        print(f"err  card #{card_id} not found")
    elif args[0] == "release" and len(args) >= 2:
        card_id = int(args[1])
        force = any(a == "--force" for a in args[2:])
        for c in state.get("board", []):
            if c["id"] == card_id:
                if c.get("status") == "in_progress" and c.get("owner") and not force:
                    print(f"err  card #{card_id} still held by {c['owner']} — release needs --force or owner==")
                    return
                c["status"] = "todo"
                c["owner"] = ""
                c["lock_ts"] = ""
                c["updated_at"] = datetime.now().isoformat()
                _save_state(state)
                print(f"ok  card #{card_id} RELEASED → [todo] (unlocked)")
                return
        print(f"err  card #{card_id} not found")
    elif args[0] == "handoff":
        board = state.get("board", [])
        now_str = datetime.now().strftime("%Y-%m-%d %H:%M")
        if not board:
            print(f"# Session Handoff — {now_str}\n\nNo board activity.")
            return
        todo = [c for c in board if c["status"] == "todo"]
        wip = [c for c in board if c["status"] == "in_progress"]
        done = [c for c in board if c["status"] == "done"]
        blocked = [c for c in board if c["status"] == "blocked"]
        print(f"# Session Handoff — {now_str}\n")
        print(f"Board: {len(board)} cards (todo:{len(todo)} wip:{len(wip)} done:{len(done)} blocked:{len(blocked)})")
        for group, label in ((blocked, "Blocked"), (wip, "In Progress"), (todo, "Todo")):
            if group:
                print(f"\n### {label}")
                for c in group:
                    print(_card_fmt(c))
                    if c.get("body"):
                        print(f"       {c['body'][:100]}")
    else:
        print("usage: board [list|post <title> [--tags x,y]|update <id> <status>|tag <id> <tags>|edit <id> <body>|claim <id> <owner>|release <id> [--force]|handoff]")

# ── HTTP Dashboard ───────────────────────────────────────────────────
class DashboardHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass  # silence

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path

        if path == "/" or path == "/index.html":
            html_path = DASHBOARD_HTML
            if html_path.exists():
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.end_headers()
                self.wfile.write(html_path.read_bytes())
            else:
                self.send_response(404)
                self.end_headers()
                self.wfile.write(b"Dashboard HTML not found")

        elif path == "/api/state":
            state = _load_state()
            data = {"project_name": state.get("project_name", WORKSPACE.name),
                    "file_count": len(state.get("file_index", {})),
                    "board": state.get("board", []),
                    "sources": state.get("sources", {})}
            body = json.dumps(data).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(body)

        elif path == "/api/handoff":
            state = _load_state()
            board = state.get("board", [])
            lines = [f"# Session Handoff — {datetime.now().strftime('%Y-%m-%d %H:%M')}\n"]
            counts = {}
            for c in board:
                s = c.get("status", "todo")
                counts[s] = counts.get(s, 0) + 1
            lines.append(f"## Board Summary ({len(board)} cards)")
            for s in ["todo", "in_progress", "done", "blocked", "cancelled"]:
                if counts.get(s, 0):
                    lines.append(f"  {s.replace('_', ' ').title()}: {counts[s]}")
            lines.append("")
            for s in ["todo", "in_progress", "blocked"]:
                items = [c for c in board if c.get("status") == s]
                if items:
                    lines.append(f"### {s.replace('_', ' ').title()}")
                    for c in items:
                        tags = ""
                        if c.get("tags"):
                            tags = f" [{','.join(c['tags'])}]"
                        owner = f" @{c['owner']}" if c.get("owner") else ""
                        lines.append(f"  #{c.get('id','?')} {c.get('title','')}{owner}{tags}")
                    lines.append("")
            body = "\n".join(lines).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/plain; charset=utf-8")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            self.wfile.write(body)

        else:
            self.send_response(404)
            self.end_headers()

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        content_len = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(content_len) if content_len > 0 else b""

        if path == "/api/board/update":
            try:
                data = json.loads(body)
                card_id = data.get("id")
                new_status = data.get("status")
                if not card_id or not new_status:
                    self.send_response(400)
                    self.end_headers()
                    return
                state = _load_state()
                for c in state.get("board", []):
                    if c["id"] == card_id:
                        c["status"] = new_status
                        c["updated_at"] = datetime.now().isoformat()
                        _save_state(state)
                        self.send_response(200)
                        self.send_header("Content-Type", "application/json")
                        self.send_header("Access-Control-Allow-Origin", "*")
                        self.end_headers()
                        self.wfile.write(json.dumps({"ok": True}).encode())
                        return
                self.send_response(404)
                self.end_headers()
                self.wfile.write(json.dumps({"error": "card not found"}).encode())
            except Exception as e:
                self.send_response(500)
                self.end_headers()
                self.wfile.write(json.dumps({"error": str(e)}).encode())

        elif path == "/api/board/post":
            try:
                data = json.loads(body)
                title = data.get("title", "untitled")
                body_text = data.get("body", "")
                status = data.get("status", "todo")
                tags = data.get("tags", [])
                owner = data.get("owner", "")
                state = _load_state()
                board = state.setdefault("board", [])
                card_id = (board[-1]["id"] + 1) if board else 1
                ts = datetime.now().isoformat()
                board.append({"id": card_id, "title": title, "body": body_text,
                               "status": status, "tags": tags, "owner": owner, "lock_ts": "",
                               "created_at": ts, "updated_at": ts})
                _save_state(state)
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                self.wfile.write(json.dumps({"ok": True, "id": card_id}).encode())
            except Exception as e:
                self.send_response(500)
                self.end_headers()
                self.wfile.write(json.dumps({"error": str(e)}).encode())

        else:
            self.send_response(404)
            self.end_headers()

    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

def cmd_dashboard(port=5000):
    print(f"Dashboard: http://127.0.0.1:{port}")
    print("Press Ctrl+C to stop")
    server = HTTPServer(("0.0.0.0", port), DashboardHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")
        server.server_close()

# ── main ─────────────────────────────────────────────────────────────
def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return

    cmd = sys.argv[1]
    args = sys.argv[2:]

    if cmd == "scan":
        cmd_scan()
    elif cmd == "index":
        cmd_index()
    elif cmd == "put" and args:
        cmd_put(args[0])
    elif cmd == "incoming":
        cmd_incoming()
    elif cmd == "apply":
        cmd_apply()
    elif cmd == "board":
        cmd_board(args)
    elif cmd == "dashboard":
        port = int(args[0]) if args else 5000
        cmd_dashboard(port)
    else:
        print(__doc__)

if __name__ == "__main__":
    main()