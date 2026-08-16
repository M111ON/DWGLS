---
luminaCreated: 2026-08-16T06:55:02.129Z
tags: []
luminaModified: 2026-08-16T06:55:02.129Z
luminaVersion: 1.3.11
---
# DWGLS 3-Phase Roadmap
## 7B on 4GB GPU via Geometry

**Generated:** 2026-08-05 | **Repo:** I:\DWGLS | **Goal:** 7B model on 4GB GPU

---

## Current State Assessment

| Asset | Status | Lines | Notes |
|-------|--------|-------|-------|
| kis_codec v4/v5/v6 | ✅ Lossless on real GGUF | 393+273 | v6 = index-based, no collisions |
| geo_param_grid | ✅ 12 GeoTypes | — | Dodeca root family |
| geo_cell_classify | ✅ 8 cell types | 140 | 3-bit parity classification |
| geo_cube_addr | ✅ Generation-indexed | 312 | Foundation layer |
| geo_sid_loader | ✅ Dual-source GEO+GGUF | 255 | Depends on FGLS_new/runner/ |
| geo_inference_bridge | ✅ GGUF→GEO mapping | 258 | Tensor metadata bridge |
| geo_tensor_map | ✅ Block mapping | 276 | Tensor name → GEO block |
| geo_phi_microscope | ✅ Observation tool | 254 | Weight inspection |
| geo_diamond_field_v4 | ✅ Diamond geometry | 1563 | Largest component |
| geo_goldberg_sphere+LUT | ✅ Goldberg polyhedra | 264 | LUT for static geo |
| frustum_gcfs + layout_v2 | ✅ Frustum geometry | — | Layout system |
| geo_dual_place | ✅ Hilbert+Peano 162→64 | 268 | Mapping layer |
| beam_entropy_container | ✅ Beam code v2 | 486 | Container format |
| **Makefile** | ❌ Missing | — | Tests compiled manually |
| **Remote repo** | ❌ No remote | — | 33 untracked files |
| **GPU code** | ❌ In FGLS_new | — | Not in this repo |
| **Pipeline integration** | ❌ In FGLS_new | — | Not in this repo |

**Key insight:** DWGLS is the **geometry research + codec lab**. FGLS_new is the **deployment target**. This roadmap keeps them cleanly separated.

---

## The #1 Most Impactful Quick Win

### 🏆 Makefile + Git Hygiene

**Why this first:** Every other task requires compiling tests. Currently you type `gcc -O2 -Wall -o tests/foo tests/foo.c -lm` for each of 25 test files. A Makefile:
- Saves 25+ manual compile commands per session
- Catches build breakage immediately
- Enables `make test` as a single verification command
- Unblocks batch testing (Phase 2)

**Effort:** 30 minutes | **Impact:** Everything else becomes faster

---

## Phase 1: This Week — Quick Wins + Infrastructure

**Theme:** "Make it build, make it clean, make it tight"

### 1.1 Makefile (Day 1)
- [ ] Create `Makefile` with targets: `all`, `test`, `clean`, `test-%`
- [ ] Auto-discover all `.c` files in `tests/`
- [ ] Common flags: `-O2 -Wall -Wextra -lm`
- [ ] `make test` runs all tests, reports pass/fail count
- [ ] `make test-kis_codec_v6` runs single test

**Exit criteria:** `make test` compiles and runs all 25 tests from one command.

### 1.2 Git Hygiene (Day 1)
- [ ] Add `dropbag/__pycache__/` and `*.pyc` to `.gitignore`
- [ ] Add `dropbag/PDF_sample/` to `.gitignore` (or commit intentionally)
- [ ] Commit all 33 untracked files with descriptive messages
- [ ] Create GitHub remote repo `DWGLS`
- [ ] Push to remote with full history

**Exit criteria:** `git status` clean, remote exists, all history pushed.

### 1.3 KIS Codec v6 Production Hardening (Day 2-3)
**Score: 82/100** — Highest practical value

- [ ] Add `v6_encode_buf()` / `v6_decode_buf()` — single-call API
- [ ] Add error codes (not just return 0/-1)
- [ ] Add compression ratio reporting (`v6_ratio()`)
- [ ] Benchmark: encode/decode time for 1M weights
- [ ] Add `v6_verify()` — decode → compare every value
- [ ] Document API in header comments

