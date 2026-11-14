# KIS — entropy / structure baseline (2026-10-30)

Baseline for the KIS experiment line. **Not** a conclusion that this is an address-mapping
mechanism. It only answers: *which transforms preserve the data, and which make it collapse.*

Scripts (same folder):
- `ortho_push.py` — push one side +d, the other −d, d = (k−1)·v
- `entropy_moves.py` — translate / scale / yoyo against entropy
- `adjacent_structure.py` — W[i] ↔ W[i+1] before and after each transform

Data: `I:\model\Qwen2.5-0.5B-Instruct-Q8_0.gguf`, offset 34,000,000, 1,020,000 bytes
→ Q8_0 de-blocked (34-byte blocks: 2-byte fp16 scale + 32 int8) → **960,000 int8 payload
values** + 30,000 fp16 scales. All numbers below are measured on that slice, not assumed.

---

## 0. What Q8_0 actually contains

| stream | n | entropy | ceiling | distinct values |
|---|---|---|---|---|
| int8 payload | 960,000 | 7.659017 | 8.000 | 255 |
| fp16 scales (uint16) | 30,000 | 14.0811 | 16.000 | 20,102 |

Both sit close to their ceiling. Quantization has already removed most redundancy.

---

## 1. 1D histogram: which transforms are rigid

`rigid 1D` = sorted bin counts identical ⇒ the histogram moved as a rigid body.

| transform | H | ΔH | rigid 1D | yoyo |
|---|---|---|---|---|
| translate +1 / +3 / +12 / +48 / +192 | 7.659017 | 0 (±8.9e-16) | **True** | exact |
| scale ×3, ×5, ×7 (odd) | 7.659017 | **0.000000** | **True** | exact |
| scale ×2 | 6.954137 | −0.704880 | False | irreversible |
| scale ×4 | 5.978533 | −1.680484 | False | irreversible |
| scale ×8 | 4.989313 | −2.669704 | False | irreversible |
| scale ×16 | 3.995366 | −3.663651 | False | irreversible |
| scale ×12 | 5.978533 | −1.680484 | False | irreversible |

**×4 = ×12 exactly**, because `gcd(4,256) = gcd(12,256) = 4`. The collapse factor is
`2^v₂(k)`, not k. The factor 3 inside 12 is free — it is a permutation.

---

## 2. 2D relationship: is W[i] ↔ W[i+1] preserved?

`rigid 2D` = sorted joint counts identical. `I(W[i];W[i+1])` is exactly invariant under any
bijection applied to both sides, so it is the correct invariant to test.
Pearson is only affine-invariant and drifts with wrap.

| transform | ΔH | rigid 1D | I(W[i];W[i+1]) | ΔMI | rigid 2D | pearson lag1 |
|---|---|---|---|---|---|---|
| identity | 0 | True | 0.108625 | 0 | True | −0.000679 |
| translate +1 | 0 | True | 0.108625 | **+0.000000** | **True** | −0.001531 |
| translate +48 | 0 | True | 0.108625 | **+0.000000** | **True** | +0.010193 |
| translate +192 | 0 | True | 0.108625 | **+0.000000** | **True** | +0.005674 |
| scale ×3 | 0 | True | 0.108625 | **+0.000000** | **True** | −0.000888 |
| scale ×5 | 0 | True | 0.108625 | **+0.000000** | **True** | −0.001615 |
| scale ×7 | 0 | True | 0.108625 | **+0.000000** | **True** | +0.002909 |
| scale ×2 | −0.704880 | False | 0.038969 | −0.069657 | False | −0.000240 |
| scale ×4 | −1.680484 | False | 0.016435 | −0.092190 | False | +0.000261 |

**Answer to the open concern:** for a bijection the pairwise relationship moves rigidly too —
`rigid 2D = True` and `ΔMI = 0` to six decimals. Not just the histogram.

---

## 3. How much relationship is actually there? (decisive test)

Plug-in MI is biased upward. Same estimator, same N, original order vs shuffled order:

| stream | N | original | shuffled | real MI |
|---|---|---|---|---|
| int8 payload | 240,000 | 0.264394 | 0.205237 | **+0.059157** |
| int8 payload | 960,000 | 0.108625 | 0.050760 | **+0.057865** |
| fp16 exponent | 7,500 | 0.394412 | 0.390162 | +0.004250 |
| fp16 exponent | 15,000 | 0.206044 | 0.211126 | −0.005083 |
| fp16 exponent | 30,000 | 0.105519 | 0.100856 | +0.004663 |

Pearson lag 1/2/3/4/8/16 on the payload: −0.0007, −0.0016, −0.0002, −0.0015, −0.0068, −0.0012.

**Real adjacent MI ≈ 0.058 bits of 8 = 0.7 %.** The fp16 scale stream has none (within noise).

