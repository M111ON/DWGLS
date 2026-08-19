# Walk = Sync — กลไกเดียว สองบทบาท (stride-37 บน Ring)

> **Walk ถามว่า "ที่ไหน" · Sync ถามว่า "เมื่อไหร่" — และเพราะ ring เป็น bijection
> คำตอบทั้งสอง deterministic + คำนวณได้ O(1) จากเลขคณิตชุดเดียวกัน**

---

## 1. ข้อเท็จจริง — stride-37 อยู่ทุกที่

| Component | Cycle | Stride | บทบาท |
|---|---|---|---|
| `core/geo_tring_walk.h` | **720** = 6 spokes × 120 | 37 (coprime 720) | tile → enc (addressing) |
| `core/geo_frame_seek.h` | **1440** = 12 faces × 120 | 37 (coprime 1440) | frame → enc (timeline seek) |
| `core/infra/geo_rail_ring.h` | **1440** | 37 | 3 lanes A/B/C — sync |
| `core/geo_frame_seek_wang.h` | 1440 = 12×120 windows | — | Wang checksum windows |
| memory (FGLS_new era) | `FRAME_CYCLE = FSW_ENC_CYCLE = GEO_FIBO_CLOCK = 1440` | 37 | canonical timeline |

เลขเดียวกัน กลไกเดียวกัน ปรากฏใน 3 layer: **tring (720)** → **frame seek (1440)** → **rail ring (1440)** —
และ 1440 = 2×720 (hex + tri — กฎ ×2: ring มีสองชั้น parity, แต่ละ face 120 slots = 60 hex + 60 tri)

## 2. ทำไม 37 ถึงทำงานได้ทั้งสองบทบาท

- `gcd(37,720) = 1` และ `gcd(37,1440) = 1` → **1 permutation cycle ครอบทุกตำแหน่ง**
  (เดินครบ 720/1440 ครั้งกลับมาที่เดิม — พิสูจน์ในโค้ด: `rail_ring_verify` / `geo_frame_seek_verify [T1]`)
- **37 เป็นเลขคี่** → enc สลับ parity ทุกก้าว (ฟันปลา ∧∨ — ฝังอยู่ใน walk ตั้งแต่ต้น)
- การเดินเป็นลำดับตำแหน่งที่ deterministic → ใช้เป็น **นาฬิกา** ได้ (ไม่ใช่แค่ index)

## 3. บทบาทที่ 1 — Walk = Addressing (coordinate = address)

```c
frame_enc(t) = (t × 37) % 1440      /* frame t อยู่ที่ enc ไหน */
tring_walk_enc(i) = (i × 37) % 720  /* tile i อยู่ที่ enc ไหน */
pipe_id = flat % 1728 · tick = flat / 1728   /* geo_cell_addr — walk clock state */
```

- **bijection**: ทุก enc ถูกเยี่ยมครั้งเดียวต่อรอบ → พิกัด = ที่อยู่ ไม่มี collision
- **enter-anywhere**: state `(seed, round, tick)` → เดินนาฬิกา → ตำแหน่ง live —
  อ่านจาก state ใดก็ได้ (เส้นทางต่าง ข้อมูลเดียวกัน)
- นี่คือ "MAP ไม่ใช่ COMPRESS" — walk เป็นตัวสร้าง address

## 4. บทบาทที่ 2 — Sync = Phasing (PARK ที่ XOR = 0)

```c
/* geo_rail_ring.h — 3 lanes = สำเนาของ walk เดียวกัน offset 120° = 480 ticks */
base = (i × 37) % 1440
A[i].enc = base          /* lane A — θ = 0°   */
B[i].enc = (base+480)%1440  /* lane B — θ = 120° */
C[i].enc = (base+960)%1440  /* lane C — θ = 240° */
```

- แต่ละ lane เดิน **stride-37 เดียวกัน** → relative phase ระหว่าง lanes คงที่ตลอด
  (ห่างกัน 480 ticks เสมอ) — โครงสร้างซิงค์ถูกฝังในตัว walk
- **sync point = PARK**: เมื่อ lane มาอยู่ตำแหน่งที่ peers สอดคล้องกัน
  (`rail_sync_ready`: XOR angular distance = 0 · `rail_sync_arriving`: ภายใน < 8 ticks)
- **freeze ถูก trigger ที่ "tick 12 boundary"** — `core/geo_bfs_hub.h`:
  `P5HRibcage ribcage; /* freeze/barrier at tick 12 */` — จุดที่ lanes sync = จุดที่
  checkpoint/freeze ปลอดภัย (Barrier — ต้นตระกูลของ `geo_ggf_ckpt` mid-round)
