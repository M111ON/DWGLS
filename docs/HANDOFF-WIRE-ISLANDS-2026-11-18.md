# Handoff: Wire Proven Islands into Main Pipeline

## Goal of next session
Take the "proven islands" (geo_jump, hyper_jump, quadtree, D4-triality, fractal, L-block) and wire them into the main pipeline (tesspack_server, breathe, graft, serve path). Test, assemble, and integrate. Follow memory #6836 priority order.

## State of play

### Done (proven, tested, committed)
- tesspack storage layer: zero-copy mmap, callback-based model loading, serves Qwen2.5/LFM2.5 via HTTP
- MoE graft: VirtualFree(MEM_DECOMMIT) unfired experts, GATE 4b PASS (maxdiff=0), OS-level RSS proof (5.00→0.63 GB, 87.4%)
- Multi-dim field: w-dim read (291/291 roundtrip), climate shift (11/11), concurrent reads (4 threads, 0 fails), field_resolve tool
- KIS codec v4-6: storage codec, used internally by tesspack
- geo_jump / hyper_jump: 4D addressing (48×3 / 36×4 towers), proven isomorphic, tests pass
- D4-triality / fractal / quadtree / param_grid: proven islands, cross-include internally
- KV dead-slot storage: 9/9 tests, 24 MB capacity, KL=0
- Cold KV park/resume: proven on Qwen2.5
- breathing_fs: architectural foundation (bottom-up load), proven concept
- 83 project memories covering constraints, architecture, config values

### Blocking
1. Proven islands have ZERO product includes — geo_jump/hyper_jump/quadtree tested internally but no tess_*/moe_*/server/breathe/graft tool includes them
2. test_voronoi_mask.c is stale — Spotlight arg mismatch, blocks trust (memory #6836 priority 1)
3. breathing_fs not wired — tesspack_server uses VirtualAlloc (top-down), not breathing (bottom-up)
4. MoE graft selective scatter — not implemented; scatter commits all pages → OOM on LFM2.5 (7.9 GB RAM)
5. No production consumer for KV dead-slot, climate change, concurrent reads, fan24

### Priority order (from memory #6836)
1. Fix stale test_voronoi_mask.c (Spotlight arg)
2. Wire D4 triality 3-views into serve/GPU zero-copy path
3. L-block + geo_jump bridge as placement adapters
4. Quadtree seek accel only if seeks measure slow
5. rail_sync/ring/phase stay parked
6. Rule: no new geometry until islands are wired — integration phase, research pause

## Open decisions
- Which product path first? tesspack_server (serve) or breathe (loading)? Serve is more visible.
- Should geo_jump replace or augment the current flat slot addressing (tess_flat/tess_unflat)?
- D4 triality 3-views: how to map onto GPU zero-copy? (needs design)
- Selective scatter for MoE: implement now or defer? (needed for LFM2.5 on <8 GB machines)

## Skills to use
- `graft` — navigate codebase, find integration points
- `focused-fix` — systematic deep-dive repair across files
- `ponytail` — minimal change, don't over-engineer
- `verification-before-completion` — run tests before claiming done
- `git-commit` — commit with conventional messages

## Artifacts
- Memory #6836: wiring queue priority + proven islands status
- Memory #6174: breathing_fs architectural foundation
- Memory #6324: hyper_jump proven (48×3/36×4 towers, 6/6 byte-identical)
- Memory #6354: MoE graft dry run (87.5% skippable)
- Memory #7002: GATE 4b PASS (maxdiff=0)
- Memory #8376: OS-level RSS proof (5.00→0.63 GB)
- Commit 4452417: tests wired into TIER1 (4 days ago)
- Commit 87da6b9: multi-dim field read + MoE report (latest)
- `docs/MOE-GRAFT-EXPERIMENT-2026-11-18.md`: full experiment report
- `docs/MULTIDIM-READ-PLAN-2026-11-18.md`: 4-increment plan (all complete)

## Key files to start with
1. `core/geo_jump.h` / `core/geo_hyper_jump.h` — understand the API
2. `tools/tesspack_server.c` — integration target (serve path)
3. `tools/breathe_view.c` — breathing integration target
4. `tests/test_voronoi_mask.c` — fix first (stale)
5. `Makefile` lines 64-68 (TIER1) and 222 (TESS) — test wiring

## Important rules
- Memory #6836: "no new geometry until islands are wired — integration phase, research pause"
- Memory #7287: stage only your own hunks (Makefile carries other workstream's targets)
- Memory #7002: callback model needs n_gpu_layers=0 + no_host=true
- Memory #6353: fused MoE kernels read UNFIRED slices → K-only resident unenforceable on stock kernel
- Memory #8350: MAP not COMPRESS, transform conserves bits, pass-through if ratio≥1.0