⇒ On Q8_0 there is almost no spatial structure left to preserve. The rigid-move result is
mathematically certain, but its *magnitude* is tiny. **Any claim about structure preservation
must be made on f32 / BF16 / dequantized weights, not on Q8_0 bytes.**

---

## 4. What the doubling map actually cuts (correction)

Earlier claim: "×2^j mod 256 = coarse-grain then relabel". That test was a tautology
(`2x ≡ 2(x mod 128)`), it did not compare against the real coarse-grain. Corrected:

`x·2^j mod 256` keeps the **LOW** m−j bits and drops the **TOP** j bits.
`x >> j` keeps the **HIGH** bits. Two different projections, different costs:

| j | H(x·2^j mod 256) | drop (low-face projection) | H(x>>j) | drop (high-face coarse-grain) | H(cut bits ∣ kept) |
|---|---|---|---|---|---|
| 1 | 6.954137 | −0.704880 | 6.688322 | −0.970695 | 0.970695 |
| 2 | 5.978533 | −1.680484 | 5.713119 | −1.945898 | 0.975203 |
| 3 | 4.989313 | −2.669704 | 4.730268 | −2.928750 | 0.982852 |
| 4 | 3.995366 | −3.663651 | 3.746369 | −3.912648 | 0.983898 |

**The chain-rule claim is confirmed exactly**: the incremental loss per doubling equals
`H(cut bit | kept bits)` — 0.970695, then +0.975203, +0.982852, +0.983898, matching the
differences of the column to six decimals. So the loss is not one number: each bit costs its
own conditional entropy, and on real weights that cost rises toward 1.0 as you cut deeper.

Note the two projections differ by ~0.25 bits at j=1. A "frustum" that projects onto the low
face is **not** the same operation as one that coarsens to the high face.

---

## 5. Orbit structure on the field alphabet (N = 20736 = 2⁸·3⁴)

`k` is a bijection on the field only if `gcd(k, 20736) = 1` (k coprime to 6).
**k = 3 is a bijection on 256 but NOT on the field.**

| k | unit on field | ord mod 256 | ord mod 81 | lcm | orbits |
|---|---|---|---|---|---|
| 5 | yes | 64 | 54 | 1728 | 12 |
| 7 | yes | 32 | 27 | 864 | 24 |
| 37 | yes | 64 | 9 | 576 | 36 |

Decomposition `k = 2^j · 3^l · odd'`: the 2-part and the 3-part are projections (lossy),
the remaining odd part is a free permutation. **37 has neither factor** — that is why it is
the stride that survives, not because the number is pretty.

---

## 6. Status

Proven on this slice:
- translate and odd-scale preserve the 1D histogram, the 2D relationship, and are exactly invertible
- even-scale collapses by `2^v₂(k)`, irreversibly, and the cost per bit is its conditional entropy
- `k` must be a unit mod the alphabet; which k qualify depends on the alphabet's prime factors

Not proven / open:
- whether any of this survives on *structured* (f32/BF16) weights — Q8_0 has only 0.058 bits of adjacency MI
- whether preserved relationship means preserved model behaviour (it does not follow; positions and values both change)
- the 2^j / 3^l projection has not been run on a 3⁴ alphabet (N = 81) with real data

---

## 7. Correction: bit costs, per projection side

Earlier note said "low bits cheap, high bits expensive". That was wrong, and the first version of
the increment column was wrong too (the conditioning mask was inverted, so it reported
`H(bit)` instead of `H(bit | kept)`). Corrected, with the mask fixed:

`x·2^j mod 256` partitions by the **low** bits, so it cuts the **top** bits, each conditioned on
the low ones:

| j | bit cut | cumulative drop | increment = H(cut | kept low bits) |
|---|---|---|---|
| 1 | b7 | −0.704880 | **0.704880** |
| 2 | b6 | −1.680484 | 0.975604 |
| 3 | b5 | −2.669704 | 0.989220 |
| 4 | b4 | −3.663651 | 0.993947 |

`x>>j` partitions by the **high** bits, so it cuts the **bottom** bits, each conditioned on
the high ones:

| j | bit cut | cumulative drop | increment = H(cut | kept high bits) |
|---|---|---|---|
| 1 | b0 | −0.970695 | 0.970695 |
| 2 | b1 | −1.945898 | 0.975203 |
| 3 | b2 | −2.928750 | 0.982852 |
| 4 | b3 | −3.912648 | 0.983898 |

Standalone `H(bit)` is ≈0.999 for **every** bit (b7 0.998953 … b0 0.999615), so no bit is
individually cheap. **Exactly one bit is cheap to cut: b7, and only conditioned on b0..b6
(0.705).** Everything else costs 0.97–0.99. The two projections cut opposite faces and must not
be described by one "cheap/expensive" ordering.

## 8. Where the structure actually lives (lag spectrum)

> **SUPERSEDED — see §10. This section's numbers came from a misaligned Q8_0 block parse
> and are wrong.** Kept for the record; do not cite.

