# Remaining proofs — 0.5 remainder, cross-dim overlap, 3456 base

Date: 2026-11-25

## 1. Tring dim is a BIJECTION, not a projection (Matryoshka does NOT apply)

**Verified from code (geo_goldberg_sphere.h):**
- `tick = (tile_id << 8) | dim` — bit-pack, bijection (not truncation)
- `GpFiber` traverses all 8 dims for a tile_id — "compute on-the-fly, no table"
- Content at each dim is independently computed, not nested

**Conclusion:** Cross-dim folding (Matryoshka-style truncation) does NOT work for tring dim. You can derive dim-d content from (tile_id, dim) but you cannot truncate. The content is independently computed per dim, not a prefix of a larger representation.

**Contrast with 8 octant views (tesseract):** Those ARE derived (store 1 cube, derive 8 views via bijection). That's a bijection, not a projection, but it's still "store once, derive rest" because the mapping is deterministic and invertible.

## 2. 0.5 remainder breaks exact bijection (MAP-not-COMPRESS)

**The 0.5 problem:**
- Tetra compound, octa compound, frustum produce half-integer positions (X.5)
- MAP-not-COMPRESS requires exact bijection (integer address space)
- 0.5 cannot be an array index → breaks bijection

**How rail_sync "absorbed" it:**
- rail_sync_ready = exact match (0°) → never true for 0.5 data
- rail_sync_arriving = within 24° → true for 0.5 (since 0.5 << 24)
- So 0.5 data is "arriving" but never "ready" → processed but not confirmed

**Verdict for current DWGLS:**
- If using MAP-not-COMPRESS (exact bijection) → 0.5 still breaks it
- If using rail_sync (threshold-based) → 0.5 is acceptable as "arriving"
- Rail_sync is dormant (not wired to production) → 0.5 remains unsolved for production

**Can it be solved?** Only if the geometry is rescaled to avoid 0.5 (e.g., double all coordinates to make halves into integers). This would double the address space. Whether this is worth it depends on whether tetra/octa/frustum structures provide enough benefit.

## 3. 3456 base: no existing structure uses it

**Verified:** No DWGLS structure currently uses 3456 as a base:
- Tring: 720 = 6 × 120
- Goldberg: 642 tiles at level 8
- Diamond: 4913 slots at level 8
- GeoJump: 144 = 16 × 9

**3456 = 6 × 576 = 6 × 24²** is a valid base for a hypothetical 6-direction structure, but no such structure exists in code. The math is sound (3456 × 6 = 20736), but there's no consumer.

**1728 = 20736/12** is used (GEO_MOD_STRIDE orbit count in geo_jump.h). This is the closest existing usage.

## 4. Cross-dim overlap on tring: independently computed, not nested

**Verified from geo_goldberg_sphere.h:**
- "Tring as perpendicular channel — iterate over all dimension layers for the same tile_id"
- "Compute on-the-fly, no table"
- Each dim has independently computed content

**Conclusion:** Cross-dim overlap exists (same tile_id, different dim) but content is independently computed, not nested. Matryoshka-style folding (truncate larger to get smaller) does NOT apply. You can derive any dim from (tile_id, dim) but not by truncation.

**Contrast with tesseract 8 octant views:** Those ARE derived (store 1 cube, derive 8 views). The difference: tesseract views are bijections of the same data (same content, different addressing). Tring dims have independently computed content (different content per dim).

## Summary of all remaining items

| Item | Status | Verdict |
|---|---|---|
| Tring dim is bijection not projection | ✅ Proven | Matryoshka does NOT apply |
| Tring dim content is independently computed | ✅ Proven | Not nested, not foldable by truncation |
| 0.5 breaks exact bijection | ✅ Proven (reasoning) | MAP-not-COMPRESS incompatible |
| Rail_sync absorbs 0.5 via threshold | ✅ Proven (code) | "Arriving" but never "ready" |
| 3456 base has no consumer | ✅ Proven (code search) | Valid math, no structure uses it |
| Cross-dim overlap exploitable | ❌ Not exploitable | Content is independently computed, not nested |

**What's left unproven:**
- Whether rescaling 0.5→1 (doubling address space) is worth the cost for tetra/octa/frustum
- Whether a new 6-direction structure using 3456 base would provide benefit
- Whether rail_sync can be wired into production (it's dormant)
- Whether fibo_spine can be wired into tesspack serve path (needs benchmark)
