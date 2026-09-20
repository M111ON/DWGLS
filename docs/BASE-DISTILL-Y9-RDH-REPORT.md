# Base vs R1-Distill: Where Does the Teaching Live? — Session Report 2026-09-18

**Pair:** `unsloth/Qwen3-0.6B-GGUF` (Q4_K_M, 378MB) vs
`ASomeoneWhoInterestedWithAI/DeepSeek-R1-Distill-Qwen3-0.6B-Q4_K_M-GGUF` (Q4_K_M, 378MB).
Same arch (qwen3, 28 layers), same quant family. Files on `F:/model/`, tesspacks baked for both.

**Tools built** (`build/`): `cmp_inventory.c`, `cmp_delta.c`, `cmp_fdelta.c`,
`cmp_y6.c`, `cmp_yN.c` (m=2..8 + tri-hex fixed), `cmp_rdh.c` (twin), `diag_q6.c`.
All dequant/Y6/RDH routines copied verbatim from reference oracles
(llama.cpp `ggml-quants.c`, `ctd_octa.h`, `rdh_capture.h`) — never from code under test.

## 1. Structural (cmp_inventory, name-matched join)

- base 310 tensors vs distill 254. **match 254/254, type/shape/size diff = 0.**
- `onlyA = 56` = **all `attn_q_norm` + `attn_k_norm`** (F32, 512B) across 28 layers.
  `onlyB = 0`. Distill = strict subset: same skeleton minus qk-norms.
- `output.weight` F32 in both files, **byte-identical**. `token_embd.weight` (155M elems)
  **byte-identical**.

## 2. Byte-delta (cmp_delta) — NEGATIVE

Per-layer byte-diff uniform **73–75%**, no peak anywhere. Global 0.00%.
Verdict: quant/imatrix confound, not training signal. Different quant calibrations
flip nibbles everywhere; byte-% measures requantization, not reasoning.

## 3. Float-delta (cmp_fdelta, reference dequant) — H2

cos **0.991–0.996** all 28 layers, rmsD ~0.003 flat (max 1.6× at blk.22),
embed rmsD = 0.000000. **Same weights + different imatrix requant + norms stripped.
No reasoning-localization signal in these GGUF bytes.**
A real 800k-trace distill SFT would move the head/embed; both are untouched.

**Bug found en route:** my `half_to_float` zeroed fp16 subnormals — Q6_K superblock
scale `0x0140` (=1.9e-5) decoded as 0, silently zeroing all Q6_K tensors
(`diag_q6.c` proved it). Fixed with the `tw_tensor_capture.h` formula; fdelta re-ran clean.
Lesson: never simplify the oracle.

## 4. Y-family detection (cmp_y6 / cmp_yN sweep, n=197)

| N | margin (top4−embed) | up27 rank | top1 |
|---|---|---|---|
| Y6 | 0.408 | 1 | blk.27.ffn_up |
| **Y9** | **0.421** | 4 | blk.26.ffn_gate |
| Y12 | 0.367 | 14 | blk.27.ffn_gate |
| tri-hex fixed | 0.326 | 7 | blk.27.ffn_gate |
| Y15 | 0.293 | 16 | — |
| Y18 | 0.221 | 195 | — |
| Y24 | 0.213 | 165 | — |

- Y6 separates: late-FFN gate/up top (only `comp=1` flags in the table),
  attn_v bottom (~0.10), embed rank 133–135 mid-low.
- **Finer ≠ better: margin peaks at Y9, collapses past Y12.**
  Mean-of-many-rotated-centroids averages disagreement away (over-averaging).
  Subdivide ×4 → 24 REFUTED as detector (24 stays correct as *address* ring only).
- Tri-hex fixed geometry (core 0/60 + satellites 0/120/240, from user image):
  correct top1 but margin below sweep-Y9. Y9 stays duty detector.
- SID 70% zone (blk.19–20) elevated, not peak. Partial agreement only.

## 5. RDH binary + twin map (cmp_rdh, verbatim reference, int-only)

- Single: ALL 196 quant tensors changed-key (requant avalanche), ALL 56 shared
  F32 norms + embed + head SAME-key. Flat 7/9 per layer. 21s.
- Twin (`rdh_capture_nib` hi=0/1): same/same=58, **mixed=0**, diff/diff=196.
  Requant flips both nibble planes uniformly — nothing partial to split.
  Twin verified working; this pair just isn't its workload (surgical edits are).
- RDH = detector (changed?), Y9 = ranker (how much?). Balance confirmed:
  dtype-blind, 10× faster than Y9, key doubles as address.

## 6. What I can and cannot claim

- CAN: distill GGUF = base weights requantized + 56 qk-norms stripped;
  embed/head untouched; norms are passive scaffolding (3rd independent confirmation
  alongside SID-cache exclusion and cosplay exclusion); Y9 is the working ranker
  (margin 0.421); subdivide-detect refuted; RDH is the balanced screener.
- CANNOT: where R1-teaching lives (not in these bytes); whether the norm-stripped
  model runs coherently (inference untested); anything about the true fine-tune
  (a community quant pipeline sits between us and it — need the BF16 pair).

## 7. Series roles (closed)

**SID = act** (steer hot) · **Y9 = rank** (how much, margin 0.421) ·
**RDH = detect + address** (balanced screener) · TW-capture = read/observe (opposite of SID).
Y9 runs via `build/cmp_yN.c m=3`; RDH via `build/cmp_rdh.c`.