`real` = MI(original) − MI(shuffled, same N, same estimator).

| lag | real MI | reading |
|---|---|---|
| 1 | **+0.057615** | adjacent within block |
| 2–8, 16 | +0.0003 … +0.0023 | none |
| 32 | **+0.076645** | Q8_0 block size |
| 33 | +0.020054 | block boundary + 1 |
| 34 | +0.000384 | none (byte stride, already de-blocked) |
| 64 / 128 / 256 / 512 | +0.0765 … +0.0774 | all multiples of 32 |
| 896 | **+0.085830** | hidden_size (28 × 32) |
| 1792 | **+0.086183** | 2 rows |
| 4864 | +0.076850 | intermediate_size (152 × 32) |

Two ablations isolate the cause (960,000 values, lag 32 / 896 / 1):

| order | lag 32 | lag 896 | lag 1 |
|---|---|---|---|
| original | 0.127582 | 0.136229 | 0.108625 |
| **shuffle inside each block** | 0.050242 | 0.050514 | 0.052005 |
| **shuffle block order** | 0.127002 | 0.127393 | 0.108569 |

- shuffling *within* a block destroys everything → the structure is tied to **position within the block**
- shuffling block order destroys nothing → it is **not** a between-block effect
- the ~0.008 extra at lag 896 over lag 32 is the row effect on top of the column effect

Q8_0 block scale is not the carrier: consecutive block scales have no autocorrelation
(lag 1 pearson +0.002953, shuffled control +0.011831).

**Conclusion: the structure is POSITIONAL (per-column), not adjacency.** The value at a fixed
offset mod 32 (≈ a column, since hidden_size 896 = 28 × 32) is consistent across rows and
carries ~0.077 bits. Adjacent values carry ~0.058. Both are small, but the ranking matters:
what is worth preserving is the column identity, not the neighbourhood.

## 9. Pre-quantization weights: not available here

Searched `I:\model`, `I:\Qwen`, `I:\DWGLS-native-fs` — only Q8_0 / Q4_* GGUFs and no
safetensors. **There is no F16 / BF16 / F32 original on this machine**, so the
"did quantization destroy the structure?" comparison cannot be run yet.

Note: dequantizing Q8_0 → F32 is a *decode of the approximation*, not recovery of the
pre-quantization values. It cannot be used as the original for this comparison. A real
F16/BF16 copy of the same model is required.

---

## 10. CORRECTION — the block parse was misaligned (supersedes §8 and the §3 numbers)

`offset 34,000,000` is **not** on a Q8_0 block boundary. The tensor starts at file offset
5,947,744, so the slice was **12 bytes into a 34-byte block** → 2 scale bytes leaked into
every 32 payload bytes, and the `(nb, 32)` reshape produced a beat pattern. That beat is
what looked like a "position profile".

Verification: a correct Q8_0 phase must satisfy `max|q| = 127` in every block
(scale = max/127 by construction) and every scale must be positive and finite.

| offset | frac(max\|q\| = 127) | frac(scale > 0 and finite) |
|---|---|---|
| 34,000,000 (used in §3, §8) | 0.7409 | 0.5188 |
| **34,000,022 (aligned)** | **1.0000** | **1.0000** |

Aligned slice (960,000 int8 values): `H = 7.673280 / 8`, support 255.

Corrected lag spectrum (`real` = MI(orig) − MI(shuffled)):

| lag | real MI |
|---|---|
| 1 | +0.001743 |
| 2 | +0.002301 |
| 4 | +0.002092 |
| 8 | +0.000793 |
| 16 | +0.000896 |
| 32 | +0.000025 |
| 64 | −0.000390 |
| 128 | +0.000193 |
| 256 | +0.000219 |
| **896** | **+0.010434** |

**Only lag 896 survives.** The lag-32 / 64 / 128 / 256 "structure" was entirely the
misalignment beat. Section 8's conclusion ("structure is positional, ~0.077 bits") is
withdrawn.

## 11. GGUF header — verified, not inferred

`gguf_probe.py` parses the header directly:

```
offset 34,000,000 -> tensor `output.weight`   type Q8_0   dims [896, 151936]
   tensor starts at 5,947,744   offset within tensor 28,052,256
   row length 896 elements = 28 Q8_0 blocks = 952 bytes
   896 % 32 == 0  ->  position mod 32 does align with column
```

`output.weight` is the **tied** embedding: the HF safetensors has no `output.weight`,
and `model.embed_tokens.weight` [151936, 896] rows 29466..30426 match it at
pearson 0.99998503, max |diff| 4.807e-04. Same tensor confirmed.

## 12. BF16 original vs Q8_0 dequantized — the answer

