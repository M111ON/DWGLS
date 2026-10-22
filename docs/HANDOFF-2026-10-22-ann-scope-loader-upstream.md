# HANDOFF-2026-10-22-ann-scope-loader-upstream.md

> **This is an upstream pointer doc. The live ANN root is `I:/tools/ann`.**
> Full handoff: `I:/tools/ann/docs/HANDOFF-2026-10-22-ann-scope-loader.md`
> Prior: `docs/HANDOFF-2026-10-21-ann-tree-side-upstream.md` (tree/fruit separation), `docs/HANDOFF-2026-10-20-ann-isolate-upstream.md` (A: perm-sidecar, B: door)

## What changed in the ann fork (2026-10-21 → 2026-10-22)

Two things: **a query can now name the zone it wants**, and **the service no longer stalls when the index is rebuilt**.

### 1. The scope / zone chain (the owner's tag idea, made executable)

The owner's long-held idea — a tag system embedded in the vector DB, with a frozen tag→coordinate LUT — was turned into a working chain instead of a parallel design:

- **Request side:** `POST /v1/state/search` accepts `scope` = a `file` group · a `kind` · `tag:<name>` · `zone:<id>`. A scoped query bypasses the route layer entirely (the candidates come from the rows themselves, so the scope cannot drift), a `file` scope widens to its `kind` when too thin, and a `tag:`/`zone:` scope is a **closed group** — never widened.
- **Data side:** `scripts/tag_lut.py` bakes a **frozen** tag→zone LUT and writes a **sid-keyed** sidecar (`ann_tags.tsv`), so a stale sidecar cannot mislabel a row. All five of the owner's rules are implemented (accrual, bake-freeze, forward, backward double-check, clean cycle) and `ann_sync.py` re-bakes it after every rebuild.
- **The measurement that decided the shape:** a tag is a **scope selector**, not a speedup for global search — recall@5 against the global oracle is 0.910 (live route) / 0.698 (kind) / 0.442 (file) / 0.34 (tag), while the answer quality barely moves (cosine gap mean 0.0110, median 0.0000) and the work drops 41×–407×.
- **The negative result worth keeping:** the semantic tag is **orthogonal** to the geometric anchor buckets (a tag's rows spread over all 8; baked tag→bucket accuracy 0.479; 79.1% conflict with the row's own stamp). So a tag LUT must never claim an anchor bucket as its zone — the tag stays a separate coordinate that a query selects.

### 2. The index reload left the request path

The index is 258 MB of JSONL (~50 s to parse). That parse used to happen **inside a request**, holding the request lock, so every request — `/health` included — queued behind it (measured live: `/health` silent for >60 s during a rebuild).

- **A background loader** parses the new file into its own buffer (no lock held) and swaps the pointer atomically; the old buffer is freed there. It runs BELOW_NORMAL priority on this 2-core box and skips while a regrow is in flight.
- **The loader also watches the stamp sidecar on its own** — the incremental regrow writes only the sidecar, so the routing table can change while the index file does not (previously invisible until the next rebuild).
- **THE GATE:** the sidecar is positional, so it is applied only when the regrow state proves the buffer's prefix is the same rows (the regrow's own append contract: `n_prev` + `last_sid`). A sidecar from another index version is refused instead of silently mislabelling the route.
- **The regrow releases the request lock** around its heavy work and takes it back for the answer.
- **Bounded waits:** the embed call had no socket timeout **and a blocking `connect()`**; the sidecar runs one slot, so `connect()` waited for the SYN retry — **53 s, while holding the request lock** — and every request queued behind a health probe. Now: non-blocking connect + `select()` + `SO_RCVTIMEO`/`SO_SNDTIMEO`; `/health` caps its probe at 2 s and then answers honestly (`"embed":"down"`).
- **`/health` gained the loader's truth:** `loader{mode, index_age_s}` (the age of the **served** buffer) plus the tag sidecar's counts.
- **`build_index.py` retries the atomic swap** (Windows/Defender held the destination open → `WinError 5`; measured live, it aborted a rebuild and left a 258 MB `.tmp`).

Measured live with `ann_sync --force` and `/health` polled every 3 s: before **3 of 77 probes timed out at 30 s**; after **1 of 61**, and the remaining cost is CPU contention on the 2-core box (a rebuild still parses the corpus 2–3× — that is the next task), not lock contention.

## Interface deltas

- `POST /v1/state/search` accepts `scope` (`file` group | `kind` | `tag:<name>` | `zone:<id>`); the response's `route.mode` may be `scope`, `scope+kind`, `scope+tag`, `scope+zone`.
- `GET /health` adds `loader{mode, index_age_s}` and `tags{path, kinds, tagged, unlabeled}`.
- New env: `LZ_HTTP_TIMEOUT_MS` (the embed wait ceiling, default 15000 ms; `/health` caps it at 2 s).

## What this means for DWGLS-native-fs

No code changes here. The 20736/CAPO/frustum/metatron headers remain the address-layer library. Two patterns are now enforced in the fork's code rather than only in prose, and both are reusable:

1. **A scope is a selector, not a filter that hides the global answer** — the zone is chosen by the caller, the answer quality is measured (cosine gap), and the hard/soft trade-off is a measured decision.
2. **A positional sidecar needs a proof of identity** — "the row counts match" is not a proof; the prefix contract is, and the loader refuses the sidecar when the proof fails.
