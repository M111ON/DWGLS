# GOLDBERG_STORAGE.md — Goldberg decagram storage (ระบบ)

> สรุป: T1.2f (decagram → Goldberg) + T1.2g (dual view) + T1.2h (ประกอบเข้าเป็น API ระบบ)
> หัวใจ: **"จับ tensor → zero-copy → goldberg storage"** — พิสูจน์ lossless บน GGUF จริง
> ด้วยตัวเลข (100% ทุกโมเดล, ~300-400 MB/s)

---

## 1. หลักการ (ทำไมถึงเป็นแบบนี้)

### Decagram ครอบ Goldberg ได้ทุก face (T1.2f, §15.80)

ตระกูล dodeca bipolar **inverted** — face f กับ f+6 เป็นคู่ตรงข้ามที่กลับทิศกัน
(ring1 = 0..5, ring2 = 6..11 — `geo_goldberg_lut.h`) → การ map sector แบบ round-robin
12 ตัวเดิมมี remainder — แต่:

```
Goldberg GP(n,0): hexagon ทั้งหมด = 10(n²−1)
→ หาร 10 ลงตัวเป๊ะทุก level 1..8   ← decagram (10 sectors × 36°) ลงตัวโดยโครงสร้าง

hex tile_id = 12 + sector·(n²−1) + offset   (sector 0..9, offset 0..n²−2)
dim         = k / hex_total                  (0..7 = gp_level depth)
tick        = (tile_id << 8) | dim           (deterministic, no table)
```

- **Bijective:** ทุก hex tile ถูก address 1 ครั้ง — zero gap, zero overlap
- **Bipolar inversion:** sector d ↔ d+5 (ทิศตรงข้าม) · face f ↔ f+6
- **12 pentagon = anchor** (tile 0..11, fixed ทุก level — จุดพับของ icosa net)

### 12 pentagon anchor = จุดพับ (จาก session ที่ผ่านมา)

สนามแบน 144² (20736) = equal-triangle tessellation — ฝัง 12 pentagon anchor ไว้
(12×1728) = รอยพับ → พับ = icosahedron (20 faces) → subdivide GP(n,0) = Goldberg
→ **"แบน" กับ "ทรงกลม" เป็นทางเลือกของ view ไม่ใช่โครงสร้างคนละอัน** (§⑰)

### Dual view: container เลือกรูปทรงได้ (T1.2g, §15.81)

payload (codebook + idx) **ไม่แตะ geometry** — GeoType ให้แค่ mask/capacity
→ วางข้อมูล 1 ชุด อ่านผ่าน dodeca(12) / icosa(20) / compound_144 / goldberg_192
→ payload เท่ากันทุก byte + decode lossless ทุก view (พิสูจน์แล้ว 10/10)

### Streaming multi-sphere (แก้ข้อจำกัด, §15.83)

1 sphere = hex_total × 8 chunks (GP(8,0) = 5040 × 64B ≈ 322KB) — tensor ใหญ่ =
หลาย sphere — **write→verify→destroy ทีละ sphere → RAM คงที่ ~1.3MB**
ไม่ขึ้นกับขนาด tensor (output.weight 144MB = 449 spheres ก็ได้)

---

## 2. API ระบบ — `core/geo_goldberg_store.h`

```c
typedef struct {
    uint8_t  level;         /* gp level 1..8                */
    uint32_t faces;         /* ggd_face_count(level)        */
    uint32_t hex_total;     /* 10(n²−1) = 630 @ L8          */
    uint64_t per_sphere;    /* hex_total × 8 chunks/sphere  */
    uint64_t chunks_stored; /* cumulative (all tensors)     */
    uint64_t bytes_stored;  /* cumulative                   */
} GoldbergStore;

void   ggs_init(GoldbergStore *s, uint8_t level);      /* clamp 1..8 */
uint32_t ggs_tile(uint8_t level, uint64_t k);          /* decagram tile */
uint8_t  ggs_dim(uint8_t level, uint64_t k);           /* 0..7 */
uint32_t ggs_spheres(const GoldbergStore *s, uint64_t n_chunks);
int    ggs_store(GoldbergStore *s, const uint8_t *data, uint64_t n_bytes);
       /* 0 = lossless · <0 = fail — streaming, verify ภายใน (memcmp) */
int    ggs_store_verify(GoldbergStore *s, ...);        /* 1 = fail */
```

- **สถานะของ store = สถิติสะสม** — ตัว data อยู่ที่ caller (zero-copy จาก mmap)
- Depends: `geo_goldberg_decagram.h` + `geo_goldberg_sphere.h` + `infra/tring.h`
- Tring ใช้ภายใน — ฟรีเองทุก sphere (caller ไม่แตะ)

---

## 3. ผลจริงบน GGUF (benchmark)

`make goldberg_probe` → `./build/goldberg_dual_probe <model.gguf> --all`

| model | tensors | stored | bytes | write+read |
|---|---|---|---|---|
| Qwen2.5-0.5B Q8 | 291/291 (100%) | lossless | 638.7 MB | 380.7 MB/s |
| Qwen3-0.6B Q8 | 310/310 (100%) | lossless | 604.1 MB | 277.2 MB/s |
| Kokoro Q8 | 775/775 (100%) | lossless | 166.5 MB | 211.0 MB/s |
| **รวม** | **1,376/1,376** | **fail 0** | **1,409.3 MB** | — |

Dual view (output.weight จริง 144MB, Q8 bytes → 0..255):
- payload เท่ากัน 4 views (dodeca/icosa/compound_144/goldberg_192) ✅
- decode lossless ทุก view ✅ · decode(dodeca) == decode(icosa) ✅ · ratio ~3.3×