Fetched by HTTP Range from `Qwen/Qwen2.5-0.5B-Instruct/model.safetensors`
(`model.embed_tokens.weight`, rows 29466..30426, 1.72 MB). The Q8_0 on disk is
**Instruct**, so the matching BF16 is the Instruct repo — Base would confound model
identity with quantization.

Same rows, same bin edges (BF16 quantiles, 16 bins), same shuffle control:

| metric | BF16 | Q8_0 | difference |
|---|---|---|---|
| H (16 bins) | 3.999984 | 3.999782 | −0.000202 |
| I(value ; slot mod 32) | **0.005803** | **0.005803** | 0.000000 |
| lag 1 real MI | +0.000282 | +0.000315 | +0.000033 |
| lag 2 | +0.000240 | +0.000256 | +0.000015 |
| lag 4 | +0.000213 | +0.000198 | −0.000016 |
| lag 8 | +0.000168 | +0.000188 | +0.000020 |
| lag 16 | +0.000194 | +0.000196 | +0.000002 |
| lag 32 | +0.000190 | +0.000204 | +0.000014 |
| lag 64 | +0.000283 | +0.000270 | −0.000014 |
| **lag 896** | **+0.010475** | **+0.010488** | +0.000013 |
| pearson(lag 896) | **+0.124266** | +0.124254 | −0.000012 |

bias floor (n = 860,160) = 0.000189.

Against the four possible outcomes:

| outcome | observed |
|---|---|
| BF16 profile same as Q8 | **YES** |
| BF16 more than Q8 | no |
| Q8 more than BF16 | no |
| neither has any | no — both have a weak column effect |

**Verdict: the positional structure exists in the original BF16. Quantization neither
created nor destroyed it.** But it is far weaker than the misaligned run suggested:
`I(value;slot) = 0.0058` bits, and the only lag that rises above noise is 896 (same column,
next row) at `0.0105` bits with pearson 0.124 — identical in both.

Profile vs cross-block, separated as requested:

| quantity | value | reading |
|---|---|---|
| I(value ; slot mod 32) | 0.005584 | tiny but real profile |
| mean_j I(X[b,j] ; X[b+1,j]) | 0.006070 (bias 0.005410) | **no cross-block dependence** |
| mean_j I(X[b,j] ; X[b+1,j+1]) | 0.005927 | same as misaligned control |
| after shuffling block order | 0.005538 | unchanged |

So it is a **profile**, not cross-block correlation — and the profile is 0.0058 bits, not
the 0.13 bits the misaligned parse reported. The real signal sits at row-to-row distance
(lag 896), which is neither adjacency nor a slot profile.

Caveat: this slice is the **embedding table**, where adjacent rows are adjacent token IDs
and therefore semantically related. A real weight matrix (q_proj, ffn_up) may differ.



---

## 13. Three tensors, three different mechanisms (2026-10-30, round 3)

Question: is the row-stride signal an embedding artefact, or a property of weight matrices?

Tensors, each with its own logical row stride from its shape (all are 896 here because
every matrix has `in_features = 896`; what differs is what a ROW means):

| tensor | shape | row means | n values |
|---|---|---|---|
| `model.embed_tokens.weight` | 151936 x 896 | token ID | 860,160 (960 rows) |
| `model.layers.0.self_attn.q_proj.weight` | 896 x 896 | output neuron | 802,816 (full) |
| `model.layers.0.mlp.up_proj.weight` | 4864 x 896 | expansion neuron | 4,358,144 (full) |

Binning: equal-width 16 bins over each tensor's own range (quantile bins would force
H = log2(16) by construction and make H meaningless). MI reported raw, with the plug-in
bias, and bias-corrected.

Ablation design correction: permuting columns with ONE shared permutation does **not**
break the row-to-row pairing (column j stays column j), so it proves nothing — the first
version of this test was wrong. Columns must be permuted **independently per row**.

### Results

| tensor | lag-896 corrected MI | pearson | permute rows | permute cols/row | row-std pearson | after row-norm |
|---|---|---|---|---|---|---|
| embed_tokens | +0.011071 | +0.1243 | +0.010426 (survives) | −0.000055 (**dies**) | −0.0031 | +0.011314 |
| q_proj L0 | +0.027409 | +0.0164 | +0.000607 (**dies**) | +0.005743 | **+0.4632** | +0.016363 |
| q_proj L12 | +0.012094 | +0.0171 | −0.000077 (**dies**) | +0.006127 | **+0.7417** | +0.003778 |
| up_proj L0 | +0.000001 | −0.0002 | −0.000004 | −0.000022 | +0.0087 | −0.000005 |
| up_proj L12 | +0.000013 | −0.0001 | +0.000020 | −0.000025 | −0.0129 | +0.000018 |

Row magnitude spread (max/min row std): embed 2.3x, q_proj L0 **75.4x**, q_proj L12 12.4x,
up_proj L0 2.5x, up_proj L12 4.0x.

### Verdicts

