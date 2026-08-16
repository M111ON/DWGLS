---
luminaCreated: 2026-08-16T06:55:02.090Z
tags: []
luminaModified: 2026-08-16T06:55:02.090Z
luminaVersion: 1.3.11
---
# REPORT — GeoFS MDIM Multi-Frame Runs
**2026-08-12 · branch `feat/geo-native-fs` · worktree `I:/DWGLS-native-fs`**

## สรุป (Summary)

GeoFS MDIM เปิดให้ไฟล์ขนาดใหญ่กว่า journal frame เดียวถูกเก็บเป็น **chain ของ runs**
(commit 1 run ต่อ 1 frame) — กำจัด single-frame cap เดิม (3843 B) ออกไป
**cap ใหม่ = 1 MiB** (volume-bounded) และ rewrite ขนาดไหนก็ได้ (grow/shrink/empty)
แบบ crash-atomic โดยไม่ต้องใช้ flag ใดๆ

- **Tests:** `test_geo_fs_mdim` **16/16 PASS** · tier-1 suite **28/28 PASS** (ไม่ regression)
- **CLI:** 100 KB file lossless รอบ 28 frames / 28 runs (create → summon → list → get → unsummon)

---

## 1. ปัญหาเดิม (Before)

| Cap | ค่า | ที่มา |
|-----|-----|------|
| single-frame | **3843 B** = 61 data slots × 63 B | `MDIM_MAX_CHANGES(62) − 1` slots ต่อ frame |
| 1 MiB? | — | ไม่มี — ไฟล์ใหญ่กว่า 3843 B summon ไม่ได้ (`MDIM_ERR_SIZE`) |

- หนึ่ง op = หนึ่ง journal frame = ≤ 62 changes → ไฟล์สูงสุด 61 data slots
- Rewrite ต้อง same-or-smaller ภายใน run-span เดิม (`n_runs` ต้องเท่ากัน) → grow/span-change ไม่ได้

## 2. การออกแบบใหม่ (After)

### 2.1 Run-span chain (coordinate = address)
```
File = [LINK][DATA...] → [LINK][DATA...] → ... → [LINK][DATA...]
```
- แต่ละ **run** = contiguous first-fit span ≤ 60 data slots (`MDIM_RUN_CHUNK`)
- **LINK slot** (`MDIM_T_LINK`): `{size = bytes ของ run นี้, prev = LINK ของ run ถัดไป (0 = สุดท้าย)}`
  → chain คือ extent เอง — ไม่มี run table, ไม่มี lookup
- **FILE entry**: `prev` → LINK แรก + run-span header ใหม่ `n_runs` / `n_data_slots`
  (ใน 64 B slot เดิม — `sizeof(MdimSlot)` ยัง = 64)
- read path เดียวกันสำหรับทั้ง single-run และ multi-run (`mdim_file_load` walk chain)

### 2.2 Chunked commits (1 run = 1 frame)
```
summon:  alloc ทั้ง chain ล่วงหน้า (extent reservation)
         → write ทีละ run (capture → frame-write → mutate → commit)
         → entry เขียนใน frame สุดท้าย
```
- 100 KB = 28 runs = 28 frames — lossless 100%
- **Entry-last** → crash กลาง chain ไม่มีไฟล์โผล่ครึ่งๆ (orphan runs ถูก sweep ตอน open)

### 2.3 Rewrite แบบ crash-atomic (ไม่มี flag)
```
write:   1. alloc NEW chain (reserve อย่างเดียว — ยังไม่ commit อะไร)
         2. write runs ใหม่ — entry ไม่แตะ → ไฟล์ยังอ่าน OLD chain ได้ตลอด
         3. frame สุดท้าย: สลับ entry → NEW chain (atomic 1 frame)
         4. free OLD chain (1 frame/run) — crash ตรงนี้แค่ leak (sweep เก็บ)
```
- Crash ณ จุดไหน → ไฟล์เป็น **old ทั้งหมด หรือ new ทั้งหมด** — ไม่มี torn
- grow / shrink / empty ได้ทุกขนาด ≤ 1 MiB (grow ต้องมีที่ว่างให้ทั้ง 2 chain พร้อมกัน)
- ไม่ต้องมี `MDIM_F_DIRTY` / `MDIM_ERR_DIRTY` อีกต่อไป (ออกแบบใหม่แล้วไม่ต้องใช้)

### 2.4 Unsummon
- TOMB entry ก่อน (ไฟล์หายทันที) → free ทีละ run → crash กลางทางแค่ leak

### 2.5 Derived bitmap + self-healing sweep
- bitmap = **derive จาก entry chains** (ไม่ใช่จาก slot types)
- rebuild: pass 1 mark entry + chain ที่ reachable → pass 2 sweep orphans เป็น **TOMB**
- crash กลาง multi-frame op → orphan blocks ถูกกวาดตอน open ครั้งถัดไป

### 2.6 กฎสำคัญที่เจอจากการเทส (Bug จริง)
**Freed slot ต้องเป็น TOMB ไม่ใช่ EMPTY** — EMPTY slot จะ *terminate* stride-37
probe walk → zeroing block ที่ free อาจทำให้ entry ที่อยู่ถัดไปใน chain หายจาก lookup
(เจอจาก CLI demo: ไฟล์หายจาก `list` หลัง unsummon) — แก้ครบ 4 จุด (unsummon,
write reclaim, sweep pass-2, summon NOSPC rollback)

