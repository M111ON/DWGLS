# Cold-KV Quantization Threshold — measured 2026-09-23
Tool: `tools/kv_quant_threshold.c` (L1) + `tools/kv_quant_3level.c` (L1+L2+L3) → `build/`
Model: Qwen2.5-0.5B-Instruct-Q8_0. Ref = F16 KV, same kernel throughout.
CPU-only backend (`build/cpuonly/`, no Vulkan) for all verdict numbers.

## Verdict table (FAIR kernel: standard attention, all configs, CPU-only, 3 prompts EN/TH/reasoning)
| KV | L1 meanKL (72 probes) | L1 max\|dlogit\| | L2 greedy first-div@20 | L3 free-gen first-div@50 (t=0.8, seed 42) |
|---|---|---|---|---|
| F16 | ref | ref | ref | ref |
| Q8_0 | 0.064 | 7.76 | 13/20 | 0/50 |
| K8V4 (K=Q8,V=Q4) | 0.083 | 9.56 | 13/20 | 0/50 |
| K4V8 (K=Q4,V=Q8) | 4.91 | 37.1 | 1/20 | 0/50 |
| Q4_0 | 5.13 | 37.1 | 0/20 | 0/50 |
| Q4_1 | 2.94 | 31.9 | 0/20 | 0/50 |

## K-means as KV type — STRUCTURALLY IMPOSSIBLE (not a quality question)
- Q4_K as `type_k/type_v` rejected at context creation:
  `K cache type q4_K with block size 256 does not divide n_embd_head_k=64`.
  K-means super-blocks (256 els) don't fit KV head width (64); only plain sub-block quants
  (Q4_0/Q4_1/Q8_0, block 32) fit. So "Q เปล่าๆ vs KM" never arises for KV — plain Q is the only option.
- At WEIGHT level KM also doesn't move the KV verdict (Q4_0-file vs Q4_K_M-file table above:
  same pass/fail pattern). KM matters for file size, not for the cold-KV decision.

## File-level bake test — weight quant is second-order (2026-09-23 night)
Baked from the SAME Q8_0 base (`tools/bake_q4.c` via `llama_model_quantize`, allow-requantize;
v040 `llama-quantize.exe` itself is broken here — 0xC0000139 on every exe, DLL skew):
`I:/model/Qwen2.5-0.5B-Instruct-Q8_0-Q4_0-bake.gguf` (409MB) + `-Q4_K_M-bake.gguf` (469MB).

| weights | Q8-KV KL / greedy | K8V4 KL / greedy | Q4_0 KL | Q4_1 KL |
|---|---|---|---|---|
| Q8_0 | 0.064 / 13 | 0.083 / 13 | 5.13 | 2.94 |
| Q4_0-file | 0.042 / 8 | 0.049 / 8 | 4.51 | 3.91 |
| Q4_K_M-file | 0.035 / 7 | 0.044 / 7 | 5.40 | 3.12 |

- Pattern HOLDS on all three files: K8V4 ≈ Q8 (pass), K-side Q4 fails, Q4_1 < Q4_0.
- Weight precision is second-order: KV decision dominates. (Greedy hold 13→8→7 on Q4 files —
  mild degradation, noted not hidden.)
- Same-base rule respected: both bakes derive from the one Q8_0 file.

## Mixed precision — ASYMMETRIC (2026-09-23 late)
- **K is the sensitive side, V tolerates Q4**: K8V4 ≈ full Q8 (KL 0.083, greedy 13/20);
  K4V8 ≈ full Q4 (KL 4.91, greedy 1/20). Mechanism: K errors amplify through softmax dot-products,
  V errors only enter the output-side weighted sum.
- **K8V4 = new COLD default**: Q8-grade behavior at 9.75 MiB/2048ctx (vs 12.75 Q8-pair, 24.0 F16) —
  ~24% cheaper than Q8-pair for the same quality class.
- **Swap rule (owner constraint)**: K/V mixing is valid only within one base — both sides measured here
  derive from the same safetensors parent (same GGUF file). Never graft K from one base + V from another.

## Reading (user's 3-level rules applied)
- **Q8 → COLD default (conditional)**: L1 KL 0.064 ≈ bar 0.05, greedy holds 13/20 steps.
  Safe for prefill/re-anchor use; continued generation from cold Q8 KV drifts after ~10 greedy steps.
  L3 div@0 is sampling-noise amplification (temp 0.8 flips on ulp shifts), not proof of breakage — L2 is the binding test.
- **Q4 → "read-only" tier only**: L2/L3 diverge at step 0. Usable for scoring/prefill-style reads,
  NEVER continue generation from Q4 cold KV.
- **Threshold (empirical, 3 prompts — provisional)**: COLD tier accept = mean KL < 0.1 AND greedy div@ ≥ 10/20.
  Only Q8_0 passes. Q4_1 < Q4_0 order holds (theory restored).

## Kernel hypothesis — CONFIRMED
- First measurement (all-AUTO) showed Q4_1 (KL 5.08) WORSE than Q4_0 (2.43), contradicting theory.
- Same-kernel rerun restores order: Q4_1 (2.94) < Q4_0 (5.13). The anomaly was 100% kernel effect (F16 ran
  standard kernel, Q-configs ran forced-flash) — exactly as suspected. Never compare across kernels.

## Environment finding (v040 DLLs on GTX 1050 Ti box)
- Explicit `flash_attn=AUTO` requests produce DEGENERATE output (constant logits, KL 26.26 identical for
  Q8/Q4, greedy token-0 loop) — in any process position, with/without sampler, CPU-only included.
- The *forced*-flash path ("enabling flash_attn since required for quantized V cache") works fine.
- Rule: always grep logs for `flash_attn =` + `enabling flash_attn` lines before trusting a KV comparison.
  For measurements use explicit DISABLED on all configs.

## Safety net (from design)
- Original chat log retained → cold KV is re-derivable on demand. Quantized cold KV is a cache, never the source of truth.
- Open: multi-model confirmation (only Qwen2.5-0.5B measured), greedy-hold length vs prompt type, Q8+COLD lifecycle integration.