**embed_tokens = COLUMN PROFILE.** Survives permuting rows entirely (0.0104 of 0.0111),
dies completely when column pairing is broken. Row-scale is not involved (row std pearson
−0.003; unchanged after row standardisation). So it is **not** token-sequence kinship —
it is that each hidden dimension has its own characteristic distribution, identical
regardless of which token row you look at.

Stability check across disjoint token ranges (per-column mean):

| comparison | pearson |
|---|---|
| rows 29466.. vs rows 150000.. | **+0.951252** |
| rows 29466.. vs rows 0.. | +0.019298 |
| control: shuffled columns | −0.022390 |

The profile is stable across the general vocabulary but **not** at the head of the vocab
(rows 0..960 = special tokens and the earliest IDs), which is its own finding.

**q_proj = real ROW-to-ROW dependence, largely row scale.** Dies under row permutation,
partly survives column un-pairing (21% at L0, 51% at L12), and adjacent rows have highly
correlated magnitudes (+0.463 at L0, +0.742 at L12). At L12, standardising each row
removes 69% of the MI → the effect is carried by row magnitude.

**up_proj = NOTHING.** Every measurement is at or below the bias floor at every lag.

### Conclusion

The row-stride signal does **not** generalise as "row adjacency". Three tensors give three
different mechanisms, and none of them is a neighbourhood effect:

1. a per-column distribution (embedding),
2. a per-row magnitude that varies smoothly along the row axis (q_proj),
3. nothing (up_proj).

All three are **axis profiles**, not adjacency. Nothing here supports a claim that
neighbouring weights carry shared information; what carries information is which axis a
value sits on and how large its row/column is.

---

## 14. q_proj split: MAGNITUDE vs SHAPE (2026-10-30, round 4)

`ROW_SCALE = std(row)`, `ROW_SHAPE = (row - mean(row)) / std(row)`.

### Decomposition of the lag-896 signal

| layer | total | shape (scale removed) | shape share | scale share |
|---|---|---|---|---|
| L0 | +0.027409 | +0.016363 | **59.7%** | 40.3% |
| L12 | +0.012094 | +0.003778 | **31.2%** | 68.8% |

The balance **shifts with depth** — L0 is mostly shape, L12 is mostly magnitude.
There is no single "q_proj structure"; it is two channels with a layer-dependent mix.

### ROW_SHAPE (row-normalised tensor)

| layer | lag-896 corrected | permute rows | permute cols/row |
|---|---|---|---|
| L0 | +0.016363 | +0.003330 (dies) | −0.000115 (dies) |
| L12 | +0.003778 | +0.000179 (dies) | −0.000134 (dies) |

Row-normalised weights still correlate with the **next row at the same column**, and the
effect needs both row adjacency and column pairing. So it is a genuine 2-D
`W[i,j] ~ W[i+1,j]` dependence, not a profile — weak (0.016 bits of H = 1.34 at L0),
but real.

### ROW_SCALE series (896 values; 16 bins gives bias 0.181, unusable — 4 bins used)

| lag | L0 corrected | L0 pearson | L12 corrected | L12 pearson |
|---|---|---|---|---|
| 1 | +0.039461 | +0.4632 | **+0.334180** | **+0.7417** |
| 2 | +0.047174 | +0.4501 | +0.319277 | +0.6860 |
| 4 | +0.023857 | +0.2185 | +0.184003 | +0.5250 |
| 8 | +0.001846 | +0.0356 | +0.060629 | +0.1688 |
| 16 | −0.006031 | +0.0521 | +0.046539 | −0.2222 |
| 32 | +0.022177 | +0.3632 | +0.127586 | +0.3658 |
| 64 | +0.008361 | +0.2034 | +0.037223 | +0.1187 |

Mean run above median: **3.27 rows (L0)**, **4.27 rows (L12)** vs 2.0 random. So the row
magnitude is smooth over roughly 3-4 neighbouring neurons and then drifts.

### A period-32 component was suspected and REFUTED — **this retraction was itself wrong,
### see §16.1. The fold test below cannot separate periodicity from smoothness.**

The ACF has a clear bump at lag 32 (+0.370 L0, +0.373 L12), which looked like a period-32
repeat of the neuron scale. Folding the series by period p and measuring
`std(folded means) / std(series)`:

| period | L0 | L12 |
|---|---|---|
| 2 | 0.0660 | 0.0376 |
| 4 | 0.0771 | 0.0547 |
| 8 | 0.1308 | 0.1163 |
| 16 | 0.3494 | 0.2153 |
| 32 | 0.4276 | 0.4543 |
| 64 | 0.4628 | 0.5155 |
| 128 | 0.5518 | 0.7018 |
| 224 | 0.5694 | 0.5349 |
| 448 | 0.7049 | 0.7071 |

Monotonic increase with period = **long-range drift, not periodicity**. A true period-32
would peak at 32 and fall afterwards. It does not. The lag-32 ACF bump is a by-product of
segments a few tens of rows long, not a repeating 32-row pattern.

