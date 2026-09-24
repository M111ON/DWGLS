# Unified Cold-KV Architecture — assembled 2026-09-23
Combines: SID evict/restore (FGLS_new) + clipboard snapshots + measured quant thresholds + timeline+delta + lifecycle naming.

## Name map (same operation, era renames)
| FGLS_new (mechanism) | DWGLS now (policy) |
|---|---|
| `evict_layer` | sweep / tier-down HOT→COLD |
| `evict_oldest` | LRU victim / zone-4 spill (Saturn) |
| `evict_all` | session close / EXPIRED delete |
| `snapshot` | base hold / dump |
| `restore` | resume / re-anchor |
| `free_snapshots` | clear clipboard |

## Architecture
```
Original chat log (source of truth, never deleted)
    │
    ▼
Base KV @ t0 (F16 full) ── snapshot/HOLD (clipboard: kv_snapshot_layer)
    │
    ▼  per step / per session-suffix
Delta (K=Q8, V=Q4 — K8V4 rule) ── spill (kv_delta_spill → cold tier)
    │
    ▼
Reconstruct @ t_n = base + Σ deltas
    │
    ▼
Re-anchor every 10 steps (measured: Q8 greedy holds 13) → new F16 base, old base freed
```

## Measured rules (docs/KV-QUANT-THRESHOLD-2026-09-23.md)
- COLD default = K8V4 (KL 0.083, greedy 13/20; 9.75MB vs 24MB F16).
- Q4 (either side K) = read-only tier; never continue generation.
- K-means (Q4_K) structurally impossible as KV type (block 256 ∤ head 64).
- Weight-file precision second-order (Q8_0 vs Q4_0-file vs Q4_K_M-file: same verdict).
- Same-base rule (#6139): mix only within one safetensors parent.
- Scope note: 10x delta saving applies ACROSS snapshots/sessions sharing a prefix
  (KV is append-only within one sequence — no magic there).

## Lifecycle (policy names)
ACTIVE → HOT → COLD (K8V4) → EXPIRED → sweep delete. Sweep = old evict with a policy name.

## Build order (when implementing)
1. Base hold (snapshot F16, clipboard semantics) — exists in FGLS_new, port the shape.
2. Delta spill path (kv_delta_spill) with K8V4 precision.
3. Re-anchor @10 policy + verify against L2 bar (greedy div@ ≥ 10/20).
4. Cross-session prefix sharing (the real 10x) — LAST, only as measured pain dictates.

## Implementation record (2026-09-23 night — all CPU-only, DISABLED kernel)
Tools: `tools/kv_cold_base.c` (step 1) → `tools/kv_cold_delta.c` (step 2) →
`tools/kv_cold_reanchor.c` (step 3) → `tools/kv_cold_prefix.c` (step 4).
Core: `core/kv_cold_base.h` (`kvcb_hold/resume/clear/save/load/spill/unspill`;
FGLS_new shape via public `llama_state_seq_*`, no tensor+248 hack, no zstd).
`make kv-cold-base kv-cold-delta kv-cold-reanchor kv-cold-prefix`.

- Step 1 PASS: HOLD 29tok/357300B, FILE-RT + RESUME-RT bit-exact.
- Step 2 measured: opaque blobs churn 98.6%/token (uniform, deltaA vs deltaB) →
  byte-delta on blobs can never pay. Spill unit = TOKEN suffix (.kvd, ~148 B).
  CHAIN1+CHAIN2 reconstruct bit-exact. Cold-stored 357620 B vs live 1169100 B @95tok (3.3x, grows per turn).
- Step 3 CORRECTS the L2 bar: greedy first-div ≥ 10/20 is unsatisfiable by ANY
  quantized KV under true continuation (K8V4: 2,8,3; Q8 control: 2,20,3).
  The 13/20 headline was wart-methodology (re-decoded last token) + mean.
  Replacement gate (binding): teacher-forced cold-top-1 ∈ ref-top-5.
  K8V4: 19,20,20/20, single miss @step19 → window10 10/10 ×3 prompts →
  re-anchor @10 LICENSED. Q4 negative control breaches in-window (7,2,6/10) →
  read-only tier confirmed. Mechanism proven: HOLD@10 → new base saved, old freed.
- Step 4 PASS: `pfx_<fnv1a>.kvcb` shared across separate processes;
  loadA (33-tok suffix) + loadB (23-tok suffix) both bit-exact vs full prefill.
  Cross-process decode determinism proven as a side effect.

## Multi-model confirmation (2026-09-23 late — MiniCPM5-1B official-q4km, CPU-only)
First non-Qwen arch (F:/model). Step-2 shape holds: CHAIN1+2 bit-exact,
3.1x @91tok (base 713676 B, deltas 144/136 B; growth ~24.9 KB/tok — 1B scale).
Step-3 gate holds: K8V4 on-track 20,19,20/20 (miss @19 only), first-div
0,19,2 (diag) → window10 10/10 ×3 prompts → re-anchor @10 LICENSED.
Same signature as Qwen (early/volatile first-div, near-perfect containment) —
gate generalizes across archs. Still open: Qwen3-0.6B (in-family), SmolLM2-360M.

## SmolLM2-360M (2026-09-23 late — I:/model, CPU-only)
Cleanest gate result yet: K8V4 on-track 20,20,20/20, first-div 4,6,20 →
window10 10/10 ×3 → LICENSED. Chain bit-exact, 3.2x @96tok.
Note: TH prompt state 6.0 MB vs EN 1.2 MB (5x) — SmolLM2 tokenizer splinters
Thai script (~5x tokens); Qwen/MiniCPM only ~1.3-1.5x. Tokenizer-driven, not KV.

## smolVLM-256M-text (2026-09-23 late — text-part GGUF only, no mmproj)
4th arch, smallest: chain bit-exact 3.2x @96tok; K8V4 on-track 19,20,20/20
(miss @step10, window edge) → window10 10/10 ×3 → LICENSED.
Gate now holds on 4/4 archs (Qwen2.5, MiniCPM5, SmolLM2, smolVLM-text).
Same 5x TH splinter as SmolLM2 (3.39 MB vs 0.68 MB).

## Qwen3-0.6B (2026-09-23 late — in-family closeout, CPU-only)
5/5: chain bit-exact 3.3x @95tok (base 3.3 MB — Qwen3 KV ~115 KB/tok, ~9x Qwen2.5);
K8V4 on-track 20,20,20/20, first-div 9,20,19 → window10 10/10 ×3 → LICENSED.
Multi-model confirmation CLOSED.

## Kokoro TTS — cold-KV NOT applicable (arch verdict, tools/gguf_tnames.c)
775 tensors: albert encoder (23, bidirectional — no causal KV) +
duration_predictor (258) + text_encoder (45) + decoder (421, CONV blocks:
encoder_block.conv1/conv2 nd=3 — vocoder-style, no autoregressive attention) +
voice_tensors (28). No KV-cache anywhere → prefix-cache/re-anchor concepts
do not transfer. Cross-modal play left: geometric fingerprint comparison
(text/vision/audio weight structure) — FGLS-geometry side, not cold-KV.
BLOCKED for VL-image work too: no mmproj on disk (smolVLM text-only).

## Serve wiring (2026-09-23 late — tools/kv_cold_chat.c, make kv-cold-chat)
Interactive chat on the COLD default (K8V4 serving ctx, DISABLED, cpuonly).
Proven Qwen2.5-0.5B: 2-turn replies correct, suffix-only decode (39 dec /
27 skipped), re-anchor @10 fires mid-generation, per-turn base + .kvd saved.
Cross-restart prefix-HIT: 17 tok resumed + 1 fresh, correct reply.
Two build traps (also in project memory): overlap decode rejected → bases
cover history-minus-last; get_logits()=output 0 → explicit ith indexing.

## Timeline+delta design note — verdict vs measurement (2026-09-23 late)
External design proposed: block cache → timeline+delta (base + per-step deltas,
reconstruct any t, re-anchor @10-12). Assessment against the receipts above:

- CONFIRMED: rewind (RESUME-RT + CHAIN1/2) and the base+deltas+re-anchor skeleton.
- CORRECTED (1): "90% same → 10% delta → 10x" does NOT hold at the byte layer
  (98.6%/token churn measured). Working mechanism = deltas in TOKEN space
  (~148 B/turn vs 12.3 KB/turn live). Honest ratio: 3.3x @95tok, 10x asymptotic.
- CORRECTED (2): per-step precision mixing ("delta Q8, then delta Q4") is a
  category error — deltas are token ids, precision lives in the context tier
  (COLD = K8V4 whole-tier; Q4/K-side breaches in-window → read-only).
- CORRECTED (3): "reuse kv_delta_spill/Ghost, nothing new" is overstated —
  those live at the XOR-byte layer the measurement rules out. Reused: the
  snapshot/restore/clipboard SHAPE from `kv_sid_evict.h`, not the code.
- NEUTRAL: breathing-container / delta-as-second-language framings —
  compatible, no measurable claims either way.
