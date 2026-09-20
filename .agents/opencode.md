# opencode.md — OpenCode-only tooling (L2-app, skip entirely if you are not OpenCode)

## Repo context graph (graft)
Repo is indexed in `graft/`. For ANY task — understanding, finding code, scoping a change — query the graph BEFORE grepping/reading source. One call usually replaces several file reads.
- `graft ask "<question>" --source` → ranked nodes with code spans inlined (top node IS the answer for understand/edit tasks).
- `graft grep "<literal>"` → EVERY occurrence (ask is top-N, misses some).
- `graft callers <symbol>` (+ `--direction out`, `--depth N`) → blast radius before renames.
- `graft skeleton <file>` → API surface, ~10× cheaper than reading.
- `graft map` → orientation. `graft build` after big changes ($0, deterministic).
- Browse: `graft/INDEX.md`.

## Memory (read-only SQLite + session tools)
- Project memory DB: `file:I:/cortexkit/magic-context/context.db?mode=ro` — FTS5 (`memories_fts MATCH '"phrase"'`, quote dashes), `SELECT DISTINCT project_path` for real paths. DWGLS = `git:29b08…` (root-commit hash).
- memcore MCP (`memcore_query`/`memcore_stats`) = cross-platform knowledge, search BEFORE raw session digging. CLI fallback: `I:/tools/cloud_ws/memcore/query.py`.
- Global opencode rules (memcore, chat-pool, cloud-memory pool, thinking playbook) auto-load from `~/.config/opencode/AGENTS.md` — do not duplicate here.

## Session handoff (opencode triggers: compact / history short)
1. Summarize: done / pending / next + cwd. 2. ASK user before `opencode new`. 3. On confirm: `obsidian_mem.cmd endsession "summary" --proj DWGLS-native-fs`.
- Cross-platform: `python tools/handoff.py --summary "…" --proj DWGLS-native-fs`. Trail: `tools/session_trail.py` → `I:/tools/cloud-workspace/trail/`.

## Local custom commands (already on PATH)
- `mc` → Magic Context dashboard (patched shim sets `XDG_DATA_HOME`/`MAGIC_CONTEXT_STORAGE_DIR`; if Projects shows 0, check those env vars in the launching shell).
- `9router [--help]` → local AI gateway (port 20128). NOTE: `oc/*` free models currently 403 upstream ("free tier only from within OpenCode") — gateway is healthy, block is provider-side.
