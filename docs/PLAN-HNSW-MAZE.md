# HNSW-Maze Assembly Plan (rev.2 — with external review)

## Changes from rev.1
- Phase 1 split into 1a (recall ≥80%) / 1b (recall ≥95%); kill only if 1a <60%
- Start with raw edges or 3-bit (8 bins), NOT 2-bit Tantrix, in Phase 1
- Add Projection step (PCA/UMAP/JL 1024→8/16-dim) before anchor assignment
- Memory metric split: Block Reads + Distance Calculations
- Phase 4 needs Anchor Projection Layer (learned 1024-dim → 4D anchor weights)
- Start 1k-5k blocks; visualize (t-SNE+maze overlay) before measuring; baselines first

## Phases
| Phase | Work | Pass | Kill |
|---|---|---|---|
| 0 | Setup + 4 metrics + Thai 1024-dim dataset | defined + loaded | data unavailable |
| 0 | Baselines (5029 Thai wiki, Qwen3-Emb-0.6B) | brute 1.06ms/q, HNSW 0.40ms/q recall@10=1.0 | — |
| 1a | Projection + anchor mapping (raw edge) | recall ≥80% vs brute-force | recall <60% |
| 1a | RESULT 2026-09-22: flat rank-perm 0.135, binned-L1 0.40 (both FAIL as designed test was wrong); anchor-bucket route B=32 top-3 = **0.84 PASS** scanning 473/5029 (9.4%) | ✅ | — |
| 1b | RESULT: top-8 true recall 0.938 (n=500; earlier 0.958 was lucky); **top-9 = 0.9538 PASS** scan 28.6% ≤30% | ✅ | — |
| 1b-ii | Reorder: gap 32.5→1.00, speedup 4.0x, recall preserved | ✅ | — |
| 1b | Tune anchors + quantization | recall ≥95% | <85% after 2 tuning rounds |
| 2 | Maze router + greedy descent | beat HNSW ≥1 metric, lose ≤10% others | lose all 3 |
| 3 | Wang gate + line-sum address | bandwidth saved ≥50%, chain ≥100 | chain <10 |
| 4 | 4D hyperoctree (3-5 days, reuse DWGLS field) | only if 1-3 pass | — |

## Mechanism map (which piece does what — read this first)
- embedding → anchors: DECIDES where a query goes (meaning → anchor id). Paid once per query, same cost for us and HNSW.
- L-block (U-fingerprint): ENCODES the anchor as orientation-per-scale = the address itself. Storage/encoding, not decision.
- geo_jump (stride-37): PLACES bytes at computed address, ns-level. Addressing primitive, not ranking.
- HNSW comparison: HNSW has no anchors — it pays distance-compares every step instead of deciding once.
- Chain: embed (decide) → L-block (encode) → geo_jump (place) → buckets (serve).

## Verdict 2026-09-23: compete as total system, not latency
- Embedding cost cancels out (both sides pay it once per query)
- SQR advantages HNSW lacks: meaningful addresses, deterministic insert (no re-link),
  tiny index (~256KB vs ~300MB), unified storage+index (KV at address), built-in lifecycle
- 4 of 5 proven at 5k scale. Missing: C implementation + 1M-scale proof
- geo_jump role clarified: placement/addressing (6.8-10.8ns measured), NOT ranking;
  L-block = anchor code/storage, NOT anchor assignment
- Next: (1) C port of bucket router, (2) 1M proof, (3) plug anchor-buckets into /v1/state/search

## Phase 2 pre-result — card #4 closed (2026-09-22 night, SIFT1M n=1000q)

- HIER 2-level (PCA25, coarse 256 × fine 10 = 2560 anchors): topC8/topF32
  recall@10=0.891 scan 1.39%; topC8/topF16 0.801 @0.70% — same tradeoff
  curve as flat buckets (B=1024 top16 = 0.911 @1.7%). Hierarchy alone
  does NOT fix the 1M wall. Scripts: `build/hier2_test.py` (+`.log`)
- faiss on raw 128-dim (C++): IVF-1024/np16 = 0.9155 @~1.56% (≈ our flat
  PCA-25 pipeline — wall is inherent to hard buckets, not an impl bug);
  IVF/np32 = 0.975 @~3.1%; HNSW-M32/ef128 = 0.9894 @0.38ms/q, build ~6min.
  Script: `build/faiss1m_test.py` (+`.log`)
- Python overhead confirmed (4–6ms/q vs ~1ms/q faiss IVF).
- Consequence: stop tuning hard buckets. Phase 2 maze router competes on
  index-memory (anchors ~256KB vs HNSW ~300MB) / build-time (~1min vs ~6min),
  or adds graph walk (greedy descent over anchor graph).
- Every phase ends in numbers, not feelings
- Fail a phase → stop there, don't drag on
- Don't fear failing 1a: fix projection/anchors, don't trash the concept

## Wiring result — anchor buckets + geo_jump in /v1/state/search (2026-09-23)

- `core/anchor_route.h` (new, self-contained C port): assign/route/train/perm/
  slot/save-load. Constraint found by test: `slot(i)=(i*37)%n` bijective
  iff 37∤n (`anch_perm` bumps nn) — `tests/test_anchor_route.c` 5/5.
- Routed-flow proof `tests/test_anchor_routed.c` 8/8 (synthetic 3-cluster:
  routed top-1 == brute top-1, 8/24 scored, 1 block read). Both in KV group.
- `tools/gguf_lazy_serve.c`: anchors RANK (top-b route over `kv_anchors.bin`),
  geo_jump PLACES (`kv_index.order` perm: bucket-major + slot-37); response
  gains `route{mode,buckets,block_reads,items_scored,items_total,us_per_item}`;
  deterministic refresh on dump + sweep. Serve object compiles clean.
- Hier artifacts: `build/sift1m_hier/` (C1/lab1/fine_cent/fine_members/perm/
  pca/meta.json) — recall reproduces to 4 decimals, perm proven bijective.
- OPEN: live proof needs ≥16 index entries (`kv_index.jsonl` now empty);
  routed mode engages on regrowth — then read `route.*` numbers.
