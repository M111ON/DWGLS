# KV → ANN bridge, Phase 1 (2026-10-28)

Two chains stay separate (#7587): ANN owns the vector space and the ranking, KV
owns the frames and the replay plan. What crosses between them is one row and one
sidecar, and nothing else.

## What exists now

**Producer records the text of every frame** — `turns.jsonl` next to `log.jsonl`,
one line per committed frame `{turn, text, tok}`, appended inside the commit loop
so a crash leaves a prefix rather than a hole. Reason: the log's token ids replay
KV but nothing downstream can search or join on them. A fresh run that starts at
turn 1 replaces the file; an appending run continues it.

**`tools/kv_ann_bridge.py`** — joins the two sides without inventing anything:

| column | source | note |
|---|---|---|
| `sid` | `sha256("<turn>:<text>")[0:16]`, prefixed `kv:` | 19 chars, well under ANN's 63 |
| `file` | the KV log path | provenance, same field ANN already uses |
| `npast` | the frame's turn | ANN's own name for the position |
| `dim`,`vec` | **memcore's stored 384-dim vector, reused** | no embedder added, no new space: the ANN sidecar serves the same e5-small space (build_index.py measured cosine 0.955) |

**`kv_frames.jsonl` sidecar** — `{sid, turn, parent, text_sha, memcore_id,
anchor_turn, anchor_blob[off,len,fnv], replay[]}`. This is the actionable half: a
search hit names a `sid`, the sidecar names the anchor to resume and the exact
deltas to replay.

## Receipts on a real run (8 turns of memcore text)

Producer: `kv_temporal_llama … 8 noseek noseek I:\Temp\turns14.txt` → 8 deltas,
1 anchor, `turns.jsonl` 8 lines.

```
kv_ann_bridge: kv=I:\Temp\kv14
  log        1 anchor(s), 8 delta(s)
  rows       8 emitted, 0 miss(es), 0 unresolvable
  dim        384
  loader-compat PASS
  deterministic PASS
  value-blind   PASS (routing columns unchanged with text destroyed)
```

Then the **real ANN binary** (`build/ann_search.exe --serve 18999`,
`ANN_INDEX_PATH=I:\Temp\kv14\ann_index.jsonl`) loaded them and served them:

```
/health  {"entries":8, "index":"I:\\Temp\\kv14\\ann_index.jsonl", "routed":0, ...}
/regrow  {"mode":"brute","nent":8,...}
/search  vec = turn 3's own vector
         → kv:856d9be1d559975e npast 3 score 1.0000   (the query's own row)
           kv:877eeeb98dcd1728 npast 8 score 0.8685
           kv:d62da6798226f080 npast 4 score 0.8627
/search  vec = mean(turn 2, turn 7)
         → npast 7  0.9591
           npast 2  0.9591
           npast 6  0.9209
```

The second query is the one that means something: ranking on the real embeddings
puts exactly the two source turns at the top, in a tie at 0.9591, and nothing
else close. The first query's 1.0000 is the tautology in #7697 — a stored row
queried with its own vector always wins — so it is reported as a loader proof,
not as retrieval quality.

`routed:0` at boot is expected, not a defect: with no `ann_anchors.bin` the server
does not self-train (#7775). One `POST /v1/state/regrow` switched it to
`mode:"brute"` for 8 rows.

## Two defects the bridge found in its own first draft

- **Exact-text join is too strict.** 5 of 8 turns matched; the 3 misses were the
  same sentences with whitespace collapsed by the extraction step. Fixed with
  NFKC + whitespace collapse on both sides (the same normalization build_index
  already needed). Now 8/8. Reported honestly because the failure mode is silent:
  a turn simply does not become a row.
- **The value-blind probe measured the wrong thing.** v1 built rows in blind mode
  and compared `npast` lists — but blind mode had destroyed the vectors, so the
  list was shorter and the check "failed" for the wrong reason. The probe now
  compares routing columns `(turn, anchor_turn, len(replay))` over **all**
  resolvable turns, so vector availability cannot mask a routing leak.

## Phase 2 — one row per message, and the join key is the sid

**A frame can cover several messages.** The producer records the frame's text; the
message list is supplied by whoever built the prompt, because that is who knows it
(`--msgs FILE`, JSONL `{"turn":N,"msgs":[{"sha","text"}]}`). With it the bridge
emits **one ANN row per message**, each carrying that message's own stored vector,
and every row of the same frame carries the same seek plan — so a hit on any
message resumes the frame that holds it. Verified: **8 frames → 16 rows, 2.00
rows/frame, one plan per frame PASS, 0 miss, 0 unresolvable**.

**The exact join key exists: the ANN sid is the memcore entry id.** Measured, not
assumed — the best-match base row for a bridge vector scored cosine
0.99999994 with max |diff| 5e-7 and the same 32-hex id, so matching floats would
be approximate while matching ids is exact. Consequence: **16/16 of these rows are
already in the live index by sid.**

That measurement decides the shape. A KV row with a reused vector adds no
information to the space; it can only occupy a top-k slot. So the bridge also
writes the form that touches nothing:

```
kv_index_map.jsonl
  {"sid": "<base sid>", "kv": {"turn", "frame_sid", "msg_sha",
                               "anchor_turn", "anchor_blob":[off,len,fnv],
                               "replay":[...]}}
```

16 base sids → KV frames, 0 unmatched. A search hit names a sid ANN already had;
the map names the frame to resume. **No new index rows, no new space, nothing
displaced by construction.**

**The map is gated, not trusted.** `--map-only` verifies every row before calling
it actionable: the anchor blob's FNV is recomputed from `kv_blobs.bin` at the
recorded `[off,len)` and the frame's replay turns must exist in the log. 16/16
pass. Three negative controls fail loud, each naming the row and the reason:

```
flipped fnv bit      15/16  anchor blob fnv 1027517907626004366 != recorded ...367
offset past end      15/16  anchor blob [1000000000,746196) is outside the blob file (746196 B)
replay turn missing  15/16  replay turn 999 is not in the log
```

## Phase 2 measurement: what adding rows actually costs (300 queries)

Done on copies of the live 63,915-row index (`I:\Temp\annbase`, `I:\Temp\annkv`);
the live directory was only read and is byte-identical afterwards. Mode for every
number below is **routed** (`route.mode:"anchor"`, `K:64`, 300/300 queries) — not
brute. Oracle = numpy cosine over the base rows, top-10, query row dropped.

| | recall@5 | recall@10 |
|---|---|---|
| base (N=63,915) | **0.982667** | **0.981000** |
| base + 8 kv rows (N=63,923) | **0.982667** | **0.980667** |
| Δ | **0.000000** | **−0.000333** |

- `kv:` rows in top-5: **0/300**. In top-10: **1/300**.
- Base rows pushed out of top-5: **0/300**. Out of top-10: **1/300**.
- The single top-10 case was decided by cosine **5e-8** — the float32 gap between a
  copy and its twin, not a semantic win; the displaced base row was still rank 11
  at 0.93082 against 0.93103.
- Cost: `items_scored` 24,253.6 → 24,257.0 per query, `examined` 62,027.7 →
  62,035.4.

So the duplicated-row path is *cheap but pointless*: it cannot win, it can only
tie its own twin and occasionally cost a rank through float rounding. That is the
measured argument for the map mode — the same information with no slot spent.

## Operational facts that cost a run to find

- The live index is `I:\tools\ann\data\memcore\ann_index.jsonl` (63,915 rows,
  263,336,994 B), not `data\ann_index.jsonl`.
- **A fresh copy does not come up routed by copying the index alone.** `regrow`
  failed twice (`{"mode":"error"}` after ~100 s stuck in `train`,
  `heal_fail` 2→3). Routed needs `ANN_ROSTER=1` + `ANN_ROSTER_PATH=<ann_anchors-roster.bin>`,
  and the tag/state sidecars (`tag_lut.json`, `ann_tags.tsv`,
  `ann_index.jsonl.stamp`, `ann_anchors-state.json`) — without them
  `tags.kinds:0` and training fails.
- **Incremental regrow is cheap**: adding 8 rows to a routed copy took **170 ms**
  (`mode:"incremental"`, `route 3.5 ms`), against ~100 s and a failure for a full
  retrain on a fresh copy.

## Boundaries (stated plainly)

- **Nothing is wired into the live ANN index.** The scratch index lives in
  `I:\Temp\kv14` / `I:\Temp\kv15`; `I:\tools\ann\data` was only read (verified
  byte-identical, 63,915 lines, after every run).
- **One row per KV frame, not per message** — *fixed in Phase 2*: with `--msgs` the
  bridge emits one row per message and every row of a frame carries the same plan.
- **The vector is memcore's, keyed by text.** A frame whose text nobody embedded
  does not become a row (reported as `no stored vector for this text`) — by
  design, rather than embedding with a different model and quietly entering a
  different space.
- **`replay[]` is a list of turns, not bytes.** Resuming still goes through
  `kv_temporal_llama` seek (anchor blob + per-turn deltas); the bridge does not
  re-implement replay.
- **No route/tag fields.** `anchor`/`anchor2` are written by ANN's own regrow, so
  the bridge leaves them alone instead of guessing bucket ids.
