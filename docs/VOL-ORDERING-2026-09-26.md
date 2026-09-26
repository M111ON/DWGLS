# Ordering spec — field-outer resolution (2026-09-26)

Three candidates investigated (A field-outer, B tess-outer status quo, C vol6-outer).
Verdict: **A wins with corrections**. B only works as degenerate 1-capo=whole-field.
C works as address map, not as storage outer.

## Resolution

1. **Outer frame = one 20736 field.** KIS owns the ONE stride-37 claim
   (`v6_slot`); delete tess's duplicate `tess_stride_scatter`. Double scatter ends.
2. **capo != tess.** capo stays a storage chunk (per-capo file + CRC, for big
   tensors); tess becomes a VIEW (`tess*1152+cube*144+slot`, 8 contiguous rows).
   Ends the conflation that breaks validators (-3/-4), scale-log, and windows.
3. **Climate stays global.** One slide, one W ring [0,144) — the only invariant
   that survives all three candidates untouched. No per-tess/per-cell slides.
4. **HJ stays 144-scoped.** Sub-tess navigator (8 cycles/tess), no promotion.
   Tess id rides the OUTER address (vol6/GBA), never hj.
5. **vol6 = address map only.** `144^6` rank/unrank + lane split, read-only.
   Residency cells hold raw bytes; per-cell metadata (KIS header, CLIM) lives
   BESIDE the inner BFS, not inside it (fixes 100%-full saturation).
6. **GBA unbounded positions stay.** `position % 144` is the cell-index function
   (aliasing OK for a cache key, never for identity). Three widths (u64/u32/u8)
   remain but each has one owner: path / file / axis.

## Work items (done 2026-09-26)

- [x] Delete `tess_stride_scatter` claim; capo load uses `v6_slot` global idx.
      → 4 forms delegate to KIS; `test_scatter_single` green.
- [x] Split capo (storage chunk) from tess (view): `tess_flat()` in serve path.
      → `tess_capo_at`/`tess_view_row`; `test_tess_view` green.
- [x] Cell metadata header beside BFS; spill-before-evict with retire semantics
      (evict must not `memset` planets/tombs — silent audit loss today).
      → dirty/gen flags, spill hook + veto, redundant-sync skip; T5 green.
- [x] rank<->KIS-chunk<->CLIM-offset bridge + cross-cell stripe API (n > 20736).
      → `v6_stripe`/`v6_cell_index`/`v6_clim_locate`; T7 green.
- [x] `TESS_SECTION_CLIM` per-capo writers stay unwritten (pack-wide only).
      → verified: no tool writes per-capo CLIM; unit-slide A+1,B+1 proven in
      `test_clim_record` (24/24).

## Killed (do not reopen without new evidence)

- Per-tess frames (18 headers/W/CRC/slides): breaks validators, scale-log
  collapse, slide coverage, window geometry — proven in B.
- vol6 as storage/lifecycle outer: BFS saturation, audit loss, no bridges — C.
- HJ promotion to 20736-scope: new tower parameterization, unneeded — hj rides
  outer address instead.
