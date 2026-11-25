# DWGLS: Deferred Systems & Error Handling Strategy

Date: 2026-11-25 · Owner's decision log

## Owner's Reasoning (why these are deferred)

1. **Complexity while unlocked:** These systems might add complexity while the core system is not yet locked/stable. Wiring them in prematurely could make debugging harder.
2. **Deterministic = indirect error correction:** The deterministic (bijection) nature of DWGLS is already a form of error correction — if mapping is exact, errors are caught by construction. This doesn't mean "do nothing," but it means these are better as a **later phase** after the core is locked.
3. **Phase approach:** Implement core first (MAP-not-COMPRESS, bijection, value-blindness). Add error handling/recovery systems (rail_sync, fibo_spine, rewind, logging) in a later phase when the core is stable.

## Deferred Systems (not wired to production)

### A. Rail Sync (core/infra/geo_rail_sync.h)
- **What:** Phase synchronization for multi-lane (6 spokes). RAIL_SYNC_THRESH=24° (10%). ready=exact, arriving=within threshold.
- **Status:** Dormant. Headers + tests only. No production consumer.
- **Wiring needed:** Add rail_sync calls to serve path + benchmark + verify latency <20% gate.
- **Owner's note:** "Universal sync primitive across all levels (4→48→576→6912→20736)."
- **Deferred because:** Complexity while unlocked. Deterministic routing already provides indirect error correction.

### B. Fibo Spine / Ribcage / Jet Bridge (core/infra/fibo_spine.h)
- **What:** 1728 pipes × 12 ticks = 20736 slots. Pipeline scheduler with async overflow (jet bridge: tick 11→residual→tick 13, skip tick 12). Ribcage freezes at tick 12.
- **Status:** Partial. Used in tools (fibo_checkpoint_sweep.c, tesspack_graft.c display) and collection (beam_value.c via fibo_tick). Not core production.
- **Wiring needed:** Wire fibo_spine tick scheduling into tesspack serve path + benchmark.
- **Owner's note:** "Combo with jet bridge is powerful (kernel-level feature, cheaper)."

### C. Temporal Rewind Buffer (tw_rewind_bridge.h, geo_rewind.h)
- **What:** 972-slot RewindBuffer (4104B chunks, GEO_WALK enc) + 20736-slot TWFaceRewind (8-byte keys, node_id). State recovery: rewind to previous known-good state.
- **Status:** Bridge exists, but geo_rewind.h is missing (referenced but not found). Partially implemented.
- **Wiring needed:** Create geo_rewind.h + wire into serve path.
- **Owner's note:** "972 is used (recently). Reports are correct."

### D. KV Temporal Parent Check (kv_temporal_read.h)
- **What:** kvl_check_parents — ensures KV log is a valid chain (parent pointers). kvl_check_unique — no duplicate turn numbers. Returns -1 on first bad one.
- **Status:** ✅ Wired into kv_temporal_llama.c. Active.
- **Owner's note:** "This one is already wired."

### E. Logging / Assertions / Retry / Circuit Breaker
- **What:** Standard error handling mechanisms (L2-L9 from standard systems analysis).
- **Status:** ❌ Not implemented. Only return codes (L1) + input validation.
- **Owner's note:** "Fail-loud preferred. But complexity while unlocked. Later phase."

### F. 0.5 Rescale (tetra/octa/frustum)
- **What:** Double all coordinates to convert 0.5→1. Address space 20736→41472.
- **Status:** Not implemented. Breaks 12⁴ symmetry (becomes 2×12⁴).
- **Alternative:** rail_sync threshold absorbs 0.5 (arriving but never ready).
- **Owner's note:** "0.5 problem was absorbed by rail_sync, not solved. Keep as later phase."

### G. 3456 Base Structure
- **What:** 6-direction structure using 3456 = 6×576 = 6×24² as base.
- **Status:** Valid math, no consumer. Needs new structure design + benchmark.
- **Owner's note:** "3456 opens residual space. Used with frustum virtual apex."

### H. Cross-Dim Folding (Matryoshka)
- **What:** Store one representation, derive others via truncation.
- **Status:** Only tesseract 8 views (store 1, derive 8). Tring dim, goldberg, diamond shell: NOT applicable (independently computed or entropy classes).
- **Owner's note:** "Needs verification of which structures are projections vs bijections."

### I. Weighted Planar Tree + Decision Tree
- **What:** Model routing as weighted tree + decision tree pruning for self-improvement.
- **Status:** Idea only. Analyzed as redundant (light cone already prunes). Not implemented.
- **Owner's note:** "Might be useful for ANN (probabilistic). Not for DWGLS deterministic."

### J. LC-Tilecube (test_lc_integration.c)
- **What:** Integrate tesspack + dim_bridge + 26cell on real GGUF tensor.
- **Status:** Test only. Byte-exact roundtrip verified. Not wired to serve path.

## Error Handling Strategy Summary

| Mechanism | Status | Phase |
|---|---|---|
| Return codes (0/-1/-2/-3) | ✅ Active | Core (now) |
| Input validation | ✅ Active | Core (now) |
| Fail-loud (no silent garbage) | ✅ Active | Core (now) |
| Bijection verification (tests) | ✅ Active | Core (now) |
| Deterministic = indirect error correction | ✅ Active | Core (now) |
| KV temporal parent check | ✅ Active | Core (now) |
| Rail_sync (threshold-based) | ⚠️ Dormant | Later phase |
| Fibo_spine/jet bridge (pipeline) | ⚠️ Partial | Later phase |
| Temporal rewind buffer (972 slots) | ⚠️ Partial (missing geo_rewind.h) | Later phase |
| Logging/assertions/retry/circuit breaker | ❌ Missing | Later phase |
| 0.5 rescale | ❌ Not implemented | Evaluate first |
| 3456 structure | ❌ Not implemented | Evaluate first |
| Cross-dim folding | ❌ Not verified | Evaluate first |

## Key Principle

> "Deterministic itself is already indirect error correction, but that doesn't mean do nothing. It's better as a later phase."

The core system (MAP-not-COMPRESS, bijection, value-blindness) provides error detection by construction. Error handling/recovery systems (rail_sync, fibo_spine, rewind, logging) are deferred to a later phase when the core is locked and stable. This avoids added complexity during the exploratory/development phase.

## Related Memories and Notes

- Memory #8420: Core equation (20736 = 144² = 128×162 = 256×81)
- Memory #8421: FGLS_new constraint (0.5 remainder, abandoned)
- Memory #8422: Rail_sync (threshold-based, dormant)
- Memory #8423: uint64→uint32 fold (geo_seed hot path)
- Memory #8424: 3456 residual decomposition
- Memory #8425: Frustum virtual apex method
- Memory #8426: Rail_sync ×12 growth chain
- Memory #8427: Letterpair/CRT connection
- Memory #8428: 312 = 26×12 = 24×13
- Memory #8429: Weighted planar tree (redundant for DWGLS)
- Note #33: Beautiful mathematical structures
- Note #34: FGLS_new history + rail_sync + 0.5 absorption
- Note #35: 3456 residual decomposition
- Note #36: Weighted planar tree + decision tree
- Document: DWGLS_GEOMETRIC_STRUCTURES_REFERENCE.md (comprehensive reference)
