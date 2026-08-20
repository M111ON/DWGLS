/* ═══════════════════════════════════════════════════════════════════════════
 * workspace_handlers.ts — Cloud Workspace Pool
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Workspace = active unit of work (NOT passive memory).
 * Bundle = lossless state: files + variables + decisions + next_steps.
 * "loaded = knows" — no search needed, just open/close.
 *
 * 4-way checkpoint: pool / vault / fact / cloud
 * ═══════════════════════════════════════════════════════════════════════════ */

export interface WorkspaceState {
  files: Record<string, string>;        // path → content
  variables: Record<string, string>;    // key → value
  decisions: Record<string, string>;    // id → description
  next_steps: string[];                 // ordered list
  context: string;                      // general context
}

export interface WorkspaceBundle {
  id: string;
  name: string;
  label: string | null;
  status: 'active' | 'paused' | 'archived';
  state: WorkspaceState;
  created_at: number;
  updated_at: number;
  last_checkpoint_at: number | null;
  file_count: number;
  variable_count: number;
  decision_count: number;
  next_step_count: number;
}

export interface WorkspaceCheckpoint {
  id: string;
  workspace_id: string;
  seq: number;
  pool_snapshot: string;   // full WorkspaceState JSON
  vault_copy: string | null;
  fact_summary: string | null;
  cloud_ref: string | null;
  checksum: string;
  created_at: number;
}

export interface Env {
  cloud_memory_db: D1Database;
  VECTORIZE: Vectorize;
  AI: Ai;
  API_KEY: string;
}

// ─── Helpers ──────────────────────────────────────────────────────────────

function corsHeaders(): Record<string, string> {
  return {
    "Access-Control-Allow-Origin": "*",
    "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
    "Access-Control-Allow-Headers": "Content-Type, Authorization, X-API-Key",
  };
}

function checkAuth(request: Request, env: Env): boolean {
  const key = request.headers.get("X-API-Key");
  return key === env.API_KEY;
}

function jsonResp(data: unknown, status = 200): Response {
  return new Response(JSON.stringify(data), {
    status,
    headers: { "Content-Type": "application/json", ...corsHeaders() },
  });
}

function errResp(msg: string, status = 400): Response {
  return jsonResp({ error: msg }, status);
}

