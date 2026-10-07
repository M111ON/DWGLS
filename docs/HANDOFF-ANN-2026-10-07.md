# Handoff — ANN search (2026-10-07 late session, pickup from opencode)

## STATE
1. **embed UP on 18095**: llama-server b10830 (v040 dir = silent stub, exits 0 on --help)
   + I:/model/multilingual-e5-small-q8_0-fixed.gguf --embedding (384-dim)
2. **ROOT CAUSE FOUND + FIXED** in tools/gguf_lazy_serve.c:
   lz_entry_parse used strstr with no-space keys ("sid":"), but reindex_docs.py
   json.dumps writes spaced keys ("sid": ") → all 302 entries silently dropped
   (items_total=0). Fix: tolerant lz_json_val() helper for sid/file/npast/
   anchor/anchor2/vec. Siblings (lz_json_int, lz_extract_content) already tolerant.
3. **serve REBUILT**: gcc exit 0, 139271 -> 139301 bytes, md5 6eca30079fe96e54c2a4527af64397c4.
   Old PID 15876 killed, backup at build/gguf_lazy_serve.bak2.exe.
   Booted with LZ_EMBED_HOST=127.0.0.1 LZ_EMBED_PORT=18095 (code default 8095 wrong).
   Serve on 8096. bg trackers: proc_cabc1b4d541a (boot), proc_86625134b01b (old, expired).
4. **SEARCH VERIFIED on new binary**: query tesspack format spec -> 3 hits, 302/302.
5. **make test-smoke PASS 15/15 FAIL 0** (via /c/mingw64/bin/mingw32-make.exe).
6. **reindex RUNNING in bg** (proc_bb0d188a5f4f): full docs/*.md 120 files (~3300 chunks).
   NOTE: reindex opens kv_index.jsonl with "w" at start -> wipes file; serve reads per
   search so search degrades DURING reindex then recovers. Backup 302-entry index:
   /tmp/kv_index.302.bak. Progress observed: 120 -> 180 lines and counting.
7. **kv_anchors.bin + kv_index.order STILL OLD** (08:13) -> must POST 8096/v1/state/regrow
   AFTER reindex completes.

## UPDATE 23:55 — embed 500 root cause found
- `W srv operator(): got exception 500 invalid unordered_map<K,T> key` repeats in bg log.
- Bisect: U+2082 SUBSCRIPT TWO alone -> 500 deterministic. `log₂` -> FAIL, `café 中文` -> OK.
  = e5-small tokenizer vocab gap, not encoding issue.
- Trigger chunks: DISCOVERY-octagram-mathematical-foundation.md chunks 11, 19 (of 21).
- reindex embed() has 4-attempt retry w/ backoff -> each bad chunk costs ~10s then
  `continue` (skipped, counted in fails). Looks stalled but progresses (241->271 lines).
- FIX applied to build/reindex_docs.py: `unicodedata.normalize("NFKC", text)` in embed()
  (₂->2). Verified: `log₂ x` -> `log2 x` -> embed OK dim 384.
- CAVEAT: running reindex (PID 5480) started BEFORE patch -> still skips 500-chunks.
  After it finishes, RE-RUN reindex with patched script for full coverage.

## UPDATE 00:07 — DONE (all 4 handoff items complete)
- Killed stuck run (PID 5480, CPU frozen, chunk had no known-bad chars -> unknown hang).
- Reran patched reindex (PID 9948): 455 -> 2881 entries, dims {384}, 121 files, bad_json 0.
- Restarted serve (PID 7444) on full index: search `tesspack format spec` ->
  items_total 2881, hits PIPELINE-MAP 0.8793.
- POST /v1/state/regrow -> {nent:2881, K:8}; anchors bin 32788B -> 12308B (00:07).
- Post-regrow search `kv remap hybrid rail` -> mode **anchor**, buckets [2,7,4],
  scored 1650/2881, hits DWGLS_INTEGRATION_MAP 0.8617. Smoke 15/15 PASS.
- Root-cause chain: ₂ (NFKC-fixable) -> ำ U+0E33 (NFKC folds to combining pair,
  embed OK) -> full sweep found 63 bad chars, NFKC fixes 51, 12 ascii-mapped
  (∉∋∖∝∤≅⋀⋁⏭⏸⟺⬠). sanitize() covers 63/63 verified live.

## UPDATE 00:45 — brace bug + index cache (bench-driven)

**Bug found by bench:** every `/v1/state/search` response was missing its final `}`.
`json.loads` failed with `Expecting ',' delimiter: line 1 column 591`. Cause: the fog
snprintf at tools/gguf_lazy_serve.c:1912 closed the fog object only (`...\"free\":%u}}`)
while `route` was still open — needed `}}}` (fog + route + root). Raw check:
`open braces 6 / close braces 5`, tail `...\"free\":10366}}`. Fixed to `}}}`,
rebuilt (140700 B, md5 `0441f997df394780ccd3acf4685c9af8`), verified PARSE OK
(6/6 braces).

**Perf root cause:** `lz_mem_search` called `lz_index_load` on EVERY request —
re-parsing 10.8 MB + 2881 mallocs. A/B proved it: swapping the index to the old
302-entry file dropped wall from 1.9s to 0.2s while `us_per_item` stayed flat
(~0.7 µs) → scoring was never the cost. Fixed with a load-once cache
(`g_idx_cache` + `stat()` mtime/size check, reload only on change; single-threaded
serve → no locking). Caller tracks `use_cache` and skips the free on a hit.

### Bench (6 queries, 2881 entries, anchor mode)

| query | wall (first) | wall (cached) | scored | top hit |
|---|---|---|---|---|
| tesspack format spec | 1.685s | — | 1305/2881 | 0.8793 PIPELINE-MAP |
| kv remap hybrid rail | 0.029s | 0.029s | 1650/2881 | 0.8617 DWGLS_INTEGRATION_MAP |
| geofs multivolume | 0.163s | 0.163s | 1448/2881 | 0.8607 MDIM_NATIVE_FS |
| kiss timeline scale | 0.078s | 0.078s | 1500/2881 | 0.8557 tess-format-spec |
| moe expert routing | 0.038s | 0.038s | 1500/2881 | 0.8564 TRACE-MOE-EXPERT-AFFINITY |
| wang latch fog | 0.016s | 0.016s | 1650/2881 | 0.8518 FORAGE-GATE-2026-10-05 |

First request in a run pays the index parse (~1.6s); every later request is
0.016–0.16s. Repeat-identical verified (same sids + scores on repeat query).
Anchor routing skips 45–55% of items.

### Verification
- `make test` TIER1 PASS 153/153 + TIER2 PASS 4/4, exit 0
- `test-smoke` PASS 15/15 FAIL 0
- Search response parses with plain `json.loads` (no brace workaround)

### Stack restart note
Both embed and serve died when the chat model was switched (background trackers
were reaped). Restarted: embed = `I:/llama/llama-b10830-win-vulkan-x64/llama-server.exe
-m I:/model/multilingual-e5-small-q8_0-fixed.gguf --embedding --port 18095`
(must cd into that dir — the exe is a launcher that needs its sibling DLLs), then
serve with `LZ_EMBED_HOST=127.0.0.1 LZ_EMBED_PORT=18095`.

## REMAINING
1. ~~wait reindex done~~ DONE: 2881 entries, dims {384}, 121 files, bad_json 0
2. ~~POST 8096/v1/state/regrow~~ DONE: {nent:2881, K:8}, anchors 12308 B
3. ~~final search check topk~~ DONE: 2881/2881 scored, anchor mode
4. **commit parser fix + brace fix + index cache** (tools/gguf_lazy_serve.c uncommitted)
5. ~~full make test~~ DONE: TIER1 153/153, TIER2 4/4
6. index cache only updates on mtime/size change — a reindex that rewrites the
   file in place with the same size in the same second would be missed (unlikely;
   reindex opens with "w")

## GOTCHAS
- taskkill needs MSYS2_ARG_CONV_EXCL="*" prefix in this shell
- plain `make` missing; use /c/mingw64/bin/mingw32-make.exe
- python = I:/python3.14.4/python.exe (not python3)
- `one` CLI missing in this shell; embed via llama-server b10830 directly
- ws_handoff/ws_update tool calls fail on JSON escaping -> handoff via this file
