# KV on real pool traffic — what broke, what it cost, what it measured

2026-10-28 · `tools/kv_turns_from_pool.py`, `tools/kv_ann_bridge.py`,
`tools/kv_traffic_probe.py`, `tools/kv_ann_live_probe.py`,
`tools/kv_temporal_llama.c`, `Makefile`

Every run below used messages lifted from a **copy** of the real chat-pool log
(`I:\Temp\poolscratch\log.jsonl`, 32,991 lines, app `chatdrop`). The live
`I:\tools\chat-pool` and `I:\tools\ann\data` were read once and never written;
all writes went to `I:\Temp\…`. File order in an append-only pool log is the
thread's own order, which is what makes adjacent messages a usable ground truth.

---

## 1. Two defects the seed script could hide

The producer had only ever been fed a built-in seed script or a hand-written
prompt file, so two mistakes were invisible.

**a. The record file could be handed back as prompts.** `turns.jsonl` beside the
log holds one JSON object per committed frame; nothing stopped a caller from
passing that same path as the prompt file, which would tokenize the producer's own
bookkeeping as user text and then record it as a frame. Now refused:

```
refusing to read the record file as prompts: I:\Temp\kvreal\turns.jsonl
exit 2
```

JSON record lines from *another* path are accepted (the `text` field is
unescaped), the prompt source is printed every run (`prompts: …` or
`prompts: built-in seed script`), and an append run that silently reuses seed
prompts warns on stderr.

**b. The pool record envelope dilutes the vector.** A pool line's text is

```
[สร้างรูปตามความต้องการของผู้ใช้#944db0d6] (user) 2025-04-03T05:45:56.590000+08:00 สร้างรูปได้ไหม
└──────────────── envelope: identity + role + clock ────────────────┘ └── body ──┘
```

