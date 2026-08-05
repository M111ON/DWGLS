# Rail Hub — Zero-Copy Sync Pipeline for llama.cpp Integration
## Sub-agent Work Plan | Aug 6, 2026

---

## Architecture Goal

```
llama.cpp asks for tensor "blk.0.weight"
    │
┌───▼──────────────────────────────────────────────┐
│              GEO RAIL HUB (this plan)              │
│                                                    │
│ 1. LOOKUP:  tensor_name → coord (gen,face,slot)    │
│ 2. CLASSIFY: coord → 3-bit cell type parity        │
│ 3. SYNC:    rail sync → wait for tick 11           │
│ 4. FREEZE:  p5h_freeze_at_tick12 (barrier)         │
│ 5. BRIDGE:  jet_bridge snapshot at this exact coord │
│ 6. DELIVER: DRamTile → zero-copy to llama.cpp      │
│                                                    │
│ TOTAL: 0 compute, 6 lookups + 1 sync + 1 freeze   │
└────────────────────────────────────────────────────┘
```

## Prerequisites (ALL EXIST — NO NEW HEADER NEEDED)

| # | File | Line | What it provides |
|---|------|------|-----------------|
| 1 | `geo_tensor_hub.h` | 1-232 | Hub open/load for .gcube + GGUF index |
| 2 | `geo_cube_container.h` | 1-387 | GCube format (read/write/CRC) |
| 3 | `geo_cell_classify.h` | 1-140 | 3-bit parity → cell type |
| 4 | `fibo_spine.h` | 1-594 | 1728 pipes × 12 ticks + Jet Bridge |
| 5 | `geo_spoke_sync.h` | 1-103 | 6-lane sync + theta |
| 6 | `geo_rail_sync.h` | 1-53 | Angular sync (now same tick) |
| 7 | `gear_lock.h` | 1-33 | CPU/GPU world count sync |
| 8 | `rdh_addr.h` | 1-59 | (ring, wedge, mirror, u) → key |
| 9 | `dramtile_store.h` | 1-1001 | Zero-copy page cache (complex) |
| 10 | `dramtile_container.h` | | Container wrapper over store |

## Plan: 4 Files To Create

### FILE A: `core/geo_rail_hub.h`
**Temperature: warm** — New file, orchestrate existing components

This file WRAPs the existing hub + rail + sync components into one hot = thin = layer.

```c
// API: identical to hub_load, but now with rail sync
int geo_rail_hub_pull(GeoTensorHub *hub, const char *tensor_name,
                      uint8_t **data, uint32_t *n_elems, uint32_t *dtype);

// IMPLEMENTATION (pseudocode):
//   1. gguf_idx → find tensor offset
//   2. rdh_key: (ring, wedge, mirror, u, v) → compute position
//   3. fibo_spine_init → attach pipe
//   4. tick until at tick 11 (rail_sync_arriving) → jet_bridge entry
//   5. p5h_freeze_at_tick12 → barrier
//   6. return snapshot pointer (no copy)
```

**Imports**: geo_tensor_hub.h, fibo_spine.h, rdh_addr.h, gear_lock.h, geo_rail_sync.h, geo_spoke_sync.h, geo_cell_classify.h

**Expected size:** ~120 lines

### FILE B: `core/geo_rail_hub_test.c`
**Temperature: hot** — Must pass
Tests:
1. Init fibo_spine + hub → open → pull one tensor via rail → data non-null
2. Pull same tensor → data identical (no corruption)
3. Pull wrong tick (at bridge): reject
4. Compare raw HPC data to hub data
5. Pull after freeze → any corrupt pipe → data zero
6. Batch: 3 tensor pulls through rail

**Expected:** 7 tests, all pass

### FILE C: `core/geo_snapshot_pin.h`
**Purpose:** Pins snapshot position after witch + jet bridge freeze

```c
// After rail_hub_pull finishes, call this to hold access
typedef struct {
    uint16_t pipe_id;
    uint8_t  tick;
    uint32_t residual_off;   // from ribcage freeze
    uint8_t  frozen;
    uint64_t coord;           // min(gen,face,slot)
} GeoSnapshot;

// O(1) pin — just records fields
int geo_snapshot_pin(GeoSnapshot *sn, uint16_t pipe_id, uint8_t tick, uint64_t coord);

// O(1) verify — check coord hasn't shifted
int geo_snapshot_verify(GeoSnapshot *sn);
```

**Imports needed:** fibo_spine.h, geo_cell_classify.h

**Expected size:** ~40 lines

### FILE D: `tests/test_rail_hub_pipeline.c`
**Full integration test:**
1. Build GCube from GGUF (like T0 in hub test)
2. Init hub + init file spine + init ribcage
3. Pull 3 tensors via rail → verify each matches original raw data
4. Measure: verify tick counts, PIPE_BRIDGED pipe count
5. Verify 35%+ of pipes are bridged (normal for Q8_0)

No compute — only timing and sync-lookup would take time.

**Compile flags:** 
```
gcc -O2 -Wall -Icore -I I:/FGLS_new/runner -I I:/FGLS_new/beam_addressing
  -o build/test_rail_pipe.exe tests/test_rail_pipe.c -lm
```

### File D: `docs/RAIL_HUB_PLAN.md` (this file)
Already exists.

---