### Suite (make test): **TIER1 87/87 + TIER2 4/4** เขียว
- `test_goldberg_decagram` 11/11 (T1.2f)
- `test_goldberg_store` **30/30** (T1.2h — API ระบบ: clamp/addressing/ขนาด/
  chunk-boundary/tail/multi-sphere/deterministic/empty)

---

## 4. บทเรียน (bug ที่ไฟล์จริงจับได้)

1. **`gp_lens_write` dormant bug** — เขียนที่ next_tick (ต่อเนื่อง) แต่ return tick
   (sparse) → อ่านกลับไม่เจอ — แก้เป็น sparse insert ที่ tick โดยตรง
   (coordinate = address) — ไม่เคยมี test ใช้ goldberg storage จริงมาก่อน
2. **decagram_tile ล้น face_max** — offset โตเกิน per_sector → modulo ด้วย hex_total
3. **Q8 bytes ≠ float** — dual view interpret-as-f32 ผิด → แปลง byte → 0..255 ก่อน

---

## 5. ต่อยอด (เปิดอยู่)

- **Persist sphere → ไฟล์** — ตอนนี้ Tring อยู่ใน RAM — ก้าวต่อไป = serialize
  sphere (เช่น .gcube / GeoFS) ให้ "storage" เป็นของจริงที่เก็บได้
- **Dedup/walk integration** — เก็บ tensor ผ่าน goldberg แล้วอ่านด้วย walk clock
  (test_fibo_walk / registry) — รวม read path สองระบบ
- **18tes (FUTURE)** — 6ico compound = 18 tesseracts × 8 cube × 144 = 20736 —
  ยังไม่ implement (ซับซ้อนเกินตอนนี้)
- **decagram ลง Goldberg ที่ level อื่น + ค่า p** — GP(n,m) นอกแกน 10(n²−1)

---

## 6. Persistence — sphere ลงไฟล์ .ggf (T1.2i, §15.85) ✅

### 6.1 FILE LAYOUT (.ggf)

```
[GGFHeader 64B]  magic "GGF0" · version · level · n_spheres ·
                 n_chunks (total 64B nodes) · n_bytes (original) ·
                 crc32 (CRC32 ของ data ทั้งหมด, padded 64B/chunk) · note[28]
[sphere 0]       [count u32] + count × [tick u32][data 64B]
[sphere 1..n]    ...
```

- **tick = gp_addr_to_tick({tile_id, dim})** — self-describing: node บอกที่อยู่ของ
  ตัวเอง → loader ตรวจว่า tick ตรงกับตำแหน่ง chunk (exp_tick) หรือไม่
- **CRC32 (zlib poly 0xEDB88320)** — เหนือ padded data ทั้งไฟล์ (seed ต่อเนื่อง)

### 6.2 หลักการ save/load

- **ggs_save:** write → verify (memcmp ภายใน sphere) → persist ทีละ sphere
  → **verify ก่อน persist = ไม่มีทางเขียน data เสียลงไฟล์**
- **ggs_load:** reconstruct chunk ตามลำดับเดิม + validate tick ตรงตำแหน่ง +
  CRC ตรวจทั้งไฟล์ → จับได้ทุกกรณี: data พัง (CRC) · tick พัง (exp_tick + CRC) ·
  node เรียงผิด/ซ้ำ (exp_tick + count bound) · count พัง (bound check)

### 6.3 API

```c
int ggs_save(const uint8_t *data, uint64_t n_bytes, uint8_t level, const char *path);
int ggs_load(const char *path, uint8_t *out_buf, uint64_t buf_cap, uint64_t *out_n_bytes);
```

### 6.4 ผลไฟล์จริง (--all --save, probe ใหม่)

| model | saved+reloaded | bytes | fail |
|---|---|---|---|
| Qwen2.5-0.5B Q8 | 291/291 | 638.7 MB | 0 |
| Qwen3-0.6B Q8 | 310/310 | 604.1 MB | 0 |
| Kokoro Q8 | 775/775 | 166.5 MB | 0 |

- **Corruption proof:** flip 1 byte กลาง .ggf จริง → `ggs_load` rc=-10 (CRC) จับได้
- suite: TIER1 88/88 (test_goldberg_file 26/26) + TIER2 4/4

### 6.5 บทเรียนรอบนี้

1. **struct padding** — GGFHeader ตอนแรก 68B ไม่ใช่ 64B (u64 alignment) →
   เรียง u64 ก่อน u32 ตามหลัง → 64B เป๊ะ
2. **loader ต้อง validate tick กับตำแหน่ง** ไม่ใช่แค่ขอบเขต — ไม่งั้น data
   สลับตำแหน่งกันแล้ว CRC ยังผ่าน (tick เดิม + data ต่างที่)
3. **Windows path** — /tmp ใน Git Bash ≠ /tmp ของโปรแกรม → ใช้ relative path

### 6.6 ต่อยอด (จาก 5)

- ~~Persist sphere → ไฟล์~~ ✅ (T1.2i) — ตอนนี้ storage เก็บได้จริง
- **Lazy read** — seek ต่อ sphere (header มี n_spheres/n_chunks แล้ว) ไม่ต้องโหลดทั้งไฟล์
- **Dedup/walk integration** — เก็บ tensor → .ggf แล้วอ่านด้วย walk clock
- **Provenance** — header มี note[28] เก็บใคร/เมื่อไหร่/level ได้อยู่แล้ว