**Exit criteria:** `kis_codec_v6_test` passes, `v6_verify()` confirms lossless, API is clean enough for external use.

### 1.4 Cell Classify → KIS Integration (Day 3-4)
**Score: 58/100** — Feeds Phase 2 pruning

- [ ] `geo_cell_classify_stats()` now outputs per-type compression potential
- [ ] Add `geo_cell_prune_mask()` — which cell types are "compressible"
- [ ] Test with real GGUF weights via phi microscope
- [ ] Document: which cell types hold most weight mass?

**Exit criteria:** You can say "Cell type III holds 23% of weights and is prunable" with data.

### 1.5 Phi Microscope → Production Analysis (Day 4-5)
**Score: 62/100** — Observation feeds optimization

- [ ] Run phi microscope on 3 real GGUF models (Qwen, Llama, Phi)
- [ ] Generate weight distribution reports per model
- [ ] Identify: which GeoType best fits each model's weight distribution?
- [ ] Save reports as `docs/phi_analysis_*.md`

**Exit criteria:** Three model analysis reports exist, each recommending a GeoType.

**Phase 1 Dependencies:**
```
Makefile ──→ can compile everything
    │
    ├──→ KIS v6 hardening ──→ production-ready codec
    │
    ├──→ Cell classify ──→ pruning masks ──→ Phase 2
    │
    └──→ Phi microscope ──→ model analysis ──→ Phase 2
```

**Phase 1 Synergy:** Makefile enables all other work. Cell classify + Phi microscope together answer: "Which weights in which models can be addressed geometrically?"

---

## Phase 2: This Month — Core Feature Development

**Theme:** "Batch process, observe, classify, containerize"

### 2.1 GEO SID Loader Batch Conversion (Week 2)
**Score: 72/100** — Second highest practical value

- [ ] `geo_sid_batch_convert(gguf_dir, geo_dir)` — convert directory of GGUFs
- [ ] Progress reporting + error recovery
- [ ] Verify each conversion with round-trip test
- [ ] Generate conversion report (sizes, ratios, timings)
- [ ] Support Q4_0, Q8_0, F16, F32 dtypes

**Exit criteria:** Convert 5 GGUF models to GEO format, verify all round-trips lossless.

### 2.2 Cube Container Data Format (Week 2-3)
**Score: 54/100** — Needed for deployment

- [ ] Define `.gcube` binary format header (magic, version, tensor count)
- [ ] Each tensor = cube face addressing (6 faces × slots)
- [ ] Add CRC-32 per tensor for integrity
- [ ] `gcube_create()` / `gcube_load()` / `gcube_verify()`
- [ ] Test: create gcube from GGUF tensor, load back, compare

**Exit criteria:** `.gcube` format spec written, one tensor round-trip verified.

### 2.3 Geometric Pruning Engine (Week 3)
**Score: 52/100** — Cell classify feeds this

- [ ] `geo_prune()` — zero out weights in "empty" cell types
- [ ] Threshold: cell types with < 1% mass → prune
- [ ] Measure: how many weights pruned, model perplexity change
- [ ] Integrate with cell classify stats from Phase 1

**Exit criteria:** Pruning reduces weight count by X%, document quality impact.

### 2.4 Diamond Field Weight Mapping (Week 3-4)
**Score: 54/100** — Needs Phase 2 data

- [ ] Map GGUF weight tensors onto diamond field v4 geometry
- [ ] Measure: how well do real weights fit diamond field addresses?
- [ ] Compare: diamond field vs 6ico compound for same model
- [ ] Document: which geometry is better for which layer type?

**Exit criteria:** Side-by-side comparison of diamond vs 6ico for one real model.

### 2.5 Memory Zone Planning (Week 4)
**Score: 48/100** — For 4GB GPU target

- [ ] Define memory zones: hot (active), warm (cached), cold (disk)
- [ ] Map: which tensors go in which zone during inference
- [ ] Calculate: total GPU memory needed for 7B at Q4_0 with geometry
- [ ] Document: memory layout diagram for 4GB target

**Exit criteria:** Memory budget document showing 7B fits in 4GB.

**Phase 2 Dependencies:**
```
Phase 1 (cell classify + phi microscope)
    │
    ├──→ Batch conversion ──→ large test corpus
    │
    ├──→ Cube container ──→ standardized format
    │       │
    │       └──→ Memory zones ──→ deployment planning
    │
    ├──→ Geometric pruning ──→ weight reduction
    │       │
    │       └──→ Diamond field mapping ──→ geometry comparison
    │
    └──→ All feeds Phase 3
```

