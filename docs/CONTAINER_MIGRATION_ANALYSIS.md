---
luminaCreated: 2026-08-16T06:55:01.264Z
tags: []
luminaModified: 2026-08-16T06:55:01.264Z
luminaVersion: 1.3.11
---
# Container Format Migration Analysis
## 5 → Layered Architecture: Practical Constraints & Risks

**Date:** 2026-08-08  
**Author:** Hermes subagent  
**Scope:** Real codebase analysis, not aspirational architecture

---

## 1. ACTUAL INVENTORY — What We Have

| # | File | Header | CRC | Magic | Alloc | mmap | Consumers |
|---|------|--------|-----|-------|-------|------|-----------|
| 1 | `geo_kis_container.h` | 24B | **CRC-64** | `KIS\0KIS` (uint64) | **None** | ✅ | geofs_core.h, gguf_tool.c, 2 tests |
| 2 | `geo_kis_4d_container.h` | 32B | **CRC-32** | `KIS4` (uint32) | **None** | ✅ | 2 tests only |
| 3 | `tesseract_container.h` | 32B | **CRC-32** | `TES4` (uint32) | **stdlib.h** ⚠️ | ❌ | 1 standalone test |
| 4 | `geo_cube_container.h` | 64B | **CRC-32** | `GCB\0` (char[4]) | **malloc/free** ⚠️ | ❌ | geo_tensor_hub.h, geo_zerocopy.h, 5+ tests |
| 5 | `beam_entropy_container.h` | None (in-memory) | None | N/A | None | ✅ | None (standalone) |
| 6 | `entropy_container.h` | None (in-memory) | None | N/A | None | ✅ | None (standalone) |
| 7 | `geo_tess_container.h` | 64B | **CRC-64** | `TESS` (uint32) | **None** | ✅ | 2 tests |
| 8 | `dramtile_container.h` | None (in-memory) | None | N/A | None | ✅ | GPU pipeline |
| 9 | `kis_codec_v6.h` | Separate | None | `KCV6` | malloc in codec | ❌ | 2 tests |

**Actually 9 container-like headers, not 5.** Plus 3 codec versions (v4/v5/v6).

---

## 2. CRITICAL FLAWS IN THE "THIN SHELL + SWAPPABLE CODEC" CONCEPT

### Flaw 1: The containers are NOT just wrappers around codecs

Each container embeds **domain-specific addressing logic**, not just storage:

- **tesseract_container.h** → octant geometry, Cayley transforms, mirror operations, axis reflection
- **geo_cube_container.h** → tensor index with 48-char names, GGML dtype, multi-tensor packaging
- **beam_entropy_container.h** → BECCoord (8-bit zone+position), stride-37 scatter, RDH addressing
- **geo_kis_4d_container.h** → scale_factor, 3-axis X/Y/Z partition, angle-resolve formula (uses `double`)

**A "thin shell" cannot unify these** because the containers encode fundamentally different geometric models. The octant math in tesseract is not a "codec" — it IS the geometry.

### Flaw 2: mmap compatibility is NOT uniform

| Container | mmap-safe? | Why |
|-----------|-----------|-----|
| geo_kis_container.h | ✅ | Fixed-size header, CRC-64 at tail, direct offset |
| geo_kis_4d_container.h | ✅ | Fixed-size header, CRC-32 at tail |
| geo_cube_container.h | ⚠️ | `gcube_read()` uses malloc/fread, BUT `geo_zerocopy.h` already implements mmap path by setting `cube->blocks = mapped_ptr` directly — **zero-copy works today** |
| tesseract_container.h | ❌ | `stdlib.h` included, runtime pointer derefs in TessContainer |
| geo_tess_container.h | ✅ | Fixed header + formula section, CRC-64 |

**Key nuance:** `geo_zerocopy.h` (line 134: `cube->blocks = p`) already solves the mmap problem for `.gcube` by pointing `blocks` into the mmap'd region. The malloc in `gcube_read()` is the **fread path only**, not the mmap path. This means the zero-copy pipeline already works — migration risk is lower than it appears.

### Flaw 3: CRC divergence is NOT cosmetic

- **CRC-64/ECMA** (poly 0x42F0E1EBA9EA3693) — used by kis_container, geo_tess_container
- **CRC-32** (poly 0xEDB88320) — used by kis_4d_container, tesseract_container, geo_cube_container
- **No CRC at all** — beam_entropy_container, entropy_container