- **480 = 1440/3 = sector 120°** — 3-fold อีกแล้ว: 3 squares ของ 12-gon (orbit R120°)
  = 3 lanes (θ 0/120/240) = 3 chiralities ของ 5-tetra compound — โครงสร้าง C3 ตัวเดียวกัน
  เกิดซ้ำใน addressing (12-gon), ring (lane offset), และ compound (chirality)

## 5. ทำไมกลไกเดียวใช้ได้ทั้งคู่ — "phase alignment" อันเดียว

| | Walk (addressing) | Rail (sync) |
|---|---|---|
| คำถาม | tile i อยู่ที่ไหน? | เมื่อไหร่ที่ lane a กับ b ตรงกัน? |
| คำตอบ | enc(i) = 37·i mod N | tick ที่ (37·i mod N) ≡ (37·j mod N) |
| ลักษณะ | index — ตำแหน่งของสิ่งหนึ่ง | clock — จังหวะที่หลายสิ่งตรงกัน |
| คุณสมบัติ | bijection → ไม่ชน ไม่ซ้ำ | deterministic → คำนวณได้ O(1) |

**ทั้งคู่คือ "phase alignment บน ring เดียวกัน"** — addressing align สิ่งหนึ่งเข้ากับ
ตำแหน่ง · sync align หลายสิ่งเข้าหากัน — เลขคณิตชุดเดียว (stride-37 mod ring)
คำถามคนละแบบ — นี่คือเหตุผลที่ "walk = sync" และเหตุผลที่ freeze/checkpoint
(via sync) กับ read (via walk) ใช้ ring เดียวกันได้โดยไม่ขัดกัน

## 6. ตัวเลขที่ปิดวง

```
1440 = 2×720          ← กฎ ×2: hex + tri (parity สองชั้น)
720  = 6×120          ← = (3 lanes × 2 polarity) × 120 — สอง prime factor ของ 12
480  = 1440/3         ← sector 120° ของ lane — 3-fold
12   = 1440/120 faces ← = FS_TICKS = WANG_WIN_SIZE — freeze boundary
      = tick ที่ Barrier ทำงาน (geo_bfs_hub)
120  = WANG_WIN_COUNT ← 1440/12 windows — Wang checksum ต่อ window
```

**12 ตัวเดียวกัน** ปรากฏเป็น: จำนวน faces ของ ring (1440/120) · ticks ของ fibo spine
(FS_TICKS) · ขนาด Wang window · จุด freeze — ring, clock, checksum, checkpoint
ผูกด้วยเลข 12 เดียว

## 7. บทเรียนจาก memory (ข้อผิดพลาดที่แก้แล้ว — สอนว่า sync ต้องอยู่บน ring เดียวกับ addressing)

1. **XOR ไม่ใช่ angular distance** — `(a ^ b) % 360` aliasing (เช่น 256^0 ให้ 256 ทั้งที่
   ระยะจริงคือ 104°) → แก้เป็น **modular distance** (ระยะวงกลมจริง) — ถ้า clock
   วัด "ระยะ" ผิด จุด PARK จะอยู่ผิด tick
2. **Ring 512 ไม่ครบรอบ** — `512×37 % 1440 = 224 ≠ 0` → ครอบแค่ 35.6% ของ enc —
   sync บน ring ที่ไม่ครบรอบ = clock เดินไม่ครบ = freeze ผิดจังหวะ →
   แก้เป็น **RAIL_RING_SIZE = 1440** (ตรง FRAME_CYCLE เป๊ะ)
3. **สรุป: ถ้า sync ไม่ใช้ ring เดียวกับ addressing → "นาฬิกา" ไม่ตรงกับ "index"**
   — นี่คือเหตุผลว่าทำไมต้องเป็นกลไกเดียว

## 8. สรุป

> **Walk = Sync: addressing กับ phasing คือเลขคณิตเดียวกัน (stride-37 mod ring)
> — walk ถาม "ที่ไหน" (index) · sync ถาม "เมื่อไหร่" (clock) — และเพราะ ring เป็น
> bijection คำตอบทั้งสอง deterministic + O(1) — freeze ที่ tick-12 = จุดที่ clock
> กับ index ตรงกัน = จุดที่ checkpoint ปลอดภัย**

---

*แหล่งอ้างอิง: `core/geo_tring_walk.h` · `core/geo_frame_seek.h` · `core/infra/geo_rail_ring.h` ·
`core/geo_frame_seek_wang.h` · `core/infra/fibo_spine.h` · `core/geo_cell_addr.h` ·
`core/geo_bfs_hub.h` (freeze/barrier at tick 12) · memory: rail_sync/phase_rail (FGLS_new era,
ring 512→1440 fix, XOR→modular fix) · §15.97 (กฎ ×2) · test_dodeca_x2 (3-fold C3)*