Embedding the whole line makes every message resemble every other message of the
same thread. Same messages, same embedder (memcore's e5-small, dim 384), self
excluded, adjacency = "a neighbour turn is in the top-k":

| n | embedded text | @1 | @3 | @5 |
|---|---|---|---|---|
| 128 | whole record line | 0.1797 | 0.4844 | 0.6094 |
| 128 | **body only** | **0.2344** | **0.5547** | **0.6406** |
| 32 | whole record line | 0.2188 | 0.6875 | 0.8438 |
| 32 | body only | 0.3438 | 0.5938 | 0.7188 |
| 16 | whole record line | 0.1250 | 0.6875 | — |
| 16 | body only | 0.3125–0.5000 | 0.8750 | — |

Chance at n=128 is 0.0157 / 0.0314 / 0.0469, so both variants retrieve far above
chance; body-only is ahead at every k **only once n is large enough**. At n=32 the
@3 comparison flips (0.688 → 0.594) and at n=16 the win was large — that was
sample noise, not a reversal. Conclusion recorded honestly: stripping the envelope
is a small, consistent gain (largest at @1, +0.055 at n=128), not the 4× that the
16-message sample suggested.

`kv_turns_from_pool.py` strips by default; `--keep-envelope` keeps it and
`--also-stripped PATH` writes both from one selection, so a comparison never
changes which messages were chosen.

## 2. The join key is the record hash, not the text

The bridge used to join a frame to a corpus record by text. On real traffic the
prompt is truncated to fit the per-turn budget, so the frame holds a prefix:

```
exact 9/16   prefix-truncated 7/16   different 0/16
```

A prefix join is not a join. The extractor writes the pool record's own `hash` per
turn (`<prompts>.src.jsonl`), and in `--pool` mode that hash **is** the row sid, so

* two frames can never share a sid (a pool hash is unique by construction),
* every row traces back to one log line (`pool_line`, `app` in the sidecar),
* text is only display material.

The ANN loader accepts `sid <= 63` chars and a pool hash is 64 hex chars, so the
sid is the first 63 with the full hash in the sidecar — and the truncation is
gated: two distinct hashes collapsing onto one sid raises
`RuntimeError: sid collision on …` instead of silently merging two rows.

Controls (both fail loud, exit 1, and no row is written for the bad turn):

```
turn 3: pool hash 0000000000000000 is not in the log
turn 17: no pool provenance for this frame
```

## 3. End-to-end on real text

```
kv_turns_from_pool: app chatdrop, 32 consecutive messages (log lines 25522…)
  -> 3 batches of prompts (12 + 4 + 16 bodies) + provenance sidecar per turn

kv_temporal_llama: 12 turns fresh, 4 appended, 16 appended → exit 0/0/0
  log 33 lines = 32 delta (turns 1..32) + 1 anchor (t1), blob 80,664 B
  CHAIN OK — every line hashes to itself, prev contiguous, anchor fnv-verified

kv_ann_bridge --pool --turns-src (3 batches):
  32 rows, 0 miss, 0 unresolvable
  32 unique sid, 0 not in the log, 0 duplicate
  loader-compat PASS · deterministic PASS · value-blind PASS
  one plan per frame PASS

kv_traffic_probe (cosine over those 32 rows, scoped):
  self top-1 present 32/32   ← the tautology, proven before it is removed
  adjacency recall@1 0.3438 (random control 0.0625, chance 0.0625)
  adjacency recall@5 0.7188 (random control 0.2500, chance 0.2923)
  resume plans 32/32 actionable
```

Adjacency is a proxy, not a semantic judgement: consecutive chat messages are not
always related, so these are lower bounds with a random control, never a bare
percentage.

## 4. The same rows inside the real server — and the scope result

`tools/kv_ann_live_probe.py` copies `I:\tools\ann\data\memcore` to scratch,
appends the rows, boots `ann_search.exe --serve` on the copy with the production
knobs (`ANN_ROSTER=1`, `ANN_TOTEM_WARM=1`, `ANN_FLOAT_KERNEL=1`,
`ANN_WALK_PRUNE=1`, `ANN_WALK_FLOOR=0.5`), and queries with the row vectors
themselves via `{"vec": …}` (no embedder needed).

| | scoped (32 KV rows only) | global (63,947-row index) |
|---|---|---|
| row is live / scored | 32/32 | **8/8** own sid at rank 1, score 1.0000 |
| adjacency @1 | 0.3438 | **0.0000** |
| adjacency @5 | 0.7188 | 0.1000 of top-5 slots are KV rows |
| top-1 is a KV row | — | 0/8 |

Loader and router receipts: `entries 63915 → 63947` (+32, no duplicates),
`routed 1`, regrow `mode anchor / nent 63947`. (The 82 s quoted in an earlier
draft was a **full re-derive** caused by a scratch copy whose state sidecar did
not match — see `docs/ANN-REGROW-MEASURED-2026-10-28.md`; the real steady-state
cost is 4–13 ms unchanged and 115 ms for +32 rows.)

**What this says:** the rows themselves are correct — each one scores 1.0000
against its own vector, so load, dim, and routing are right. But 32 rows are
0.05% of the index, so they do not win top-1 against 63,915 distractors, and
adjacency retrieval inside the global index collapses to zero. The earlier plan
("add KV rows so a hit resolves to a frame") therefore only works **scoped**, or
through the map path (attach plans to rows the index already has). Both paths
exist and both are proven; global competition is the one that does not work, and
that is now a measured statement rather than a preference.

## 5. What is proven, what is not

Proven on real data:

* the producer runs on real pool text, resumes from the log, appends new prompts,
  and refuses its own record file;
* a frame joins to exactly one pool record through that record's hash;
* `sid → seek plan` is actionable for every row (log-derived, anchor FNV-verified);
* body-only embedding beats the whole record line at n=128 at every k (small);
* rows load and route in the real server, and score 1.0 against themselves.

Not proven / open:

* scoped retrieval quality is measured on 32 messages; n=128 exists only for the
  embedding comparison, not for KV frames;
* adjacency says nothing about whether the retrieved frame is *useful*;
* nothing calls the producer from the pool append path yet, and KV records carry
  no `rkey`/`enc` (machine records by design — `task-tdb3cf58e` tracks the
  `None`-address guard that a future endpoint would need);
* global-index retrieval of KV rows is not viable at this density; if a use case
  needs it, it needs a scope or the map, not more rows.