**Unifying to CRC-64 requires:**
1. Re-serializing ALL existing `.gcube` files (they have CRC-32 embedded)
2. Breaking wire compatibility with any existing `.gcube` files on disk
3. Risk: `geo_tensor_hub.h` and `geo_zerocopy.h` both parse `.gcube` directly — they will fail on old files

### Flaw 4: The "shell_container.h" dependency is BROKEN

`geo_chord.h` includes `shell_container.h` — **this file does not exist** in the repo. This means:
- The chord-based multi-pointer system is dead code
- Any "shell" abstraction was started but never completed
- Building `geo_chord.h` will fail

### Flaw 5: Sacred constants are hardcoded, not parameterized

Every container hardcodes: `20736`, `1728`, `144`, `12`, `6912`, `3456`, `37`

A unified shell would need to make these configurable, but they are **geometric invariants**, not parameters. You cannot swap them at runtime — they define the KIS address space itself.

---

## 3. MIGRATION RISKS — Ranked by Severity

### 🔴 RISK 1: Wire format breakage (CRITICAL)

**What breaks:** Any `.gcube` file written by `gcube_write()` uses:
- 64B header + 80B tensor entries + CRC-32 at tail
- `geo_zerocopy.h` parses this layout directly via mmap pointer arithmetic

**Migration cost:** Need either:
- A dual-read path (detect old format by magic, auto-convert)
- Or a bulk converter that re-writes all `.gcube` files

**Impact:** `geo_tensor_hub.h` (line 48: `#include "geo_cube_container.h"`) is the primary runtime loader. It calls `gcube_read()` which calls `fread()` and `malloc()`. This is the **hot path** and it uses **malloc** — violating the sacred constraint.

### 🟠 RISK 2: malloc creep in the current code

| Container | malloc in hot path? |
|-----------|-------------------|
| geo_cube_container.h | **YES** — `gcube_read()`, `gcube_write()`, `gcube_verify()` all use malloc |
| tesseract_container.h | **YES** — includes stdlib.h |
| geo_kis_container.h | No — uses caller-provided buffer |
| geo_kis_4d_container.h | No — uses caller-provided data pointer |
| beam_entropy_container.h | No — pre-allocated 144×144 field |

**Migration note:** If you unify, you MUST ensure the new shell does not introduce malloc in the hot path. The current `geo_cube_container.h` already violates this constraint.

### 🟠 RISK 3: Include dependency explosion

Current dependency graph for containers:
```
geo_kis_container.h → geo_frame_seek.h, geo_adaptive_store.h
tesseract_container.h → hyperbolic_seek.h, geo_kis_projection.h
geo_cube_container.h → (none beyond stdlib)
beam_entropy_container.h → fibo_tick.h, geo_frame_seek.h, beam_timer.h, rdh_capture.h
geo_tess_container.h → (self-contained)
dramtile_container.h → dramtile_store.h
```

**Any unified shell must include ALL of these dependencies** — or the shell becomes the entire codebase. A thin shell cannot exist if the codec layers need geometry-specific includes.

### 🟡 RISK 4: Test coverage fragmentation

| Container | Dedicated tests | Cross-tests |
|-----------|----------------|-------------|
| geo_kis_container.h | `kis_container_place.c`, `kis_multi_container.c` | geofs tests |
| geo_kis_4d_container.h | `test_kis_4d_container.c`, `test_kis_4d_scale_all.c` | None |
| tesseract_container.h | `test_tess_header.c`, `test_octant_selfinv.c` | None |
| geo_cube_container.h | `test_cube_container.c` | tensor_hub, zerocopy, rail_hub tests |
| beam_entropy_container.h | None | None |
| geo_tess_container.h | `test_octant_selfinv.c` | None |

**Migration risk:** `geo_cube_container.h` changes cascade to 5+ test files. Any unified format change must update ALL of them.

### 🟡 RISK 5: The geo_tess_container.h vs tesseract_container.h collision

These are TWO DIFFERENT formats with the SAME name concept:

| Property | tesseract_container.h | geo_tess_container.h |
|----------|----------------------|---------------------|
| Magic | `TES4` (0x54455334) | `TESS` (0x54455353) |
| Header | 32B | 64B |
| CRC | CRC-32 | CRC-64 |
| Includes | hyperbolic_seek.h, geo_kis_projection.h | Self-contained |
| Purpose | 4D tesseract octants | Single-cube .tess format |
| Sections | None | LUT, OMAP, STAB, META |

