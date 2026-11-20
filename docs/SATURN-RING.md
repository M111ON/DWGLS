# Saturn-Ring Store — Usage & API

Owner's analogical design (Saturn + vinyl player). Fixed radii, ratio needle, two-way promote/demote.
Spec: `docs/SATURN-RING-SPEC-2026-09-17.md`. Header: `core/saturn_ring.h`.

## What it is

A fixed-capacity staging store. Ring payload slots live in **VRAM on GPU 1** (CUDA);
metadata (valid/ticks/keys) stays host-side. No bump-alloc, no holes, no compact —
eviction is replacement within a fixed ring. Capacity is set at init and never grows.

- Ring0 (hot): 1/8 of budget, ratio weight 3
- Ring1 (cold): 7/8 of budget, ratio weight 1
- Mirror window: zones 1..4 = DRAM∩VRAM mirrored (sync-back baseline); zones 5..N = VRAM carry

## Build

```sh
gcc -O2 -I. -Icore -I/i/cuda_temp/include file.c \
    -L/i/cuda_temp/lib/x64 -lcudart -lm
```

Or via Makefile: `make test-saturn_ring`.

Requires: CUDA runtime, GPU 1 with enough free VRAM (`n_slots * slot_sz`).

## Quick start

```c
#include "core/saturn_ring.h"

SaturnStore s;
if (saturn_init(&s, 8, 32, 1024, 3, 1) != 0) { /* no GPU / OOM */ }

uint8_t buf[1024];
for (int i = 0; i < 1024; i++) buf[i] = (uint8_t)(i * 13 + 7);

saturn_promote(&s, "weight.layer0", buf, 1024);   // H2D into hot ring

uint8_t *p = saturn_get(&s, "weight.layer0");      // D2H into staging
if (p) { /* p points to slot_sz bytes, valid until next get on same ring */ }

// thread-safe alternative (no shared staging):
uint8_t out[1024];
saturn_read(&s, "weight.layer0", out, 1024);

// GPU-side consumer (no copy):
uint8_t *devptr = saturn_get_devptr(&s, "weight.layer0");

saturn_demote_cold(&s, "cold.item", buf, 512);

if (saturn_verify(&s) != 0) { /* invariant broken */ }

saturn_free(&s);
```

## API

| Function | Purpose |
|---|---|
| `saturn_init(s, hot_slots, cold_slots, slot_sz, hot_w, cold_w)` | Allocate rings on GPU 1 (fallback GPU 0) |
| `saturn_free(s)` | Release VRAM + host buffers |
| `saturn_reset(s)` | Rebuild from saved config (loses data, clears `faulted`) |
| `saturn_promote(s, name, data, sz)` | Place into hot ring (demotes overflow hot→cold) |
| `saturn_demote_cold(s, name, data, sz)` | Place into cold ring |
| `saturn_get(s, name)` | Find + D2H copy into per-ring staging (single-thread) |
| `saturn_read(s, name, dst, sz)` | Thread-safe find + D2H into caller buffer |
| `saturn_get_devptr(s, name)` | Device pointer for GPU consumers (no copy) |
| `saturn_mirror_snapshot(s)` | Copy zones 1..mirror_n (hot slots) to DRAM |
| `saturn_mirror_sync_back(s)` | Restore zones 1..mirror_n from DRAM baseline |
| `saturn_carry_recover(s)` | Sync-back + drop carry zones + re-promote upward |
| `saturn_verify(s)` | Check magic + capacity invariants |
| `saturn_hash(name)` | FNV-1a name hash |

## Invariants

- `used <= cap` always (fixed radii, no leak)
- Oversize (`sz > slot_sz`) rejected, never split
- Byte-identical roundtrip through promote/get cycles
- `faulted` set on CUDA failure; call `saturn_reset` to recover

## Thread safety

`saturn_get` uses a shared staging buffer per ring — single-thread only.
Use `saturn_read` for concurrent callers.

## Limitations

- Backend is CUDA (not Vulkan); no backend vtable yet
- Volatile only (no disk persistence)
- Requires GPU 1 with enough free VRAM at init
- Mirror window zones are slot-index based (zones 1..mirror_n = hot slots 0..mirror_n-1)