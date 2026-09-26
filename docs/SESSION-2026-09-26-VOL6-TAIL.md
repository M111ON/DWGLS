# Session 2026-09-26 — vol6 tail: wangate → full-A → anchor joint → MoE jet → rebake → stream fix

## Done (all measured, all green)

1. **Wang+tantrix block-edge gate** (`core/bfs_wangate.h`, `tests/test_bfs_wangate.c`)
   Seal lays tantrix chain (entry=prev exit, exit=xor-fold of ENCODED bytes).
   Verify: TAMPER −1 / BREAK −2; `bwt_read` DROPs instead of serving garbage.
   BFS 14/14.

2. **Full A: view-ordered inference** (`tools/tesspack_llama_view.c --tiles`)
   `load_from_pack_tiles` reads every capo in 1152-slot units, same dest layout.
   qwen2.5 dense: 291/291 from pack (669.8 MB, 3.4 s), output byte-identical to
   flat (675,710,816 B, cmp2 IDENTICAL), llama 12/12 both modes
   (logits + 24-token bitwise identical). One 0xC0000005 flake on first tiles
   run — env, passed on rerun.
   Bonus fix: `GGML_TYPE_SIZE_DECL` escape for TUs including both ggml.h and
   geo_tess_container.h (int-typed fwd decls conflict with ggml.h's enum-typed).

3. **Anchor → tess-view joint** (`core/anchor_tess.h`, `tests/test_anchor_tess.c`, GGUF)
   8→16-dim chunk-mean centroid (dim chosen by sweep, see below).
   291 tensors: routed-top1 291/291, noise sweep discriminates
   (stable@5=288 → @50=54 at d=16), routed tile-loads == full loads 3/3,
   index save/load roundtrip identical.
   Dim sweep (stable@5/@50): d4=113/6, d8=265/146, d12=277/177,
   d16=288/54, d24=289/100. Default AT_DIM=16, rationale recorded in header.
   Honest finding: 26 tensors sit near bucket walls (sizes up to 852 KB —
   not sub-tile noise). Recorded as measured.

4. **Jet → MoE** (`tests/test_moe_jet.c` SIXICO 5/5; live socket in `moe_expert_route.c`)
   Trace test: real huihui-1b gate top-4 + real flat addresses —
   336 wants → 336 coalesced, 33 bridges, C1=309/B=27/C3=0.
   Live: rebuild loop emits wants (L = quantized slot hop), 84 wants →
   84 coalesced, 33 bridges, C3=83/B=0/C1=1 (pool offsets MB-apart → spin
   is the correct answer). Dispatch itself still simulated — structural proof.

5. **Re-bake region from huihui-1b** (84 tensors, 84/84 lossless, GATE PASS)
   Old qwen3 region kept as `moe_expert_region.qwen3moe.bak`.
   moe-route GATE PASS: 84/84 byte-match, logits maxdiff=0, 40/40 tokens.

6. **moe-stream segfault fixed** — two layers:
   (a) hardcoded N_EXPERTS=64/n_embd=2560 vs huihui gate [1024,3] f32 →
       derive n_embd/n_experts/k_eff from gate dims, pad like route.c;
   (b) "down is F16" assumption vs actual Q6_K (dtype 14) → 2× OOB read →
       true Q6_K dequant (oracle = llama ggml-quants.c on disk) + dtype branch.
   Now: matmul 4/4 (maxdiff=0, cos=1.0), byte 12/12.

## Conceptual map (user-confirmed)
- anchor_route = ANN (coarse→fine, SIFT1M-proven). 291 vectors don't need ANN
  for speed — the value is content-route → geometry-serve.
- Chain: search (anchor, semantic) → locate (vol6/HJ, address) →
  verify (wangate/planet) → serve (tess-view).
- jet_bridge = GPU want coalescer (railway model: spine + ribcage spurs +
  off-rail bubble, ~11×). Socket `geo_pipeline_want/tick` existed with no
  real consumer; MoE expert loading is the fit (top-K/layer = scattered wants).

## Open / debts
- moe_expert_region.bin is now huihui's; qwen3-MoE flows need re-bake
  (backup exists).
- AT_DIM=16 is measured-best on qwen2.5/291 tensors — re-sweep on other models.
- Jet dispatch is counted, not executed — real I/O batching is the next step.
- 26 near-wall tensors: no action, recorded.
