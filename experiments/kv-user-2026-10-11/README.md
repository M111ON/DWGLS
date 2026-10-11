# kv-user-2026-10-11 — KV as a USER of ANN (separate field)

Owner ruling (2026-10-11): **KV is a USER of the ANN route, not part of ANN.**
They are different fields (คนละสนาม). KV manages its own side and *feeds a
result to ANN* — ANN must not carry KV vocabulary, paths, or endpoints.

So this campaign parks the KV-side artifacts that were sitting inside the ANN
repo (`I:\tools\ann`), out of the ANN build path, until the ANN core is clean.

## What moved here (from `I:\tools\ann`)

| file | origin | what it is |
|---|---|---|
| `kv_tess_cache.h` | `I:\tools\ann\include\kv_tess_cache.h` | geometry-addressed KV cache (one KV block = one 20736-byte window), bucket-distance victim, coalesce |
| `test_kv_tess.c` | `I:\tools\ann\tests\test_kv_tess.c` | self-check for the above |

**Not wired to anything in ANN**: neither file appears in the ANN `Makefile`
(`TESTS := test_anchor_route test_anchor_routed test_wang_latch`) and
`src/ann_search.c` never includes `kv_tess_cache.h`. The move is therefore
non-breaking — the ANN core keeps building and serving unchanged.

## Vendored headers (so this folder compiles standalone)

`kv_tess_cache.h` borrows the ANN route structure, so it needs the ANN route
headers. They are copied here (exact copies) so the campaign builds without
reaching back into the ANN repo:

```
kv_tess_cache.h ─┬─ geo_curve_snapshot.h      (gc_addr / gc_decompose)
                 └─ anchor_letter.h ─┬─ anchor_route.h
                                     └─ geo_wang_latch.h ── geo_id_route_log.h
```

Build + run (verified 2026-10-11, ALL PASS):

```
gcc -O2 -std=c11 -I. -o test_kv_tess.exe test_kv_tess.c -lm && ./test_kv_tess.exe
```

**Do not edit these vendored copies to change store semantics** — fix the
caller. Re-vendor from `I:\tools\ann\include\` if the ANN route headers move.

## The distinction that must stay clean

- **ANN core** (the main thing): vector index + anchor-bucket route + Wang
  latch tombstone + P3 route log. Serves `POST /v1/state/search`.
- **KV side** (this folder): geometry-addressed KV block store. It *consumes*
  the ANN anchor-route idea (`al_route` / `anch_route`) as a borrowed
  structure; it does not own the index, the anchors, or the search server.

The `kv_` prefix that leaked into the ANN core's default filenames
(`data/kv_index.jsonl`, `data/kv_anchors.bin`, `data/kv_index.order`) and the
`/v1/state/*` endpoint naming is a leftover of KV being the first user — it is
being renamed out of the ANN core so the core stops reading as "the KV thing".

## Rule for re-use

When KV needs ANN, KV calls ANN through ANN's own interface (index/route
returning candidates). ANN never learns KV's storage format. Take the
*structure* (anchor route), not the *code* (no cross-include).
