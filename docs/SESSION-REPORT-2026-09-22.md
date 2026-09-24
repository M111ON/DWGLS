# Session Report — 2026-09-21→22: Bench → KV-Index Architecture + Expert Streaming

## Proven (with evidence)

### 1. Tesspack roundtrip (byte-identical rebuilds)
| Model | Tensors | SHA256 vs source |
|---|---|---|
| huihui-moe-1b | 338 | identical |
| qwen3-06b-base | 310 | identical |
| LFM2.5-8B-A1B | 256 | identical |
| spark-X2.5-1.7B | 226 | identical |
| Qwen3-4B | (bake) | 12/12 lazy PASS |

### 2. Tesspack >4GB bug — FOUND + FIXED
- Pack header offsets truncated to 32-bit → any pack over 4GB lost its embedded header
- File size display wrong via 32-bit `long` overflow (5.4GB shown as 1084MB)
- Fix: hi-32 bits in free hdr slots 11-14 + `TPAK_OFF64` macro (6 reader sites + writer)
- Old packs read back identically

### 3. LFM2.5 8B_A1B(active 1B parameter) serving recipe (re-confirmed live)
- llama-cli + ngl35 = 54.6 tok/s; hard ceiling ngl45; full offload = device-lost
- LFM metadata: 24 layers, 32 experts top-4, embd 2048, vocab 65536
- Thesis: serving difficulty follows ACTIVE params (8B-A1B easier than 4B dense)

### 4. Sept-15 work recovered
- Patched zc2 DLLs restaged; lazy serve 12/12 on huihui AND spark
- Spark needed fused-qkv split serve (user-mode skips fused → F32 splits via ggml to_float, layout verified, L3 bitwise still passes)
- Source-mmap release after bake: peak 4088→3031MB (all models)

### 5. Live streaming serve (`--serve`)
- Persistent HTTP + per-request residency + evict mode + session slots (KV reuse, kv_reused flag)
- KV Block Store: page-aligned dump/restore, roundtrip IDENTICAL across sessions
- Semantic index: Qwen3-Embedding-0.6B (Thai 0.72/0.54) → block address, search proven
- Lifecycle: ACTIVE/HOT/COLD/EXPIRED + sweep DELETE proven
- Precision routing: difficulty scorer + /v1/route + upstream forward proven

### 6. MoE pipeline (huihui)
- Bake 84/84 → route (dynamic dims fix) → graft SHA-identical → logits maxdiff=0 → 40/40 tokens
- GATE 4b zero-warmup PASS (2.43s) after fixing callback pointer semantics + no_host
- Dual serve 23/23 (evict isolation + recovery)
- Expert-slice residency measurement shipped; huihui correctly shows 84/84 (K=E=3, all experts always used)

## NOT proven (explicit boundary)
- **Live expert selectivity** (K<<E): needs 64-expert model runnable on bigger iron (LFM OOMs on 8GB box via CPU path; GPU user-buffer path unbuilt)
- **GPU serve-from-field**: no_host forces CPU; Vulkan staging acceptance is DLL-side work
- **Cold-KV quantization + mixed-precision cold cache**: designed, not implemented
- **HNSW-Maze assembly**: planned (Phase 0-4), Phase 1 is load-bearing and unstarted

## Addendum — morning session (KV-index stack + HNSW-Maze Phase 0/1a)

### Built and proven live (lazy `--serve`)
- Session slots (KV reuse per sid, kv_reused flag), page-aligned KV dump/restore
  (cross-session roundtrip IDENTICAL), semantic index (Qwen3-Emb → block,
  cats/cars retrieval correct), lifecycle sweep (EXPIRED deleted, index rewritten),
  difficulty router (/v1/route EN+TH correct, auto-tier + TIER_UPSTREAM forward proven)
- Expert-slice residency (LZ_MOE_EXP): huihui correctly 84/84 (K=E=3)

### HNSW-Maze plan rev.2 (`docs/PLAN-HNSW-MAZE.md`)
- Phase 0 baselines (5029 Thai wiki): brute 1.06ms/q, HNSW 0.40ms/q recall@10=1.0
- Phase 1a PASS 0.84 (B=32 top-3 buckets, scan 9.4%); flat rank-perm (0.135) and
  binned-L1 (0.40) failed — test was wrong, not concept; pushed to plateau ~0.88
- Remaining: Phase 1b (better anchors → 95%), Phase 2+ (router vs HNSW)
- SQR draft + GeoPixel/HNSW mapping notes; security: integrity free, auth/index-poisoning deferred

## Addendum 2 — SIFT scaling (2026-09-22 night)
- SIFTsmall 10k (official GT): B=64 top12 recall@10=0.993 scan 19%; B=256 top16 0.960 scan 6.8% — B-scales law holds ≤10k
- **SIFT1M: flat buckets BREAK** — B=2560 top16 recall 0.84 (0.7% scan), B=1024 0.911 (1.7%). c≈2.56 does not transfer to 1M
- Verdict: hierarchical anchors required before Phase 2 (card #4). Python overhead noted — C implementation needed for fair HNSW compare

## Design decisions locked
- KV-index architecture (#6110): storage/index/working-set split, lifecycle, 2D precision ladder
- Positioning: no free lunch — quality upgrade at true generation cost
- Merge order: SQR anchors → IVF → HNSW → PQ/codebook → KV blocks, only as measured pain dictates
