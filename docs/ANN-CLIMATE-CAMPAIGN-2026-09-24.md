# ANN–Climate Retrieval Campaign — 2026-09-24

Full record of one night: from "maze vs HNSW" to the climate doctrine.
Every number below was produced on this box and reproduced by the main agent.
Code: `experiments/ann-climate-2026-09-24/sift/probe_*.c` (SIFT, C) and `experiments/ann-climate-2026-09-24/chatmap/e*.py` (chat map, Python; runnable originals in `I:/tools/chat-pool/probes/`).
Board cards #5, #12–#23. Memory: #6233–#6236.

> Same-night side quest (out of scope here, recorded for completeness):
> GPU serve path proven + ONION fix, commit `ec761ce` (tesspack_server 291/291 dense,
> 338/338 MoE on 2×GTX 1050 Ti). LFM K<<E live still needs lazy serve or bigger iron (#3).

---

## 1. The starting question (card #5, closed)

SIFT1M, n=1000 queries, recall@10. Same-box matchup of our maze router vs HNSW:

| method | recall@10 | scan% | ms/q | index | build |
|---|---|---|---|---|---|
| flat B=2560 top16 | 0.8406 | 0.71 | — (py) | — | — |
| hier topC8/topF16 | 0.801 | 0.70 | — | — | — |
| hier topC8/topF32 | 0.891 | 1.39 | — | — | — |
| maze b16 / b32 / b64 / b128 | 0.6068 / 0.7421 / 0.8701 / **0.9617** | 0.69 / 1.37 / 2.71 / **5.32** | 4.35 / 6.9 / 13.6–18.2 / **32.1** | 0.44 MB anchors | graph 0.2 s, full-pipe ~140 s |
| HNSW-M32/ef128 (faiss) | **0.9894** | — | **0.38** | ~300 MB | **~356 s** |

Gate (PLAN-HNSW-MAZE): beat HNSW ≥1 metric, lose ≤10% others.
Verdict: strict gate FAIL on latency (84×). Maze wins index-mem (680×) + build.
Ceiling ≈ IVF-2560 — the **hard-bucket wall**: once true neighbors fall outside
visited buckets, no rerank recovers them. Tuning stopped here.

---

## 2. SIFT probe campaign (all delegated, all reproduced)

| # | probe | code | result | gate | mechanism |
|---|---|---|---|---|---|
| A | QT-vertical selection + anchor walk (L8/16/32) | probe_qt_vertical.c | 0.5628@0.70% / 0.7427@1.38% / 0.8858@2.71%, fewer hops (10.7 vs 19) | FAIL (tie maze) | selection is not the bottleneck |
| B | same routing, QT best-first refine under exact caps | probe_qt_horizontal.c | 0.5684 / 0.7100 / 0.8421 at 54/126/280 ms/q (8–20× worse) | FAIL | PCA25 bounds misorder; per-query tree cost dwarfs savings |
| G | Grover-flavored R=2 elimination vs exact-all (B0) vs one-shot top-25% (B1) | probe_grover_elim.c | CH 0.4873/0.6380/0.7879 @25% budget; B0 0.6068/0.7421/0.8701; **B1 == CH exactly** | FAIL | iteration on a total order is vacuous; bucket-shared bounds drop true top-10 buckets. Survives: single-round cutoff (4× ms/q at ~0.1 recall cost) |
| C | cube-octree on PCA3 (~2556 leaves) replaces buckets | probe_geo_cells.c | 0.3251@0.69% / 0.4841@1.36% / 0.6696@2.70% vs flat 0.84/0.88/0.91 | FAIL 0/3 | PCA3 blind; axis splits ignore density (max leaf 1138 vs avg 391); Voronoi orients to data |
| P1 | route coherence: neighbor-query bucket Jaccard + cross-recall | probe_coherence.c | Jacc 0.30/0.36/0.43; cross 0.665 vs own 0.890 (−0.225) | FAIL (except closest 20%, L2<204: Jacc 0.55) | prefix cache only for near-duplicate traffic |
| P2 | **top-2 multi-entry overlap** (double lists, halved C) | probe_multientry.c | double 0.8169@0.62% / 0.9082@1.17% / **0.9657@2.20%** vs single 0.8050/0.9061/0.9667 | **WIN 2/3** | ceiling first: 89.1% queries span 4+ buckets. Dedup makes doubled buckets cheaper per unique vector. Cost: 2× bytes |
| S | stacking: QT-select on double lists | probe_qt_multi.c | 0.7176@1.18% / 0.8689@2.25% / 0.9579@4.24% vs flat-double above | FAIL | QT saves hops, fat lists cost 2×/hop — selector quality dominates. **Champion: flat top-C + top-2 overlap** (double build via coarse shortlist: 21 s) |

---

## 3. Theory checks (same night)

- **Loshu joints?** No Loshu rule exists in code — current joints are kNN=8 by centroid L2
  (`maze_walk_cli.c:107-139`). The "8" recurs three ways (D4=8, octagon=8, kNN=8);
  Loshu lives in `docs/LOSHU-PROCESS.md` (fingerprint + Wang gate), never as a joint rule.
  A Loshu-joint ablation needs the rule defined first — not guessed.
- **Grover?** Zero Grover/quantum-computing content in repo (grep hits were `addr quantum`
  = precision unit). No quantum hardware ⇒ classical simulation gives no speedup.
  The transferable part (iterative elimination of non-answers) was tested as probe G above.
- **External systems (verified by search):** CubeGraph (2026, nested graphs + dynamic
  stitching — names our failure mode: *fragmentation destroys graph routing connectivity*),
  geovectorscale (composite RTree+HNSW, 10–100× with spatial predicate),
  MobilityDB (GiST/SP-GiST quad-trees, multi-entry MGiST/MSP-GiST ~10× on trajectories).
  Transfer rule (#6233): decoupled nesting wins ONLY with an **independent** spatial key
  (filter ⊥ vector). SIFT has none — our negatives are consistent with their theory.
  Transfer target: the SERVE index (lifecycle/address/session are independent keys).
- **Determinism:** our path has no RNG (deterministic selection + exact rerank;
  deterministic Lloyd anchors + checksums). Approximation lives in coverage, never in
  randomness. Fixed-point boundary discipline (owner's .999/1.000 rule) gives
  reproducibility; it does not move recall.

---

## 4. Owner ontology (recorded #6235, no prior record existed)

Container kis+hyper / breathe_fs range (0,20736] = **infinity timeline**, 3 axes.
Stepping back lands hyperbolic immediately. **W does not slide the timeline** — it indexes
which parallel-possibility slice you stand on. Past = deterministic, unfound future =
chaos/entropy/noise. White-cat-255 = the principle that one tiny point-shift opens
countless futures (3 channels × 255 AND millions of near-identical whites);
every fork grows major roots 360° with decimal (int-both-sides) resolution.
**Climate change = timeline window sliding [0-20736]→[1-20737]→…**,
shifting positions bit by bit like UV texture shift, reshuffling neighborhoods.
Operational consequences: fan-out 360° retrieval; static anchors decay (regrowth required);
W-slices imply multi-scale search.

---

## 5. Real-data campaign (chat-pool map, 8391 — 25,424 lines, 4.6 days)

rkey = deterministic content hash on 144×144 (identity addressing, NOT similarity locality).
Lexical proxy everywhere: token Jaccard (stated openly; no local embeddings).

| # | probe | code | result | verdict |
|---|---|---|---|---|
| E1 | scales: lexical top-10 coverage in windows 12/24/48/72 | e1_scales.py | 0.078 / 0.170 / 0.330 / 0.466 (full scan needed for 0.8) | NEGATIVE — answers map-global **under hash addressing** |
| E2 | fan-out thread recovery (spatial vs lexical vs union) | e2_fanout.py | **MALFORMED, not conclusive** — prev is one global chain (branching 0, no threads); CHAT burst spans 28.5 s so cohorts = whole corpus, all arms ~0 | needs entity/tag GT redesign |
| E3 | **drift**: 10 slices, occupancy Jaccard, hot-16 turnover, staleness | e3_drift.py | Jaccard 0.05–0.45; hot-16 turnover ~16/16 EVERY slice; slice-1 coverage 3.03% → **0% by slice 5**; two regimes (entropy 11.1 → 10.1) | **CONFIRMED — static anchors die in ~0.5 day; regrowth REQUIRED** |
| E4 | **four layers transfer**: content k-means k=64 (4.2 s) + flat/overlap/cutoff/hier | e4_foursq.py | flat C=4: **0.837@8.5%** (E1: 0.330@13.4%); cutoff 0.803@7.0%; overlap C=8 0.920@15.7%; hier trails flat | **PASS — E1 overturned**: answers were never global, hash addressing scattered them. Identity addressing costs ~5× scan for similarity tasks |
| E5 | **marathon**: fresh vs stale-1 anchors + rebuild cost over 10 slices | e5_marathon.py | fresh mean 0.7919; stale-1 0.6559 (**−0.136, reuse dead**); regrow total **4.02 s** (0.4 s/slice). HNSW arm (faiss, tiny slices — informational): 0.61 | stale reuse dead for everyone; marathon won by always-regrow |
| E6 | **delta-merge** (base + flat exact delta vs fresh regrow, 8 triples) | e6_delta.py | base+delta **0.8273** vs fresh 0.5857 (margin +0.27, 8/8) at delta build 0.0003 s | **PASS with caveats**: 54% scan + 5× ms (exact-delta operating point); needs periodic compaction — delta-merge EXTENDS regrow interval, never eliminates it |

---

## 6. Closing doctrines

1. **Snapshot-champion vs climate-champion.** HNSW rules snapshots (0.9894@0.38 ms).
   Under ~12 h climate shift it pays 6 min rebuild + blue-green double-instance + sync +
   verify + a stale-serving window every cycle: 12 min/day, 6 h/month, 3 d/year.
   Ours: graph 0.2 s, full-pipe ~140 s, regrow 0.4 s/slice, atomic file-swap, no second
   instance (~2 min/year). HNSW loses **at build, before reaching query**.
2. **Break-even: ≈6,800 queries/rebuild.** (216 s extra build ÷ 31.6 ms saved per query,
   our numbers.) Above it with static distribution → HNSW amortizes. Below it or
   shifting → we win. Same amortization math as the GPU batch break-even (≥512).
3. **Champion config (measured):** flat top-C routing + top-2 overlap + single-round
   cutoff option + always-regrow (0.4 s/slice) + delta-merge between regrows.
   QT-select, QT-refine, Grover-iteration, geometric cells, general prefix cache:
   measured, rejected with mechanisms — not from boredom.
4. **What survives as open work:** #3 LFM K<<E live (lazy serve or bigger iron);
   #12 BreathingFS consumer-or-incubation; serve-side nesting on lifecycle/address keys;
   E2 redesign with entity/tag GT; multi-scale search across W-slices; fan-out diversity
   (neighborhood-cloud coverage, not just recall@10); Phase 3 Wang gate.
