# Should KV frames live in their own index? — measured, not argued

2026-10-28 · `tools/kv_index_split_probe.py`

The question was whether the KV frames that feed ANN should sit in a **separate
DB** from the memory index, or in the same one. Three arrangements of the same
data were built and asked the same questions.

| arm | what it is |
|---|---|
| **A — kv_only** | 2,000 real pool messages (own e5 vectors, sid = pool hash) + the 32 frame rows that carry seek plans → `ann_search.exe` on its own port |
| **B — mem_only** | copy of the live memcore index, 63,915 rows |
| **C — mixed** | B plus the same 32 frame rows (the arrangement tested before) |

Live data was read only. Work dir `I:\Temp\kvsplit`; receipt
`I:\Temp\kvsplit\kv_index_split.json`.

## 1. Retrieval — the decisive result

200 KV vectors as queries, self excluded, ground truth = "the neighbouring turn is
in the top-k" (the messages are consecutive turns of the same log):

| arm | @1 | @5 | top-1 is a KV row | items_scored p50 | ms p50 |
|---|---|---|---|---|---|
| **kv_only** | **0.2700** | **0.5200** | **200/200** | **2,032** | **3.04** |
| mixed (global) | 0.0000 | 0.0000 | 0/200 | 17,616 | 10.25 |

The frames are simply not reachable inside a 63,947-row index: 200 of 200 queries
failed to put any KV row first, and the same 200 queries find them immediately in
their own index. The work difference is 8.7× (2,032 vs 17,616 rows scored) and
3.4× wall time, which is a second, independent reason.

Note the numbers are not comparable to the earlier 0.3438: that was 32 consecutive
messages of one app; this is 2,000 messages across 5 apps, so the adjacency ground
truth is noisier. Same direction, different corpus.

## 2. Pollution — the argument I made did not hold

I claimed mixing would put dead frames into memory results. Measured on 200 memory
queries against B and C:

```
kv rows in memory top-5      mem_only 0/1000    mixed 0/1000
top-1 changed by adding KV rows            0/200
top-5 set changed by adding KV rows        0/200
```

At 32 rows in 63,915 the frames are invisible to memory queries — they disturb
nothing. So contamination is **not** a reason to separate; it is at best a latent
risk that this experiment cannot see, because nothing here expires. The honest
case for separation is §1 (retrievability + cost), not §2.

## 3. The resume path survives separation

```
32 frame rows in the KV index: own sid found 32, rank 1 32, plan attached 32
```

So `search → sid → seek plan → KV resume` runs entirely on the separated index, and
the plan gate (map-vs-log, anchor FNV) stays where it was, in the sidecar.

## 4. What separation costs

| | kv_only | memory |
|---|---|---|
| entries | 2,032 | 63,915 / 63,947 |
| boot | **2.1 s** | 31.3–31.8 s |
| regrow | full 259.8 ms (K=8, derive anchors) / incremental 23.6 ms for +32 | 4–13 ms unchanged, 115 ms for +32 (see `docs/ANN-REGROW-MEASURED-2026-10-28.md`; the 82 s figure earlier was a full re-derive on a copy that broke the state contract) |
| process | one more `ann_search.exe` | — |

Two operational facts that matter more than the timings:

* `ann_keepalive.py` is pinned to `:8096`, so it does **not** supervise a KV index
  server on another port. A second process needs its own supervisor (or a
  keepalive config change), otherwise it is a silent single point of failure.
* the KV index must derive its own anchors on first build (`routed 0 → regrow →
  mode anchor`); a fresh index never self-trains, same as everywhere else.

## 5. Verdict

**Separate.** Reasons, in order of strength: (1) frames are unfindable in the
shared index — 0/200 vs 200/200 top-1; (2) 8.7× less work, 3.4× faster; (3) the
lifecycle argument (frames expire, memory rows do not) still stands as design even
though this probe could not measure it. Do **not** cite "KV rows pollute memory
results" as the reason; the measurement says they do not, at this density.

At 2,032 rows the KV index still scores every row per query (items_scored =
2,032), so bucketed routing buys nothing yet — the KV index can grow to tens of
thousands before that matters, and the route layer is already there when it does.

## 6. Operating shape this implies

```
memory index   :8096   ann_keepalive supervises it       sid = corpus hash
KV frame index :own    needs its own supervisor          sid = pool hash[:63]
lookup order:  memory search → sid in map?  → yes: resume plan (no KV search)
                                            → no : KV index search → plan
```

That keeps the zero-cost path first: when the frame's text is already in memory,
the map answers without touching the KV index at all (measured earlier: 16/16
rows already present by sid, best-match cosine 0.99999994).
