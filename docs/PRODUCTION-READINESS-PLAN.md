---
luminaCreated: 2026-08-16T06:55:02.048Z
tags: []
luminaModified: 2026-08-16T06:55:02.048Z
luminaVersion: 1.3.11
---
# DWGLS Production Readiness Plan

**Status**: Working prototype → Target: Production geometric filesystem + inference pipeline  
**Baseline**: `make test` 25/25 PASS, CLI demo LOSSLESS, ratio 0.82  
**Architecture**: GPU = PULL through DRamTile unified memory chain (not compute)

---

## 🎯 Production Definition

> **Production =** GGUF model loads from `.gcube` via `rail_hub` puller → llama.cpp tensor hook → zero-copy GPU bandwidth path, with **Breathing FS** as the storage layer providing:
> - Memory-mapped access (no full-file load)
> - **RDH bijection verify** (read at coordinate → bitwise match = integrity)
> - **Seeker MVCC** (seeker position = version, scale = time)
> - Measured throughput > baseline (sqlite/LMDB/raw mmap)
> - Concurrent read support

**Core Insight**: Geometric FS replaces traditional requirements:
| Traditional | Geometric (Breathing + RDH) |
|-------------|-----------------------------|
| WAL logs intent before write | **Write = place at coordinate** — atomic by geometry (single 144-byte slot) |
| CRC verifies bits | **RDH bijection verify** — read at coordinate → bitwise match = valid |
| fsck repairs corruption | **bfs_go_home() = lossless** — all deltas = 0 at home position |
| MVCC | **Seeker position = version** (scale = time) |

---

## ✅ Phase 1 Progress (Aug 10, 2026)

**Completed — Breathing FS Hardening (Storage Layer):**

| Item | Status | Artifact |
|------|--------|----------|
| **Versioned .img header** | ✅ DONE | `core/bfs_persist.h` — **v2 packed layout** (`BFS_IMG_MIN_SIZE` 5,796 / `MAX` 79,524 B), magic `BIMG`, CRC32 trailer |
| **mmap path** | ✅ DONE | `bfs_mmap_open/close` — Windows `MapViewOfFile` + Linux `mmap`, zero-copy `bfs_mmap_read` decodes straight from mapping |
| **Plain save/load** | ✅ DONE | `bfs_save_img` / `bfs_load_img` — portable, CRC-checked |
| **RDH bijection verify** | ✅ DONE | `bfs_rdh_verify_all` — encode(decode(x))==x per block, 140/140 blocks verified |
| **Seeker MVCC** | ✅ DONE | `bfs_mvcc_snapshot/restore` — 8-slot ring, position = version, scale = time |
| **Bug found+fixed** | ✅ DONE | `bfs_go_home()` never returned seeker to home_pos — lossless invariant broken; fixed in `breathing_fs.h` |
| **CLI upgrade** | ✅ DONE | `tools/breathing_fs_cli.c` — old ad-hoc binary I/O replaced by versioned image; new `mmap` + `verify` commands |
| **Benchmark** | ✅ DONE | `bench/fs_bench.c` — 6 paths, all lossless-verified (LMDB deferred: zero-external-deps policy) |
| **Test** | ✅ DONE | `tests/test_bfs_persist.c` — **65/65 PASS** (v2), registered in Makefile TIER1 |

**Benchmark numbers (56 files, avg 2.5 blocks/file):**
```
B1 write           13.86 MB/s      B4 image save   16.22 MB/s
B2 read (mem)      18.98 MB/s      B4 image load   21.49 MB/s
B3 read (mmap)     18.65 MB/s      B5 rdh verify   2.37 ms (140 blocks)
B6 scale cycle     0.04 ms         image / payload 1.215x overhead (v3 derived; v1 was 4.05x)
```

**Next (Phase 1 remainder):**
- ✅ `docs/FS_FORMAT_SPEC.md` — format documentation (DONE Aug 10)
- ✅ `bfs_mmap_write` — write-through mapping (`bfs_mmap_sync`, in-place, no re-serialize; CLI `write-map`)
- Python validate (optional — deferred)

---