**These are already two different "layered" attempts.** One is old (32B, CRC-32), one is new (64B, CRC-64). The new one already looks like a partial shell design. This is the natural migration target, not a new abstraction.

### 🟢 RISK 6: kis_codec v4/v5/v6 are already separate from containers

The codecs (kis_codec_v4.h, v5.h, v6.h) are ALREADY decoupled from container formats. They operate on flat `int8_t[]` arrays. The container formats are the serialization layer on top.

**This means:** The "swappable codec" part is partially done. The real problem is the **container shell**, not the codec.

---

## 4. WHAT ACTUALLY NEEDS TO UNIFY

The real overlap is narrower than it appears:

| Concern | Currently split across | Can unify? |
|---------|----------------------|-----------|
| Magic + version + CRC | All 5 formats | ✅ Yes — standard header |
| Data payload | Different structures per format | ⚠️ Partially — keep codec-specific |
| Addressing logic | Embedded in each container | ❌ No — geometry-specific |
| Serialization | Some use fread, some buffer | ✅ Yes — standard serialize/deserialize |
| Tensor index | Only in geo_cube_container | ✅ Optional section |
| File layout | Header + data + CRC | ✅ Yes — standardize |

---

## 5. PRACTICAL MIGRATION PATH

### Phase 0: Fix existing violations (prerequisite)
- Remove malloc from `gcube_read()` / `gcube_write()` — require caller-provided buffer
- Create `shell_container.h` (currently missing — geo_chord.h depends on it)
- Decide: CRC-64 everywhere, or live with dual CRC

### Phase 1: Standardize geo_tess_container.h as the reference
The `geo_tess_container.h` (64B header, CRC-64, sections, self-contained) is ALREADY closest to the target design. It has:
- Standard header (64B, CRC-64)
- Optional sections (LUT, OMAP, STAB, META) — this IS the "swappable codec" pattern
- Self-contained (no external deps)
- Lossless verification (decode→compare)

**This is the shell.** Extend it to cover gcube-style tensor indexing.

### Phase 2: Migrate gcube → extended .tess format
- Add tensor index as a .tess SECTION (section type `TIDX`)
- Add GCubeFileHeader fields as .tess header fields
- Write converter: `gcube → .tess` (one-time, offline)

### Phase 3: Keep kis_container and beam_entropy as-is
These are runtime-only formats with no file serialization overlap with .tess. They serve different purposes:
- `geo_kis_container.h` → adaptive storage runtime state
- `beam_entropy_container.h` → in-memory 144×144 field

**Do not force-fit them into the .tess shell.** They are not file formats.

### Phase 4: Deprecate tesseract_container.h and geo_kis_4d_container.h
- `tesseract_container.h` (32B, CRC-32) → replaced by geo_tess_container.h
- `geo_kis_4d_container.h` (32B, CRC-32) → only 2 test files use it, migrate tests

---

## 6. CONSTRAINTS THAT CANNOT BE RELAXED

1. **No malloc in hot path** — geo_cube_container.h VIOLATES this. Fix first.
2. **O(1) access** — All containers use direct offset math. A shell MUST preserve this.
3. **CRC-64 preferred** — 3 of 5 formats use CRC-32. Migration cost: re-serialize all files.
4. **mmap support** — geo_cube_container.h VIOLATES this. Fix by requiring flat layout.
5. **Sacred: 20736, 1728, 144** — These are hardcoded as `#define` in every header. A unified shell should define them ONCE, but this is cosmetic, not structural.
6. **Lossless verification** — geo_tess_container.h has it. Others don't. Add to shell.

---

## 7. THE HONEST ASSESSMENT

**The "5 overlapping formats" framing is slightly misleading.** The actual situation:

- **2 real file formats:** `.gcube` (tensor storage) and `.tess` (KIS geometry)
- **3 runtime-only containers:** kis_container, kis_4d_container, beam_entropy (no file I/O, in-memory only)
- **3 codec versions:** v4, v5, v6 (already separate from containers)
- **1 broken dependency:** shell_container.h (doesn't exist)

**The real problem is NOT 5 overlapping formats.** It's:
1. `geo_cube_container.h` violates no-malloc and mmap constraints
2. CRC-32 vs CRC-64 split across file formats
3. Two tesseract-like formats (tesseract_container + geo_tess_container) doing similar things
4. No single entry point for "open a .tess or .gcube file"

**The migration should target the real problems, not a theoretical unification.**
