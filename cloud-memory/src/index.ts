export interface Env {
  cloud_memory_db: D1Database;
  VECTORIZE: Vectorize;
  AI: Ai;
  API_KEY: string;
}

interface IngestRequest {
  id: string;
  source_file: string;
  chunk_index: number;
  text: string;
  vector: number[];
}

interface SearchRequest {
  q: string;
  k?: number;
}

interface SearchResult {
  id: string;
  source_file: string;
  chunk_index: number;
  text: string;
  score: number;
}

interface ContextResult extends SearchResult {
  context: string;
  ctx_start: number;
  ctx_end: number;
}

const CTX_RADIUS = 2; // chunks before/after each match to include as context

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

async function sha256Hex(text: string): Promise<string> {
  const buf = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(text));
  return [...new Uint8Array(buf)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

async function handleIngest(request: Request, env: Env): Promise<Response> {
  if (!checkAuth(request, env)) {
    return new Response("Unauthorized", { status: 401, headers: corsHeaders() });
  }

  const body = await request.json<IngestRequest[]>();
  if (!Array.isArray(body) || body.length === 0) {
    return new Response("Invalid request: expected array of chunks", {
      status: 400,
      headers: corsHeaders(),
    });
  }

  // Batch insert into D1 (max 500 per batch for SQLite limits)
  const BATCH_SIZE = 100;
  let inserted = 0;

  for (let i = 0; i < body.length; i += BATCH_SIZE) {
    const batch = body.slice(i, i + BATCH_SIZE);

    // D1 batch insert
    const stmt = env.cloud_memory_db.prepare(
      "INSERT OR REPLACE INTO chunks (id, source_file, chunk_index, text) VALUES (?, ?, ?, ?)"
    );
    const statements = batch.map((chunk) =>
      stmt.bind(chunk.id, chunk.source_file, chunk.chunk_index, chunk.text)
    );
    await env.cloud_memory_db.batch(statements);

    // Vectorize batch upsert
    const vectors = batch.map((chunk) => ({
      id: chunk.id,
      values: chunk.vector,
      namespace: chunk.source_file.slice(0, 10),
    }));
    await env.VECTORIZE.upsert(vectors);

    inserted += batch.length;
  }

  // Data changed → cached query results are stale. Invalidate.
  await env.cloud_memory_db
    .prepare("DELETE FROM query_cache")
    .run();

  return new Response(
    JSON.stringify({ ok: true, inserted }),
    { headers: { "Content-Type": "application/json", ...corsHeaders() } }
  );
}

// ---------------------------------------------------------------------------
// Query cache — repeated searches hit D1 only (no Workers AI, no Vectorize).
// Invalidated on ingest. TTL as safety net.
// ---------------------------------------------------------------------------
const CACHE_TTL_MS = 7 * 24 * 3600 * 1000;
const CACHE_TOPK = 20; // always compute top 20, slice per request

function normalizeQuery(q: string): string {
  return q.trim().toLowerCase().replace(/\s+/g, " ");
}

async function getCachedQuery(env: Env, qhash: string): Promise<string | null> {
  const row = await env.cloud_memory_db
    .prepare("SELECT result, ts FROM query_cache WHERE hash = ?")
    .bind(qhash)
    .first<{ result: string; ts: number }>();
  if (!row) return null;
  if (Date.now() - (row.ts || 0) > CACHE_TTL_MS) return null;
  return row.result;
}

async function putCachedQuery(env: Env, qhash: string, q: string, result: string): Promise<void> {
  await env.cloud_memory_db
    .prepare("INSERT OR REPLACE INTO query_cache (hash, q, result, ts) VALUES (?, ?, ?, ?)")
    .bind(qhash, q, result, Date.now())
    .run();
}

async function ensureQueryCacheTable(env: Env): Promise<void> {
  await env.cloud_memory_db
    .prepare("CREATE TABLE IF NOT EXISTS query_cache (hash TEXT PRIMARY KEY, q TEXT, result TEXT, ts INTEGER)")
    .run();
}

async function searchChunks(env: Env, q: string, k: number): Promise<{ results: ContextResult[]; query: string }> {
  // Embed query using Workers AI (bge-m3)
  const embeddingResponse: any = await env.AI.run("@cf/baai/bge-m3", {
    text: q,
  });

  let queryVector: number[];
  if (Array.isArray(embeddingResponse?.data?.[0]) && typeof embeddingResponse.data[0][0] === "number") {
    queryVector = embeddingResponse.data[0];
  } else if (embeddingResponse?.data?.[0]?.embedding) {
    queryVector = embeddingResponse.data[0].embedding;
  } else {
    throw new Error(`Unexpected embedding response: ${JSON.stringify(Object.keys(embeddingResponse || {}))}`);
  }

  const results: VectorizeMatches = await env.VECTORIZE.query(queryVector, {
    topK: k,
    includeValues: false,
    includeMetadata: true,
  });

  if (!results || !results.matches || results.matches.length === 0) {
    return { results: [], query: q };
  }

  const ids = results.matches.map((m) => m.id);
  const placeholders = ids.map(() => "?").join(",");
  const rows = await env.cloud_memory_db
    .prepare(`SELECT id, source_file, chunk_index, text FROM chunks WHERE id IN (${placeholders})`)
    .bind(...ids)
    .all();

  const textMap = new Map<string, { source_file: string; chunk_index: number; text: string }>();
  if (rows.results) {
    for (const row of rows.results) {
      textMap.set(row.id as string, {
        source_file: row.source_file as string,
        chunk_index: row.chunk_index as number,
        text: row.text as string,
      });
    }
  }

  // Group matches by source_file so we can fetch surrounding context once per file
  const hits = results.matches
    .map((match) => {
      const chunk = textMap.get(match.id);
      return {
        id: match.id,
        source_file: chunk?.source_file || "",
        chunk_index: chunk?.chunk_index || 0,
        text: chunk?.text || "",
        score: match.score,
      };
    })
    .filter((h) => h.source_file !== "");

  // Build per-file index windows (chunk_index range to pull)
  const fileWindows = new Map<string, { min: number; max: number; hits: typeof hits }>();
  for (const h of hits) {
    let w = fileWindows.get(h.source_file);
    if (!w) {
      w = { min: h.chunk_index - CTX_RADIUS, max: h.chunk_index + CTX_RADIUS, hits: [] };
      fileWindows.set(h.source_file, w);
    }
    w.min = Math.max(0, Math.min(w.min, h.chunk_index - CTX_RADIUS));
    w.max = Math.max(w.max, h.chunk_index + CTX_RADIUS);
    w.hits.push(h);
  }

  // Fetch context chunks for each file window
  const ctxByFile = new Map<string, Map<number, string>>();
  for (const [file, w] of fileWindows) {
    const ctxRows = await env.cloud_memory_db
      .prepare("SELECT chunk_index, text FROM chunks WHERE source_file = ? AND chunk_index BETWEEN ? AND ? ORDER BY chunk_index")
      .bind(file, w.min, w.max)
      .all<{ chunk_index: number; text: string }>();
    const map = new Map<number, string>();
    if (ctxRows.results) {
      for (const r of ctxRows.results) {
        map.set(r.chunk_index as number, r.text as string);
      }
    }
    ctxByFile.set(file, map);
  }

  // Build results with expanded context (dedupe overlapping windows per file)
  const resultsOut: ContextResult[] = [];
  const seenWindows = new Map<string, Set<string>>(); // file -> set of "min:max"

  for (const h of hits) {
    const ctxMap = ctxByFile.get(h.source_file);
    const keys = ctxMap ? [...ctxMap.keys()].sort((a, b) => a - b) : [];
    const hitIdx = keys.indexOf(h.chunk_index);

    // Expand outward from the hit until we'd cover an already-emitted window
    let lo = hitIdx;
    let hi = hitIdx;
    while (lo > 0 && keys[lo - 1] >= h.chunk_index - CTX_RADIUS) lo--;
    while (hi < keys.length - 1 && keys[hi + 1] <= h.chunk_index + CTX_RADIUS) hi++;

    // Skip if this window was already emitted (same source, overlapping)
    const winKey = `${keys[lo]}:${keys[hi]}`;
    const seen = seenWindows.get(h.source_file) || new Set<string>();
    if (seen.has(winKey)) {
      continue;
    }
    seen.add(winKey);
    seenWindows.set(h.source_file, seen);

    const ctxParts: string[] = [];
    for (let i = lo; i <= hi; i++) {
      ctxParts.push(ctxMap.get(keys[i]) || "");
    }
    const context = ctxParts.join("\n\n---\n\n").trim();

    resultsOut.push({
      ...h,
      context,
      ctx_start: keys[lo],
      ctx_end: keys[hi],
    });
  }

  // Sort by best score
  resultsOut.sort((a, b) => b.score - a.score);

  // Deduplicate near-identical contexts from different files.
  // Skip the session header (up to the first speaker marker), then fingerprint
  // a window of tokens so forked sessions sharing an opening conversation collapse.
  const seenFingerprints = new Set<string>();
  const deduped: ContextResult[] = [];
  for (const r of resultsOut) {
    const ctx = r.context || "";
    const markerIdx = ctx.search(/\*\*(?:You|Assistant)[:*]/);
    const body = markerIdx >= 0 ? ctx.slice(markerIdx) : ctx.slice(0, 2000);
    const tokens = body.match(/[\p{L}\p{N}]{4,}/gu) || [];
    const fp = tokens.slice(0, 12).join(" ").toLowerCase();
    if (!fp || seenFingerprints.has(fp)) continue;
    seenFingerprints.add(fp);
    deduped.push(r);
  }

  return { results: deduped, query: q };
}

async function handleSearch(request: Request, env: Env): Promise<Response> {
  // Search is public — no auth required for reading

  const body = await request.json<SearchRequest>();
  const k = Math.min(body.k || 5, 20);

  if (!body.q || body.q.trim().length === 0) {
    return new Response("Invalid request: query is required", {
      status: 400,
      headers: corsHeaders(),
    });
  }

  const qnorm = normalizeQuery(body.q);
  const qhash = await sha256Hex(qnorm);

  try {
    // Cache hit → return stored result (slice to requested k)
    await ensureQueryCacheTable(env);
    const cached = await getCachedQuery(env, qhash);
    if (cached) {
      const parsed = JSON.parse(cached) as { results: ContextResult[]; query: string };
      return new Response(
        JSON.stringify({ results: parsed.results.slice(0, k), query: parsed.query, cached: true }),
        { headers: { "Content-Type": "application/json", ...corsHeaders() } }
      );
    }

    // Compute with topK=CACHE_TOPK so any k ≤ CACHE_TOPK is served from cache
    const result = await searchChunks(env, qnorm, CACHE_TOPK);
    await putCachedQuery(env, qhash, qnorm, JSON.stringify(result));

    return new Response(
      JSON.stringify({ ...result, results: result.results.slice(0, k), cached: false }),
      { headers: { "Content-Type": "application/json", ...corsHeaders() } }
    );
  } catch (e) {
    return new Response(
      JSON.stringify({ error: String(e) }),
      { status: 500, headers: { "Content-Type": "application/json", ...corsHeaders() } }
    );
  }
}

async function handleStatus(request: Request, env: Env): Promise<Response> {
  if (!checkAuth(request, env)) {
    return new Response("Unauthorized", { status: 401, headers: corsHeaders() });
  }

  const chunkCount = await env.cloud_memory_db
    .prepare("SELECT COUNT(*) as count FROM chunks")
    .first<{ count: number }>();

  return new Response(
    JSON.stringify({
      ok: true,
      chunks: chunkCount?.count || 0,
      vectorize_index: "cloud-memory-vectors",
    }),
    { headers: { "Content-Type": "application/json", ...corsHeaders() } }
  );
}

async function handleListSources(_request: Request, env: Env): Promise<Response> {
  const rows = await env.cloud_memory_db
    .prepare("SELECT source_file, COUNT(*) as cnt FROM chunks GROUP BY source_file ORDER BY source_file")
    .all<{ source_file: string; cnt: number }>();
  return new Response(JSON.stringify(rows.results), {
    headers: { "Content-Type": "application/json", ...corsHeaders() },
  });
}

async function handleCleanup(request: Request, env: Env): Promise<Response> {
  if (!checkAuth(request, env)) {
    return new Response("Unauthorized", { status: 401, headers: corsHeaders() });
  }

  const body = await request.json<{ prefix?: string; pattern?: string; exact?: string }>();

  let whereClause = "";
  let params: string[] = [];

  if (body.exact) {
    whereClause = "WHERE source_file = ?";
    params = [body.exact];
  } else if (body.pattern) {
    whereClause = "WHERE source_file LIKE ?";
    params = [body.pattern];
  } else if (body.prefix) {
    whereClause = "WHERE source_file LIKE ?";
    params = [body.prefix + "%"];
  } else {
    return new Response("Need prefix, pattern, or exact", { status: 400, headers: corsHeaders() });
  }

  const rows = await env.cloud_memory_db
    .prepare(`SELECT id FROM chunks ${whereClause}`)
    .bind(...params)
    .all<{ id: string }>();
  const ids = (rows.results || []).map(r => r.id);

  if (ids.length === 0) {
    return new Response(JSON.stringify({ deleted: 0 }), {
      headers: { "Content-Type": "application/json", ...corsHeaders() },
    });
  }

  for (let i = 0; i < ids.length; i += 50) {
    const batch = ids.slice(i, i + 50);
    const ph = batch.map(() => "?").join(",");
    await env.cloud_memory_db.prepare(`DELETE FROM chunks WHERE id IN (${ph})`).bind(...batch).run();
  }
  for (let i = 0; i < ids.length; i += 100) {
    const batch = ids.slice(i, i + 100);
    await env.VECTORIZE.deleteByIds(batch);
  }

  return new Response(JSON.stringify({ deleted: ids.length }), {
    headers: { "Content-Type": "application/json", ...corsHeaders() },
  });
}

const MCP_PROTOCOL_VERSION = "2025-06-18";

function mcpJsonResponse(id: unknown, result: unknown): Response {
  return new Response(
    JSON.stringify({ jsonrpc: "2.0", id, result }),
    { headers: { "Content-Type": "application/json", ...corsHeaders() } }
  );
}

function mcpErrorResponse(id: unknown, code: number, message: string): Response {
  return new Response(
    JSON.stringify({ jsonrpc: "2.0", id, error: { code, message } }),
    { headers: { "Content-Type": "application/json", ...corsHeaders() } }
  );
}

const MCP_TOOLS = [
  {
    name: "search_memory",
    description:
      "Semantic search over all chat history (OpenCode + Hermes, ~31k chunks). Use this FIRST for any question about past conversations. Returns matching chunks with surrounding context. Preferred over list_sources. Repeated identical queries are cached (fast). Example: user asks 'what did we discuss about DRamTile?' → call this.",
    inputSchema: {
      type: "object",
      properties: {
        q: { type: "string", description: "Search query (any language, English or Thai)" },
        k: { type: "number", description: "Number of results (default 5, max 20)" },
      },
      required: ["q"],
    },
  },
  {
    name: "list_sources",
    description:
      "NOT recommended for normal use — returns ~43KB JSON with all 468 files. Use search_memory instead to find relevant content. Only use if you really need the full file listing.",
    inputSchema: { type: "object", properties: {} },
  },
  {
    name: "memory_status",
    description: "Quick health check: total chunk count. Use only for diagnostics.",
    inputSchema: { type: "object", properties: {} },
  },
  {
    name: "memory_init",
    description:
      "Fetch the full index manifest ONCE per session (all files, chunk counts, dim=1024, address_space=20736). Cache the result — do NOT call again unless new data was synced. After this, use memory_get to navigate chunks locally without re-embedding. Call at start of session if planning multiple searches.",
    inputSchema: { type: "object", properties: {} },
  },
  {
    name: "memory_get",
    description:
      "Fetch chunk text(s) by source_file and chunk index range. Use AFTER search_memory to read more context around a hit — no embedding needed, very cheap. Example: search returns chunk 15 from file X → call memory_get(file X, from=13, to=17) to read surrounding chunks.",
    inputSchema: {
      type: "object",
      properties: {
        source_file: { type: "string", description: "Exact source file name from search results" },
        from: { type: "number", description: "Start chunk index (inclusive)" },
        to: { type: "number", description: "End chunk index (inclusive, default = from)" },
      },
      required: ["source_file", "from"],
    },
  },
  {
    name: "memory_remember",
    description:
      "Save a fact, decision, or summary to cloud memory. Requires api_key parameter. Use when user says 'remember this', 'save to memory', or states something important worth preserving across sessions.",
    inputSchema: {
      type: "object",
      properties: {
        text: { type: "string", description: "The text to remember" },
        source: { type: "string", description: "Optional label (default: 'user-memory')" },
        api_key: { type: "string", description: "API key for write access (required)" },
      },
      required: ["text", "api_key"],
    },
  },
];

async function handleMcp(request: Request, env: Env): Promise<Response> {
  if (request.method !== "POST") {
    return new Response("Method not allowed", { status: 405, headers: corsHeaders() });
  }

  let payload: any;
  try {
    payload = await request.json();
  } catch {
    return mcpErrorResponse(null, -32700, "Parse error");
  }

  const { id, method, params } = payload;

  // Accept API key from header, query param, or tool argument (for MCP clients that support it)
  const url = new URL(request.url);
  const mcpKey = request.headers.get("X-API-Key") || url.searchParams.get("key") || "";

  switch (method) {
    case "initialize":
      return mcpJsonResponse(id, {
        protocolVersion: MCP_PROTOCOL_VERSION,
        capabilities: { tools: {} },
        serverInfo: { name: "cloud-memory", version: "1.0.0" },
      });

    case "notifications/initialized":
      // Notification — no response body expected
      return new Response(null, { status: 202, headers: corsHeaders() });

    case "ping":
      return mcpJsonResponse(id, {});

    case "tools/list":
      return mcpJsonResponse(id, { tools: MCP_TOOLS });

    case "tools/call": {
      const toolName = params?.name;
      const args = params?.arguments || {};

      try {
        switch (toolName) {
          case "search_memory": {
            const q = String(args.q || "");
            if (!q.trim()) {
              return mcpErrorResponse(id, -32602, "search_memory requires 'q'");
            }
            const k = Math.min(Number(args.k) || 5, 20);
            const result = await searchChunks(env, q, k);
            return mcpJsonResponse(id, {
              content: [{ type: "text", text: JSON.stringify(result, null, 2) }],
            });
          }

          case "list_sources": {
            const rows = await env.cloud_memory_db
              .prepare("SELECT source_file, COUNT(*) as cnt FROM chunks GROUP BY source_file ORDER BY source_file")
              .all<{ source_file: string; cnt: number }>();
            return mcpJsonResponse(id, {
              content: [{ type: "text", text: JSON.stringify(rows.results, null, 2) }],
            });
          }

          case "memory_status": {
            const chunkCount = await env.cloud_memory_db
              .prepare("SELECT COUNT(*) as count FROM chunks")
              .first<{ count: number }>();
            return mcpJsonResponse(id, {
              content: [{ type: "text", text: JSON.stringify({
                chunks: chunkCount?.count || 0,
                vectorize_index: "cloud-memory-vectors",
              }, null, 2) }],
            });
          }

          case "memory_init": {
            // Compact manifest: file → chunk count, plus dim/address-space metadata.
            const rows = await env.cloud_memory_db
              .prepare("SELECT source_file, COUNT(*) as cnt FROM chunks GROUP BY source_file ORDER BY source_file")
              .all<{ source_file: string; cnt: number }>();
            const files = (rows.results || []).map((r) => ({
              source_file: r.source_file,
              n_chunks: r.cnt,
            }));
            return mcpJsonResponse(id, {
              content: [{ type: "text", text: JSON.stringify({
                ok: true,
                manifest_version: 1,
                generated_at: Date.now(),
                embedding_dim: 1024,
                address_space: 20736,
                total_files: files.length,
                files,
              }, null, 2) }],
            });
          }

          case "memory_get": {
            const sourceFile = String(args.source_file || "");
            const from = Math.max(0, Number(args.from) || 0);
            const to = Math.max(from, Number(args.to) !== undefined ? Number(args.to) : from);
            if (!sourceFile) {
              return mcpErrorResponse(id, -32602, "memory_get requires 'source_file'");
            }
            const rows = await env.cloud_memory_db
              .prepare("SELECT chunk_index, text FROM chunks WHERE source_file = ? AND chunk_index BETWEEN ? AND ? ORDER BY chunk_index")
              .bind(sourceFile, from, to)
              .all<{ chunk_index: number; text: string }>();
            return mcpJsonResponse(id, {
              content: [{ type: "text", text: JSON.stringify({
                source_file: sourceFile,
                chunks: rows.results || [],
              }, null, 2) }],
            });
          }

          case "memory_remember": {
            const text = String(args.text || "").trim();
            const source = String(args.source || "user-memory");
            // Auth: check tool argument key, then MCP-level key (header/query param)
            const apiKey = String(args.api_key || "") || mcpKey;
            if (!text) {
              return mcpErrorResponse(id, -32602, "memory_remember requires 'text'");
            }
            if (apiKey !== env.API_KEY) {
              return mcpErrorResponse(id, -32603, "Invalid API key — write access denied");
            }
            // Embed via Workers AI
            const embeddingResponse: any = await env.AI.run("@cf/baai/bge-m3", { text });
            let vector: number[];
            if (Array.isArray(embeddingResponse?.data?.[0]) && typeof embeddingResponse.data[0][0] === "number") {
              vector = embeddingResponse.data[0];
            } else if (embeddingResponse?.data?.[0]?.embedding) {
              vector = embeddingResponse.data[0].embedding;
            } else {
              return mcpErrorResponse(id, -32603, "Embedding failed");
            }
            // Generate ID: source + timestamp + first 16 chars hash
            const ts = Date.now();
            const textHash = await sha256Hex(text.slice(0, 64));
            const chunkId = `${source}/${ts}_${textHash.slice(0, 16)}`;
            // Insert into D1
            await env.cloud_memory_db
              .prepare("INSERT OR REPLACE INTO chunks (id, source_file, chunk_index, text) VALUES (?, ?, 0, ?)")
              .bind(chunkId, source, text)
              .run();
            // Insert into Vectorize
            await env.VECTORIZE.upsert([{ id: chunkId, values: vector, namespace: source.slice(0, 10) }]);
            // Invalidate query cache
            await env.cloud_memory_db.prepare("DELETE FROM query_cache").run();
            return mcpJsonResponse(id, {
              content: [{ type: "text", text: JSON.stringify({
                ok: true,
                id: chunkId,
                source,
                chunks: 1,
              }, null, 2) }],
            });
          }

          default:
            return mcpErrorResponse(id, -32602, `Unknown tool: ${toolName}`);
        }
      } catch (e) {
        return mcpJsonResponse(id, {
          content: [{ type: "text", text: `Error: ${String(e)}` }],
          isError: true,
        });
      }
    }

    default:
      return mcpErrorResponse(id, -32601, `Method not found: ${method}`);
  }
}

function handleWebUI(): Response {
  const html = `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Cloud Memory</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body { font-family: system-ui, -apple-system, sans-serif; background: #0a0a0a; color: #e0e0e0; min-height: 100vh; }
  .container { max-width: 640px; margin: 0 auto; padding: 16px; }
  h1 { font-size: 1.5rem; margin-bottom: 4px; color: #fff; }
  .subtitle { color: #888; font-size: 0.85rem; margin-bottom: 20px; }
  .search-box { display: flex; gap: 8px; margin-bottom: 12px; }
  input[type="text"] { flex: 1; padding: 12px 16px; border: 1px solid #333; border-radius: 8px; background: #1a1a1a; color: #fff; font-size: 1rem; outline: none; }
  input[type="text"]:focus { border-color: #6366f1; }
  input[type="password"] { width: 100%; padding: 10px 12px; border: 1px solid #333; border-radius: 8px; background: #1a1a1a; color: #fff; font-size: 0.85rem; outline: none; margin-bottom: 12px; }
  button { padding: 12px 20px; border: none; border-radius: 8px; background: #6366f1; color: #fff; font-size: 1rem; cursor: pointer; white-space: nowrap; }
  button:active { background: #4f46e5; }
  button:disabled { background: #333; cursor: not-allowed; }
  .results { display: flex; flex-direction: column; gap: 12px; }
  .result { background: #1a1a1a; border: 1px solid #222; border-radius: 10px; padding: 14px; }
  .result-header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 8px; }
  .result-source { font-size: 0.75rem; color: #6366f1; word-break: break-all; }
  .result-score { font-size: 0.7rem; background: #1e1e2e; color: #a0a0ff; padding: 2px 8px; border-radius: 12px; flex-shrink: 0; margin-left: 8px; }
  .result-text { font-size: 0.88rem; line-height: 1.5; color: #ccc; white-space: pre-wrap; word-break: break-word; max-height: 300px; overflow-y: auto; }
  .stats { color: #666; font-size: 0.75rem; text-align: center; margin: 8px 0; }
  .error { color: #f87171; font-size: 0.85rem; margin: 8px 0; }
  .loading { color: #888; font-size: 0.85rem; }
</style>
</head>
<body>
<div class="container">
  <h1>Cloud Memory</h1>
  <p class="subtitle">Semantic search across 303 chat sessions &middot; 12,139 chunks</p>
  <div class="search-box">
    <input type="text" id="query" placeholder="Search your memories..." />
    <button id="searchBtn" onclick="doSearch()">Search</button>
  </div>
  <div id="status"></div>
  <div id="results" class="results"></div>
</div>
<script>
  const W = location.origin;
  const q = document.getElementById('query');
  const st = document.getElementById('status');
  const rs = document.getElementById('results');
  const btn = document.getElementById('searchBtn');
  q.addEventListener('keydown', e => { if (e.key === 'Enter') doSearch(); });

  async function doSearch() {
    const query = q.value.trim();
    if (!query) return;
    btn.disabled = true;
    st.innerHTML = '<div class="loading">Searching...</div>';
    rs.innerHTML = '';
    try {
      const res = await fetch(W + '/search', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ q: query, k: 8 })
      });
      if (!res.ok) { const t = await res.text(); throw new Error(t); }
      const data = await res.json();
      st.innerHTML = '<div class="stats">' + data.results.length + ' results</div>';
      rs.innerHTML = data.results.map(h => '<div class="result">' +
        '<div class="result-header"><span class="result-source">' + esc(h.source_file) +
        '</span><span class="result-score">' + h.score.toFixed(3) + '</span></div>' +
        '<div class="result-text">' + esc((h.context || h.text || '').substring(0, 2000)) +
        '</div></div>').join('');
    } catch (e) {
      st.innerHTML = '<div class="error">Error: ' + esc(e.message) + '</div>';
    }
    btn.disabled = false;
  }
  function esc(t) { const d = document.createElement('div'); d.textContent = t; return d.innerHTML; }
</script>
</body>
</html>`;
  return new Response(html, {
    headers: { "Content-Type": "text/html; charset=utf-8", ...corsHeaders() },
  });
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const url = new URL(request.url);
    const path = url.pathname;

    // Handle CORS preflight
    if (request.method === "OPTIONS") {
      return new Response(null, { status: 204, headers: corsHeaders() });
    }

    try {
      switch (path) {
        case "/ingest":
          if (request.method !== "POST") {
            return new Response("Method not allowed", { status: 405, headers: corsHeaders() });
          }
          return await handleIngest(request, env);

        case "/search":
          if (request.method !== "POST") {
            return new Response("Method not allowed", { status: 405, headers: corsHeaders() });
          }
          return await handleSearch(request, env);

        case "/status":
          return await handleStatus(request, env);

        case "/list-sources":
          return await handleListSources(request, env);

        case "/cleanup":
          if (request.method !== "POST") {
            return new Response("Method not allowed", { status: 405, headers: corsHeaders() });
          }
          return await handleCleanup(request, env);

        case "/mcp":
          return await handleMcp(request, env);

        case "/":
          return handleWebUI();

        default:
          return new Response(
            JSON.stringify({
              name: "cloud-memory-worker",
              endpoints: {
                "GET /": "Search UI (open in browser)",
                "POST /mcp": "MCP server (tools: search_memory, list_sources, memory_status)",
                "POST /ingest": "Store chunks with vectors (body: IngestRequest[])",
                "POST /search": "Semantic search (body: {q: string, k?: number})",
                "GET /status": "Database stats",
              },
            }),
            { headers: { "Content-Type": "application/json", ...corsHeaders() } }
          );
      }
    } catch (error) {
      return new Response(
        JSON.stringify({ error: String(error) }),
        { status: 500, headers: { "Content-Type": "application/json", ...corsHeaders() } }
      );
    }
  },
};