## ✅ Phase 2 Progress (Aug 10, 2026)

**Completed — Core Geometric Pipeline Bridge (connect existing pieces):**

| Item | Status | Artifact |
|------|--------|----------|
| **Survey existing assets** | ✅ DONE | `fibo_spine.h`, `gear_lock.h`, `dramtile_store.h`, `geo_rail_hub.h`, `geo_cell_addr.h` all already in repo — Phase 2 = integration, not new geometry |
| **BFSHub bridge** | ✅ DONE | `core/geo_bfs_hub.h` — BMP image ↔ GeoPipeline connector: block → cell_addr → (pipe,tick) → spine ceremony → jet_bridge_hop → gear_cpu_tick → zero-copy pointer into mapping |
| **Zero-copy pull** | ✅ DONE | `bfs_hub_pull()` returns pointer INTO mmap (no malloc, no copy) — verified all pulls land in mapping |
| **Full-file decode** | ✅ DONE | `bfs_hub_pull_file()` — lossless via dyn_decode per block |
| **Batch pull** | ✅ DONE | `bfs_hub_pull_batch()` |
| **Test** | ✅ DONE | `tests/test_geo_bfs_hub.c` — 56/56 PASS, registered Makefile TIER1 |
| **Benchmark** | ✅ DONE | `bench/rail_bench.c` — **7.61 ns/pull, 131 M pulls/s**, zero-copy + lossless confirmed |

**Benchmark numbers (24 files, 60 blocks, 200 repeats):**
```
R1 pull latency      7.61 ns/pull     (address + ceremony + pointer return)
R2 pull throughput   131,434 K pulls/s
R3 zero-copy in-map  YES (12,000 pulls)
R4 full-decode lossless YES
bridges fired        12,060    gear cpu ops 12,060 (worlds=94)
```

**What was reused (zero new geometry):**
- `geo_cell_addr.h` — flat id → (pipe, tick) O(1) arithmetic
- `fibo_spine.h` — 1728×12 ceremony + Jet Bridge (tick 11)
- `gear_lock.h` — CPU sync (128×162 = 20736 = GEO_FULL)
- `bfs_persist.h` — mmap zero-copy (Phase 1 output)

**Bridges → makes test:** `make test` 27/27 (TIER1 24 + TIER2 3)

**Next (Phase 2 remainder):**
- ⏳ Rail Hub puller thread + CUDA kernel (Phase 2.5) — needs GPU hardware
- ⏳ GearLock GPU mirror (Gear2 pinned memory) — separate repo (needs ggml)

---

## ✅ Anchor-Based Delta Seeker (Aug 10, 2026) — user insight implemented

**User insight:** "hyperbolic delta มี pattern ชัดเจนคาดเดาได้ — ใช้ frame seek เก็บแค่ anchor ก็เพียงพอ"

**ความจริงที่พิสูจน์แล้ว:** `delta = home_pos × (scale − 1)` — เป็น **pure function** ไม่ใช่ข้อมูล

### ✅ v2: ทำให้ไฟล์หดได้จริง (implemented)

| v1 (was) | v2 (now) | ผล |
|----------|----------|-----|
| DeltaLog 1024 B | ✂️ removed (derived) | −1024 B |
| BlockMeta 16 B (เก็บ current+delta) | 8 B (anchor only) | −1152 B |
| Data 512 B × 144 = 73728 B fixed | **packed** Σ payload | variable |

**ผลวัดจริง (v3, consensus round 1):**
- ไฟล์ว่าง: 81,700 B (v1) → **4,332 B** (v3; v2 = 5,796)
- 1 file 11 B: **4,476 B** · 56 files/140 blocks: **24,492 B** (v2 = 25,956)
- Overhead ratio: 4.05x (v1) → 1.29x (v2) → **1.215x** (v3) — fixed TOC 4,328 B
  คือ overhead เดียว (owners/e_sizes/header-redundancy ถูก derive หมด)
- Codec unlock (commit เดียวกัน): BITPACK signed fix → Q4 signed block 96B/144B
  = **0.667x lossless**; CODEBOOK ตัดออกจาก classify (พิสูจน์ว่าแพ้เสมอ)

