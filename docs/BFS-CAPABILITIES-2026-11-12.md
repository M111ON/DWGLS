# DWGLS b_fs Capabilities — 2026-11-12

## Overview

DWGLS (4D Geometry + KIS Timeline) now has a unified storage foundation
`b_fs` (breathing filesystem) that all subsystems dock to. This document
summarizes what the system can currently do, verified by passing tests.

## Phase 1 — b_fs foundation (`core/b_fs_base.h`)

- **Canonical storage API**: `bfs_write(name, data, size)` / `bfs_read(name, out, size)` /
  `bfs_move_seeker(scale)` — the only storage primitive.
- **20736-slot address space** (144×144), seeker-based addressing, scale-based
  space movement (contract toward 0 / expand toward infinity).
- **Lossless write/read** verified on synthetic data and partial last blocks
  (test_b_fs_base T1).
- **Twin (hot+cold)**: `BfsTwin` holds hot (RAM/mmap) + cold (storage) views at
  one address; `bfs_twin_sync` parks hot→cold; roundtrip verified (T2).
- **Pair rule**: bijective cross-dimension address transfer (affine-modular,
  stride-37, proven coprime with 20736). `bfs_pair_transfer` / `bfs_pair_inverse`
  are inverses; verified over all 20736 addresses (T3, T4).

## Phase 2 — cross-dimension adapters (`core/b_fs_dim.h`)

- Dimensions: FLAT, TESS (18 towers × 1152), KIS (stride-37 permutation).
- `bfs_dim_to_flat` / `bfs_dim_from_flat` are inverses for all dims & addresses.
- `BfsDimTwin` roundtrip verified for all dim pairs & addresses.
- Cross-dim write in FLAT → read at transferred address returns same bytes.

## Phase 3 — KV cache on b_fs (`core/b_fs_kv.h`)

- `BfsKvCache` stores KV entries as BFS files (name → bytes).
- `bfs_kv_put` / `bfs_kv_get` / `bfs_kv_has` with roundtrip verification.
- Persistence across save/load image (`bfs_save_img` / `bfs_load_img`).
- Partial last block handled.

## Phase 4 — ANN index on b_fs (`core/b_fs_ann.h`)

- `BfsAnnIndex` stores entries (name, flat_addr, size) in BFS.
- Search returns **addresses** (flat slots), never payload meaning —
  value-blindness preserved (MAP not COMPRESS).
- `bfs_ann_put` / `bfs_ann_addr_of` / `bfs_ann_get` with lossless payload
  retrieval; persistence verified.

## Real model verification (`tests/kis_real_gguf_test.c`)

Adaptive storage + KIS container roundtrip verified lossless on real GGUF
weights (Q8_0 decode → adaptive store → readback → container serialize/verify):

| Model | Tensors | Target tensor | Entropy | Result |
|---|---|---|---|---|
| Qwen2.5-0.5B-Instruct Q8_0 | 291 | output.weight | 10 buckets | 15/15 PASS |
| Qwen3-0.6B Q8_0 | 310 | token_embd.weight | 17 buckets | 15/15 PASS |
| SmolLM2-360M-Instruct Q8_0 | — | token_embd.weight | — | 15/15 PASS |

## Test summary

| Suite | Checks | Status |
|---|---|---|
| test_b_fs_base (Phase 1) | 12 | PASS |
| test_b_fs_dim (Phase 2) | 7 | PASS |
| test_b_fs_kv (Phase 3) | 16 | PASS |
| test_b_fs_ann (Phase 4) | 15 | PASS |
| kis_real_gguf (3 models) | 45 | PASS |
| **Total** | **95** | **0 fail** |

## Remaining work

- Migrate existing subsystems (DT, ANN service) to use b_fs as backing store
  instead of their own storage.
- Cross-dimension data transfer (actual layout adapters between tess/kis).
- Wire twin hot/cold into real serving path.
