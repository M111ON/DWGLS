---
luminaCreated: 2026-08-16T06:55:01.855Z
tags: []
luminaModified: 2026-08-16T06:55:01.855Z
luminaVersion: 1.3.11
---
# GeoFS MDIM — Multidimensional Native Volume

Status: **working prototype** — 20/20 tests green (28/28 tier-1), crash-simulated under 1656 power cycles plus 382 torn 4 KB page writes, CLI demo verified (100 KB file lossless across 28 frames).
Branch: `feat/geo-native-fs` · Worktree: `I:/DWGLS-native-fs`

## What this is

A flat, mmap-able volume file where **geometry is the filesystem**:

- **20736 slots × 64 B = 1,327,104 B** — the `GEO_GEO_FULL = 20736` address space,
  one slot per coordinate.
- A slot is either a **name entry** (name bonded to its computed coordinate by
  stride-37 open addressing — *no hash, no LUT, no collision table*) or a **data
  block** (contiguous run, run start = inode).
- The **timeline journal** is the write path: every op commits a CRC'd frame at
  the ring tail, so the volume is crash-safe, versioned, and never needs a
  full snapshot re-serialize.

## The four views (same bytes, four coordinate systems)

| View | Decomposition | Meaning |
|------|---------------|---------|
| **flat** | `flat(0..20735)` | raw slot index |
| **cube** | `(x,y,z)`, 12×12×144 | cube addressing |
| **rail** | `(pipe, tick)`, pipe = slot/12, tick = slot%12 | KIS pipes |
| **time** | `(t, x)`, t = slot/144 | timeline slices |
| **cell** | `(row, col)`, 144×144 | 6ico compound cells |

All five are pure arithmetic (`mdim_view_coords` / `mdim_view_flat`), tested
for full 20736 roundtrip. A file summoned at flat 185 is *also* at
`cube(1,7,2)`, `rail(0,185,0)`, `time(5,0,0)`, `cell(1,41,0)` — reachable
through any axis, no conversion stored.

## Files

- `core/geofs_mdim.h` — the whole volume: superblock, slot allocator, stride-37
  name bonding, five views, journal (write-ahead frames, checkpoint fold,
  eviction, crash recovery, `read@frame` versioned reads), multi-frame
  run-span chains, save/load, mmap open.
- `tests/test_geo_fs_mdim.c` — 19 tests.
- `tools/mdim_cli.c` — CLI: `create / summon / get / list / info / view /
  history / unsummon / mmap`.

## CLI tour

```bash
make mdim_cli
./build/mdim_cli create vol.bin                 # 20736-slot volume
./build/mdim_cli summon vol.bin hello.txt f.txt # write file (commit frame)
./build/mdim_cli list vol.bin                   # entries, blocks, born frame
./build/mdim_cli view vol.bin 185               # same slot in all 5 coords
./build/mdim_cli history vol.bin hello.txt      # timeline versions
./build/mdim_cli get vol.bin hello.txt out.txt  # lossless read
./build/mdim_cli unsummon vol.bin hello.txt     # delete
./build/mdim_cli mmap vol.bin x.txt f.txt       # page-cache write path
```

## Design rules honored (from AGENTS.md)

- **No hash, no lookup** — name→coordinate is arithmetic (stride-37 probe
  over the space itself); free space is first-fit over a contiguous-run bitmap.
- **Coordinate = address** — the slot *is* the inode; nothing stored twice;
  the free list and directory are derived, not stored.
- **Timeline = field, not pipeline** — the journal ring is the timeline;
  "enter anywhere" = commit at any frame; versions fall out of geometry.
- **Lossless proven by decode** — every test decodes and compares every byte.

## Multi-frame runs (v2 — files beyond one journal frame)

Files larger than one journal frame are stored as a **chain of runs**:
`[LINK][DATA...] → [LINK][DATA...] → ...`. Each run is one contiguous
first-fit span (≤ 60 data slots); its LINK slot holds `{size = this run's
bytes, prev = next run's LINK}`. The FILE entry points at the first LINK
and carries a run-span header (`n_runs`, `n_data_slots`).

