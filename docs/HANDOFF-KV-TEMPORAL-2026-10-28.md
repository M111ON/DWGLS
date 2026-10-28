# HANDOFF — KV temporal (recall · lease · ANN bridge · split index)

2026-10-28 · board: `task-td074e79f` (handoff) · `task-tc47e4371` (what shipped)
· open decision: `task-tdb3cf58e`

This doc is the entry point. It points at the work; it does not repeat it.

## What exists and where

| piece | file | state |
|---|---|---|
| log format + reader | `core/kv_temporal_read.h` | contract: anchors/deltas, escaped keys, partial-line skip, unsigned FNV, ascending sort |
| producer / reader | `tools/kv_temporal_llama.c` | modes `run`, `append`, `seek=<turn>`, `noseek`; two locks; per-turn replay batching |
| lease | `core/kv_lease.h`, `tools/kv_lease_cli.c` | on-disk table, layout-versioned, `reserve/write/beat/close/reap/status` |
| KV → ANN bridge | `tools/kv_ann_bridge.py` | rows, sidecar, map; `--msgs`, `--map-only`, `--blind` |
| lookup | `tools/kv_lookup.py` | memory first, KV index second, both end at a gated plan |
| second index | `tools/kv_index_supervisor.py` | refuses `:8096`; nothing schedules it yet |
| lease/bytes probe | `tools/kv_expiry_probe.py` | proves reap ≠ data loss |

Docs: `KV-TEMPORAL-RECALL-2026-10-27.md` (recall + locks),
`KV-DELTA-BRANCH-ANALYSIS-2026-10-28.md` (what of the branch idea survived),
`KV-ANN-BRIDGE-2026-10-28.md` (rows vs map),
`KV-ANN-SPLIT-LOOKUP-2026-10-28.md` (split + lookup + supervisor + expiry),
`ANN-REGROW-MEASURED-2026-10-28.md` (regrow modes and the fingerprint fix).

## The four numbers that decide things

* **kv_only @1 0.2700 / @5 0.5200 vs mixed @1 0.0000 / @5 0.0000** (2,000 real
  pool messages, 32 frames, 200 queries, self excluded) — a small index in a big
  one is unreachable; the split is justified by reachability, not by pollution
  (measured pollution: 0/1000 top-5 slots, top-1 changed 0/200).
* **layer1 0 / layer2 31 / miss 1** on 32 real pool messages — the memory index
  cannot name a frame for pool rows that are not in the corpus.
* **regrow unchanged 4–6 ms, incremental 42–178 ms, full 56–59 s** — after
  `I:\tools\ann` 86c93af a copy/restore no longer pays the full path.
* **reap: CLOSED gen 2, slots returned, plan still passes, resume exit 0** — an
  expired lease means space came back, never that the frame is gone.

## Next, in order

1. Wire the producer into the real pool append path. Decide first:
   `task-tdb3cf58e` (pool-side guard vs a real `rkey` for KV records).
2. Install `kv_index_supervisor.py` as a scheduled task — until then the second
   index is a silent single point of failure.
3. Lookup as a service with an explicit map-reload policy (a stale map is
   refused by `map-vs-log`, so it must never be silently served).
4. Layer-2 precision is bounded by framed turns (32) — that is producer
   coverage, not lookup work.

## Do not redo (argued down with measurements)

RCU · git-style merge · tesseract-sized reservations (1152 slots = 5.6% > the 3%
cap; 1–3% = 2–5 anchors × 128 slots) · %-only waste gates · adding KV rows to the
memory index (kv rows never entered top-5: 0/300 queries).

## Environment facts that cost hours

* live memory index `I:\tools\ann\data\memcore`, `:8096`, owned by
  `ann_keepalive.py` which prefers `build/ann_search_batch.exe`.
* a lone copy of the index does **not** self-train: `regrow` returns `mode:error`
  after ~100 s in `train`; it needs `ANN_ROSTER=1` + `ANN_ROSTER_PATH` +
  `tag_lut.json`, `ann_tags.tsv`, `.stamp`, `ann_anchors-state.json`.
* ANN search response key is `hits`; a self-query returns top-1 at 1.000000 —
  drop the query's own sid or the measurement is a tautology (`#7697`).
* embeddings for KV rows: `memcore v2._emb()` (e5-small, dim 384); ~274 s for
  2,000 rows — cache them.
* `kv_temporal_llama` is quadratic on a full sweep: use `noseek` on long logs.
* llama runtime `I:\llama\llama-v040-bin-win-vulkan-x64`, model
  `I:\model\Qwen2.5-0.5B-Instruct-Q8_0.gguf`.