**Phase 2 Synergy:** Batch conversion produces the test corpus. Cube container standardizes output. Pruning + diamond mapping together answer: "Can geometry reduce 7B to fit 4GB?"

---

## Phase 3: This Quarter — Integration + Deployment

**Theme:** "Wire it together, ship it, show it"

### 3.1 Web Visualization (Month 2)
**Score: 48/100** — Demo/showcase value

- [ ] HTML/JS viewer for GEO tensor maps
- [ ] Interactive: click cell type → see weight distribution
- [ ] 3D: rotate Goldberg sphere, see weight overlay
- [ ] Deploy as static page (GitHub Pages)

**Exit criteria:** Interactive web page showing GEO weight mapping for one model.

### 3.2 4D Rotation Codec Parameters (Month 2-3)
**Score: 48/100** — Advanced research

- [ ] 4D rotation matrices for weight permutation
- [ ] Measure: does 4D rotation improve compression over v6?
- [ ] Compare: rotation vs index-based (v6) vs hybrid
- [ ] Document: when does rotation help?

**Exit criteria:** Benchmark report: rotation vs v6 for 3 models.

### 3.3 GEO→FGLS_new Bridge (Month 3)
- [ ] Export GEO format from DWGLS
- [ ] Import in FGLS_new runner
- [ ] End-to-end: GGUF → GEO (DWGLS) → inference (FGLS_new)
- [ ] Benchmark: inference speed with GEO vs raw GGUF

**Exit criteria:** 7B model runs via GEO path in FGLS_new.

### 3.4 7B on 4GB Validation (Month 3)
- [ ] Full pipeline: GGUF → GEO → gcube → memory zones → inference
- [ ] Measure: peak GPU memory, inference tokens/sec
- [ ] Compare: geometry path vs standard GGUF path
- [ ] Document: final memory layout + performance

**Exit criteria:** 7B model runs in ≤4GB GPU memory via geometry path.

**Phase 3 Dependencies:**
```
Phase 2 (batch, container, pruning, mapping, memory)
    │
    ├──→ Web visualization ──→ showcase
    │
    ├──→ 4D rotation ──→ advanced codec research
    │
    └──→ GEO→FGLS bridge ──→ 7B on 4GB validation
            │
            └──→ SHIP IT
```

---

## Synergy Map (Cross-Phase)

```
                    PHASE 1                    PHASE 2                    PHASE 3
                    ───────                    ───────                    ───────
Makefile ─────────→ Batch testing ────────────→ Continuous integration
    │
Cell classify ────→ Pruning masks ───────────→ Geometric pruning ───────→ 7B fits 4GB
    │                       │
Phi microscope ───→ Model analysis ───────────→ GeoType selection ──────→ Optimal mapping
    │
KIS v6 hardening ─→ Production codec ─────────→ Cube container ─────────→ Deployment format
    │
GEO SID loader ──→ Batch conversion ─────────→ Test corpus ────────────→ Validation data
```

---

## Risk Register

| Risk | Impact | Mitigation |
|------|--------|------------|
| No Makefile blocks everything | High | Do it first, 30 min |
| 33 untracked files → lost work | Medium | Commit today |
| GEO SID depends on FGLS_new/runner/ | Medium | Keep dependency explicit, document |
| 7B may not fit 4GB even with geometry | High | Memory zone planning in Phase 2 catches this early |
| Web viz = scope creep | Low | Keep it static HTML, no server |

---

## Success Metrics

| Metric | Phase 1 | Phase 2 | Phase 3 |
|--------|---------|---------|---------|
| Tests passing | 25/25 | 25/25 + new | 25/25 + integration |
| Git commits | Clean working tree | +10 feature commits | +20 total |
| GGUFs converted | 0 | 5 | 10+ |
| Compression ratio | Baseline (v6) | +pruning improvement | Final |
| GPU memory (7B) | N/A | Budget estimate | ≤4GB validated |
| Documentation | AGENTS.md | +analysis reports | +deployment guide |

---

## Immediate Next Actions (Today)

1. **Create Makefile** — `make test` works for all 25 tests
2. **Git hygiene** — `.gitignore` update, commit all, push to remote
3. **Start KIS v6 hardening** — production API wrapper

Everything else builds on these three.
