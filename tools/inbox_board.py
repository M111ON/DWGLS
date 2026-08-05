#!/usr/bin/env python3
"""DWGLS Inbox Board — cross-session sticky notes.
Usage:
  python tools/inbox_board.py list
  python tools/inbox_board.py post "title" ["body"]
  python tools/inbox_board.py update <id> <status>
  python tools/inbox_board.py handoff
"""
import json, sys, os
from pathlib import Path
from datetime import datetime

SCRIPT_DIR = Path(__file__).parent
WORKSPACE = SCRIPT_DIR.parent
VAULT_DIR = WORKSPACE / ".vault"
STATE_FILE = VAULT_DIR / ".inbox_state.json"

STATUSES = ("todo", "in_progress", "done", "blocked", "cancelled")

def _load():
    if STATE_FILE.exists():
        return json.loads(STATE_FILE.read_text("utf-8"))
    return {"cards": [], "next_id": 1}

def _save(state):
    VAULT_DIR.mkdir(exist_ok=True)
    STATE_FILE.write_text(json.dumps(state, indent=2, ensure_ascii=False), "utf-8")

def cmd_list():
    state = _load()
    cards = state.get("cards", [])
    if not cards:
        print("Board empty.")
        return
    for c in cards:
        tag = f" [{c['status']}]" if c.get("status") else ""
        body = f" — {c['body']}" if c.get("body") else ""
        print(f"  #{c['id']}{tag} {c['title']}{body}")

def cmd_post(args):
    title = args[0] if args else "note"
    body = args[1] if len(args) > 1 else ""
    state = _load()
    card = {
        "id": state["next_id"],
        "title": title,
        "body": body,
        "status": "todo",
        "created": datetime.now().isoformat(timespec="seconds"),
    }
    state["cards"].append(card)
    state["next_id"] += 1
    _save(state)
    print(f"Posted #{card['id']}: {title}")

def cmd_update(args):
    if len(args) < 2:
        print("Usage: update <id> <status>")
        return
    cid, status = int(args[0]), args[1]
    if status not in STATUSES:
        print(f"Status must be one of: {', '.join(STATUSES)}")
        return
    state = _load()
    for c in state["cards"]:
        if c["id"] == cid:
            c["status"] = status
            _save(state)
            print(f"Updated #{cid} → {status}")
            return
    print(f"Card #{cid} not found")

def cmd_handoff():
    state = _load()
    cards = state.get("cards", [])
    pending = [c for c in cards if c.get("status") in ("todo", "in_progress", "blocked")]
    done = [c for c in cards if c.get("status") == "done"]
    print(f"=== DWGLS Board Handoff ===")
    print(f"Pending: {len(pending)}  |  Done: {len(done)}")
    for c in pending:
        print(f"  #{c['id']} [{c['status']}] {c['title']}")
    if done:
        print("Recently done:")
        for c in done[-5:]:
            print(f"  #{c['id']} [done] {c['title']}")

if __name__ == "__main__":
    args = sys.argv[1:]
    if not args or args[0] == "list":
        cmd_list()
    elif args[0] == "post":
        cmd_post(args[1:])
    elif args[0] == "update":
        cmd_update(args[1:])
    elif args[0] == "handoff":
        cmd_handoff()
    else:
        print(__doc__)
