# Breathing FS Image Format Spec (BIMG v3)

**Status**: Implemented — `core/bfs_persist.h` (v3, Aug 10 2026, consensus round 1)
**Test**: `tests/test_bfs_persist.c` 66/66 PASS + hub 56/56 + anchor 51/51 + breath 22/22, `make test` 29/29

---

## Overview

BIMG is the persistent on-disk format of the Breathing FS geometric
filesystem. v2 made the image actually small (packed payloads, derived
delta). **v3 removes every redundant table** — the image now stores only
two per-block arrays (meta anchor + offsets), everything else is derived:

| v2 (was) | v3 (now) | Saved |
|----------|----------|-------|
| OwnerMap 144×u32 = 576 B | ✂️ removed — derived from file runs (contiguous-run alloc invariant) | −576 B |
| EncodedSize 144×u16 = 288 B | ✂️ removed — derived: size[i] = next_used_off − off[i] | −288 B |
| BlockMeta 8 B/block (home u32 + strategy + scale + payload_size) | 4 B/block — u32: home_pos(15) \| strategy(3) | −576 B |
| Header 64 B (stored static geometry fields) | 40 B (geometry is compile-time) | −24 B |
| payload_size / scale_at_write per block | derived from offsets + global header scale | — |

Magic: `0x474D4942` ("BIMG") · Version: **3** · Size: **variable**,
`= 4328 + data_size + 4` bytes. Max 78,060 B (all 144 blocks full).

All integers little-endian.

## Derived-on-parse (v3 core principle: nothing stored twice)

```
owners        ← file table runs  (home_block + n_blocks; contiguous alloc)
block sizes   ← enc_offs deltas  (next used offset; last → data_end; O(144²))
payload_size  ← block size (same value)
current/delta ← home_pos × global header scale (seeker scale f64 @24)
```

Integrity check added: Σ file n_blocks must equal header n_blocks_used
(parse returns -4 otherwise).

## Why smaller (measured)

| case | v2 | v3 | delta |
|------|-----|-----|-------|
| empty image | 5,796 B | **4,332 B** | −25.3% |
| 1 file (11 B) | 5,963 B | **4,476 B** | −25% |
| 56 files / 140 blocks | 25,956 B | **24,492 B** | −5.6% |
| overhead (full bench) | 1.29x | **1.215x** (fixed TOC 4,328 B only) | |

On real data the bigger win is the **codec unlock** (same commit):
- BITPACK signed fix (min-offset): Q4-style signed blocks {−8..7} →
  96 B vs raw 144 = **0.667x** (−33%), lossless verified
- CODEBOOK removed from classify (provably never smaller; was stealing
  quant blocks from BITPACK → always RAW). Decode of old images intact.

## Layout (v3)

```
Offset       Size     Region
────────────────────────────────────────────────────────────
0            40       ImageHeader (n_files@8 n_used@12 total@16
                      data_size@20 scale f64@24 cur@32 home@36)
40           3136     FileTable   (64 × 49 B)
3176         576      BlockMeta   (144 × 4 B)   ← home15|strategy3
3752         576      EncOffset   (144 × u32)   ← absolute offsets; 0=empty
4328         data_size  DataRegion (PACKED payloads, block order)
4328+data_size 4      CRC32 trailer (of bytes [0, data_end))
────────────────────────────────────────────────────────────
```

## Migration

v2 → v3 is a format break (like v1 → v2): no production images exist, so
images are regenerated — no converter needed. Parser rejects non-v3
magic/version. Codec's CODEBOOK stays decode-able for legacy payloads.