### ROW_MEAN carries nothing

Corrected MI at every lag is negative or at noise (L0 lag1 −0.059, L12 lag1 −0.0097;
above-median flips 424/441 of 895, i.e. random). The structure is in **magnitude**, not
offset.

## 15. Embedding: per-column conditional MI (the owner's caution, settled)

Direct test: median-split each of the 896 columns, then
`I(V[b,j] ; V[b+1,j])` per column, 2 bins (bias 0.000752).

| quantity | value |
|---|---|
| mean over 896 columns | 0.000764 |
| bias | 0.000752 |
| **corrected** | **+0.000012** |
| columns above 0.01 corrected | **0 of 896** |
| shuffle control (corrected) | −0.000050 |

**Zero row-to-row dependence in every single column.** The embedding's lag-896 MI is
entirely a column profile (each hidden dimension has its own distribution), exactly as the
permute-columns ablation indicated. It is not token-sequence kinship.

---

## 16. Corrections after detrend + MAD, and the KIS gate (2026-10-30, round 5)

### 16.1 The period-32 retraction in §14 was WRONG — period-32 is real

The fold test used there cannot separate periodicity from smoothness: folding a *smooth*
series by a larger period groups distant points, so `std(folded means)` rises with p even
when there is no periodicity at all. It was an invalid test.

Proper tests — periodogram and the ACF neighbourhood:

| layer | strongest period in 20..60 | power share 28-36 | acf lag 24 | acf lag 32 | acf lag 40 |
|---|---|---|---|---|---|
| L0 | 30.90 (1.000), 32.00 (0.998) | 0.0441 | −0.050 | **+0.370** | −0.052 |
| L12 | **32.00 (1.000)** | **0.2791** | −0.031 | **+0.373** | +0.063 |

The ACF has a clean **local maximum at exactly lag 32** with negative values on both sides,
and the periodogram peak sits on the bin that is exactly period 32.000 (896 = 28 x 32, so
that bin is leakage-free). Detrending does not remove it:

| series | acf1 | acf8 | acf16 | acf32 | acf64 |
|---|---|---|---|---|---|
| L0 raw | +0.463 | +0.036 | +0.053 | +0.370 | +0.213 |
| L0 linear-detrended | +0.462 | +0.033 | +0.051 | **+0.369** | +0.210 |
| L0 moving-avg(64) detrended | +0.391 | −0.101 | −0.095 | **+0.357** | +0.283 |
| L12 raw | +0.742 | +0.170 | −0.225 | +0.373 | +0.122 |
| L12 linear-detrended | +0.741 | +0.166 | −0.232 | **+0.367** | +0.111 |
| L12 moving-avg(64) detrended | +0.712 | +0.047 | −0.453 | **+0.337** | +0.141 |

**Verdict: a real period-32 component in ROW_SCALE, not long-range drift.** It is a
property of the BF16-trained weights, so it has nothing to do with Q8_0 block size.
Magnitude: 4.4% of ROW_SCALE power at L0, **27.9% at L12**. Cause unknown.

### 16.2 L0's shape share was an artefact of std normalisation

`std/MAD` per row: L0 median 1.04 but **max 2.53** (outlier-dominated rows); L12 median
1.03, max 1.22 (clean).

| layer | normalisation | total MI | shape MI | shape share |
|---|---|---|---|---|
| L0 | mean/std | 0.027612 | 0.016566 | 60.0% |
| L0 | **median/MAD** | 0.027612 | 0.002926 | **10.6%** |
| L12 | mean/std | 0.012297 | 0.003981 | 32.4% |
| L12 | **median/MAD** | 0.012297 | 0.003845 | **31.3%** |

**Corrected decomposition: L0 = 10.6% shape / 89.4% scale; L12 = 31.3% shape / 68.7% scale.**
Both layers are scale-dominated; L12 genuinely carries more shape. The §14 figure of
59.7% for L0 was std-normalisation bias.

Also note MI is not additive, so "share" is a ratio of two MI measurements, not a partition
of information content. It is a descriptive ratio only.

## 17. KIS_POSITIONAL_PRESERVATION_GATE

`tests/test_kis_positional_gate.c`, wired into TIER1 of the Makefile.

Config as specified (`field_size` 20736, `row_stride` 896, `preserve_column_id`,
`preserve_row_id`, `preserve_locality`, `robust_normalize` = median/MAD, `verify_roundtrip`).

Injected structure reproduces the measured q_proj signature (smooth row scale, row-
correlated shape, per-column profile). Self-check requires `pearson(W[i,j],W[i+1,j]) > 0.15`
before the gate is allowed to run.

Three checks, reported separately, never combined:

