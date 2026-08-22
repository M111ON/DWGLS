# WORKTREE EXPERIMENTS REPORT — ผลการทดลองแบบหลายมุมมอง (2026-08-22)

> สาม worktrees เปิดขนานจาก HEAD `07fdaf2` เพื่อทดสอบ assumption ที่ main tree
> ไม่เคยถาม ทุก experiment มี oracle อิสระ และทุก failure ระหว่างทางถูกบันทึกไว้
> ใน section "failures encountered" ของแต่ละ tree

---

## Tree 1: `exp/alt-scale-semantics` — ZERO-COPY BREATHING ✅

### สมมติฐานที่ทดสอบ
Main tree พิสูจน์ breathing แบบ **RELOCATE** (ย้าย bytes จริง ~670MB/event)
แล้วสรุปไปว่า "breathing ต้อง copy" — แต่ปรัชญาของระบบบอก *bytes never move*
→ ครึ่งหนึ่งของสัญญายังไม่เคยถูกพิสูจน์

### การทดลอง (`tools/geo_breathing_relabel.c`)
- bake Qwen2.5 5305 parts @scale1 — **copy ครั้งสุดท้ายของ session**
- events เปลี่ยนแค่ LOG: expand×2 · shuffle S₃ v4 · unshuffle v5 · collapse ÷2
- consumer อ่านด้วย **coordinate**: `coord_to_part(L)` walk log backward
  (expand → odd L = hole; shuffle/unshuffle → inverse view perm บน cube-local)
- ตัวชี้วัดสำคัญ: **memcpy counter ระหว่าง breathe ต้อง = 0**

### ผล
| state | verify | holes |
|---|---|---|
| after expand ×2 | 5305/5305 PASS | odd coords empty ✓ |
| after shuffle v4 | 5305/5305 PASS | |
| after unshuffle v5 | 5305/5305 PASS | |
| home again | 5305/5305 PASS | |

**copies during ALL breathing: 0** · carried state = 4 events (32 B)

### failures encountered
- `%llu` printf warning บน mingw gcc8 → fix `-D__USE_MINGW_ANSI_STDIO=1`
- build dir ไม่มีใน worktree ใหม่ → สร้างก่อน compile

### ข้อสรุป
**Two-tier contract**: logical-coordinate consumers หายใจฟรี (relabel, 0 copy);
physical-offset consumers (GPU) ใช้ relocate-on-demand — ทั้งสอง mode
lossless บน weights จริง และ v1 "tautology" ได้รับการฟื้นฟู: identity path
คือ feature เมื่อ consumer เป็น coordinate-based

commit `929122c`

---

## Tree 2: `exp/sparse-backing` — SINGLE SPARSE BACKING ✅

### สมมติฐานที่ทดสอบ
Storage decision #181: baked model ควรอยู่ใน **1 sparse backing file** แทน
569 volume files — แต่ (ก) perf จะต่ำกว่า RAM window ไหม? (ข) hole จะกิน disk จริงไหม?

### การทดลอง (`tools/geo_sparse_serve.c`)
- NTFS sparse: `FSCTL_SET_SPARSE` + SetEndOfFile 2.66GB logical → mmap
- bake Qwen2.5 5305 parts ผ่าน iso_fold addressing
- วัด on-disk จริงด้วย `GetCompressedFileSizeA`
- VERIFY memcmp + 6-view XOR sweep เหมือน RAM window version

### ผล
```
DISK   logical 2.72 GB · on-disk 0.68 GB · holes saved 2.04 GB (75% free)
VERIFY 669.8 MB byte-identical · 0 bad ✓
view   GB/s: [3.90 cold] 11.44 · 11.42 · 11.46 · 11.60 · 11.44  (warm)
RESULT: SPARSE BACKING LOSSLESS · files exposed: 1
```

### failures encountered
- `FSCTL_SET_SPARSE` undeclared บน mingw → define CTL_CODE เอง + `<winioctl.h>`
- view 0 ช้า (171ms cold page-in) — expected: first touch reads from disk backing

