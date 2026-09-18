# Saturn-Ring Store — SPEC (2026-09-17, owner's analogical design)

## Image
Saturn (symmetric): core = DRAM, rings = VRAM at FIXED radii (min/max constant).
Needle reads two ways (promote/demote) with proportional ratio (vinyl player).

## Rules (structural, no tables)
1. **Fixed radii**: every ring has constant capacity. Ring full = full, no bump,
   no holes, no compact. Eviction is replacement within the ring, never resize.
   (Kills the entire vramtile bug class: bump-alloc + hole accounting.)
2. **Two-way needle**: single cursor migrates entries core↔ring both directions.
3. **Ratio policy**: hot:cold quota proportional to measured bandwidth
   (cf. colibri 9:3 drive weighting). No unbounded LRU — the ratio IS the policy.
4. **Core mutable, rings immutable-sized**: DRAM core holds working set;
   VRAM rings are fixed-size staging (upload via backend fn, e.g. Vulkan later).
5. **Symmetry**: ring geometry identical in all orientations (no preferred side —
   no hot-spot sector; wear/heat spreads by rotation, cf. GearLock clocks).

## Numbers (first concrete instance)
- Core: DRAM file-twin, unbounded working set (existing DRamTileStore).
- Ring0 (hot): 1/8 of VRAM budget, ratio weight 3.
- Ring1 (cold): 7/8 of VRAM budget, ratio weight 1.
- Migration: promote on access_tick rank within quota; demote overflow
  ring0→ring1→evict. All O(1), int-only, no malloc in steady state.

## Mirror window + carry + sync-back (owner refinement, same day)
- Zones 1..N (e.g. 10): zones 1..4 = DRAM∩VRAM mirrored (dual-resident,
  lockstep — same bytes both sides, read from either).
- Zones 5..N = VRAM carry alone (capacity without duplication).
- Zone 4 = sync-back point (last known-good mirror, Planet-style baseline):
  fault in carry zones → drop to zone 4 state, re-promote upward.
  Never rebuild from zero — recovery always starts at the mirror line.

## Non-goals
- No real VRAM in v1 (RAM-backed rings, honest label — the hypothesis
  "borrowed VRAM works" stays open until a Vulkan upload_fn lands).
- No repair of vramtile.h (frozen; lessons folded in, code not reused).

## Acceptance
- Fixed-capacity invariant holds under stress (used ≤ cap always, 0 leaks).
- Ratio respected within tolerance over N migrations (count hot:cold).
- Byte-identical roundtrip through promote/demote cycles.