| transform | address | value | column id | row scale | row shape |
|---|---|---|---|---|---|
| identity (control) | PASS | PASS | +1.0000 ✅ | 0.0000 ✅ | +0.9977 ✅ |
| shift +1 | PASS | PASS | +0.0603 ❌ | 0.0026 ✅ | +0.9976 ✅ |
| shift +1 row | PASS | PASS | +0.9849 ❌ | 0.7758 ❌ | +0.9500 ✅ |
| shift +7 rows | PASS | PASS | +0.5492 ❌ | 2.3244 ❌ | +0.8375 ✅ |
| affine 37*i mod N | PASS | PASS | +0.0370 ❌ | 1.2482 ❌ | −0.6367 ❌ |
| axis-separable (rows x5, cols x3) | PASS | PASS | −0.0090 ❌ | 3.0935 ❌ | −0.4687 ❌ |
| **tensor-level move to new base** | PASS | PASS | **+1.0000 ✅** | **0.0000 ✅** | **+0.9977 ✅** |

**Result: checks 1 and 2 pass for 7/7 maps; check 3 passes for 2/7.**

Address identity and value integrity are what "lossless" normally means, and they are
**not sufficient**. Recovering the bytes does not recover the structure in place: every
intra-tensor reindex destroys column identity and row locality in the new layout, even
though the inverse map recovers the data exactly.

The one transform that survives is relocation at **tensor granularity** — the block keeps
its internal layout and only its base address changes, so the consumer needs nothing but
the new base.

**Design rule: move whole tensors, never reindex inside one.** If an intra-tensor map is
unavoidable, the map is part of the data and must travel with it. The address book is
load-bearing, not an optimisation.

Note this answers the owner's question directly: physical relocation does NOT by itself
preserve logical relations. It preserves them only when the consumer is told the new
address, or when the relocation is coarse enough that the internal layout is untouched.

---

## 18. MODULE_A / MODULE_B on REAL q_proj (2026-10-30, round 6)

`tests/test_kis_relocation_gate.c`, wired into TIER1, SKIPs with exit 0 when no tensor
path is given (GEO_FAST convention). Real data via `dump_tensor.py` (HTTP Range, BF16 -> f32).

### MODULE_A — TENSOR_RELOCATION

`model.layers.0.self_attn.q_proj.weight`, 896 x 896 = 802,816 cells = 38.72 fields.

| placement | address | value | col-id | row-scale | row-shape |
|---|---|---|---|---|---|
| whole tensor at base 0 (control) | PASS | PASS | +1.000000 | +1.000000 | +0.016448 |
| whole tensor at base 1 field | PASS | PASS | +1.000000 | +1.000000 | +0.016448 |
| whole tensor at base 3 fields | PASS | PASS | +1.000000 | +1.000000 | +0.016448 |
| intra-tensor affine 37*i mod n | PASS | PASS | −0.024806 | −0.068367 | −0.008008 |

L12 behaves identically (base 0/1/3: +1.000000 / +1.000000 / +0.017142; affine:
+0.025934 / +0.075721 / +0.003013).

The affine row is a **sensitivity control**: it must break the structure, otherwise the
metrics are blind and the passing rows prove nothing. It does break.

**Tensor-level relocation is structure-neutral on real weights.** Base address is a free
parameter; internal layout is not.

### MODULE_B — FIELD_BOUNDARY

Arithmetic confirmed exactly: `20736 = 896*23 + 128` and `7*20736 = 145152 = 162*896`.

| field | first row | first col | rows fully inside | rows cut |
|---|---|---|---|---|
| 0 | 0 | 0 | 23 | 1 |
| 1 | 23 | 128 | 22 | 2 |
| 2 | 46 | 256 | 22 | 2 |
| 3 | 69 | 384 | 22 | 2 |
| 4 | 92 | 512 | 22 | 2 |
| 5 | 115 | 640 | 22 | 2 |

Phase returns to column 0 at field 7 (row 162), i.e. period 7 fields, as predicted.

A 39-field tensor:

| quantity | value |
|---|---|
| rows whole inside a single field | **863 of 896 (96.32%)** |
| rows straddling a boundary | **33 of 896 (3.68%)** |
| row-aligned alternative | 23 whole rows + 128 pad = **0.617% overhead**, zero cuts |

Reader comparison (row-scale series vs reference):

| reader | L0 corr | L12 corr | adj-row pearson L0 |
|---|---|---|---|
| contiguous (reference) | +1.000000 | +1.000000 | +0.016448 |
| naive, assumes column 0 each field | **+0.536889** | **+0.676183** | −0.002339 |
| phase-aware, closed form | +1.000000 | +1.000000 | +0.016448 |

**Verdict: the 20736/896 boundary is not a hazard by itself.** The phase is a closed-form
function of the field index — row 23k, column (128k mod 896), period 7 — so a reader can
compute it and recover the data exactly. Nothing needs to be stored.