**Derived on parse:** `current_pos = home×scale`, `delta = current−home` —
เก็บแค่ anchor (8B/block) ตาม insight

**Files:** `core/bfs_persist.h` v2, `docs/FS_FORMAT_SPEC.md` v2
**Tests:** persist 65/65 + hub 56/56 + anchor 51/51 → `make test` 28/28, zero warnings
**v1→v2:** breaking change (ไม่มี production images — ไม่ต้อง migrate)

---

## ✅ Continuous Auto-Compress Delta Engine (Aug 10, 2026) — user vision

**User insight:** "ระบบเคลื่อนไปมาอยู่ตลอด — หาจุดยึดเป็น constrain มา scope ขนาด
delta คล้ายที่ seeker ทำ — compress delta เกิดขึ้นเองได้ตลอด ขนานกับอีกฝั่ง"

**กลไก (implemented):** `core/bfs_breath.h`
- ระบบหายใจตลอด (scale oscillate) ทุก breath ทุก block ได้ delta = home×(scale−1)
- **KEY: anchor เป็น constraint เคลื่อนที่** — พอ |Δ| เกิน `BFS_BREATH_BOUND=127`
  → re-anchor (anchor ตามไปที่ตำแหน่งปัจจุบัน, Δ กลับเป็น 0) → **bounded ถาวร**
- bounded Δ → encode int8 (4x เล็กกว่า int32 v1)
- เกิดอัตโนมัติทุก tick **เป็น side channel ขนานกับ main path** (payload ไม่ถูกแตะ)

**ผลพิสูจน์ (test 22/22, 5,000 breaths):**
- ทุก breath |stored Δ| ≤ 127 — bounded by construction
- re-anchor 26 ครั้งใน long-run — anchor ตามการเคลื่อนที่จริง
- main path อ่าน lossless ตลอดขณะหายใจขนาน (500 interleaved reads)
- delta layer: 4 B (int8) vs 16 B (int32) = **4x smaller**
- encode/decode int8 exact + clamp

**เส้นเชื่อมกับ seeker:** window = K/scale scope ADDRESS SPACE; anchor scope DELTA —
constraint → scope → bounded → compact (ตระกูลเดียวกัน)

`make test` 29/29 (TIER1 26 + TIER2 3) · zero warnings

---

### 1. Breathing FS — Storage Layer (Priority: CRITICAL)

| Feature | Current | Needed | Effort |
|---------|---------|--------|--------|
| **Memory mapping** | `calloc` + `fread` whole file | `mmap` + page-aligned blocks (Windows `MapViewOfFile` / Linux `mmap`) | 3-5 days |
| **RDH bijection verify** | None | Per-block RDH read-verify on access | 2 days |
| **Seeker MVCC** | Single-threaded | Seeker position = version, scale = time | 2-3 days |
| **File format spec** | Ad-hoc binary | Versioned header + TOC + block index | 1 day |
| **fsck / salvage** | None | Offline repair tool (RDH-based) | 2 days |
| **Benchmarks** | None | vs sqlite/LMDB/mmap (read/seq/rand) | 2 days |

### 2. Core Geometric Pipeline — Memory + Sync + Pipeline + Transport (Priority: CRITICAL)

| Component | File | Current | Needed | Effort |
|-----------|------|---------|--------|--------|
| **DRamTile** | `geofs_core.h` (stub) | Stub only | Unified GPU-CPU memory pool, page-pinned, mmap-backed | 5 days |
| **GearLock** | `gear_lock.h` | Constants only | Spinlock-free ring buffer, stride-37 on 20736 | 3 days |
| **Fibo Spine** | `fibo_spine.h` | Complete header | Integration: 1728 pipes × 12 ticks, Jet Bridge at tick 11 | 2 days |
| **Jet Bridge** | `fibo_spine.h` | Complete header | Zero-copy CPU↔GPU transport via residual_space | 3 days |
| **Rail Hub Puller** | `geo_rail_hub.h` | Not started | Background thread + CUDA kernel `rail_hub_pull()` | 5 days |