- **Chunked commits** — one run per journal frame (write-ahead → mutate →
  commit); a 100 KB file lands as 28 frames/28 runs, lossless.
- **Entry written last** — a crash mid-chain leaves no visible file; the
  orphaned runs are swept by the derived-bitmap rebuild on the next open.
- **Rewrites re-layout the chain, crash-atomic without flags** — the NEW
  chain is written first (the file keeps reading its old chain), the entry
  switches to it in ONE atomic frame, then the old chain is freed. A crash
  at any point leaves the file fully old or fully new — never torn. Grow,
  shrink, and empty rewrites are all allowed (any size ≤ 1 MiB); grow needs
  free space for both chains briefly.
- **Every freed slot is a TOMB**, never EMPTY — an EMPTY slot terminates a
  stride probe walk, so zeroing a freed block could hide entries past it.

## Crash-simulation tests (v3 — journal fault injection)

A test-only macro (`MDIM_CRASH_HOOK`, empty by default, overridable before
including the header) lets a test **kill the volume at every journal stage
boundary** of a mutation — stage 1: frame materialized, base untouched ·
stage 2: base mutated, frame uncommitted · stage 3: frame committed — for
summon, rewrite, and unsummon alike. A kill snapshots the volume bytes at
that exact instruction boundary (RAM state — bitmap, counters, staged
changes — is lost); reloading the snapshot exercises the real recovery path
(super → recover → rebuild) exactly as a power cycle would.

Three new tests (17–19):

- **17 — crash sweep:** every (frame × stage) kill point of multi-frame
  summon / grow / shrink / unsummon — 456 kill points, each verified.
- **18 — crash stress:** 1200 random mid-op kills across 3 seeds, each
  followed by a real power-cycle reload (cycling RAM / mmap / file open
  paths), with 2–7 unrelated files live the whole time, then a clean
  settle phase and a final save→load full verify. **~30% of kills also
  tear a 4 KB page of the journal ring** (a torn sector write on top of
  the power loss — the ring is exactly two 4 KB pages). A torn reload
  must EITHER recover to a fully consistent volume OR fail loud with
  `MDIM_ERR_CORRUPT` — a fail-loud volume is discarded (backup restore)
  and the run continues. Measured: 382 torn pages → 283 fail-loud, 99
  clean recoveries, **0 silent tears**.
- **19 — torn writes:** a torn byte inside a committed frame must fail the
  LOAD loud (`MDIM_ERR_CORRUPT` — never silent), while a torn byte in the
  ring's unused slack must load clean.
- **20 — timeline torn frames:** a committed frame's change records are
  torn on a LIVE volume, then EVERY versioned read (`read_at` / `state_at`)
  in the retained range must fail loud `MDIM_ERR_CORRUPT` — never wrong
  history. Tearing the newest frame poisons every versioned read; tearing
  a middle frame fails only the reads that must consult it, while reads
  strictly above it stay byte-correct (the "not wrong data" boundary). A
  targeted case tears a change record's SLOT-INDEX field — which the
  per-change CRC does not cover — proving the frame-level CRC carries the
  load. Direct base reads (`verify`) survive: the tear poisons only the
  timeline, not current data.

The oracle is strict: summon killed → target absent or byte-exact; rewrite
killed → target old or new, byte-exact; unsummon killed → target present or
absent; unrelated files always byte-identical; and the derived counters
(`n_files`, `n_blocks_used`) and bitmap must always agree with the FILE
entries present — a torn or leaked block is a test failure.

**Two real bugs found and fixed by these tests:**

1. **Windows mmap leak** — `mdim_volume_free` closed the mapping handle but
   never called `UnmapViewOfFile` (closing the handle does NOT unmap views).
   A leaked view kept the file's section alive, so a later `fopen("wb")` or
   `remove()` on the same path failed (errno 22). Fixed: unmap before close.
