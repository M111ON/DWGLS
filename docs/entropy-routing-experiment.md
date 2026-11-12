# Experiment: Constrained Entropy Routing (Integer-Only)

**Date:** 2026-11-11
**Status:** Closed — all tests PASS, committed as experiment
**Scope:** `entropy_routing.c`, `test_entropy_routing.c`, `test_entropy_tensor.c`, `test_entropy_model.c`

## 1. Purpose

Model geometric entropy management through a constrained pivot point A
(R = 0.1, Q16.16) across 4 quadrants and a central void, using
integer-only arithmetic. Prove lossless round-trip (bit-exact) for three
operations: diagonal shift (homothety), perpendicular shift (entropy
injection into void), and quadrant reflection (invert+offset).

## 2. Files

| File | Role |
|---|---|
| `entropy_routing.c` | Core library (Q16.16, `int32_t` only, `<stdint.h>`+`<stdio.h>`) |
| `test_entropy_routing.c` | Unit round-trip: diagonal / perpendicular / reflect |
| `test_entropy_tensor.c` | 1024-block synthetic tensor + file byte-compare |
| `test_entropy_model.c` | Real model tensor (`multilingual-e5-small`, `token_embd_norm.weight`) |

## 3. Results

| Test | Blocks | Result |
|---|---|---|
| shift_diagonal round-trip | 1024 | PASS |
| shift_perpendicular round-trip | 1024 | PASS |
| reflect_quadrant self-inverse | 1024 | PASS |
| file byte-compare (orig vs roundtrip) | 1024 | IDENTICAL |
| real-model (76 states, 3 ops) | 76 | PASS (all) |
| conservation (sum quad + void) | — | YES (both shifts) |

## 4. Key Findings

- **Lossless proven** — every operation has an exact inverse; forward+inverse
  restores bit-exact state on synthetic, file, and real-model data.
- **void_entropy is the entropy ledger** — perpendicular moves park entropy
  into the void instead of discarding it, enabling perfect inversion and
  conservation (`Σquad + void = constant`).
- **Symmetric integer division** — round-toward-zero adjustment required for
  odd values; caught via fuzz testing (naive `>> 1` is floor division,
  asymmetric for negatives).
- **Compression: NOT achieved** — experiment on 6 file types (image, text,
  audio, structured binary, random) showed the transform makes zlib
  compression *worse* (redistributes values, inflates ranges, destroys
  local patterns). Consistent with project doctrine #6192 (MAP not COMPRESS)
  and #7069 (compression must read values). The transform is a reversible
  reorganization primitive, not a compressor.
- **Energy compaction claim (external notebook) rejected** — diagonal shift
  did not increase near-zero count or improve compression on real data.

## 5. Adopted from external notebook

- Conserved `shift_perpendicular` (perpendicular displacement + symmetric
  division by 2/4, remainder to quad[0]).
- `reflect_quadrant` centered on `void_entropy` (Opposite Face Rule).
- `check_constraint()` for the R = 0.1 circle.
- Symmetric division fix for odd remainders.

## 6. Rejected / not adopted

- "Lossless image compression via energy compaction" — unsupported by data.
- `reflect` as energy-conserving (it is NOT; use swap for energy models).

## 7. Constraints honored

- Integer-only (`int32_t`, Q16.16), no `float`/`double`.
- Only `<stdint.h>` and `<stdio.h>` in core library.
- No complex loops; bitwise/basic math only.
- No external libraries.

## 8. Conclusion

The model is a fast (O(1)/state), lossless, reversible, conserved entropy
routing primitive suitable for reorganization, faulting, and streaming
pipelines. It does not reduce size and must not be framed as compression.
Ready for integration experiments (KV cache park/resume, MoE capacity
reallocation) as a building block.