A reader that assumes column 0 every field misaligns 38 of 39 fields and its row-scale
series no longer matches (corr 0.54 at L0, 0.68 at L12). That failure is a reader bug, not
a property of the boundary.

What the boundary genuinely costs is **single-field completeness**: 3.68% of rows need two
fields. Row-aligned packing (23 whole rows + 128 pad, 0.617% overhead) removes that
entirely. That is the real trade: 0.617% space against 3.68% of rows being unreadable from
one field.

Both modules PASS on real q_proj L0 and L12.

---

## 19. KIS storage baseline locked + 5-tensor sweep (2026-10-30, round 7)

### 19.1 Owner's arithmetic, verified exactly

| claim | check |
|---|---|
| `20736 = 896*23 + 128` | ✓ |
| `7*20736 = 162*896 = 145152` | ✓ |
| `row(f) = 23f + floor(f/7)` | ✓ (= `rows_per_field*f + floor(f*phase/cols)`) |
| `col(f) = 128*(f mod 7)` | ✓ (= `(phase*f) mod cols`) |
| `20736 = 648*32`, `896 = 28*32` | ✓ advance 4 groups/field, period 7 |
| q_proj row-aligned costs no extra allocation | ✓ 39/39 fields, 808,704 both, free 5,888 both |
| `4992 + 896 = 5888` | ✓ (39 fields x 128 pad, plus 1 spare row in the last field) |
| embed 6566 -> 6606 fields (+40) | ✓ |

### 19.2 The closed form generalises, but it is PER-TENSOR

`phase = 20736 mod row_stride`, `period = row_stride / gcd(phase, row_stride)`.
`row_stride` is not always 896 — `down_proj` is `[896, 4864]`, so its row stride is 4864.

| tensor | shape | row stride | fields contig | fields aligned | delta | rows straddling | phase | period | 32-groups advance |
|---|---|---|---|---|---|---|---|---|---|
| embed_tokens | 151936x896 | 896 | 6566 | 6606 | **+40** | 5628 (3.70%) | 128 | 7 | 4 |
| q_proj | 896x896 | 896 | 39 | 39 | 0 | 33 (3.68%) | 128 | 7 | 4 |
| v_proj | 128x896 | 896 | 6 | 6 | 0 | 5 (3.91%) | 128 | 7 | 4 |
| o_proj | 896x896 | 896 | 39 | 39 | 0 | 33 (3.68%) | 128 | 7 | 4 |
| up_proj | 4864x896 | 896 | 211 | 212 | +1 | 180 (3.70%) | 128 | 7 | 4 |
| down_proj | 896x**4864** | **4864** | 211 | 224 | **+13** | **199 (22.21%)** | **1280** | **19** | **40** |

`down_proj`: `20736 = 4864*4 + 1280`, so only 4 rows fit per field, the phase is 1280
cells (0.263 rows) and the period is 19 fields.

**Row-aligned packing is free for stride 896 (0 to +1 field) and expensive for stride
4864 (+13 fields = +6.2% allocation, 286,720 free vs 17,152).** It is an optional layout
to be chosen per shape, exactly as stated.

### 19.3 MODULE_A on four real tensors

| tensor | adj-row pearson | base 0 / 1 field / 3 fields | intra-tensor affine |
|---|---|---|---|
| q_proj L0 | +0.016448 | +1.000000 / +1.000000 / +0.016448 (all OK) | col −0.024806, scale −0.068367, shape −0.008008 |
| v_proj L0 | +0.001150 | all OK | col −0.037217, scale +0.422619, shape −0.007478 |
| o_proj L0 | −0.001233 | all OK | col +0.003272, scale −0.048491, shape −0.001392 |
| down_proj L0 | −0.000837 | all OK | col −0.002192, scale −0.005334, shape +0.000280 |

MODULE_A PASS 4/4, MODULE_B PASS 4/4. Tensor-level relocation is structure-neutral on every
real tensor tested; every intra-tensor reindex destroys the structure.

### 19.4 Side observation

`adj-row pearson` at L0: q_proj +0.0164, v_proj +0.0012, o_proj −0.0012, down_proj −0.0008.
Row adjacency is a **q_proj-only** property among these four, at L0. It should not be
described as a general property of weight matrices.

### 19.5 KIS Storage Rules — locked baseline

1. **Tensor Relocation** — the base address may change.
2. **Internal Layout** — must not change arbitrarily.
3. **Field Phase** — computed closed-form, never stored.
4. **Reverse Mapping** — must return address and value.
5. **Row-aligned Packing** — optional layout, chosen per shape.

`Move Base + Preserve Layout + Closed-form Reader` is the KIS baseline at this stage.

Note the separation the owner drew: the mod-32 alignment of field boundaries and the
period-32 of row scale share the number 32 but are **not evidence of a shared mechanism**.
The first is `20736 mod 32` arithmetic on addresses; the second is a statistical property
of trained weights.