## 3. Constants ใหม่

```c
#define MDIM_T_LINK         7u          /* run-link slot */
#define MDIM_F_CHAIN        0x01u       /* multi-run file */
#define MDIM_RUN_CHUNK      60u         /* max data slots / run */
#define MDIM_RUN_BYTES      (60 * 63)    /* 3780 B / run */
#define MDIM_MAX_FILE_BYTES (1024u*1024u) /* 1 MiB hard cap */
#define MDIM_CAP_ONE_FRAME  (60 * 63)    /* 3780 B single-frame */
#define MDIM_MAX_RUNS       (cap/RUN_BYTES + 2)  /* chain walk guard */
```
- Super version: 1 → **2** (load/mmap reject `!= 2` → `MDIM_ERR_CORRUPT`)
- เอกสารแก้ตัวเลขเดิมที่เคยประมาณผิด ("~56 KB" → จริง 3843 B → ใหม่ 3780 B single-frame / 1 MiB cap)

## 4. Test Results

`make test-test_geo_fs_mdim` → **16/16 PASS**:

| # | Test | ครอบคลุม |
|---|------|---------|
| 1–2 | init / views | super + regions, flat↔coords roundtrip 20736 |
| 3–6 | summon/read, views, name bonding, unsummon | probe chains, tombstones |
| 7 | rewrite | shrink + **grow** (ใหม่), verify |
| 8 | timeline versions | `read_at(frame)` ผ่าน write 2-frame |
| 9–10 | crash recovery | uncommitted frame rollback, corrupt = fail-loud |
| 11 | save → load | journal + chain รอด |
| 12 | ring wrap | eviction, newest readable |
| 13 | mmap | page-cache volume |
| 14 | **multi-frame run** | 100 KB summon, run-span header, lossless, chain integrity, accounting |
| 15 | **arbitrary-size rewrite** | shrink 28→14 runs, grow 14→28, empty, versioned read, unsummon, blocks freed |
| 16 | **orphan sweep** | derived bitmap frees unreachable chain, space reusable |

`make tier1` → **28/28 PASS** (ไม่ regression — มีแค่ `test_geo_fs_mdim.c` + `mdim_cli.c` ที่ include header นี้)

## 5. CLI Verification

```bash
make mdim_cli
./build/mdim_cli create vol.bin          # 20736 slots × 64B = 1327104 B
./build/mdim_cli summon vol.bin big.bin bigfile.bin   # → 102400 B, 28 runs, frame 28
./build/mdim_cli list vol.bin            # runs column: big.bin 28 runs
./build/mdim_cli get vol.bin big.bin out.bin          # lossless (cmp ✓)
./build/mdim_cli unsummon vol.bin notes.txt           # TOMB entry → free chain
./build/mdim_cli mmap vol.bin big.bin bigfile.bin     # zero-copy path, lossless
./build/mdim_cli history vol.bin big.bin              # timeline versions
```
ผลลัพธ์: 100 KB roundtrip lossless ทั้ง normal และ mmap path

## 6. Crash model สรุป

| จุด crash | ผลลัพธ์ |
|-----------|--------|
| summon กลาง chain (run บางส่วน commit แล้ว) | ไม่มีไฟล์ (entry ยังไม่เขียน) → orphan runs swept บน open |
| rewrite ก่อน frame สลับ entry | ไฟล์ = old content เต็ม (entry ยังชี้ old chain) |
| rewrite หลัง frame สลับ entry | ไฟล์ = new content เต็ม |
| rewrite ตอน free old chain | ไฟล์ = new content เต็ม; remnant swept |
| unsummon หลัง TOMB entry | ไฟล์หาย; remnant swept |
| uncommitted frame (crash ระหว่าง write/stamp) | recover undo → กลับไปก่อนหน้า |

## 7. Limits (v2, documented)

- 1 MiB hard cap (`MDIM_MAX_FILE_BYTES`), volume-bounded (~1.27 MB usable)
- Rewrite ต้องมีที่ว่างให้ old + new chain พร้อมกัน (grow ใกล้เต็ม → `MDIM_ERR_NOSPC`)
- Journal ring = 128 slots → multi-frame op evict frames ตัวเอง; history depth = 1 ring
- ยังไม่มี directory tree / symlinks / permissions

## 8. ไฟล์ที่แก้ (uncommitted)

```
core/geofs_mdim.h            — run-span chains, chunked ops, derived-bitmap sweep, super v2
tests/test_geo_fs_mdim.c     — 16 tests (3 ใหม่: 14/15/16) + 13 เดิมปรับ contract
tools/mdim_cli.c             — runs column, summon cap message
docs/MDIM_NATIVE_FS.md       — living spec (v2)
docs/HANDOFF-2026-08-12-mdim-multiframe.md — session handoff
docs/REPORT-MDIM-MULTIFRAME-2026-08-12.md  — ฉบับนี้
```

## 9. Next steps

1. **Mount layer** — WinFSP/Dokany shim ให้ `CreateFile` เปิด volume ได้
2. **Full spine** — scale จาก 20736 ไป 1728-pipe spine (FS_PIPES × FS_TICKS)
3. **Path tree** — directory slots (entry array แบบ inode direct-block)
4. **Chain defrag** — merge runs ที่อยู่ติดกัน / shrink peak (old+new พร้อมกัน)
