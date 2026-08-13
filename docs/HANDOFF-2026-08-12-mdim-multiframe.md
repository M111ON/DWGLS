# DWGLS Handoff — 2026-08-12 (GeoFS MDIM multi-frame runs)

## สถานะปัจจุบัน

- **Branch/worktree:** `feat/geo-native-fs` @ `I:/DWGLS-native-fs`
- **Tests:** `test_geo_fs_mdim` = **16/16 PASS** · tier-1 suite = **28/28 PASS**
- **CLI:** `build/mdim_cli` — 100 KB file lossless รอบ 28 frames / 28 runs (normal + mmap path)
- **Report:** `docs/REPORT-MDIM-MULTIFRAME-2026-08-12.md`
- ยังไม่ commit — ตาม workflow เดิม

## ⭐ สิ่งที่ทำ (session นี้)

### 1. Multi-frame runs — ไฟล์เกิน 1 journal frame
```
File = chain ของ runs:  [LINK][DATA...] → [LINK][DATA...] → ... → [LINK][DATA...]
```
- แต่ละ run = contiguous first-fit span (≤ 60 data slots)
- LINK slot เก็บ `{size = bytes ของ run นี้, prev = LINK ของ run ถัดไป (0 = สุดท้าย)}`
- FILE entry ชี้ไปที่ LINK แรก + run-span header `n_runs` / `n_data_slots` (ใน 64 B slot เดิม, `sizeof` ยัง = 64)
- **Chunked commits:** 1 run = 1 journal frame (capture → frame-write → mutate → commit)
  - 100 KB = 28 runs = 28 frames → lossless 100%
- **Entry เขียนใน frame สุดท้าย** → crash กลาง chain ไม่มีไฟล์โผล่ครึ่งๆ
- **Rewrite (mdim_write):** auto-commit, **full chain re-layout** — เขียน NEW chain ก่อน
  (ไฟล์ยังอ่าน old chain อยู่), สลับ entry ใน frame สุดท้าย (atomic 1 frame), แล้วค่อย free OLD chain
  → crash ณ จุดไหน = ไฟล์ old เต็มหรือ new เต็ม ไม่มี torn
  → **grow/shrink/empty ได้ทุกขนาด** ≤ 1 MiB (span change ได้ด้วย) — ไม่ต้องมี DIRTY flag แล้ว
  → ข้อเสีย: ต้องมีที่ว่างให้ old+new พร้อมกันตอน grow
- **Unsummon:** TOMB entry ก่อน (ไฟล์หายทันที) แล้ว free ทีละ run
- **Self-healing sweep:** bitmap ถูก *derive จาก entry chains* (ไม่ใช่จาก slot types)
  → crash กลาง op ที่ run โดน commit แล้วแต่ยังไม่มี entry = orphan → swept ตอน open ครั้งถัดไป

### 2. Bug จริงที่เจอและแก้ (สำคัญ) ⚠️
**Freed slot ต้องเป็น TOMB, ห้าม EMPTY** — EMPTY slot จะ *terminate* stride-37 probe walk
→ zeroing block ที่ free อาจทำให้ entry ที่อยู่ถัดไปใน chain หายจาก lookup
(เจอจาก CLI demo: ไฟล์หายจาก `list` หลัง unsummon ไฟล์อื่น)
แก้ครบ 4 จุด: unsummon, rewrite reclaim, sweep pass-2, summon NOSPC rollback

### 3. ตัวเลขจริง (แก้ doc ที่เคยประมาณผิด)
| Cap | ค่าเดิม (doc) | ค่าจริง | ใหม่ |
|-----|--------------|---------|------|
| single-frame | ~56 KB (ประมาณผิด) | 3843 B | **3780 B** (`MDIM_CAP_ONE_FRAME` = 60×63) |
| per-op hard cap | 3843 B | — | **1 MiB** (`MDIM_MAX_FILE_BYTES`, volume-bounded ~1.27 MB) |

- Version bump: super version 1 → 2 (load/mmap เช็ค `!= 2 → CORRUPT`)

## ไฟล์ที่แก้ (uncommitted)

```
core/geofs_mdim.h          — MDIM_T_LINK, MDIM_F_CHAIN/DIRTY, run-span helpers,
                             summon/write/unsummon chunked, rebuild=chain-derived,
                             super v2, MDIM_MAX_FILE_BYTES = 1 MiB
tests/test_geo_fs_mdim.c   — 16 tests (14: 100 KB summon lossless / 15: rewrite+versions+
                             unsummon / 16: orphan sweep) + 13 ตัวเก่าปรับ contract
tools/mdim_cli.c           — summon cap ข้อความใหม่ + runs column ใน list
docs/MDIM_NATIVE_FS.md     — อัปเดต multi-frame runs + limits จริง
```

## งานค้าง (next possible)

1. **Mount layer** — WinFSP/Dokany shim ให้ `CreateFile` เปิด volume ได้
   (breathing_fs_cli มี write-map/get surface ให้ mirror)
2. **Full spine** — scale จาก 20736 ไป 1728-pipe spine (FS_PIPES × FS_TICKS)
3. **Path tree** — directory slots (entry array แบบ inode direct-block)
4. **Chain defrag** — merge runs / ลด peak ตอน grow (old+new พร้อมกัน)

## Key decisions (ต้องเอาไปต่อ)

- Chain-of-runs = extent แบบ coordinate-address: ไม่มี run table, link ชี้ไปใน space เอง
- Entry-last สำหรับ summon (ไม่มีไฟล์ครึ่งๆ) / rewrite = new-chain-first + atomic entry switch
- bitmap = derived จาก chains → crash self-heal บน open
- TOMB ≠ EMPTY: TOMB = free แต่ keep probe chain alive
- Multi-frame op evict frames ตัวเองจาก ring 128 (history depth = 1 ring) — document ไว้แล้ว

## Build / test

```bash
cd I:/DWGLS-native-fs
make mdim_cli
make test-test_geo_fs_mdim      # 16/16
```