2. **Stale-frame rollback** — recover() trusted any uncommitted frame at the
   computed tail as *the* crash frame. After a crash-recovery left a rolled-
   back frame behind, a later compaction restarted the ring at slot 1
   without wiping the dead zone; a second crash whose tail landed on that
   stale frame rolled back its ANCIENT before-images into the base
   (resurrecting a deleted file's entry). Fixed: the ring is wiped on
   compaction and after recovery — *the ring contains only live frames*;
   anything beyond the live tail is guaranteed clean.
3. **Torn crash frame rolled back partially** — recover's rollback of the
   uncommitted frame skipped torn changes one by one (the old "torn change
   → leave" v1 limitation). A torn page could therefore restore HALF a
   frame's before-images into the base — silently torn data. Upgraded to a
   strict contract: the crash frame is validated IN FULL (every before-
   image CRC + index, plus a readable frame span) before any restore; any
   tear → `MDIM_ERR_CORRUPT` (fail-loud), never a partial rollback.
4. **Timeline served wrong history on torn frames** — `state_at` verified
   only each change record's before-image; a tear in a record's SLOT INDEX
   or in a frame header (frame_no / n_changes / prev / magic) could
   silently stop or misdirect the undo walk and return wrong history with
   `MDIM_OK`. Hardened: `state_at` now CRC-verifies EVERY frame before
   trusting any field or undoing any record — any torn byte in a committed
   frame is fail-loud `MDIM_ERR_CORRUPT` on every versioned read that
   reaches it (and reads above a torn middle frame stay byte-correct).

## Timeline cost (benchmarked — `make bench_mdim_timeline`)

`state_at(F)` = 1.3 MB base copy + a walk newest→oldest that CRC-verifies
every retained frame before undoing changes newer than F (the hardening
added in the crash work). Measured at `-O2`, 3000 iters, MinGW gcc 8.1 on
this machine (avg / min µs per call):

| read | full ring, deepest version | floor (newest) | hardening overhead |
|------|---------------------------|----------------|--------------------|
| `state_at` | **250 / 199 µs** | 118 / 87 µs | 132 / 112 µs |
| `read_at`  | **634 / 522 µs** | — | malloc + probe + chain read |

Worst case = deepest valid version (checkpoint+1) on a full 128-slot ring:
18 retained frames, 8064 B CRC'd (the whole ring) + 3456 B undo memcpy.
The 1.3 MB base copy dominates (~87–118 µs); the frame-level CRC walk
adds ~112–132 µs at worst — the bitwise (table-free) CRC is the bulk of
that; a table-driven CRC would cut it ~8×. `read_at` doubles the cost via
a fresh 1.3 MB snapshot allocation per call.

## Current limits (v2, documented)

- Single-frame cap = 3780 B (`MDIM_CAP_ONE_FRAME`); the real per-op cap is
  now the volume: **1 MiB hard cap** (`MDIM_MAX_FILE_BYTES`), bounded by
  the 20,607-slot data region (~1.27 MB usable).
- Volume is 20736 slots (~1.3 MB) — one *pipe* of the 1728×12 KIS spine.
- Journal ring = 128 slots; frames evicted oldest-first with checkpoint fold
  (multi-frame ops evict their own early frames; history depth = one ring).
- Rewrites are full re-layouts: they briefly need the old + new chains to
  coexist (a grow near capacity can hit `MDIM_ERR_NOSPC`).
- No directory tree, no symlinks, no permissions yet.

## Next steps (ordered)

1. **Mount layer** — WinFSP/Dokany shim so `CreateFile` works on the volume
   (breathing_fs_cli already has the write-map/get surface to mirror).
2. **Full spine** — scale from 20736 to the 1728-pipe spine (FS_PIPES × FS_TICKS).
3. **Path tree** — directory slots whose payload is an entry array (real
   inode direct-block style) on top of the flat name space.
