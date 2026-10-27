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

## Boundaries (stated plainly)

- **Nothing is wired into the live ANN index.** The scratch index lives in
  `I:\Temp\kv14`; `I:\tools\ann\data` was not touched.
- **One row per KV frame, not per message.** A frame can cover several chat
  messages; correlating them needs the producer to record the message ids, which
  it does not do yet. Today the join is turn ↔ text.
- **The vector is memcore's, keyed by text.** A frame whose text nobody embedded
  does not become a row (reported as `no stored vector for this text`) — by
  design, rather than embedding with a different model and quietly entering a
  different space.
- **`replay[]` is a list of turns, not bytes.** Resuming still goes through
  `kv_temporal_llama` seek (anchor blob + per-turn deltas); the bridge does not
  re-implement replay.
- **No route/tag fields.** `anchor`/`anchor2` are written by ANN's own regrow, so
  the bridge leaves them alone instead of guessing bucket ids.
