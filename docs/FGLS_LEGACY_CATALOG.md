# FGLS Legacy Catalog

> Headers from `I:\FGLS_new\core\` not used in current DWGLS fork
> **ไม่ได้ใช้ตอนนี้ ≠ จะไม่ได้ใช้อีกเลย**
> Scanned: 2026-09-01

---

## How to Read This Document

- **Status:** `active` = working code, could be revived | `stub` = placeholder | `deprecated` = superseded by DWGLS
- **Relevance:** How useful this could be for DWGLS's current direction
- **Superseded by:** If DWGLS already has a replacement, what it is

---

## Category A — Potentially Useful (Revivable)

### geo_field_core.h — Unified Geometric Field

**What it does:** Full encode/decode pipeline: file → 64B chunks → GpAddr → FrustumBlock → serialize. Multi-resolution (zoom in/out by changing gp_level). Shape dimension access via Metatron routing.

**Capabilities:**
- `geo_field_encode()` / `geo_field_decode()` — full roundtrip
- `geo_field_scale_addr()` — convert address between gp_levels
- `geo_field_shape_route()` — navigate between pentagon faces
- `geo_field_fiber_walk()` — traverse all dimensions of a tile
- `geo_field_save()` / `geo_field_load()` — file persistence
- `geo_field_roundtrip()` — verify lossless

**Status:** ✅ Active code, complete pipeline
**Relevance:** ⭐⭐⭐ — Could integrate with DWGLS's Goldberg storage
**Superseded by:** Partially by `geo_goldberg_store.h` (DWGLS), but multi-resolution zoom is unique

### geo_onion_shell.h — Shell Hex Indexer

**What it does:** Hex shell geometry (shell N = 6N+1 cells). Hilbert-like traversal respecting sector boundaries. Scale law: shell N holds chunks at scale 16^N.

**Capabilities:**
- `onion_init()` / `onion_free()` — lifecycle
- `onion_chunk_at()` — map (shell, cell) → chunk_idx
- `onion_locate()` — reverse: chunk → (shell, cell)
- `onion_header_write()` / `onion_header_read()` — 64B header serialize
- `hex_shell()`, `hex_total_cells()`, `hex_ring_enum()` — hex math

**Status:** ✅ Active code
**Relevance:** ⭐⭐ — Hex shell indexing could complement DWGLS's decagram layout
**Superseded by:** No direct replacement in DWGLS

### geo_temporal_ring.h + geo_temporal_lut.h — Temporal Ring

**What it does:** 720-slot ring (stride-37 walk). Position = order = time. Self-healing gap detection. Chiral pair routing.

**Capabilities:**
- `tring_init()` / `tring_tick()` — lifecycle + advance
- `tring_pos()` — O(1) chunk → walk position
- `tring_pair_pos()` — chiral partner routing
- `tring_first_gap()` — detect missing slots
- `tring_snap()` / `tring_verify_next()` — ordered ingress
- `GEO_WALK[720]` / `GEO_WALK_IDX[720]` — full LUT tables

**Status:** ✅ Active code, LUT tables included
**Relevance:** ⭐⭐ — Similar to DWGLS's TRing walk but with gap detection
**Superseded by:** `geo_tring_walk.h` (DWGLS) — simpler, no LUT tables

### geo_diamond_to_scan.h — DiamondBlock → ScanEntry Bridge

**What it does:** Bridge between DiamondBlock and ScanEntry for the Hilbert64 encoder pipeline. 4-field assignment (Spatial/Temporal/Chroma/Ghost).

**Capabilities:**
- `diamond_to_scan_entry()` — DiamondBlock → ScanEntry
- `diamond_scan_stream()` — batch: raw bytes → ScanEntry[]
- `scan_to_diamond()` — reverse: ScanEntry → DiamondBlock
- 4-field clock: G1(spatial), G2(temporal), G3(chroma), World_B(ghost)

**Status:** ✅ Active code, complete pipeline
**Relevance:** ⭐⭐⭐ — Bridges DiamondBlock to scanner, could enable new encode paths
**Superseded by:** No direct replacement

### geo_addr_net.h — TRing → GeoNetAddr

**What it does:** O(1) LUT mapping enc → {polarity, spoke, hilbert_idx}. 720-entry table built once.

**Capabilities:**
- `geo_addr_net_init()` — build LUT (once)
- `geo_net_encode()` — O(1) lookup
- Polarity: 50/50 split (ROUTE/GROUND)
- Hilbert buckets: 120 positions for batch sorting

**Status:** ✅ Active code
**Relevance:** ⭐⭐ — Simple O(1) polarity/spoke lookup
**Superseded by:** No direct replacement

### geo_tring_fec.h — TRing + RS-FEC Integration

**What it does:** Forward error correction on TRing stream. Encode → inject gaps → recover → reconstruct.

**Capabilities:**
- `tring_fec_prepare()` — init FEC context
- `tring_fec_encode()` — file → FEC-encoded stream
- `tring_fec_recv()` — receive packet
- `tring_fec_recover()` — recover lost packets
- `tring_fec_reconstruct()` — rebuild original file

**Status:** ✅ Active code
**Relevance:** ⭐⭐⭐ — FEC is valuable for any streaming/storage system
**Superseded by:** No direct replacement in DWGLS

### geo_metatron_reshape.h — Hilbert × Peano Dual-Line

**What it does:** 3⁴ grid (81 Peano cells). Crop to 6×6 = 36 active + 28 shadow = 64 DiamondBlock. North/South pole flip.

**Capabilities:**
- `peano_l2()` — Peano L2 coordinate (9×9 grid)
- `peano_crop()` — classify cells (VERTEX/EDGE/CORE/SHADOW)
- `ico_enc()` / `ico_decompose()` — icosphere addressing
- `ico_cpair()` — north↔south flip (self-inverse)
- `ico_meta_cpair()` — diameter line on 1440 cycle
- `geo_metatron_reshape_verify()` — self-test (6 checks)

**Status:** ✅ Active code with verification
**Relevance:** ⭐⭐ — Similar to DWGLS's dual_place but more detailed cell classification
**Superseded by:** `geo_dual_place.h` (DWGLS) — simpler, proven

---

## Category B — Experimental / Research

### geo_payload_store.h — Simple KV Store

**What it does:** 2048-entry address→value store. Linear scan O(n).

**Status:** ⚠️ Simple prototype
**Relevance:** ⭐ — Superseded by DtSlotRegion (much more capable)
**Superseded by:** `dramtile_store.h` (DWGLS)

### geo_tring_goldberg_wire.h — TRing ↔ Goldberg Wire

**What it does:** Wiring between TRing stream and Goldberg pipeline. Blueprint system.

**Status:** ⚠️ Mostly stubs (tgw_write returns empty)
**Relevance:** ⭐ — Incomplete, but the wiring concept is useful
**Superseded by:** `geo_goldberg_store.h` (DWGLS) — complete streaming store

### goldberg_shutter.h — Ring Confidence System

**What it does:** Confidence scoring for Goldberg ring operations.

**Status:** ⚠️ Unknown (not read in detail)
**Relevance:** ⭐ — Could be useful for quality metrics

---

## Category C — Superseded by DWGLS

### geo_goldberg_tile.h → geo_goldberg_decagram.h

**Old:** Goldberg tile mapping (bridge to collection/)
**New:** Decagram 10-sector layout (exact n²−1 per sector)
**Why replaced:** Decagram is exact, no remainder; tile was a bridge stub

### kis_codec.h / kis_codec_v3.h → kis_codec_v4.h

**Old:** Legacy codecs
**New:** v4 with lossless permutation encoding
**Why replaced:** v4 proven on real GGUF, v3 had position reconstruction issues

### geo_compound_cfg.h → geo_param_grid.h

**Old:** Compound configuration
**New:** Parameterized grid with 11 GeoTypes
**Why replaced:** Param grid is more general, all shapes from one root

### geo_tring_addr.h → geo_tess_wiring.h

**Old:** Tring address bridge (to collection/)
**New:** Tesseract wiring (rescope ↔ physical)
**Why replaced:** Tesseract addressing is the current protagonist

### geo_goldberg_tile.h → geo_goldberg_decagram.h

**Old:** Goldberg tile (bridge stub)
**New:** Decagram layout
**Why replaced:** Decagram is proven, tile was a bridge

---

## Category D — Likely Deprecated

### fabric_wire.h / fabric_wire_drain.h

**What it was:** Wire routing and drain system
**Why deprecated:** Superseded by Fibo Spine pipeline
**Relevance:** ⭐ — The concept lives in fibo_spine.h

### geo_onion_shell.h

**What it was:** Onion shell hex indexer
**Why deprecated:** Not integrated into DWGLS's storage model
**Relevance:** ⭐⭐ — Could be useful for multi-resolution storage

### geo_metatron_route.h / geo_metatron_reshape.h

**What it was:** Metatron routing between faces
**Why deprecated:** DWGLS uses tesseract adjacency instead
**Relevance:** ⭐⭐ — Different routing model, could coexist

### geo_temporal_ring.h / geo_temporal_lut.h

**What it was:** 720-slot temporal ring with LUT
**Why deprecated:** DWGLS uses TRing walk (simpler, no LUT)
**Relevance:** ⭐⭐ — Gap detection is unique, could be useful

### geo_tring_fec.h / geo_fec_rs.h

**What it was:** Forward error correction on TRing
**Why deprecated:** Not integrated into DWGLS
**Relevance:** ⭐⭐⭐ — FEC is universally useful

### geo_diamond_to_scan.h

**What it was:** DiamondBlock → ScanEntry bridge
**Why deprecated:** DWGLS doesn't use ScanEntry pipeline
**Relevance:** ⭐⭐ — Could enable new encode paths

### geo_field_core.h

**What it was:** Unified geometric field (encode/decode/zoom)
**Why deprecated:** DWGLS uses Goldberg store + tesseract addressing
**Relevance:** ⭐⭐⭐ — Multi-resolution zoom is unique and powerful

### geo_addr_net.h

**What it was:** TRing → GeoNetAddr O(1) LUT
**Why deprecated:** DWGLS doesn't use GeoNetAddr
**Relevance:** ⭐⭐ — Simple polarity/spoke lookup

---

## Summary: What Could Be Revived

| Header | Capability | Why Useful | Effort to Integrate |
|---|---|---|---|
| `geo_field_core.h` | Multi-resolution zoom | Unique — DWGLS has no zoom | Medium |
| `geo_tring_fec.h` | Forward error correction | Universal — any storage needs FEC | Low |
| `geo_diamond_to_scan.h` | DiamondBlock → ScanEntry | New encode paths | Medium |
| `geo_onion_shell.h` | Hex shell indexing | Multi-resolution complement | Low |
| `geo_addr_net.h` | Polarity/spoke O(1) lookup | Simple utility | Trivial |
| `geo_temporal_ring.h` | Gap detection + self-healing | Unique — DWGLS has no gap detect | Low |
| `geo_metatron_reshape.h` | Peano cell classification | More detailed than dual_place | Low |

**Bottom line:** ~7 headers have unique capabilities not present in DWGLS. The most valuable are:
1. **geo_field_core.h** — multi-resolution zoom (no equivalent in DWGLS)
2. **geo_tring_fec.h** — FEC (no equivalent in DWGLS)
3. **geo_temporal_ring.h** — gap detection (no equivalent in DWGLS)
