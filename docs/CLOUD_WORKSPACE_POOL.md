# Cloud Workspace Pool — Documentation

> **Version:** 2.1.0
> **Status:** Deployed on Cloudflare Workers
> **Date:** 2026-08-20

---

## Table of Contents

1. [Concept: Workspace vs Memory](#1-concept-workspace-vs-memory)
2. [Architecture](#2-architecture)
3. [Quick Start](#3-quick-start)
4. [CLI Reference](#4-cli-reference)
5. [REST API Reference](#5-rest-api-reference)
6. [MCP Tools Reference](#6-mcp-tools-reference)
7. [Browser UI](#7-browser-ui)
8. [Lifecycle Management](#8-lifecycle-management)
9. [Checkpoints & Memory Bridge](#9-checkpoints--memory-bridge)
10. [Multi-Agent Coordination](#10-multi-agent-coordination)
11. [Deployment](#11-deployment)
12. [Troubleshooting](#12-troubleshooting)

---

## 1. Concept: Workspace vs Memory

```
MEMORY (passive)                  WORKSPACE (active)
────────────────                  ─────────────────
chunks of conversation            bundle: files + vars + decisions + next-steps
"search for what I knew"          "load state and continue working"
requires embedding + vector       no search — already knows what's in it
passive (waits to be asked)       active (held at all times)
lossy OK (summary/forgotten)      lossless — state cannot be lost
```

**Key insight:** The market (Claude Projects, ChatGPT memory, Gemini) builds "memory" — but what users actually want is **workspace** — state continuity. Memory requires search; workspace is already loaded.

**Workspace = "I'm already holding this"** — no "did I find it?" only "open/close".

---

## 2. Architecture

```
┌─────────────────────────────────────────────────────┐
│              cloud-memory-worker v2.1.0              │
│              https://cloud-memory-worker.aexid03     │
│                  .workers.dev                        │
│                                                       │
│  ┌─── Memory (passive) ───┐  ┌─── Workspace (active) ──┐
│  │ search_memory          │  │ ws_create               │
│  │ memory_get             │  │ ws_list                 │
│  │ memory_remember        │  │ ws_load   (lossless)    │
│  │ memory_init            │  │ ws_update (merge)       │
│  │ list_sources           │  │ ws_checkpoint (4-way)   │
│  │ memory_status          │  │ ws_delete               │
│  └────────────────────────┘  │ ws_search               │
│                              │ ws_heartbeat            │
│  ┌─── Lifecycle ─────────┐  │ ws_claim / ws_release   │
│  │ Cron: auto-stale      │  │ ws_stale_detect         │
│  │   (every 6 hours)     │  │ ws_pool_status          │
│  └───────────────────────┘  └─────────────────────────┘
│                                                       │
│  D1 Database:                                         │
│    chunks + query_cache (memory)                      │
│    workspaces + workspace_state +                     │
│    workspace_checkpoints (workspace)                  │
└─────────────────────────────────────────────────────┘
```

### Files

| File | Lines | Purpose |
|------|-------|---------|
| `cloud-memory/src/index.ts` | ~1400 | Worker: 19 MCP tools + REST + Cron |
| `cloud-memory/src/workspace_handlers.ts` | ~1600 | Handlers: CRUD + lifecycle + 2 dashboards |
| `cloud-memory/scripts/ws_sync.py` | ~750 | CLI: 13 commands for cross-machine sync |
| `cloud-memory/workspace_schema.sql` | 60 | D1 migration |
| `cloud-memory/wrangler.toml` | 17 | Cloudflare config |

### Workspace Bundle (lossless state)

```json
{
  "files": { "src/main.ts": "console.log('hello')" },
  "variables": { "fish_count": "5", "water_temp": "26" },
  "decisions": { "dec_001": "Use TypeScript for type safety" },
  "next_steps": ["Add unit tests", "Deploy to production"],
  "context": "Working on aquarium management system"
}
```

### 4-Way Checkpoint

| Layer | What | When |
|-------|------|------|
| **pool** | Full state snapshot (automatic, lossless) | On checkpoint |
| **vault** | Persistent copy (for long-term storage) | Optional |
| **fact** | Extracted facts → searchable in memory | Auto on checkpoint |
| **cloud_ref** | External storage reference | Optional |

---

## 3. Quick Start

### Prerequisites

- Python 3.8+
- API key (from `.api_key` file or `CF_MEMORY_KEY` env var)

### Install CLI

```bash
# No installation needed — just run directly
python cloud-memory/scripts/ws_sync.py --help

# Or add to PATH
export PATH="$PATH:$(pwd)/cloud-memory/scripts"
```

### First Workspace

```bash
# 1. Create a workspace
python ws_sync.py create "my-project" ./my-project

# 2. Edit files
echo "# My Project" > ./my-project/README.md
echo '{"version": "1.0"}' > ./my-project/_variables.json

# 3. Push to cloud
python ws_sync.py push ./my-project

# 4. Verify
python ws_sync.py list

# 5. Pull on another machine
python ws_sync.py pull <workspace_id> ./my-project-b
```

---

## 4. CLI Reference

### Workspace CRUD

```bash
# Create workspace + pull to local
ws_sync.py create "name" [dir] [--label TAG]

# List active workspaces
ws_sync.py list [--status active|paused|archived|all]

# Pull workspace to local directory
ws_sync.py pull <workspace_id> [dir]

# Push local changes to cloud
ws_sync.py push <dir>

# Diff local vs cloud
ws_sync.py diff <dir>

# Delete permanently
ws_sync.py delete <workspace_id>

# Search within workspace
ws_sync.py search <workspace_id> "query" [-k 10]
```

### Checkpoint

```bash
# Checkpoint (auto-extracts facts → searchable memory)
ws_sync.py checkpoint <dir> ["message"] [--archive]
```

### Lifecycle

```bash
# Send heartbeat (keeps the write lease alive — owner only)
ws_sync.py heartbeat <dir> [--agent NAME]

# Acquire the write lease (exclusive WRITE access; auto-takeover if stale)
ws_sync.py claim <dir> [--agent NAME]

# Release the write lease
ws_sync.py release <dir> [--agent NAME]

# Auto-pause stale workspaces (> 24h idle)
ws_sync.py stale-detect

# Pool status (with lifecycle info)
ws_sync.py status
```

### Concurrency & Write-Lease Policy

Workspace state is a shared editable bundle, so concurrent WRITE access is
guarded by a **heartbeat-backed lease** — while READ and CREATE are always
free:

| Operation | Locked? | Rule |
|-----------|---------|------|
| **READ** (load / list / status) | ❌ never | anyone may read at any time |
| **CREATE** (new workspace / new file) | ❌ never | new keys never conflict |
| **WRITE** (update / checkpoint) | ✅ owner-only | must hold a live lease |
| **Heartbeat** | — | extends the lease **only** for the current owner; a passive observer cannot keep someone else's lock alive |

- A lease is held while its owner refreshes `heartbeat_at`. After
  `CLAIM_TIMEOUT_MS` (**10 minutes**) of silence the lease expires and any
  writer may **take over** automatically — no manual unblock needed.
- If agent B tries to write while agent A holds a live lease, the worker
  returns `409` naming the owner and how long until takeover is possible.
- Workflow: an agent `claim`s (or writes — `update`/`checkpoint` acquire the
  lease implicitly), heartbeats periodically during a long session, and
  `release`s when done. Every write refreshes the lease timestamp.

### Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `CF_MEMORY_URL` | `https://cloud-memory-worker.aexid03.workers.dev` | Worker URL |
| `CF_MEMORY_KEY` | (from `.api_key` file) | API key for write access |
| `WS_AGENT` | `$COMPUTERNAME` | Agent identifier for claim/heartbeat |

---

## 5. REST API Reference

### Memory Endpoints (existing)

| Method | Path | Auth | Description |
|--------|------|------|-------------|
| `POST` | `/ingest` | ✓ | Store chunks with vectors |
| `POST` | `/search` | ✗ | Semantic search |
| `GET` | `/status` | ✓ | Database stats |
| `GET` | `/list-sources` | ✗ | List all indexed files |
| `POST` | `/cleanup` | ✓ | Delete by prefix/pattern |

### Workspace Endpoints (new)

| Method | Path | Auth | Description |
|--------|------|------|-------------|
| `GET` | `/workspace` | ✗ | Dashboard UI (HTML) |
| `GET` | `/workspace/editor` | ✗ | Editor UI (HTML) |
| `POST` | `/workspace/create` | ✓ | Create workspace |
| `GET` | `/workspace/list?status=active` | ✗ | List workspaces |
| `GET` | `/workspace/pool-status` | ✗ | Pool status with lifecycle |
| `POST` | `/workspace/stale-detect` | ✓ | Auto-pause stale |
| `GET` | `/workspace/:id` | ✗ | Load full workspace |
| `POST` | `/workspace/:id` | ✓ | Update state (merge) |
| `POST` | `/workspace/:id/checkpoint` | ✓ | 4-way checkpoint |
| `GET` | `/workspace/:id/checkpoints` | ✓ | List checkpoints |
| `POST` | `/workspace/:id/archive` | ✓ | Archive workspace |
| `POST` | `/workspace/:id/delete` | ✓ | Permanent delete |
| `POST` | `/workspace/:id/search` | ✗ | Search within workspace |
| `POST` | `/workspace/:id/heartbeat` | ✗ | Heartbeat — extends write lease **only** for current owner |
| `POST` | `/workspace/:id/claim` | ✓ | Acquire write lease (auto-takeover if stale > 10 min) |
| `POST` | `/workspace/:id/release` | ✓ | Release write lease |

### Example: Create Workspace

```bash
curl -X POST https://cloud-memory-worker.aexid03.workers.dev/workspace/create \
  -H "Content-Type: application/json" \
  -H "X-API-Key: YOUR_KEY" \
  -d '{
    "name": "my-project",
    "label": "project",
    "state": {
      "files": { "README.md": "# Hello" },
      "variables": { "version": "1.0" },
      "decisions": { "dec_001": "Use TypeScript" },
      "next_steps": ["Step 1", "Step 2"],
      "context": "Building a cool thing"
    }
  }'
```

### Example: Search Within Workspace

```bash
curl -X POST https://cloud-memory-worker.aexid03.workers.dev/workspace/WS_ID/search \
  -H "Content-Type: application/json" \
  -d '{ "q": "TypeScript", "k": 10 }'
```

### Example: Checkpoint

```bash
curl -X POST https://cloud-memory-worker.aexid03.workers.dev/workspace/WS_ID/checkpoint \
  -H "Content-Type: application/json" \
  -H "X-API-Key: YOUR_KEY" \
  -d '{ "fact_summary": "Completed phase 1 of project" }'
```

---

## 6. MCP Tools Reference

### Memory Tools (existing)

| Tool | Description |
|------|-------------|
| `search_memory` | Semantic search across all chat history |
| `memory_get` | Fetch chunks by source_file + index range |
| `memory_remember` | Save fact/decision to memory |
| `memory_init` | Fetch full index manifest |
| `list_sources` | List all indexed files |
| `memory_status` | Health check + workspace count |

### Workspace Tools (new)

| Tool | Auth | Description |
|------|------|-------------|
| `ws_create` | ✓ | Create workspace with initial state |
| `ws_list` | ✗ | List active/paused/archived workspaces |
| `ws_load` | ✗ | Load full workspace state (lossless) |
| `ws_update` | ✓ | Update state entries (merge) |
| `ws_checkpoint` | ✓ | 4-way snapshot (auto-extracts facts) |
| `ws_delete` | ✓ | Permanent delete |
| `ws_search` | ✗ | Search within workspace state |
| `ws_archive` | ✓ | Archive (soft delete) |
| `ws_heartbeat` | ✗ | Keep workspace alive |
| `ws_claim` | ✓ | Claim exclusive access |
| `ws_release` | ✓ | Release claim |
| `ws_stale_detect` | ✓ | Auto-pause stale workspaces |
| `ws_pool_status` | ✗ | Full pool status with lifecycle |

### Example: MCP Call

```json
{
  "method": "tools/call",
  "params": {
    "name": "ws_create",
    "arguments": {
      "name": "DWGLS-refactor",
      "label": "project",
      "files": { "src/main.ts": "export {}" },
      "next_steps": ["Phase 1", "Phase 2"],
      "api_key": "YOUR_KEY"
    }
  }
}
```

---

## 7. Browser UI

### Dashboard (`/workspace`)

- Pool stats: Active, Paused, Claimed, Stale, Files
- Workspace cards with lifecycle info (who claimed, heartbeat age)
- Click to expand full state bundle
- Action buttons: Heartbeat, Claim, Release, Search, Delete
- Claim, Release, and Delete prompt once for the API key and reuse
  `localStorage.CF_MEMORY_KEY`

### Editor (`/workspace/editor`)

| Tab | Function |
|-----|----------|
| **Create** | Create new workspace with name, label, initial file |
| **Edit** | Load workspace → edit files → save |
| **Variables** | Add/update key-value pairs |
| **Decisions** | Record decisions with IDs |
| **Next Steps** | Ordered list management |

Create/update actions prompt once for the API key and reuse
`localStorage.CF_MEMORY_KEY`.

---

## 8. Lifecycle Management

### Heartbeat

Agents should send heartbeats every 5-15 minutes while actively working:

```bash
ws_sync.py heartbeat ./my-project
# or via API
POST /workspace/:id/heartbeat { "agent": "my-laptop" }
```

Without heartbeat for > 24 hours → workspace auto-pauses.

### Claim / Release

For multi-agent coordination:

```bash
ws_sync.py claim ./my-project    # exclusive access
ws_sync.py release ./my-project  # release when done
```

Rules:
- Only one agent can claim at a time
- Same agent can re-claim (idempotent)
- Stale claims (> 1 hour) can be overridden
- Only the claiming agent can release

### Stale Detection

- **Cron trigger:** Runs every 6 hours automatically
- **Manual:** `ws_sync.py stale-detect`
- **Threshold:** 24 hours without heartbeat
- **Action:** Sets status to `paused`, clears claim

### Status Values

| Status | Meaning |
|--------|---------|
| `active` | Currently in use (or recently active) |
| `paused` | Idle > 24h, no heartbeat |
| `archived` | Manually archived (preserved but hidden from pool) |

---

## 9. Checkpoints & Memory Bridge

### What Happens on Checkpoint

1. **Pool snapshot** — Full state serialized as JSON (lossless)
2. **Checksum** — SHA-256 of snapshot for integrity
3. **Fact extraction** — Auto-extracts from state:
   - Decisions → `[workspace:X] Decision: ...`
   - Next steps → `[workspace:X] Next step: ...`
   - Variables → `[workspace:X] Variable key = value`
   - Context → `[workspace:X] Context: ...`
   - File names → `[workspace:X] Working on N file(s): ...`
4. **Embed** — Facts embedded via bge-m3 (Workers AI)
5. **Push to memory** — Stored in `chunks` table with source `ws-fact/`
6. **Searchable** — Facts discoverable via `search_memory`

### Why This Matters

- Workspace context becomes discoverable across sessions
- "What decisions did I make on this project?" → `search_memory("workspace:DWGLS decisions")`
- Bridge between active workspace and passive memory

---

## 10. Multi-Agent Coordination

### Scenario: Two Agents Working on Same Project

```
Agent A (laptop)                 Agent B (desktop)
───────────────                  ────────────────
ws_sync.py claim ./proj          ws_sync.py claim ./proj
→ "claimed by A"                 → "already claimed by A" (409)

# ... work ...                   # wait or work on different workspace

ws_sync.py release ./proj        ws_sync.py claim ./proj
→ "released"                     → "claimed by B" ✓
```

### Stale Claim Override

If Agent A crashes and doesn't release:

```
Agent A claims at 10:00
Agent A crashes (no release)
Agent B tries at 11:30 (> 1h later)
→ stale claim detected → Agent B can override
```

---

## 11. Deployment

### Prerequisites

- Cloudflare account (free tier works)
- Wrangler CLI installed
- Logged in: `npx wrangler login`

### Deploy

```bash
cd cloud-memory

# 1. D1 database (already exists: cloud-memory-db)
npx wrangler d1 list

# 2. Apply schema migration
npx wrangler d1 execute cloud-memory-db --remote --file ./workspace_schema.sql

# 3. Set API key (already set: API_KEY)
npx wrangler secret list

# 4. Deploy worker
npx wrangler deploy
```

### Configuration

```toml
# wrangler.toml
name = "cloud-memory-worker"
main = "src/index.ts"
compatibility_date = "2026-08-18"

[[d1_databases]]
binding = "cloud_memory_db"
database_name = "cloud-memory-db"
database_id = "6f5eec11-3035-4774-8e08-98abc856ef13"

[[vectorize]]
binding = "VECTORIZE"
index_name = "cloud-memory-vectors"

[ai]
binding = "AI"

[triggers]
crons = ["0 */6 * * *"]  # auto-pause stale every 6h
```

### Free Tier Limits

| Resource | Free Tier |
|----------|-----------|
| Workers requests | 100,000/day |
| D1 reads | 10M/day |
| D1 writes | 1M/day |
| D1 storage | 5 GB |
| Vectorize | 10M vectors |
| Cron triggers | 1 |

---

## 12. Troubleshooting

### "Workspace not found"

```bash
# Check if workspace exists
ws_sync.py list

# If empty, create new
ws_sync.py create "my-project" ./my-project
```

### "Unauthorized"

```bash
# Check API key
cat .api_key

# Or set env var
export CF_MEMORY_KEY="your-key"
```

### "Already claimed by..."

```bash
# Check who claimed
ws_sync.py status

# If stale (> 1h), override will work automatically
ws_sync.py claim ./my-project
```

### Schema migration fails

```bash
# Tables auto-created on first request (ensureWorkspaceTables)
# Or apply manually:
npx wrangler d1 execute cloud-memory-db --remote --file ./workspace_schema.sql
```

### TypeScript build error (nested template literals)

The HTML handlers use array-based string building to avoid TypeScript/esbuild parse errors with nested backticks. If you see `Expected ";" but found "class"`, ensure the HTML is built using array join, not template literals with inner `<script>` backticks.

---

## Summary

| Feature | Status |
|---------|--------|
| Workspace CRUD | ✅ |
| Lossless state bundle | ✅ |
| Cross-machine sync (CLI) | ✅ |
| 4-way checkpoint | ✅ |
| Checkpoint → memory bridge | ✅ |
| Dashboard UI | ✅ |
| Editor UI | ✅ |
| Lifecycle (heartbeat/claim/release) | ✅ |
| Auto stale detection (cron) | ✅ |
| MCP tools (19 total) | ✅ |
| REST API (13 endpoints) | ✅ |
| Search within workspace | ✅ |
| Permanent delete | ✅ |

---

*Last updated: 2026-08-20*