### ข้อสรุป
Gate ผ่านทุกข้อ: lossless เท่าเดิม · 1 ไฟล์ · on-disk ≈ payload ·
warm sweep 11.4+ GB/s **เร็วกว่า RAM window เดิม** (page cache effect) —
free-space metaphor กลายเป็นคุณสมบัติจริงของ storage layer

commit `5fbd2f0`

---

## Tree 3: `exp/negative-port` — PORT + UNIVERSE + DUPSCAN ✅✅✅

### 3a. Negative Port (`tools/geo_port_serve.c`)
**โจทย์:** LFM2.5-2.6B ต้องการ 22,014 parts > window 20,736 — เดิม `FAIL exceeds window`

**กลไก:** overflow parts ผ่าน negative port → residual spill file +
bond registry `{part_id → spill_offset}` (16 B/bond); read path replay bond

**ผล:**
```
in-field 20,736/20,736 ✓ · residual 1,278/1,278 ✓ (memcmp both tiers)
bonds carried: 20,448 bytes · field on-disk 2.71 GB (sparse) + residual 0.17 GB
old behaviour 'FAIL exceeds window' → eliminated
```

**failures encountered:** bake 252s / verify 70s — cold-disk bound (first read
ของ 2.7GB model), ไม่ใช่ addressing overhead

### 3b. Universe Seed (`tools/geo_universe_test.c`)
**โจทย์:** สองโมเดลอยู่ window เดียวกันได้ไหมโดยไม่ชน? (P4 gate แรก)

**ผล:** SmolLM2 (3072 parts, cubes 0..5) + smolVLM-text (1483 parts, cubes 6..11)
ใน sparse file เดียว:
```
SmolLM2 3072/3072 PASS · smolVLM 1483/1483 PASS · collision 0
files exposed: 1 · on-disk 0.57 GB of 2.72 logical
```
partition math = base + fold ปิดในตัว ไม่มี directory

### 3c. Chunk-Level Dup Scan (`tools/geo_dupscan.c`)
**โจทย์:** ภายใน baked weights มี chunk ซ้ำกันเองแค่ไหน? (tensor-level tied
137MB เคยรู้ — chunk level ไม่เคยวัด)

**วิธี:** FNV-1a digest ต่อ part → sort → group equal digests →
memcmp-confirm pairwise (hash collision ปลอม dup ไม่ได้)

**ผล:**
| model | chunks | dup groups | extra chunks | waste |
|---|---|---|---|---|
| Qwen2.5-0.5B | 5305 | 1,104 | 1,104 | **144.6 MB = 21.6%** |
| Qwen3-0.6B | 5015 | 0 | 0 | 0% |
| SmolLM2-360M | 3072 | 0 | 0 | 0% |
| Kokoro-TTS | 1846 | 0 | 0 | 0% |

**ข้อสรุปสองชั้น:**
1. dup-scan จับ tied embeddings อัตโนมัติ (144.6MB ≈ 137MB known + เศษ)
   **โดยไม่รู้สถาปัตยกรรมโมเดล** — ตรงหลัก "detect pre-existing structure"
2. duplication เป็น **per-model structure** — dedup-on-bake savings
   content-determined (21.6% vs 0%), ไม่ใช่ formula-based

commits: `cdece7a` (port) · `a9208f8` (universe) · `8ee6aac` (dupscan)

---

## ภาพรวม: สิ่งที่ multi-perspective ให้ที่มุมเดียวไม่เห็น

| belief ก่อน | หลัง 3 worktrees |
|---|---|
| breathing ต้อง copy 670MB/event | relabel mode = **0 copies**; relocation = service สำหรับ physical consumers |
| storage layer ช้ากว่า mmap | single sparse file sweep **เร็วกว่า** RAM window mode |
| โมเดลใหญ่กว่า window = FAIL | negative port + bond 20KB → serve ครบ |
| หลายโมเดล = หลายจักรวาล | universe seed: 2 โมเดล 1 ไฟล์ lossless |
| dedup ต้องรู้ architecture | dup-scan จับ structure อัตโนมัติ 21.6% (Qwen2.5 only) |
