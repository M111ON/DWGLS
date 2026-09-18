# MoE Expert–Prompt Affinity Trace — REPORT (2026-09-17)

## Question
Do MoE experts exhibit per-prompt affinity — i.e. does the same expert subset fire
for the same class of prompt, so that experts can be statically clustered/pinned
(or dead experts dropped) at the page level?

## Method (DUAL_TRACE page-fault trace)
- Tool: `tools/dual_lazy_serve.c`, `DUAL_TRACE=1` env, CPU-only backend
  (`argv[5]=build/`, only `ggml-cpu-sse42.dll` — excludes Vulkan H2D-copy confound).
- Primitive (proven): `EmptyWorkingSet` drops file-mapping pages (verified 0 pages
  post-discard), then `QueryWorkingSet` per expert slice after each `llama_decode`.
- Per-decode hook: step 0 = prompt batch (union, informational), steps 1..n = single
  tokens. Gate-expert (`ffn_gate_exps`) mask recorded per layer per step; histogram
  pooled over decode steps only.
- Model: `F:/model/huihui-moe-1b-q4_k_m.gguf` — 28 layers × 3 experts, top-1
  (`expert_used_count=1` confirmed in GGUF KV). 6 prompts × 6 tokens = 36 steps.

## Results
- Routing is **token-dynamic**: the chosen expert varies token-to-token within one
  generation and across prompts. Single-expert resolution confirmed live (top-1).
- Static layers (same expert ≥80% of steps): **2/28** (L5→e2 30/36, L15→e0 30/36).
- Dead (layer,expert) pairs (never fired in 36 steps): **4/84** —
  L27e0, L3e2, L4e1, L8e2. All other pairs fire somewhere.
- Typical layer split, e.g. L0: 19/7/10, L3: 18/18/0, L24: 10/15/11.

## Verdict
**No static per-prompt affinity — page-level expert clustering/pinning is NOT
possible, and 4/84 dead pairs are NOT worth acting on.** Closed, no follow-up.

## Methodology trap (recorded so nobody re-hits it)
`_exps` slices are 32B-packed, **not page-aligned**: a slice's head page is shared
with the previous slice's tail, and its tail page with the next slice's head.
Querying the full slice range makes a firing expert raise a false positive on its
lower neighbor (observed as an all-multi-fire `all-9` artifact across 3 full runs:
per-prompt union 252/252, then per-step all-multi, both misread as "dense").
Fix: skip BOTH head and tail page per slice (`slen > 8192` else honest skip).
Intermediate suspects ruled out with evidence: Vulkan H2D copy (CPU-only rerun
identical), warmup pass (`cparams.warmup` defaults false in this tree), loader
ignoring `expert_used_count` (loader reads it; qwen3moe throws if 0).

## Regression guard
Normal path untouched: `make dual-lazy-serve` → 21/21 PASS (run twice during this work).
Memory: finding pinned as ID 956 (CONFIG_VALUES).