## Work Package Schedule (parallel-capable)

### Sub-agent A → task: `FILE C (snapshot) + FILE A (hub wrapper)`
**Constraint:** C must run first (smaller), then A imports C
**ETA:** 30 min

### Sub-agent B → task: `FILE B (tests) + FILE E (integration test)`  
**Constraint:** Must run AFTER A and C file complete
**ETA:** 30 min

### Sub-agent C → task: `DRamTile: lighter version`
**Constraint:** dramtile_store is 1,001 lines (explicit, map storage);
we need a super-light version for zero-copy push to llama.cpp
**ETA:** Optional

---

## API reference (not needed but here for quick lookup)

```c
// geo_cell_classify.h
GeoCellClassify gf = {0};
uint8_t ct = geo_cell_classify(&gf, gen, face, slot);
cell_type_name(ct); // "III", "IID", "IDI", "IDD", "DII", "DID", "DDI", "DDD"

// fibo_spine.h
fibo_spine_init(&fs);
uint16_t pipe_id = coord % FS_PIPES;  // 0..1727
uint8_t  tick    = coord % FS_TICKS_PER_CYCLE;  // 0..11
while (fs.pipes[pipe_id].local_tick != FS_JET_BRIDGE_TICK) {
    fibo_spine_tick(&fs, pipe_id);
}
p5h_ribcage_init(&ribcage, &fs);
jet_bridge_hop(&fs, pipe_id, NULL, 0, NULL);  // skip copy, just bridge
p5h_freeze_at_tick12(&ribcage);
// after barrier: all coordinate positions are fixed
```

---

## Build Checklist (send to L2 get/set task dispatch)

| Step | File | Owner | Status |
|------|------|-------|--------|
| 1 | `core/geo_rail_hub.h` | Agent A | ⏸ |
| 2 | `core/geo_snapshot/geo_snapshot_pin.h` | Agent A | ⏸ |
| 3 | `tests/test_rail_hub_pull.c` | Agent B | ⏸ |
| 4 | `tests/test_rail_pipeline.c` | Agent B | ⏸ |
| 5 | `Makefile` update: tier-2 entries | Either | ⏸ |
| 6 | Run `make test` after all | Either | ⏸ |

---

## Success Criteria

1. **All necessary header files compile, clean** — Wall Wextra, no warnings (gg_sorrow wrr in header is fine)
2. **7/7 tests pass** in test_rail_hub_pull
3. **All 5 tensors pull correctly** verify in test_rail_pipeline
4. **Code adds zero latency** (only sync + offset, only sync ops)
5. **Zero compute** — send by calling clock_wait + fences only

---

## CRITICAL: Distribution Constraints

- ALL files accessible by both FGLS_new and DWGLS
- shared header: `gguf_index.h` from FGLS leads
- `dramtile_store.h + dramtile_container.h` already in infra/
- `geo` prefixed files from DWGLS core
- No modification to existing files (just imports)
- Can be tested via `make test` after Done

---

## CODE: Proposed `geo_cell_addressing.h` tuple structure

Will be added as separate header for clean addressing:

```c
// geo_cell_addressing.h — Convert tensor name → (ring, face, slot) address
#include "geo_cube_container.h"   // for GCube find
#include "geo_cell_classify.h"   // for classify
#include "fibo_spine.h"          // for pipe/tick mapping
#include "gear_lock.h"           // for CPU/GPU lock

// self-contained, no allocations, no floats, O(1)
static inline uint64_t geo_cell_addr_from_offset(
    uint32_t tensor_offset,   // offset in GCube block list
    uint32_t n_elems,          // total elements in tensor
    uint32_t *gen, uint32_t *face, uint32_t *slot
) {
    // Basic mapping: linear → 3D cell address
    uint64_t cell = tensor_offset / GEAR_GEO_FULL;  // 128x162 block
    *gen  = cell & 7;          // 3 lower bits (0..7)
    *face = (cell >> 3) & 7;   // next 3 bits (0..7)
    *slot = (cell >> 6) & 15;  // upper 2 bits (0..15) ← simplified
    return cell;
}
```

---

## Two Sub-Agent Assignment

### Agent A: `geo_rail_hub.h + geo_cell_addr.h`

**Output:** Two headers
- `core/geo_rail_hub.h` — synchronous rail hub
- `core/geo_cell_addr.h` — coordinate mapping

**Testing:**
Command: `gcc -O2 -Wall -Icore -std=c11 -o null tests/null_rail_compile.c -lm`

### Agent B: `test_rail_hub.c + test_pipeline.c`

**Dependency:** A finished first
**Output:** Two test files
**Expected:**

1. `build/test_rail_hub_static.exe` — 17 rapid tolerance test
2. `build/test_rail_pipeline.exe` — full deep pipe compile then run

---

## Timing

| Phase | Effort | Est time | Status |
|-------|--------|----------|--------|
| A: headers | 30 min | ● | pending |
| B: test | 30 min | ● | pending |
| C: cleanup & verify | 10 min | ● | pending |
| Total | ~1 hr | ● | pending |

---

## Immediate next action

1. Copy this plan to board as #19
2. Invoke sub-agent A and B simultaneously
3. Monitor compilation and test output

---

**LAST UPDATED:** Aug 6, 2026
**OWNER:** Self — plan ready to dispatch