async function sha256Hex(text: string): Promise<string> {
  const buf = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(text));
  return [...new Uint8Array(buf)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

function generateId(): string {
  return crypto.randomUUID();
}

// ─── Write-lease (concurrency guard) ──────────────────────────────────────
// Workspace state is a shared editable bundle. Concurrent writers cause
// silent lost updates, so WRITE operations (update/checkpoint) require an
// exclusive lease backed by heartbeats:
//   - READ (load/list/status)      → never locked (anyone may read anytime)
//   - CREATE (new workspace/file)  → never locked (new keys never conflict)
//   - WRITE (update/checkpoint)    → owner must hold a live lease; if another
//     agent's lease is expired (> CLAIM_TIMEOUT_MS without heartbeat) the
//     writer may take over automatically.
export const CLAIM_TIMEOUT_MS = 10 * 60 * 1000; // 10 min

interface LeaseRow {
  claimed_by: string | null;
  claimed_at: number | null;
  heartbeat_at: number | null;
}

/**
 * Returns the workspace's current lease info (never blocks a read).
 */
async function readLease(env: Env, wsId: string): Promise<LeaseRow | null> {
  return env.cloud_memory_db
    .prepare("SELECT claimed_by, claimed_at, heartbeat_at FROM workspaces WHERE id = ?")
    .bind(wsId)
    .first<LeaseRow>();
}

/**
 * Acquire (or refresh) the write lease for `agent`. Returns null on success,
 * or a Response to return when another agent holds a live lease.
 */
async function requireWriteLease(
  env: Env,
  wsId: string,
  agent: string
): Promise<Response | null> {
  const row = await readLease(env, wsId);
  if (!row) return errResp("Workspace not found", 404);

  const now = Date.now();
  if (row.claimed_by && row.claimed_by !== agent) {
    const lastActive = row.heartbeat_at || row.claimed_at || 0;
    const idleMs = now - lastActive;
    if (idleMs < CLAIM_TIMEOUT_MS) {
      return errResp(
        `Workspace is being edited by '${row.claimed_by}' (last activity ${Math.round(idleMs / 60000)} min ago). Read-only until the ${Math.round((CLAIM_TIMEOUT_MS - idleMs) / 60000)}-min lease expires or they release.`,
        409
      );
    }
    // Expired lease → takeover
  }

  // Acquire / refresh lease atomically
  await env.cloud_memory_db
    .prepare("UPDATE workspaces SET claimed_by = ?, claimed_at = ?, heartbeat_at = ? WHERE id = ?")
    .bind(agent, now, now, wsId)
    .run();
  return null;
}

/** Refresh the heartbeat of the current lease holder (keeps lease alive). */
async function refreshLease(env: Env, wsId: string): Promise<void> {
  await env.cloud_memory_db
    .prepare("UPDATE workspaces SET heartbeat_at = ? WHERE id = ?")
    .bind(Date.now(), wsId)
    .run();
}

// ─── Ensure tables exist (idempotent) ─────────────────────────────────────

export async function ensureWorkspaceTables(env: Env): Promise<void> {
  await env.cloud_memory_db.prepare(`
    CREATE TABLE IF NOT EXISTS workspaces (
      id TEXT PRIMARY KEY,
      name TEXT NOT NULL,
      label TEXT,
      status TEXT DEFAULT 'active',
      file_count INTEGER DEFAULT 0,
      variable_count INTEGER DEFAULT 0,
      decision_count INTEGER DEFAULT 0,
      next_step_count INTEGER DEFAULT 0,
      created_at INTEGER,
      updated_at INTEGER,
      last_checkpoint_at INTEGER
    )
  `).run();

  await env.cloud_memory_db.prepare(`
    CREATE TABLE IF NOT EXISTS workspace_state (
      workspace_id TEXT NOT NULL,
      category TEXT NOT NULL,
      key TEXT NOT NULL,
      value TEXT NOT NULL,
      updated_at INTEGER,
      PRIMARY KEY (workspace_id, category, key)
    )
  `).run();

  await env.cloud_memory_db.prepare(`
    CREATE TABLE IF NOT EXISTS workspace_checkpoints (
      id TEXT PRIMARY KEY,
      workspace_id TEXT NOT NULL,
      seq INTEGER NOT NULL,
      pool_snapshot TEXT NOT NULL,
      vault_copy TEXT,
      fact_summary TEXT,
      cloud_ref TEXT,
      checksum TEXT NOT NULL,
      created_at INTEGER
    )
  `).run();

  // Add lifecycle columns if not exists (migration)
  try {
    await env.cloud_memory_db.prepare(`ALTER TABLE workspaces ADD COLUMN claimed_by TEXT`).run();
  } catch { /* column already exists */ }
  try {
    await env.cloud_memory_db.prepare(`ALTER TABLE workspaces ADD COLUMN claimed_at INTEGER`).run();
  } catch { /* column already exists */ }
  try {
    await env.cloud_memory_db.prepare(`ALTER TABLE workspaces ADD COLUMN heartbeat_at INTEGER`).run();
  } catch { /* column already exists */ }

  await env.cloud_memory_db.prepare(
    `CREATE INDEX IF NOT EXISTS idx_ws_status ON workspaces(status)`
  ).run();
  await env.cloud_memory_db.prepare(
    `CREATE INDEX IF NOT EXISTS idx_ws_state_ws ON workspace_state(workspace_id)`
  ).run();
  await env.cloud_memory_db.prepare(
    `CREATE INDEX IF NOT EXISTS idx_ws_ckpt_ws ON workspace_checkpoints(workspace_id)`
  ).run();
}

// ─── Fact extraction from workspace state ────────────────────────────────

function extractFactsFromState(state: WorkspaceState, wsName: string, wsId: string): string[] {
  const facts: string[] = [];

  // Each decision is a fact
  for (const [id, desc] of Object.entries(state.decisions)) {
    facts.push(`[workspace:${wsName}] Decision: ${desc}`);
  }

  // Each next_step is a fact
  for (const step of state.next_steps) {
    facts.push(`[workspace:${wsName}] Next step: ${step}`);
  }

  // Key variables (skip internal ones)
  for (const [key, val] of Object.entries(state.variables)) {
    if (val && val.length < 500) {
      facts.push(`[workspace:${wsName}] Variable ${key} = ${val}`);
    }
  }

  // Context as a single fact
  if (state.context && state.context.trim().length > 0) {
    const ctx = state.context.trim().slice(0, 1000);
    facts.push(`[workspace:${wsName}] Context: ${ctx}`);
  }

  // File names (not contents) as facts
  const fileNames = Object.keys(state.files);
  if (fileNames.length > 0) {
    facts.push(`[workspace:${wsName}] Working on ${fileNames.length} file(s): ${fileNames.slice(0, 10).join(", ")}${fileNames.length > 10 ? ", ..." : ""}`);
  }

  return facts;
}

// ─── Load full workspace bundle from D1 ───────────────────────────────────

async function loadBundle(env: Env, wsId: string): Promise<WorkspaceBundle | null> {
  const meta = await env.cloud_memory_db
    .prepare("SELECT * FROM workspaces WHERE id = ?")
    .bind(wsId)
    .first<any>();
  if (!meta) return null;

  const rows = await env.cloud_memory_db
    .prepare("SELECT category, key, value FROM workspace_state WHERE workspace_id = ? ORDER BY category, key")
    .bind(wsId)
    .all<{ category: string; key: string; value: string }>();

  const state: WorkspaceState = {
    files: {},
    variables: {},
    decisions: {},
    next_steps: [],
    context: "",
  };

  const nextSteps: { key: string; value: string }[] = [];

  for (const row of rows.results || []) {
    switch (row.category) {
      case "file":
        state.files[row.key] = row.value;
        break;
      case "variable":
        state.variables[row.key] = row.value;
        break;
      case "decision":
        state.decisions[row.key] = row.value;
        break;
      case "next_step":
        nextSteps.push({ key: row.key, value: row.value });
        break;
      case "context":
        state.context = row.value;
        break;
    }
  }

  state.next_steps = nextSteps
    .sort((a, b) => {
      const ai = Number(a.key.replace(/^step_/, ""));
      const bi = Number(b.key.replace(/^step_/, ""));
      return ai - bi;
    })
    .map((step) => step.value);

  return {
    id: meta.id,
    name: meta.name,
    label: meta.label,
    status: meta.status,
    state,
    created_at: meta.created_at,
    updated_at: meta.updated_at,
    last_checkpoint_at: meta.last_checkpoint_at,
    file_count: meta.file_count,
    variable_count: meta.variable_count,
    decision_count: meta.decision_count,
    next_step_count: meta.next_step_count,
  };
}

// ─── Handlers ─────────────────────────────────────────────────────────────

/**
 * POST /workspace/create
 * Body: { name: string, label?: string, state?: Partial<WorkspaceState> }
 */
export async function handleWsCreate(request: Request, env: Env): Promise<Response> {
  if (!checkAuth(request, env)) return errResp("Unauthorized", 401);

  const body = await request.json<{
    name?: string;
    label?: string;
    state?: Partial<WorkspaceState>;
  }>();

  const name = body.name?.trim();
  if (!name) return errResp("name is required");

  const id = generateId();
  const now = Date.now();

  // Insert workspace metadata
  await env.cloud_memory_db
    .prepare(
      `INSERT INTO workspaces (id, name, label, status, created_at, updated_at)
       VALUES (?, ?, ?, 'active', ?, ?)`
    )
    .bind(id, name, body.label || null, now, now)
    .run();

  // Insert initial state entries
  const state = body.state || {};
  const statements: D1Statement[] = [];

  if (state.files) {
    for (const [key, value] of Object.entries(state.files)) {
      statements.push(
        env.cloud_memory_db
          .prepare("INSERT OR REPLACE INTO workspace_state (workspace_id, category, key, value, updated_at) VALUES (?, 'file', ?, ?, ?)")
          .bind(id, key, value, now)
      );
    }
  }
  if (state.variables) {
    for (const [key, value] of Object.entries(state.variables)) {
      statements.push(
        env.cloud_memory_db
          .prepare("INSERT OR REPLACE INTO workspace_state (workspace_id, category, key, value, updated_at) VALUES (?, 'variable', ?, ?, ?)")
          .bind(id, key, value, now)
      );
    }
  }
  if (state.decisions) {
    for (const [key, value] of Object.entries(state.decisions)) {
      statements.push(
        env.cloud_memory_db
          .prepare("INSERT OR REPLACE INTO workspace_state (workspace_id, category, key, value, updated_at) VALUES (?, 'decision', ?, ?, ?)")
          .bind(id, key, value, now)
      );
    }
  }
  if (state.next_steps) {
    for (let i = 0; i < state.next_steps.length; i++) {
      statements.push(
        env.cloud_memory_db
          .prepare("INSERT OR REPLACE INTO workspace_state (workspace_id, category, key, value, updated_at) VALUES (?, 'next_step', ?, ?, ?)")
          .bind(id, `step_${i}`, state.next_steps[i], now)
      );
    }
  }
  if (state.context) {
    statements.push(
      env.cloud_memory_db
        .prepare("INSERT OR REPLACE INTO workspace_state (workspace_id, category, key, value, updated_at) VALUES (?, 'context', 'context', ?, ?)")
        .bind(id, state.context, now)
    );
  }

  if (statements.length > 0) {
    await env.cloud_memory_db.batch(statements);
  }

  // Update counts
  const fileCount = state.files ? Object.keys(state.files).length : 0;
  const varCount = state.variables ? Object.keys(state.variables).length : 0;
  const decCount = state.decisions ? Object.keys(state.decisions).length : 0;
  const stepCount = state.next_steps ? state.next_steps.length : 0;

  await env.cloud_memory_db
    .prepare(
      `UPDATE workspaces SET file_count=?, variable_count=?, decision_count=?, next_step_count=? WHERE id=?`
    )
    .bind(fileCount, varCount, decCount, stepCount, id)
    .run();

  const bundle = await loadBundle(env, id);
  return jsonResp({ ok: true, workspace: bundle }, 201);
}

/**
 * GET /workspace/list
 * Query: ?status=active (default) | paused | archived | all
 */
export async function handleWsList(request: Request, env: Env): Promise<Response> {
  const url = new URL(request.url);
  const status = url.searchParams.get("status") || "active";

  let query = "SELECT * FROM workspaces";
  const params: any[] = [];

  if (status !== "all") {
    query += " WHERE status = ?";
    params.push(status);
  }
  query += " ORDER BY updated_at DESC";

  const rows = params.length > 0
    ? await env.cloud_memory_db.prepare(query).bind(...params).all<any>()
    : await env.cloud_memory_db.prepare(query).all<any>();

  return jsonResp({
    ok: true,
    count: (rows.results || []).length,
    workspaces: (rows.results || []).map((r) => ({
      id: r.id,
      name: r.name,
      label: r.label,
      status: r.status,
      file_count: r.file_count,
      variable_count: r.variable_count,
      decision_count: r.decision_count,
      next_step_count: r.next_step_count,
      created_at: r.created_at,
      updated_at: r.updated_at,
      last_checkpoint_at: r.last_checkpoint_at,
    })),
  });
}

/**
 * GET /workspace/:id
 * Load full workspace state (lossless bundle).
 */
export async function handleWsLoad(request: Request, env: Env, wsId: string): Promise<Response> {
  const bundle = await loadBundle(env, wsId);
  if (!bundle) return errResp("Workspace not found", 404);
  return jsonResp({ ok: true, workspace: bundle });
}

/**
 * POST /workspace/:id/update
 * Body: { state: Partial<WorkspaceState> }
 * Merges into existing state (upsert).
 */
export async function handleWsUpdate(request: Request, env: Env, wsId: string): Promise<Response> {
  if (!checkAuth(request, env)) return errResp("Unauthorized", 401);

  // Verify workspace exists
  const existing = await env.cloud_memory_db
    .prepare("SELECT id FROM workspaces WHERE id = ?")
    .bind(wsId)
    .first();
  if (!existing) return errResp("Workspace not found", 404);

  const body = await request.json<{ state?: Partial<WorkspaceState>; agent?: string }>();
  const state = body.state || {};

  // Write guard: require a live write lease (read-only load always allowed)
  const leaseErr = await requireWriteLease(env, wsId, body.agent || "unknown");
  if (leaseErr) return leaseErr;

  const now = Date.now();
  const statements: D1Statement[] = [];

  // Upsert files
  if (state.files) {
    for (const [key, value] of Object.entries(state.files)) {
      statements.push(
        env.cloud_memory_db
          .prepare("INSERT OR REPLACE INTO workspace_state (workspace_id, category, key, value, updated_at) VALUES (?, 'file', ?, ?, ?)")
          .bind(wsId, key, value, now)
      );
    }
  }

  // Upsert variables
  if (state.variables) {
    for (const [key, value] of Object.entries(state.variables)) {
      statements.push(
        env.cloud_memory_db
          .prepare("INSERT OR REPLACE INTO workspace_state (workspace_id, category, key, value, updated_at) VALUES (?, 'variable', ?, ?, ?)")
          .bind(wsId, key, value, now)
      );
    }
  }

  // Upsert decisions
  if (state.decisions) {
    for (const [key, value] of Object.entries(state.decisions)) {
      statements.push(
        env.cloud_memory_db
          .prepare("INSERT OR REPLACE INTO workspace_state (workspace_id, category, key, value, updated_at) VALUES (?, 'decision', ?, ?, ?)")
          .bind(wsId, key, value, now)
      );
    }
  }

  // Replace next_steps (ordered list — replace all)
  if (state.next_steps !== undefined) {
    // Delete existing
    statements.push(
      env.cloud_memory_db
        .prepare("DELETE FROM workspace_state WHERE workspace_id = ? AND category = 'next_step'")
        .bind(wsId)
    );
    // Insert new
    for (let i = 0; i < state.next_steps.length; i++) {
      statements.push(
        env.cloud_memory_db
          .prepare("INSERT OR REPLACE INTO workspace_state (workspace_id, category, key, value, updated_at) VALUES (?, 'next_step', ?, ?, ?)")
          .bind(wsId, `step_${i}`, state.next_steps[i], now)
      );
    }
  }

  // Upsert context
  if (state.context !== undefined) {
    statements.push(
      env.cloud_memory_db
        .prepare("INSERT OR REPLACE INTO workspace_state (workspace_id, category, key, value, updated_at) VALUES (?, 'context', 'context', ?, ?)")
        .bind(wsId, state.context, now)
    );
  }

  if (statements.length > 0) {
    await env.cloud_memory_db.batch(statements);
  }

  // Update workspace metadata counts
  const allState = await env.cloud_memory_db
    .prepare("SELECT category, COUNT(*) as cnt FROM workspace_state WHERE workspace_id = ? GROUP BY category")
    .bind(wsId)
    .all<{ category: string; cnt: number }>();

  const counts: Record<string, number> = {};
  for (const row of allState.results || []) {
    counts[row.category] = row.cnt;
  }

  await env.cloud_memory_db
    .prepare(
      `UPDATE workspaces SET updated_at=?, file_count=?, variable_count=?, decision_count=?, next_step_count=? WHERE id=?`
    )
    .bind(
      now,
      counts["file"] || 0,
      counts["variable"] || 0,
      counts["decision"] || 0,
      counts["next_step"] || 0,
      wsId
    )
    .run();

  const bundle = await loadBundle(env, wsId);
  return jsonResp({ ok: true, workspace: bundle });
}

/**
 * POST /workspace/:id/checkpoint
 * Body: { vault_copy?: string, fact_summary?: string, cloud_ref?: string }
 *
 * Creates a 4-way checkpoint:
 *   pool  = full state snapshot (automatic)
 *   vault = persistent copy (provided by caller)
 *   fact  = extracted facts (provided by caller)
 *   cloud = external reference (provided by caller)
 */
export async function handleWsCheckpoint(
  request: Request,
  env: Env,
  wsId: string
): Promise<Response> {
  if (!checkAuth(request, env)) return errResp("Unauthorized", 401);

  // Load current state
  const bundle = await loadBundle(env, wsId);
  if (!bundle) return errResp("Workspace not found", 404);

  const body = await request.json<{
    vault_copy?: string;
    fact_summary?: string;
    cloud_ref?: string;
    agent?: string;
  }>();

  // Write guard: checkpoint mutates workspace (snapshot + facts) → require lease
  const leaseErr = await requireWriteLease(env, wsId, body.agent || "unknown");
  if (leaseErr) return leaseErr;

  // Create pool snapshot (lossless full state)
  const poolSnapshot = JSON.stringify(bundle.state);

  // Compute checksum (simple hash of snapshot)
  const checksum = await sha256Hex(poolSnapshot);

  // Get next sequence number
  const lastSeq = await env.cloud_memory_db
    .prepare("SELECT MAX(seq) as max_seq FROM workspace_checkpoints WHERE workspace_id = ?")
    .bind(wsId)
    .first<{ max_seq: number }>();
  const seq = (lastSeq?.max_seq || 0) + 1;

  const cpId = `cp-${wsId}-${seq}`;
  const now = Date.now();

  await env.cloud_memory_db
    .prepare(
      `INSERT INTO workspace_checkpoints (id, workspace_id, seq, pool_snapshot, vault_copy, fact_summary, cloud_ref, checksum, created_at)
       VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`
    )
    .bind(
      cpId,
      wsId,
      seq,
      poolSnapshot,
      body.vault_copy || null,
      body.fact_summary || null,
      body.cloud_ref || null,
      checksum,
      now
    )
    .run();

  // Update workspace's last_checkpoint_at
  await env.cloud_memory_db
    .prepare("UPDATE workspaces SET last_checkpoint_at = ? WHERE id = ?")
    .bind(now, wsId)
    .run();

  // ─── Auto-extract facts → push to memory (searchable) ────────
  const facts = extractFactsFromState(bundle.state, bundle.name, wsId);
  let factsPushed = 0;
  if (facts.length > 0) {
    try {
      // Embed all facts in one batch
      const embeddingResponse: any = await env.AI.run("@cf/baai/bge-m3", {
        text: facts.join("\n---\n"),
      });
      let allVectors: number[][] = [];
      if (Array.isArray(embeddingResponse?.data?.[0])) {
        if (typeof embeddingResponse.data[0][0] === "number") {
          // Single embedding for all facts combined — split later
          // Actually, bge-m3 processes each text separately
          allVectors = embeddingResponse.data.map((d: any) => d.embedding || d);
        } else {
          allVectors = embeddingResponse.data.map((d: any) => d.embedding || d);
        }
      }

      // If we got fewer vectors than facts, embed individually
      if (allVectors.length < facts.length) {
        allVectors = [];
        for (const fact of facts) {
          const resp: any = await env.AI.run("@cf/baai/bge-m3", { text: fact });
          const vec = resp?.data?.[0]?.embedding || resp?.data?.[0];
          if (Array.isArray(vec)) allVectors.push(vec);
        }
      }

      // Insert facts as memory chunks
      const stmt = env.cloud_memory_db.prepare(
        "INSERT OR REPLACE INTO chunks (id, source_file, chunk_index, text) VALUES (?, ?, ?, ?)"
      );
      const statements: D1Statement[] = [];
      const vectors: { id: string; values: number[]; namespace: string }[] = [];

      for (let i = 0; i < facts.length; i++) {
        const factId = `ws-fact/${wsId}/${cpId}_${i}`;
        const factSource = `ws-fact/${bundle.name}/checkpoint-${seq}`;
        statements.push(stmt.bind(factId, factSource, i, facts[i]));
        if (allVectors[i]) {
          vectors.push({ id: factId, values: allVectors[i], namespace: "ws-fact" });
        }
      }

      if (statements.length > 0) {
        await env.cloud_memory_db.batch(statements);
      }
      if (vectors.length > 0) {
        await env.VECTORIZE.upsert(vectors);
      }
      factsPushed = facts.length;
    } catch (e) {
      // Non-fatal: checkpoint succeeded, fact extraction is best-effort
      console.error("Fact extraction failed:", e);
    }
  }

  return jsonResp({
    ok: true,
    checkpoint: {
      id: cpId,
      workspace_id: wsId,
      seq,
      checksum,
      created_at: now,
      snapshot_size: poolSnapshot.length,
      facts_extracted: factsPushed,
    },
  });
}

/**
 * POST /workspace/:id/archive
 * Archive a workspace (set status = 'archived').
 */
export async function handleWsArchive(
  request: Request,
  env: Env,
  wsId: string
): Promise<Response> {
  if (!checkAuth(request, env)) return errResp("Unauthorized", 401);

  const now = Date.now();
  await env.cloud_memory_db
    .prepare("UPDATE workspaces SET status = 'archived', updated_at = ? WHERE id = ?")
    .bind(now, wsId)
    .run();

  return jsonResp({ ok: true, id: wsId, status: "archived" });
}

/**
 * DELETE /workspace/:id
 * Permanently delete workspace and all its state + checkpoints.
 */
export async function handleWsDelete(
  request: Request,
  env: Env,
  wsId: string
): Promise<Response> {
  if (!checkAuth(request, env)) return errResp("Unauthorized", 401);

  // Verify exists
  const existing = await env.cloud_memory_db
    .prepare("SELECT id FROM workspaces WHERE id = ?")
    .bind(wsId)
    .first();
  if (!existing) return errResp("Workspace not found", 404);

  // Delete in order: checkpoints → state → workspace
  await env.cloud_memory_db
    .prepare("DELETE FROM workspace_checkpoints WHERE workspace_id = ?")
    .bind(wsId)
    .run();
  await env.cloud_memory_db
    .prepare("DELETE FROM workspace_state WHERE workspace_id = ?")
    .bind(wsId)
    .run();
  await env.cloud_memory_db
    .prepare("DELETE FROM workspaces WHERE id = ?")
    .bind(wsId)
    .run();

  return jsonResp({ ok: true, deleted: wsId });
}

/**
 * POST /workspace/:id/search
 * Body: { q: string, k?: number }
 * Search within a workspace's state (files, variables, decisions, next_steps).
 * Returns matching entries with context.
 */
export async function handleWsSearch(
  request: Request,
  env: Env,
  wsId: string
): Promise<Response> {
  const body = await request.json<{ q: string; k?: number }>();
  const q = (body.q || "").trim().toLowerCase();
  const k = Math.min(body.k || 10, 50);

  if (!q) return errResp("q is required");

  const bundle = await loadBundle(env, wsId);
  if (!bundle) return errResp("Workspace not found", 404);

  const results: {
    category: string;
    key: string;
    match: string;
    snippet: string;
  }[] = [];

  function searchIn(category: string, key: string, value: string) {
    const lower = value.toLowerCase();
    const idx = lower.indexOf(q);
    if (idx === -1) return;

    const start = Math.max(0, idx - 80);
    const end = Math.min(value.length, idx + q.length + 80);
    const snippet = (start > 0 ? "..." : "") +
      value.slice(start, idx) +
      "**" + value.slice(idx, idx + q.length) + "**" +
      value.slice(idx + q.length, end) +
      (end < value.length ? "..." : "");
    results.push({ category, key, match: value.slice(idx, idx + q.length), snippet });
  }

  // Search files
  for (const [path, content] of Object.entries(bundle.state.files)) {
    searchIn("file", path, content);
  }

  // Search variables
  for (const [key, val] of Object.entries(bundle.state.variables)) {
    searchIn("variable", key, val);
  }

  // Search decisions
  for (const [id, desc] of Object.entries(bundle.state.decisions)) {
    searchIn("decision", id, desc);
  }

  // Search next_steps
  for (let i = 0; i < bundle.state.next_steps.length; i++) {
    searchIn("next_step", `step_${i}`, bundle.state.next_steps[i]);
  }

  // Search context
  if (bundle.state.context) {
    searchIn("context", "context", bundle.state.context);
  }

  return jsonResp({
    ok: true,
    workspace_id: wsId,
    query: q,
    results: results.slice(0, k),
    total: results.length,
  });
}

/**
 * GET /workspace/:id/checkpoints
 * List checkpoints for a workspace.
 */
export async function handleWsCheckpoints(
  request: Request,
  env: Env,
  wsId: string
): Promise<Response> {
  const rows = await env.cloud_memory_db
    .prepare(
      "SELECT id, seq, vault_copy, fact_summary, cloud_ref, checksum, created_at FROM workspace_checkpoints WHERE workspace_id = ? ORDER BY seq DESC"
    )
    .bind(wsId)
    .all<any>();

  return jsonResp({
    ok: true,
    count: (rows.results || []).length,
    checkpoints: (rows.results || []).map((r) => ({
      id: r.id,
      seq: r.seq,
      has_vault: !!r.vault_copy,
      has_facts: !!r.fact_summary,
      has_cloud_ref: !!r.cloud_ref,
      checksum: r.checksum,
      created_at: r.created_at,
    })),
  });
}

// ─── Workspace Lifecycle ─────────────────────────────────────────────────

/**
 * POST /workspace/:id/heartbeat
 * Body: { agent?: string } — agent/machine identifier
 * Updates heartbeat timestamp. Called periodically by active agents.
 * The lease (write lock) is only extended when the caller is the current
 * owner — a passive observer may not keep someone else's lock alive.
 */
export async function handleWsHeartbeat(
  request: Request,
  env: Env,
  wsId: string
): Promise<Response> {
  const body = await request.json<{ agent?: string }>();
  const agent = body.agent || "unknown";
  const now = Date.now();

  // Verify workspace exists + read current lease
  const existing = await env.cloud_memory_db
    .prepare("SELECT id, claimed_by FROM workspaces WHERE id = ?")
    .bind(wsId)
    .first<{ id: string; claimed_by: string | null }>();
  if (!existing) return errResp("Workspace not found", 404);

  // Only the current lease owner may extend the lock's heartbeat.
  const isOwner = !existing.claimed_by || existing.claimed_by === agent;
  const heartbeatAt = isOwner ? now : null;

  if (heartbeatAt !== null) {
    await env.cloud_memory_db
      .prepare("UPDATE workspaces SET heartbeat_at = ?, updated_at = ? WHERE id = ?")
      .bind(now, now, wsId)
      .run();
  } else {
    // Non-owner ping: record activity but do NOT extend the lock.
    await env.cloud_memory_db
      .prepare("UPDATE workspaces SET updated_at = ? WHERE id = ?")
      .bind(now, wsId)
      .run();
  }

  return jsonResp({ ok: true, heartbeat_at: heartbeatAt !== null ? now : null, lease_extended: isOwner });
}

/**
 * POST /workspace/:id/claim
 * Body: { agent: string } — agent/machine identifier
 * Claims the workspace for exclusive use (write lock).
 * A claim is a heartbeat-based lease: it stays held while the owner
 * refreshes heartbeat_at, and expires after CLAIM_TIMEOUT_MS of silence
 * (then anyone may take over).
 */
export async function handleWsClaim(
  request: Request,
  env: Env,
  wsId: string
): Promise<Response> {
  if (!checkAuth(request, env)) return errResp("Unauthorized", 401);

  const body = await request.json<{ agent?: string }>();
  const agent = body.agent || "unknown";
  const now = Date.now();

  // Check if already claimed by someone else
  const existing = await env.cloud_memory_db
    .prepare("SELECT id, claimed_by, claimed_at, heartbeat_at FROM workspaces WHERE id = ?")
    .bind(wsId)
    .first<{ id: string; claimed_by: string | null; claimed_at: number | null; heartbeat_at: number | null }>();
  if (!existing) return errResp("Workspace not found", 404);

  if (existing.claimed_by && existing.claimed_by !== agent) {
    // Lease liveness = last heartbeat (or claim time if never heartbeated)
    const lastActive = existing.heartbeat_at || existing.claimed_at || 0;
    const idleMs = now - lastActive;
    if (idleMs < CLAIM_TIMEOUT_MS) {
      return errResp(`Workspace locked by '${existing.claimed_by}' (last activity ${Math.round(idleMs / 60000)} min ago)`, 409);
    }
    // Stale lease — allow takeover
  }

  await env.cloud_memory_db
    .prepare("UPDATE workspaces SET claimed_by = ?, claimed_at = ?, heartbeat_at = ? WHERE id = ?")
    .bind(agent, now, now, wsId)
    .run();

  return jsonResp({ ok: true, claimed_by: agent, claimed_at: now, timeout_ms: CLAIM_TIMEOUT_MS });
}

/**
 * POST /workspace/:id/release
 * Body: { agent: string }
 * Releases claim on workspace.
 */
export async function handleWsRelease(
  request: Request,
  env: Env,
  wsId: string
): Promise<Response> {
  if (!checkAuth(request, env)) return errResp("Unauthorized", 401);

  const body = await request.json<{ agent?: string }>();
  const agent = body.agent || "unknown";

  const existing = await env.cloud_memory_db
    .prepare("SELECT id, claimed_by FROM workspaces WHERE id = ?")
    .bind(wsId)
    .first<{ id: string; claimed_by: string | null }>();
  if (!existing) return errResp("Workspace not found", 404);

  if (existing.claimed_by && existing.claimed_by !== agent) {
    return errResp(`Workspace claimed by '${existing.claimed_by}', cannot release`, 403);
  }

  await env.cloud_memory_db
    .prepare("UPDATE workspaces SET claimed_by = NULL, claimed_at = NULL WHERE id = ?")
    .bind(wsId)
    .run();

  return jsonResp({ ok: true, released: wsId });
}

/**
 * POST /workspace/stale-detect
 * Find and auto-pause workspaces with stale heartbeats (> 24h without activity).
 * Returns list of paused workspaces.
 */
export async function handleWsStaleDetect(
  request: Request,
  env: Env
): Promise<Response> {
  if (!checkAuth(request, env)) return errResp("Unauthorized", 401);

  const now = Date.now();
  const staleThreshold = 24 * 3600 * 1000; // 24 hours

  // Find active workspaces with stale heartbeat (or never heartbeated but old)
  const stale = await env.cloud_memory_db
    .prepare(`
      SELECT id, name, heartbeat_at, claimed_by, updated_at
      FROM workspaces
      WHERE status = 'active'
        AND (
          (heartbeat_at IS NOT NULL AND heartbeat_at < ?)
          OR (heartbeat_at IS NULL AND updated_at < ?)
        )
    `)
    .bind(now - staleThreshold, now - staleThreshold)
    .all<{ id: string; name: string; heartbeat_at: number | null; claimed_by: string | null; updated_at: number }>();

  const paused: { id: string; name: string; idle_hours: number }[] = [];

  for (const ws of stale.results || []) {
    await env.cloud_memory_db
      .prepare("UPDATE workspaces SET status = 'paused', claimed_by = NULL, claimed_at = NULL WHERE id = ?")
      .bind(ws.id)
      .run();

    const lastActivity = ws.heartbeat_at || ws.updated_at;
    const idleHours = Math.round((now - lastActivity) / 3600000);
    paused.push({ id: ws.id, name: ws.name, idle_hours: idleHours });
  }

  return jsonResp({
    ok: true,
    paused_count: paused.length,
    paused,
  });
}

/**
 * GET /workspace/pool-status
 * Full pool status: active, paused, claimed, stale, etc.
 */
export async function handleWsPoolStatus(
  request: Request,
  env: Env
): Promise<Response> {
  const now = Date.now();

  const active = await env.cloud_memory_db
    .prepare("SELECT COUNT(*) as count FROM workspaces WHERE status = 'active'")
    .first<{ count: number }>();

  const paused = await env.cloud_memory_db
    .prepare("SELECT COUNT(*) as count FROM workspaces WHERE status = 'paused'")
    .first<{ count: number }>();

  const archived = await env.cloud_memory_db
    .prepare("SELECT COUNT(*) as count FROM workspaces WHERE status = 'archived'")
    .first<{ count: number }>();

  const claimed = await env.cloud_memory_db
    .prepare("SELECT id, name, claimed_by, claimed_at, heartbeat_at FROM workspaces WHERE claimed_by IS NOT NULL AND status = 'active'")
    .all<{ id: string; name: string; claimed_by: string; claimed_at: number; heartbeat_at: number | null }>();

  // Workspaces with heartbeat > 2h old (potential stale)
  const potentiallyStale = await env.cloud_memory_db
    .prepare(`
      SELECT id, name, heartbeat_at, updated_at
      FROM workspaces
      WHERE status = 'active'
        AND heartbeat_at IS NOT NULL
        AND heartbeat_at < ?
    `)
    .bind(now - 2 * 3600 * 1000)
    .all<{ id: string; name: string; heartbeat_at: number; updated_at: number }>();

  return jsonResp({
    ok: true,
    active: active?.count || 0,
    paused: paused?.count || 0,
    archived: archived?.count || 0,
    total: (active?.count || 0) + (paused?.count || 0) + (archived?.count || 0),
    claimed: (claimed.results || []).map(ws => ({
      id: ws.id,
      name: ws.name,
      claimed_by: ws.claimed_by,
      claimed_ago_min: Math.round((now - ws.claimed_at) / 60000),
      heartbeat_ago_min: ws.heartbeat_at ? Math.round((now - ws.heartbeat_at) / 60000) : null,
    })),
    potentially_stale: (potentiallyStale.results || []).map(ws => ({
      id: ws.id,
      name: ws.name,
      idle_hours: Math.round((now - (ws.heartbeat_at || ws.updated_at)) / 3600000),
    })),
  });
}

// ─── Workspace Dashboard ──────────────────────────────────────────────────

import { dashboardHtml, editorHtml } from "./workspace_html";

export function handleWorkspaceDashboard(_env: Env): Response {
  const html = dashboardHtml();
  return new Response(html, {
    headers: { "Content-Type": "text/html; charset=utf-8", ...corsHeaders() },
  });
}

// ─── Workspace Editor UI ──────────────────────────────────────────────────

export function handleWorkspaceEditor(_env: Env): Response {
  const html = editorHtml();
  return new Response(html, {
    headers: { "Content-Type": "text/html; charset=utf-8", ...corsHeaders() },
  });
}

// dummy to skip old code
const _DELETED_OLD_HTML = "";
// OLD HTML REMOVED
/*
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Cloud Workspace Pool</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { font-family: system-ui, -apple-system, sans-serif; background: #0a0a0a; color: #e0e0e0; min-height: 100vh; }
  .container { max-width: 800px; margin: 0 auto; padding: 16px; }
  h1 { font-size: 1.5rem; margin-bottom: 4px; color: #fff; }
  .subtitle { color: #888; font-size: 0.85rem; margin-bottom: 20px; }
  .pool-stats { display: flex; gap: 12px; margin-bottom: 20px; flex-wrap: wrap; }
  .stat-card { background: #1a1a1a; border: 1px solid #222; border-radius: 10px; padding: 14px; flex: 1; min-width: 80px; text-align: center; }
  .stat-num { font-size: 1.8rem; font-weight: 700; color: #6366f1; }
  .stat-label { font-size: 0.75rem; color: #888; margin-top: 4px; }
  .ws-list { display: flex; flex-direction: column; gap: 10px; }
  .ws-card { background: #1a1a1a; border: 1px solid #222; border-radius: 10px; padding: 14px; cursor: pointer; transition: border-color 0.2s; }
  .ws-card:hover { border-color: #6366f1; }
  .ws-card.claimed { border-color: #f59e0b; }
  .ws-card.stale { border-color: #ef4444; }
  .ws-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px; }
  .ws-name { font-size: 1rem; font-weight: 600; color: #fff; }
  .ws-badges { display: flex; gap: 6px; }
  .ws-badge { font-size: 0.65rem; padding: 2px 8px; border-radius: 12px; }
  .ws-badge.active { background: #1e293b; color: #4ade80; }
  .ws-badge.paused { background: #1e293b; color: #fbbf24; }
  .ws-badge.claimed { background: #422006; color: #fbbf24; }
  .ws-badge.stale { background: #450a0a; color: #f87171; }
  .ws-meta { display: flex; gap: 12px; font-size: 0.75rem; color: #888; flex-wrap: wrap; }
  .ws-meta span { display: flex; align-items: center; gap: 4px; }
  .ws-lifecycle { margin-top: 8px; font-size: 0.75rem; color: #94a3b8; }
  .ws-lifecycle .agent { color: #fbbf24; font-weight: 600; }
  .ws-lifecycle .stale-warn { color: #f87171; font-weight: 600; }
  .ws-actions { margin-top: 8px; display: flex; gap: 6px; }
  .ws-actions button { padding: 4px 10px; font-size: 0.7rem; border-radius: 6px; }
  .btn-hb { background: #1e3a5f; }
  .btn-claim { background: #422006; }
  .btn-release { background: #374151; }
  .ws-state { margin-top: 10px; font-size: 0.8rem; color: #aaa; }
  .ws-state summary { cursor: pointer; color: #6366f1; }
  .ws-state pre { margin-top: 8px; padding: 10px; background: #111; border-radius: 6px; font-size: 0.75rem; overflow-x: auto; max-height: 200px; overflow-y: auto; }
  .empty { color: #666; text-align: center; padding: 40px; font-size: 0.9rem; }
  .error { color: #f87171; font-size: 0.85rem; margin: 8px 0; }
  .loading { color: #888; font-size: 0.85rem; }
  button { padding: 8px 16px; border: none; border-radius: 8px; background: #6366f1; color: #fff; font-size: 0.85rem; cursor: pointer; }
  button:active { background: #4f46e5; }
  .refresh { margin-bottom: 16px; }
</style>
</head>
<body>
<div class="container">
  <h1>\u26a1 Cloud Workspace Pool</h1>
  <p class="subtitle">Active workspaces \u2014 loaded state, no search needed</p>
  <div class="pool-stats" id="stats"></div>
  <button class="refresh" onclick="loadPool()">Refresh</button>
  <div id="status"></div>
  <div id="workspaces" class="ws-list"></div>
</div>
<script>
  const W = location.origin;
  const st = document.getElementById('status');
  const wsEl = document.getElementById('workspaces');
  const statsEl = document.getElementById('stats');
  const now = Date.now();

  function timeAgo(ts) {
    if (!ts) return 'never';
    const min = Math.round((now - ts) / 60000);
    if (min < 60) return min + 'm ago';
    const hr = Math.round(min / 60);
    if (hr < 24) return hr + 'h ago';
    return Math.round(hr / 24) + 'd ago';
  }

  async function loadPool() {
    st.innerHTML = '<div class="loading">Loading...</div>';
    wsEl.innerHTML = '';
    try {
      const [active, paused, poolStatus] = await Promise.all([
        fetch(W + '/workspace/list?status=active').then(r => r.json()),
        fetch(W + '/workspace/list?status=paused').then(r => r.json()),
        fetch(W + '/workspace/pool-status').then(r => r.json()).catch(() => null),
      ]);
      const activeCount = active.count || 0;
      const pausedCount = paused.count || 0;
      const totalFiles = (active.workspaces || []).reduce((s, w) => s + (w.file_count || 0), 0);
      const totalVars = (active.workspaces || []).reduce((s, w) => s + (w.variable_count || 0), 0);
      const claimedCount = (poolStatus?.claimed || []).length;
      const staleCount = (poolStatus?.potentially_stale || []).length;

      statsEl.innerHTML = \`
        <div class="stat-card"><div class="stat-num">\${activeCount}</div><div class="stat-label">Active</div></div>
        <div class="stat-card"><div class="stat-num">\${pausedCount}</div><div class="stat-label">Paused</div></div>
        <div class="stat-card"><div class="stat-num">\${claimedCount}</div><div class="stat-label">Claimed</div></div>
        <div class="stat-card"><div class="stat-num">\${staleCount}</div><div class="stat-label">Stale</div></div>
        <div class="stat-card"><div class="stat-num">\${totalFiles}</div><div class="stat-label">Files</div></div>
      \`;
      st.innerHTML = '';

      const all = [...(active.workspaces || []), ...(paused.workspaces || [])];
      if (all.length === 0) {
        wsEl.innerHTML = '<div class="empty">No workspaces yet. Create one with ws_sync.py create &lt;name&gt;</div>';
        return;
      }

      const claimedMap = {};
      (poolStatus?.claimed || []).forEach(c => { claimedMap[c.id] = c; });
      const staleMap = {};
      (poolStatus?.potentially_stale || []).forEach(s => { staleMap[s.id] = s; });

      wsEl.innerHTML = all.map(ws => {
        const updated = timeAgo(ws.updated_at);
        const claimed = claimedMap[ws.id];
        const stale = staleMap[ws.id];
        const cardClass = claimed ? 'claimed' : (stale ? 'stale' : '');
        let badges = \`<span class="ws-badge \${ws.status}">\${ws.status}</span>\`;
        if (claimed) badges += \`<span class="ws-badge claimed">\u{1F511} \${esc(claimed.claimed_by)}</span>\`;
        if (stale) badges += \`<span class="ws-badge stale">\u26a0 idle \${stale.idle_hours}h</span>\`;

        let lifecycle = '';
        if (claimed) {
          lifecycle = \`<div class="ws-lifecycle">Claimed by <span class="agent">\${esc(claimed.claimed_by)}</span> (\${claimed.claimed_ago_min}min ago)\u2003\u2003Heartbeat \${claimed.heartbeat_ago_min !== null ? claimed.heartbeat_ago_min + 'min ago' : 'never'}</div>\`;
        }

        return \`<div class="ws-card \${cardClass}" onclick="loadWs('\${ws.id}')">
          <div class="ws-header">
            <span class="ws-name">\${esc(ws.name)}</span>
            <div class="ws-badges">\${badges}</div>
          </div>
          <div class="ws-meta">
            <span>\u{1f4c4} \${ws.file_count || 0} files</span>
            <span>\u{1f527} \${ws.variable_count || 0} vars</span>
            <span>\u{1f4cb} \${ws.decision_count || 0} decisions</span>
            <span>\u{1f4cc} \${ws.next_step_count || 0} steps</span>
          </div>
          <div class="ws-meta"><span>\u{1f550} \${updated}</span></div>
          \${lifecycle}
          <div id="ws-detail-\${ws.id}"></div>
        </div>\`;
      }).join('');
    } catch (e) {
      st.innerHTML = '<div class="error">Error: ' + esc(e.message) + '</div>';
    }
  }

  async function loadWs(id) {
    const el = document.getElementById('ws-detail-' + id);
    if (el.innerHTML) { el.innerHTML = ''; return; }
    el.innerHTML = '<div class="loading">Loading state...</div>';
    try {
      const res = await fetch(W + '/workspace/' + id);
      const data = await res.json();
      if (!data.ok) throw new Error(data.error);
      const ws = data.workspace;
      const s = ws.state || {};
      let html = '<details open><summary>\u{1f4e6} State Bundle</summary><pre>';
      html += JSON.stringify(s, null, 2);
      html += '</pre></details>';
      html += \`<div class="ws-actions">
        <button class="btn-hb" onclick="event.stopPropagation(); doHeartbeat('\${id}')">\u2764 Heartbeat</button>
        <button class="btn-claim" onclick="event.stopPropagation(); doClaim('\${id}')">\u{1f511} Claim</button>
        <button class="btn-release" onclick="event.stopPropagation(); doRelease('\${id}')">\u{1f513} Release</button>
        <button class="btn-release" onclick="event.stopPropagation(); doSearchWs('\${id}')">\u{1f50d} Search</button>
        <button class="btn-release" style="color:#f87171" onclick="event.stopPropagation(); doDeleteWs('\${id}')">\u{1f5d1} Delete</button>
      </div>\`;
      el.innerHTML = html;
    } catch (e) {
      el.innerHTML = '<div class="error">' + esc(e.message) + '</div>';
    }
  }

  async function doHeartbeat(id) {
    try {
      await fetch(W + '/workspace/' + id + '/heartbeat', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ agent: 'dashboard' }) });
      loadPool();
    } catch(e) { alert('Heartbeat failed: ' + e.message); }
  }
  async function doClaim(id) {
    const agent = prompt('Agent name:');
    if (!agent) return;
    try {
      const r = await fetch(W + '/workspace/' + id + '/claim', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ agent }) });
      const d = await r.json();
      if (!d.ok) alert('Error: ' + d.error); else loadPool();
    } catch(e) { alert('Claim failed: ' + e.message); }
  }
  async function doRelease(id) {
    const agent = prompt('Agent name:');
    if (!agent) return;
    try {
      const r = await fetch(W + '/workspace/' + id + '/release', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ agent }) });
      const d = await r.json();
      if (!d.ok) alert('Error: ' + d.error); else loadPool();
    } catch(e) { alert('Release failed: ' + e.message); }
  }

  async function doSearchWs(id) {
    const q = prompt('Search query:');
    if (!q) return;
    try {
      const r = await fetch(W + '/workspace/' + id + '/search', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ q, k: 20 }) });
      const d = await r.json();
      if (!d.ok) return alert('Error: ' + d.error);
      const results = d.results || [];
      if (results.length === 0) return alert('No matches for: ' + q);
      const msg = results.map(r => '[' + r.category + '] ' + r.key + '\n  ' + r.snippet).join('\n\n');
      alert(results.length + ' match(es):\n\n' + msg);
    } catch(e) { alert('Search failed: ' + e.message); }
  }

  async function doDeleteWs(id) {
    if (!confirm('DELETE workspace ' + id + '? This cannot be undone.')) return;
    try {
      const r = await fetch(W + '/workspace/' + id + '/delete', { method: 'POST' });
      const d = await r.json();
      if (!d.ok) alert('Error: ' + d.error); else loadPool();
    } catch(e) { alert('Delete failed: ' + e.message); }
  }

  function esc(t) { const d = document.createElement('div'); d.textContent = t; return d.innerHTML; }
  loadPool();
</script>
</body>
</html>`;
  return new Response(html, {
    headers: { "Content-Type": "text/html; charset=utf-8", ...corsHeaders() },
  });
}

// ─── Workspace Editor UI ──────────────────────────────────────────────────

export function handleWorkspaceEditor(env: Env): Response {
  const html = `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Workspace Editor</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { font-family: system-ui, -apple-system, sans-serif; background: #0a0a0a; color: #e0e0e0; min-height: 100vh; }
  .container { max-width: 900px; margin: 0 auto; padding: 16px; }
  h1 { font-size: 1.5rem; margin-bottom: 4px; color: #fff; }
  .subtitle { color: #888; font-size: 0.85rem; margin-bottom: 16px; }
  .tabs { display: flex; gap: 4px; margin-bottom: 16px; }
  .tab { padding: 8px 16px; border-radius: 8px; background: #1a1a1a; border: 1px solid #333; color: #888; cursor: pointer; font-size: 0.85rem; }
  .tab.active { background: #6366f1; color: #fff; border-color: #6366f1; }
  .panel { display: none; }
  .panel.active { display: block; }
  label { display: block; font-size: 0.8rem; color: #888; margin-bottom: 4px; margin-top: 12px; }
  input, textarea, select { width: 100%; padding: 10px 12px; border: 1px solid #333; border-radius: 8px; background: #1a1a1a; color: #fff; font-size: 0.9rem; font-family: inherit; outline: none; }
  input:focus, textarea:focus { border-color: #6366f1; }
  textarea { min-height: 120px; resize: vertical; }
  button { padding: 10px 20px; border: none; border-radius: 8px; background: #6366f1; color: #fff; font-size: 0.9rem; cursor: pointer; margin-top: 12px; }
  button:active { background: #4f46e5; }
  button.secondary { background: #374151; }
  .msg { margin-top: 8px; padding: 8px 12px; border-radius: 6px; font-size: 0.8rem; }
  .msg.ok { background: #064e3b; color: #4ade80; }
  .msg.err { background: #450a0a; color: #f87171; }
  .ws-select { display: flex; gap: 8px; margin-bottom: 16px; }
  .ws-select select { flex: 1; }
</style>
</head>
<body>
<div class="container">
  <h1>\u270f\ufe0f Workspace Editor</h1>
  <p class="subtitle">Create, edit, and push workspace state from browser</p>

  <div class="tabs">
    <div class="tab active" onclick="showTab('create')">Create</div>
    <div class="tab" onclick="showTab('edit')">Edit</div>
    <div class="tab" onclick="showTab('vars')">Variables</div>
    <div class="tab" onclick="showTab('decisions')">Decisions</div>
    <div class="tab" onclick="showTab('steps')">Next Steps</div>
  </div>

  <!-- CREATE -->
  <div id="panel-create" class="panel active">
    <label>Workspace Name</label>
    <input type="text" id="ws-name" placeholder="e.g. DWGLS-refactor">
    <label>Label (optional)</label>
    <input type="text" id="ws-label" placeholder="e.g. project, research">
    <label>Initial File Path</label>
    <input type="text" id="ws-filepath" placeholder="e.g. src/main.ts">
    <label>Initial File Content</label>
    <textarea id="ws-filecontent" placeholder="// file content..."></textarea>
    <button onclick="doCreate()">Create Workspace</button>
    <div id="msg-create"></div>
  </div>

  <!-- EDIT -->
  <div id="panel-edit" class="panel">
    <div class="ws-select">
      <select id="edit-ws"><option value="">Select workspace...</option></select>
      <button class="secondary" onclick="loadEditWs()">Load</button>
    </div>
    <div id="edit-content" style="display:none;">
      <label>File Path</label>
      <input type="text" id="edit-filepath" placeholder="path/to/file">
      <label>File Content</label>
      <textarea id="edit-filecontent" rows="10"></textarea>
      <button onclick="doUpdateFiles()">Save File</button>
      <button class="secondary" onclick="doPush()">\u2b06 Push to Cloud</button>
      <div id="msg-edit"></div>
    </div>
  </div>

  <!-- VARS -->
  <div id="panel-vars" class="panel">
    <div class="ws-select">
      <select id="vars-ws"><option value="">Select workspace...</option></select>
      <button class="secondary" onclick="loadVarsWs()">Load</button>
    </div>
    <div id="vars-content" style="display:none;">
      <label>Variable Key</label>
      <input type="text" id="var-key" placeholder="key">
      <label>Variable Value</label>
      <input type="text" id="var-value" placeholder="value">
      <button onclick="doAddVar()">Add/Update Variable</button>
      <button class="secondary" onclick="doPush()">\u2b06 Push to Cloud</button>
      <pre id="vars-list" style="margin-top:12px;padding:10px;background:#111;border-radius:6px;font-size:0.8rem;max-height:300px;overflow:auto;"></pre>
      <div id="msg-vars"></div>
    </div>
  </div>

  <!-- DECISIONS -->
  <div id="panel-decisions" class="panel">
    <div class="ws-select">
      <select id="dec-ws"><option value="">Select workspace...</option></select>
      <button class="secondary" onclick="loadDecWs()">Load</button>
    </div>
    <div id="dec-content" style="display:none;">
      <label>Decision ID</label>
      <input type="text" id="dec-id" placeholder="e.g. dec_001">
      <label>Description</label>
      <textarea id="dec-desc" placeholder="What was decided and why..."></textarea>
      <button onclick="doAddDecision()">Record Decision</button>
      <button class="secondary" onclick="doPush()">\u2b06 Push to Cloud</button>
      <pre id="dec-list" style="margin-top:12px;padding:10px;background:#111;border-radius:6px;font-size:0.8rem;max-height:300px;overflow:auto;"></pre>
      <div id="msg-dec"></div>
    </div>
  </div>

  <!-- STEPS -->
  <div id="panel-steps" class="panel">
    <div class="ws-select">
      <select id="steps-ws"><option value="">Select workspace...</option></select>
      <button class="secondary" onclick="loadStepsWs()">Load</button>
    </div>
    <div id="steps-content" style="display:none;">
      <label>Add Next Step</label>
      <input type="text" id="step-text" placeholder="What to do next...">
      <button onclick="doAddStep()">Add Step</button>
      <button class="secondary" onclick="doPush()">\u2b06 Push to Cloud</button>
      <ol id="steps-list" style="margin-top:12px;padding-left:20px;font-size:0.85rem;"></ol>
      <div id="msg-steps"></div>
    </div>
  </div>
</div>
<script>
  const W = location.origin;
  let currentWs = null;  // full workspace state
  let wsCache = {};      // id -> workspace data

  function showTab(name) {
    document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
    document.querySelectorAll('.panel').forEach(p => p.classList.remove('active'));
    document.querySelector(\`.tab:nth-child(\${['create','edit','vars','decisions','steps'].indexOf(name)+1})\`).classList.add('active');
    document.getElementById('panel-'+name).classList.add('active');
  }

  async function refreshWsLists() {
    try {
      const r = await fetch(W + '/workspace/list?status=active');
      const d = await r.json();
      const wss = d.workspaces || [];
      ['edit-ws','vars-ws','dec-ws','steps-ws'].forEach(id => {
        const sel = document.getElementById(id);
        sel.innerHTML = '<option value="">Select workspace...</option>' + wss.map(w => \`<option value="\${w.id}">\${w.name}</option>\`).join('');
      });
    } catch(e) {}
  }

  function showMsg(id, msg, ok) {
    document.getElementById(id).innerHTML = '<div class="msg '+(ok?'ok':'err')+'">'+msg+'</div>';
    setTimeout(() => document.getElementById(id).innerHTML = '', 4000);
  }

  async function doCreate() {
    const name = document.getElementById('ws-name').value.trim();
    if (!name) return showMsg('msg-create', 'Name required', false);
    const label = document.getElementById('ws-label').value.trim() || undefined;
    const fp = document.getElementById('ws-filepath').value.trim();
    const fc = document.getElementById('ws-filecontent').value;
    const body = { name, label };
    if (fp && fc) body.files = { [fp]: fc };
    try {
      const r = await fetch(W + '/workspace/create', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body)
      });
      const d = await r.json();
      if (d.ok) {
        showMsg('msg-create', 'Created: ' + d.workspace.id, true);
        refreshWsLists();
      } else showMsg('msg-create', d.error, false);
    } catch(e) { showMsg('msg-create', e.message, false); }
  }

  async function loadEditWs() {
    const id = document.getElementById('edit-ws').value;
    if (!id) return;
    try {
      const r = await fetch(W + '/workspace/' + id);
      const d = await r.json();
      if (!d.ok) throw new Error(d.error);
      currentWs = d.workspace;
      document.getElementById('edit-content').style.display = '';
      const files = currentWs.state.files || {};
      const keys = Object.keys(files);
      if (keys.length > 0) {
        document.getElementById('edit-filepath').value = keys[0];
        document.getElementById('edit-filecontent').value = files[keys[0]];
      }
      showMsg('msg-edit', 'Loaded: ' + currentWs.name + ' (' + keys.length + ' files)', true);
    } catch(e) { showMsg('msg-edit', e.message, false); }
  }

  async function doUpdateFiles() {
    if (!currentWs) return;
    const fp = document.getElementById('edit-filepath').value.trim();
    const fc = document.getElementById('edit-filecontent').value;
    if (!fp) return showMsg('msg-edit', 'File path required', false);
    try {
      const r = await fetch(W + '/workspace/' + currentWs.id, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ state: { files: { [fp]: fc } } })
      });
      const d = await r.json();
      if (d.ok) { currentWs = d.workspace; showMsg('msg-edit', 'Saved ' + fp, true); }
      else showMsg('msg-edit', d.error, false);
    } catch(e) { showMsg('msg-edit', e.message, false); }
  }

  async function doPush() {
    if (!currentWs) return showMsg('msg-edit', 'Load a workspace first', false);
    showMsg('msg-edit', 'Pushed to cloud ✓', true);
  }

  async function loadVarsWs() {
    const id = document.getElementById('vars-ws').value;
    if (!id) return;
    try {
      const r = await fetch(W + '/workspace/' + id);
      const d = await r.json();
      if (!d.ok) throw new Error(d.error);
      currentWs = d.workspace;
      document.getElementById('vars-content').style.display = '';
      document.getElementById('vars-list').textContent = JSON.stringify(currentWs.state.variables || {}, null, 2);
    } catch(e) { showMsg('msg-vars', e.message, false); }
  }

  async function doAddVar() {
    if (!currentWs) return;
    const key = document.getElementById('var-key').value.trim();
    const val = document.getElementById('var-value').value.trim();
    if (!key) return showMsg('msg-vars', 'Key required', false);
    try {
      const r = await fetch(W + '/workspace/' + currentWs.id, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ state: { variables: { [key]: val } } })
      });
      const d = await r.json();
      if (d.ok) { currentWs = d.workspace; document.getElementById('vars-list').textContent = JSON.stringify(currentWs.state.variables || {}, null, 2); showMsg('msg-vars', 'Saved ' + key, true); }
      else showMsg('msg-vars', d.error, false);
    } catch(e) { showMsg('msg-vars', e.message, false); }
  }

  async function loadDecWs() {
    const id = document.getElementById('dec-ws').value;
    if (!id) return;
    try {
      const r = await fetch(W + '/workspace/' + id);
      const d = await r.json();
      if (!d.ok) throw new Error(d.error);
      currentWs = d.workspace;
      document.getElementById('dec-content').style.display = '';
      document.getElementById('dec-list').textContent = JSON.stringify(currentWs.state.decisions || {}, null, 2);
    } catch(e) { showMsg('msg-dec', e.message, false); }
  }

  async function doAddDecision() {
    if (!currentWs) return;
    const id = document.getElementById('dec-id').value.trim();
    const desc = document.getElementById('dec-desc').value.trim();
    if (!id || !desc) return showMsg('msg-dec', 'ID and description required', false);
    try {
      const r = await fetch(W + '/workspace/' + currentWs.id, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ state: { decisions: { [id]: desc } } })
      });
      const d = await r.json();
      if (d.ok) { currentWs = d.workspace; document.getElementById('dec-list').textContent = JSON.stringify(currentWs.state.decisions || {}, null, 2); showMsg('msg-dec', 'Recorded: ' + id, true); }
      else showMsg('msg-dec', d.error, false);
    } catch(e) { showMsg('msg-dec', e.message, false); }
  }

  async function loadStepsWs() {
    const id = document.getElementById('steps-ws').value;
    if (!id) return;
    try {
      const r = await fetch(W + '/workspace/' + id);
      const d = await r.json();
      if (!d.ok) throw new Error(d.error);
      currentWs = d.workspace;
      document.getElementById('steps-content').style.display = '';
      renderSteps();
    } catch(e) { showMsg('msg-steps', e.message, false); }
  }

  function renderSteps() {
    const steps = currentWs?.state?.next_steps || [];
    document.getElementById('steps-list').innerHTML = steps.map(s => '<li>' + esc(s) + '</li>').join('');
  }

  async function doAddStep() {
    if (!currentWs) return;
    const text = document.getElementById('step-text').value.trim();
    if (!text) return;
    const steps = [...(currentWs.state.next_steps || []), text];
    try {
      const r = await fetch(W + '/workspace/' + currentWs.id, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ state: { next_steps: steps } })
      });
      const d = await r.json();
      if (d.ok) { currentWs = d.workspace; renderSteps(); document.getElementById('step-text').value = ''; showMsg('msg-steps', 'Added step', true); }
      else showMsg('msg-steps', d.error, false);
    } catch(e) { showMsg('msg-steps', e.message, false); }
  }

  function esc(t) { const d = document.createElement('div'); d.textContent = t; return d.innerHTML; }
  refreshWsLists();
</script>
</body>
</html>`;
  return new Response(html, {
    headers: { "Content-Type": "text/html; charset=utf-8", ...corsHeaders() },
  });
}
*/
