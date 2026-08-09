# Breathing FS Image Format Spec (BIMG v2)

**Status**: Implemented — `core/bfs_persist.h` (v2, Aug 10 2026)
**Test**: `tests/test_bfs_persist.c` 65/65 PASS + hub 56/56 + anchor 51/51, `make test` 28/28

---

## Overview

BIMG is the persistent on-disk format of the Breathing FS geometric
filesystem. **v2 makes the image actually small** — three removals
based on the anchor insight (delta = home×(scale−1) is a pure function):

| v1 (was) | v2 (now) | Saved |
|----------|----------|-------|
| DeltaLog 256×u32 = 1024 B | ✂️ removed (derived) | −1024 B |
| BlockMeta 16 B/block (stored current+delta) | 8 B/block (anchor only, derived on parse) | −1152 B |
| Data region fixed 512 B × 144 = 73728 B | **packed** — Σ actual encoded payloads | variable |

Magic: `0x474D4942` ("BIMG") · Version: **2** · Size: **variable**,
`= 5792 + data_size + 4` bytes. Max 79,524 B (all 144 blocks full).

All integers little-endian.

## Why packed (v2 core change)

v1 was a fixed 82 KB fortress — every block reserved 512 B even when
empty or tiny. v2 packs payloads contiguously and tracks each block's
absolute offset in the EncOffset table. File size now reflects **actual
data**, not reserved address space:

- empty image: 5,796 B (was 81,700 B) — **14x smaller**
- 1 file: 5,963 B
- 56 files (140 blocks full): 25,956 B (was 81,700 B) — **3.1x smaller**
  (Aug-10 never-expand fix: codec falls back to RAW when a strategy would
  expand; before the fix this was 51,156 B / 2.54x)

Overhead vs payload drops from 4.05x → 1.29x on the full-file bench
(fixed TOC 5,792 B is now the only overhead — the codec never inflates).

## Layout (v2)

```
Offset       Size     Region
────────────────────────────────────────────────────────────
0            64       ImageHeader (data_size @36)
64           3136     FileTable   (64 × 49 B)
3200         576      OwnerMap    (144 × u32)
3776         1152     BlockMeta   (144 × 8 B)   ← anchor only
4928         288      EncodedSize (144 × u16)
5216         576      EncOffset   (144 × u32)   ← absolute offsets
5792         data_size  DataRegion (PACKED payloads)
5792+data_size 4      CRC32 trailer (of bytes [0, data_end))
────────────────────────────────────────────────────────────
```

## ImageHeader (64 B)

| Offset | Type | Field | Meaning |
|--------|------|-------|---------|
| 0 | u32 | magic | 0x474D4942 ("BIMG") |
| 4 | u32 | version | 2 |
| 8 | u32 | header_size | 64 |
| 12 | u32 | block_size | 144 |
| 16 | u32 | block_count | 144 |
| 20 | u32 | max_files | 64 |
| 24 | u32 | n_files | live files |
| 28 | u32 | n_blocks_used | allocated blocks |
| 32 | u32 | total_bytes | logical payload bytes |
| **36** | **u32** | **data_size** | **packed data region bytes (v2)** |
| 40 | f64 | scale | seeker scale |
| 48 | u32 | seek_pos | seeker current_pos |
| 52 | u32 | home_pos | seeker home_pos |
| 56 | u32 | delta_count | **0 (delta_log removed — derived)** |
| 60 | u32 | reserved | 0 |

## BlockMeta entry (8 B × 144) — ANCHOR ONLY

| Offset | Type | Field |
|--------|------|-------|
| 0 | u32 | home_pos |
| 4 | u8  | strategy |
| 5 | u8  | scale_at_write |
| 6 | u16 | payload_size |

**Derived on parse** (never stored):
```
current_pos = home_pos × header.scale   (mod space_size)
delta       = current_pos − home_pos    = home_pos × (scale − 1)
```
This is the v2 proof: positions are a *function* of the anchor, so
storing them (v1) was redundant.

## EncOffset (144 × u32)

Absolute byte offset of each block's payload inside the image
(0 = empty). Packed layout: `enc_off[i] = DATA_OFF + Σ(payloads before i)`.
Zero-copy decode uses `map_ptr + enc_off[block]`.

## CRC

CRC32 (dyn_crc32) over all bytes `[0, data_end)` where
`data_end = 5792 + data_size`. Trailer sits at `data_end`.

| Failure | Code |
|---------|------|
| size < 5,796 or null args | -1 |
| bad magic | -1 |
| bad version (≠2) | -2 |
| bad block geometry | -3 |
| CRC mismatch | -4 |

## v1 → v2 compatibility

v2 is a **breaking format change** (dropped regions + packed data).
Old v1 images fail the version check at open. Regenerate with
`create` + `write` (or await a migration tool — not needed yet since
no production images exist).

## Access semantics

### Plain load (`bfs_load_img`)
1. ftell for actual size, fread into max-size static buffer
2. `bfs_img_parse`: validate magic → version → geometry → CRC
3. Regions parsed; payloads copied into `block_encoded[]`; current/delta derived

### mmap open (`bfs_mmap_open`)
1. Map the whole (variable-size) file
2. Same parse; payloads stay in mapping (zero-copy)
3. `bfs_mmap_read` decodes from `map_ptr + enc_off[block]`

### Write-through (`bfs_mmap_sync`)
1. Serialize to temp buffer; if new size > mapping → remap/grow file
2. memcpy into mapping, flush, refresh CRC, re-parse TOC

### Integrity (`bfs_rdh_verify_all`)
decode → re-encode → compare per used block (bijection proof), plus
CRC gate at open catches any bit-rot in packed data.

## Portability

- byte-by-byte serialization (no struct dumps — no padding drift)
- platform mmap behind `#if defined(_WIN32)`
- header-only, static inline, zero-malloc hot path

## Constants (sacred, unchanged)

| Constant | Value |
|----------|-------|
| BFS_TOTAL_SLOTS | 20736 |
| BFS_BLOCKS | 144 |
| BFS_SLOTS_BLOCK | 144 |
| BFS_SEEKER_K | 5184 |
| BFS_MAX_FILES | 64 |
| BFS_IMG_MIN_SIZE | 5796 |
| BFS_IMG_MAX_SIZE | 79524 |