**Dependencies**: Breathing FS mmap → DRamTile → GearLock → Fibo Spine → Jet Bridge → Rail Hub Puller

### 3. GGUF Bake Pipeline (Priority: HIGH)

| Feature | Current | Needed | Effort |
|---------|---------|--------|--------|
| **Tensor → tesseract chunks** | Dynamic codec only | Per-tensor chunking + addressing (WHERE = tesseract, HOW = dynamic codec) | 3 days |
| **Weight classification** | 5-strategy auto | Profile per tensor type (attn/ffn/emb) | 2 days |
| **Manifest + index** | None | `.gcube` format with tensor directory | 2 days |
| **bake CLI** | Not started | `gguf_bake model.gguf model.gcube` | 2 days |

### 4. llama.cpp Integration (Priority: HIGH)

| Feature | Current | Needed | Effort |
|---------|---------|--------|--------|
| **Tensor load hook** | None | Override `ggml_backend_get_data` | 2 days |
| **Async prefetch** | None | Rail hub pull → GPU before compute | 3 days |
| **Fallback path** | None | CPU path if rail hub unavailable | 1 day |
| **Zero-copy verify** | None | End-to-end bitwise match test | 2 days |

### 5. Observability & Operations (Priority: MEDIUM)

| Feature | Current | Needed | Effort |
|---------|---------|--------|--------|
| **Metrics** | None | Prometheus `/stats` endpoint | 2 days |
| **Logging** | printf | Structured logs (JSON) + levels | 1 day |
| **Health checks** | None | `/health` for k8s/liveness | 1 day |
| **Config** | Hardcoded | YAML/TOML for all tunables | 1 day |

---

## 🗓️ Phased Plan (Revised)

### Phase 1: Breathing FS Hardening — Storage Layer (2-3 weeks)
**Goal**: Production-grade geometric storage layer with mmap + RDH + seeker MVCC

```
Week 1: mmap + RDH bijection verify + versioned header
Week 2: Seeker MVCC (scale = time) + fsck (RDH-based)
Week 3: Benchmarks vs LMDB/sqlite + docs + integration test
```

**Deliverable**: `breathing_fs.h` with mmap, RDH verify, seeker MVCC; `bench/fs_bench.c`

### Phase 2: Core Geometric Pipeline — Memory + Sync + Pipeline + Transport (5 weeks)
**Goal**: GPU bandwidth puller working through full geometric chain

```
Week 1: DRamTile allocator (page-pinned, GPU-accessible, mmap-backed from Breathing FS)
Week 2: GearLock ring (stride-37 on 20736, lock-free)
Week 3: Fibo Spine integration (1728 pipes × 12 ticks, Jet Bridge at tick 11)
Week 4: Jet Bridge transport (residual_space, zero-copy CPU↔GPU)
Week 5: Rail Hub Puller thread + CUDA kernel + synthetic tensor bench
```

**Deliverable**: `rail_hub.c/h` + `dram_tile.h` + `rail_hub_pull()` saturating PCIe 4.0 x16 (32 GB/s)

### Phase 3: Bake Pipeline (2 weeks)
**Goal**: GGUF → .gcube end-to-end

```
Week 1: Tensor chunking (tesseract addressing) + manifest + bake CLI
Week 2: Profile strategies per tensor type + verify bitwise match
```

**Deliverable**: `gguf_bake` CLI, `.gcube` format spec

### Phase 4: llama.cpp Integration (2 weeks)
**Goal**: Real model inference via rail_hub

```
Week 1: Tensor hook + async prefetch + fallback
Week 2: Benchmark vs baseline (llama.cpp CPU/CUDA)
```

**Deliverable**: Patched llama.cpp + benchmark results

### Phase 5: Observability + Release (1 week)
**Goal**: Operable in production

```
Metrics, logging, health, config, release automation
```

---

## 📊 Success Criteria (Exit Gates)

