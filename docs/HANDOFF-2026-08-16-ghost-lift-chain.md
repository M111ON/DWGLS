# HANDOFF — 2026-08-16: Ghost Lift + Capacity + Real-File Chain

Session ที่ปิดวงจรครบ: จาก bond → envelope → capacity accounting →
chain ไฟล์จริง → adaptive scheme chooser ทุกชั้นมีเทสต์จริงบนข้อมูลจริง

## 🔗 สิ่งที่สร้าง (7 core headers ใหม่ + 9 เทสต์ + 2 tools)

| layer | file | เทสต์ |
|---|---|---|
| bond store (ซ่อม) | `core/residual_space.h` — origin_key bug (3 paths ให้ consistent) | `test_residual_space` **59/59** (เดิมเทสต์ผิด header!) |
| bond primitive | `core/pogls_bond.h` + `pogls_config.h` (จาก FGLS_new, identical) | — |
| ghost lift | `core/geo_ghost_lift.h` — bond = birth identity `(block_id, from_scale)`, log = route `{from→to}` 5B | `test_ghost_lift` **47/47** |
| envelope (§11.6 ตัดสินใจแล้ว) | `core/geo_ghost_envelope.h` — `envelope_depth(gate)`: 1.0→5, 2.0→4, 0.5→6; hard ceiling 7 | `test_ghost_envelope` **39/39** |
| capacity (§11.6) | `core/geo_cap_account.h` — Σ envelope ≤ 20736, CAP_LIFT/REJECT/ADMIT, reject ไม่ silent | `test_cap_account` **23/23** |
| scheme chooser | `core/geo_placement_choose.h` — PER_FILE vs GLOBAL-targeted, margin | `test_cap_scheme` **12/12** |

### เทสต์ tuning บนข้อมูลจริง (ทุกตัวใน TIER1, ข้ามถ้าไม่มีไฟล์)

| เทสต์ | ข้อมูล | ผล |
|---|---|---|
| `test_cap_tune_real` | GGUF จริง 4 โมเดล | lifts 95.5%, field 24,279 → **4 windows** (targeted 6,070×) |
| `test_cap_tune_safetensors` | safetensors จริง 3 ไฟล์ (LFM2.5-VL, smolVLM, zimage-ae) | 5,160 → **3 windows** |
| `test_cap_tune_fs` | F:/notebookLM 1,035 ไฟล์ + zip GeoGebra | folder → 1 window, zip 1,474 entries → 0 |
| `test_cap_chain_roundtrip` | PDF จริง 19.8 MB | 1,209 chunks → **lossless byte-for-byte** |
| `test_cap_chain_big` | mp4 จริง 57 MB | eviction 2,655 LRU ถูกต้อง + streaming lossless + whole-resident |
| `test_rs_persist` (§15.34) | mp4 จริง 57 MB | serialize 3,525 entries (56.5 MB) → restart → reload → **lossless byte-for-byte** |

### Tools (manual, `make cap_scan` / `make cap_scheme`)

- `tools/cap_chain_scan.c` — สแกนทั้ง folder: **1,035/1,035 lossless** (7.7 GB, 1m33s)
- `tools/cap_scheme_choose.c` — folder จริง: per-file 116.3M vs global 100.0M slots (1.2×) → PER_FILE @margin50

## 📊 ตัวเลขสำคัญ

```
make test: TIER1 74/74 + TIER2 4/4 ✅
GHOST_LOG_MAX 256 → 4096 (สมมาตรกับ RS_DEFAULT_CAPACITY)
envelope_depth: gate 1.0 (default) = 5  ← "k 4-5 เหมาะสมที่สุด"
lifting ตัด field footprint: GGUF 8.5×, targeted ranks 6,070×
```

## 🧭 คำศัพท์/หลักการ (ตรงกับ TIMELINE_WORKING_MODEL §15.31-15.33)

- **bond = identity, log = route** — from_scale เป็นส่วนหนึ่งของที่อยู่ (เสาเข็มห้ามขยับ), to_scale อยู่ใน log เท่านั้น
- **ย่อฟรี ขยายจ่าย** — depth = to > from ? to−from : 0; contraction ไม่ lift
- **residual_space = LRU cache** — ไฟล์ใหญ่กว่า cache → streaming เป็น windows (พิสูจน์ lossless) หรือขยาย capacity
- **สอง cost model**: fp block model (cap_admit accounting/limit) vs size model (view_of จริง — เปรียบเทียบ scheme)

## ⏭️ ขั้นต่อไป (เปิดไว้)

1. **Wire scheme chooser เข้า scan** — เมื่อ chooser บอก GLOBAL ให้รัน chain แบบ global-targeted + พิสูจน์ lossless
2. **วัด locality cost ของ GLOBAL** — read-back ms / cache-line coverage เทียบ PER_FILE (§15.30 วิธี)
3. ~~**Persist residual_space**~~ — **ทำแล้ว §15.34** (test_rs_persist 36/36: mp4 57 MB serialize → restart → reload → lossless; `rs_serialize`/`rs_load` + `ghost_log_serialize`/`ghost_log_load`)
4. **Global-rank mode ใน cap_chain_scan** — chain 498K chunks ลำดับเดียว + targeted → ดู field windows จริง
   - เพิ่ม option ให้ cap_chain_scan serialize residual image ลงดิสก์ตอนจบ (ตอนนี้ persistence พิสูจน์ใน test แล้ว ยังไม่ wire ใน tool)
5. **18tes (GEO_COMPOUND_144)** — ตัวเอก ยังเป็น FUTURE (test_6ico_tesseract ผ่านเป็น base)
6. **กลุ่ม rail (test_rail_hub, test_geo_fs_bench, test_real_gguf_microscope)** — พักไว้ รอ residual space + jet puller

## ⚠️ ข้อควรระวัง

- `make test` exit code ใช้ได้ (เทสต์ใหม่ return 0/1 จริง) แต่เทสต์ legacy บางตัว exit 0 ทั้งที่ FAIL ข้างใน — ดู output จริง
- เทสต์ tuning ใช้ไฟล์จริง (I:/model, F:/notebookLM) — ข้ามเงียบๆ ถ้าไม่มี (TIER1 ยังเขียว)
- `.freebuff/` = client metadata — อย่า commit (อยู่ใน .gitignore แล้ว)
- `master` ที่ I:/DWGLS มี gguf_box.h เก่า hdr_size=16 — งาน fix อยู่ branch นี้ ระวัง merge ทับ
