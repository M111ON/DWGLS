# BIMG Image Format Spec — Breathing FS Storage Layer

**Version**: 1 (2026-08-10) · **Header**: `core/bfs_persist.h` · **Tests**: `tests/test_bfs_persist.c`

## Purpose

BIMG is the persistent container of the Breathing FS. It is a **fixed-size,
fixed-offset binary image** that holds the complete 20736-slot address space
(144 blocks × 144 slots) plus all TOC metadata, seeker state, and delta log.

- **Fixed offsets** → no TOC walk, O(1) region lookup
- **mmap-friendly** → decode payloads straight from the mapping (zero-copy)
- **CRC-32 trailer** → whole-image integrity at open

## Layout (82,000 bytes total)

| Offset | Size | Region |
|--------|------|--------|
| 0 | 64 | Header |
| 64 | 3136 | FileTable (64 entries × 49 B) |
| 3200 | 576 | BlockOwner (144 × u32, 0xFFFFFFFF = free) |
| 3776 | 2304 | BlockMeta (144 × 16 B) |
| 6080 | 288 | EncodedSize (144 × u16) |
| 6368 | 1024 | DeltaLog (256 × u32) |
| 7392 | 576 | EncOffset (144 × u32 — payload offset in mapping) |
| 7968 | 73728 | DataRegion (144 × 512 B fixed stride) |
| 81696 | 4 | CRC-32 trailer (of bytes [0, 81696)) |

## Header (64 B)

| Off | Type | Field | Meaning |
|-----|------|-------|---------|
| 0 | u32 | magic | `BFS_IMG_MAGIC` = 0x474D4942 ("BIMG") |
| 4 | u32 | version | `BFS_IMG_VERSION` = 1 |
| 8 | u32 | header_size | 64 |
| 12 | u32 | block_size | 144 (slots per block) |
| 16 | u32 | block_count | 144 |
| 20 | u32 | max_files | 64 |
| 24 | u32 | n_files | live file count |
| 28 | u32 | n_blocks_used | allocated blocks |
| 32 | u32 | total_bytes | logical payload bytes |
| 36 | u32 | reserved | 0 |
| 40 | f64 | scale | seeker scale |
| 48 | u32 | seek_pos | seeker current_pos |
| 52 | u32 | home_pos | seeker home_pos |
| 56 | u32 | delta_count | delta log entries |
| 60 | u32 | reserved | 0 |

## FileTable entry (49 B × 64)

| Off | Size | Field |
|-----|------|-------|
| 0 | 32 | name (char[32]) |
| 32 | 4 | n_blocks |
| 36 | 4 | home_block |
| 40 | 4 | total_bytes |
| 44 | 4 | strategies[4] (u8 count per strategy) |
| 48 | 1 | valid |

## BlockMeta entry (16 B × 144)

| Off | Size | Field |
|-----|------|-------|
| 0 | 4 | home_pos |
| 4 | 4 | current_pos |
| 8 | 4 | delta (i32) |
| 12 | 1 | strategy |
| 13 | 1 | scale_at_write |
| 14 | 2 | payload_size |

## EncOffset (144 × u32)

Absolute byte offset of each block's encoded payload **within the mapped
image** (0 = block free/empty). Enables zero-copy decode via
`bfs_mmap_read` — `dyn_decode` reads `map_ptr + enc_off[b]`.

## Integrity

- **CRC-32 trailer** at offset 81696 over `[0, 81696)` — detects any
  bit-rot / truncation at open (return -4)
- **RDH bijection verify** (`bfs_rdh_verify_all`) — for every used block:
  decode → re-encode → compare strategy + payload bytes.
  `encode(decode(x)) == x` ⟺ lossless bijection at that coordinate.
- Magic/version/geometry checks at parse (return -1/-2/-3)

## API (all header-only, static inline, zero-malloc hot path)

| Function | Purpose |
|----------|---------|
| `bfs_img_serialize(fs, buf)` | BreathingFS → 82 KB byte image |
| `bfs_img_parse(buf, size, fs, enc_off)` | validate + parse → BreathingFS |
| `bfs_save_img(path, fs)` | fwrite serialized image |
| `bfs_load_img(path, fs)` | fread + parse (payloads copied in) |
| `bfs_mmap_open(path, mfs)` | MapViewOfFile/mmap + parse (zero-copy) |
| `bfs_mmap_close(mfs)` | unmap + close handles |
| `bfs_mmap_read(mfs, name, out, n, actual)` | zero-copy decode from mapping |
| `bfs_mmap_sync(mfs)` | write-through: serialize into mapping + flush |
| `bfs_rdh_verify_all(mfs)` | RDH bijection verify, returns verified count |

## Error Codes

| Code | Meaning |
|------|---------|
| -1 | bad magic / null args / file too small |
| -2 | bad version |
| -3 | bad geometry (block_size/count mismatch) |
| -4 | CRC mismatch |
| -5 | mmap view failed |

## Windows / Linux mmap

```c
#if defined(_WIN32)
  CreateFileA → CreateFileMappingA → MapViewOfFile   (PAGE_READWRITE)
  FlushViewOfFile → UnmapViewOfFile → CloseHandle
#else
  open(O_RDWR) → fstat → mmap(MAP_SHARED, PROT_READ|WRITE)
  msync(MS_SYNC) → munmap → close
#endif
```