| Phase | Must Pass |
|-------|-----------|
| **1** | `make test` 25/25 + `fs_bench` > LMDB read throughput + fsck (RDH-based) recovers from kill -9 |
| **2** | `rail_hub_pull` saturates PCIe 4.0 x16 (32 GB/s) on synthetic tensor via full chain (Breathing FS → DRamTile → GearLock → Fibo Spine → Jet Bridge → GPU) |
| **3** | `gguf_bake` → `.gcube` → `rail_hub_pull` → bitwise match original GGUF tensors |
| **4** | llama.cpp generates identical tokens to baseline, latency ≤ baseline |
| **5** | All metrics exposed, config-driven, CI/CD green |

---

## 🔗 Dependencies & Risks

| Risk | Mitigation |
|------|------------|
| CUDA driver / WSL2 GPU passthrough | Test on native Linux + WSL2 early |
| llama.cpp API changes | Pin version, maintain fork |
| Breathing seeker math edge cases | Property-based tests (rapidcheck) |
| mmap on Windows | Platform-specific `MapViewOfFile`, test early |
| No Docker (Windows constraint) | Native Windows build, WSL2 for Linux testing |

---

## 💰 Resource Estimate

| Role | Weeks |
|------|-------|
| Systems engineer (C/CUDA) | 12-14 |
| Build/CI engineer | 2 (part-time) |
| **Total calendar** | **12-14 weeks** |

---

## 📁 Files to Create (Phase 1 Start)

```
core/
├── breathing_fs.h        ← extend: mmap, RDH verify, seeker MVCC, versioned header
├── dram_tile.h           ← new: unified GPU-CPU memory pool (page-pinned, mmap-backed)
├── rail_hub.h            ← new: GearLock, Fibo Spine, Jet Bridge, puller API
├── gguf_bake.h           ← new: tensor chunking, manifest, .gcube format
tools/
├── breathing_fs_cli.c    ← add: fsck, bench, mvcc-demo
├── gguf_bake.c           ← new: bake CLI
├── rail_hub_test.c       ← new: synthetic tensor pull benchmark
bench/
├── fs_bench.c            ← new: vs LMDB/sqlite/mmap
├── rail_bench.c          ← new: PCIe saturation test
tests/
├── test_breathing_fs_mmap.c
├── test_breathing_fs_mvcc.c
├── test_rail_hub.c
docs/
├── FS_FORMAT_SPEC.md
├── DRAM_TILE_API.md
├── RAIL_HUB_API.md
├── GGUF_BAKE_SPEC.md
```

---

## ✅ Next Action (Immediate)

**Start Phase 1 Week 1:**
1. Add `mmap` path to `breathing_fs.h` (Windows: `MapViewOfFile`, Linux: `mmap`)
2. Add per-block **RDH bijection verify** (read at coordinate → bitwise match)
3. Add **Seeker MVCC**: seeker position = version, `scale` = logical time
4. Version the `.img` header (magic, version, block_size, block_count, TOC offset, seeker state)
5. Write `bench/fs_bench.c` comparing to LMDB

---

## 🔑 Geometric Constants (Sacred)

| Constant | Value | Source | Meaning |
|----------|-------|--------|---------|
| `GEAR_GEO_FULL` | 20736 | `gear_lock.h` | Full gear cycle = 128 × 162 |
| `FS_PIPES` | 1728 | `fibo_spine.h` | 12 × 144 pipes |
| `FS_TICKS_PER_CYCLE` | 12 | `fibo_spine.h` | Ticks 0..11 |
| `FS_SLOTS` | 20736 | `fibo_spine.h` | = GEAR_GEO_FULL |
| `FS_JET_BRIDGE_TICK` | 11 | `fibo_spine.h` | Exit tick |
| `FS_REENTRY_TICK` | 13 | `fibo_spine.h` | Re-entry tick (mod 12 = 1) |
| `FS_TICK_12` | 12 | `fibo_spine.h` | Barrier boundary (skipped) |
| `RC_N_CYCLES` | 144 | `fibo_spine.h` | Fibo completeness cycles |
| `K_BREATHING` | 5184 | `breathing_fs.h` | 20736 / 4 = window constant |
| `HYP_AXIS_SLOTS` | 6912 | `hyperbolic_seek.h` | 20736 / 3 axes |

---

*Updated with full geometric architecture integration. All layers map to single 20736 address space.*