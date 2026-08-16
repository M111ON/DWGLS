# LEGACY_TESTS.md — เทสต์ที่ถูก deprecated เป็นประวัติการพัฒนา

> **สถานะ:** บันทึกเมื่อ 2026-08-16 — หลัง rescope 2026-08-14
> **จุดประสงค์:** เทสต์ในไฟล์นี้ = **ประวัติ** ว่าเราทำอะไรมาบ้าง เพื่อย้อนกลับมาดูตอนตัดสินใจ
> **ห้ามใช้ยืนยันระบบปัจจุบัน** — เทสต์ทั้งหมดเขียนก่อน rescope (7-10 ส.ค.) และไม่ถูกทบทวนกับ model ใหม่
> **เกณฑ์ตัดสิน:** AGENTS.md (§🧭 Rescope) + `docs/TIMELINE_WORKING_MODEL.md` §9 (แต่ละทางที่พัง → กฎ)
> **วิธีรันเอง:** `gcc -O2 -I. -Icore -Icore/infra -o build/x tests/<name>.c -lm && ./build/x`
> (บางตัวต้องการ `-IFGLS_new/runner` ตาม comment ในไฟล์ — cross-repo)

---

## หลักการที่ใช้แบ่ง (จาก rescope 2026-08-14)

Model ปัจจุบันเปลี่ยนหลักการเหล่านี้ — เทสต์ที่เทสต์แนวคิดตรงข้าม = legacy:

| หลักการเก่า (ที่เทสต์เหล่านี้ยึด) | หลักการใหม่ (rescope) | ที่มา |
|---|---|---|
| scale = float/fixed-point (×0.1, ×65536) | scale = base-2 shift ล้วน — **ห้ามบวก/ลบ** | TIMELINE_WORKING_MODEL §9 |
| hyperbolic = คณิตศาสตร์ (complex re/im, pythagoras, angle ratio) | hyperbolic = **passive scale-change log** (delta ∝ events) | AGENTS.md rescope |
| compression = address collision / unique-count | **MAP not COMPRESS** — เราไม่มองค่าข้างใน | AGENTS.md, §3 |
| address map array (lookup table) | coordinate = address — **ไม่มี hash/lookup** | AGENTS.md |
| สร้าง geometry (pyramid, projection, radius) | geometry = template combinatorial เท่านั้น | AGENTS.md working rule |

---

## ✅ ย้ายเข้า TIER1 แล้ว (ตรง model ปัจจุบัน — 8 ตัว, 2026-08-16)

| เทสต์ | ผ่าน | ทำไมตรง model |
|---|---|---|
| `test_6ico_tesseract` | 14/14 | โครงสร้าง combinatorial ล้วน: 20736 → 144 cubes → 18 tesseracts — ตรงกับแผน **18tes (future)** |
| `test_hyper_delta_format` | 13/13 | `hyper_delta.h` = integration target ของ scale log (rescope handoff) |
| `test_residual_space` | **59/59** (เขียนใหม่ 2026-08-16) | **เดิมเทสต์ผิด header!** — เทสต์เก่า include `hyperbolic_seek.h` (legacy angle math) ทั้งที่ชื่อ "residual_space". เขียนใหม่ให้เทสต์ `residual_space.h` จริง: freeze/thaw แบบ bond-addressed, **origin_key = birth pile identity** (เจอ bug: path empty-slot ตั้ง `origin_key = pogls_bond_key(piece)` ไม่ตรงกับ rs_verify → แก้เป็น `piece->geo_key` ครบ 3 path), **เสาเข็มห้ามขยับ** (ขยับ seed → bond_key เปลี่ยน → thaw/verify ล้มอัตโนมัติ), refreeze=update, tombstone/sweep/expire_by_origin, LRU eviction เมื่อ zone เต็ม, high-entropy, stats, invalid input, bond verify determinism+symmetry — คือฐานของ **ghost lift** (ขั้นต่อไป: wire ghost log → residual_space + envelope §11.6) |
| `test_breathing_fs` | ผ่าน | ตระกูลเดียวกับ test_bfs_* ที่อยู่ใน TIER1 อยู่แล้ว (storage layer) |
| `test_shell` | 14/14 | DWGLS shell + codec interface — โครงสร้างหลัก |
| `test_tess_header` | ผ่าน | `.tess` container header — container ที่ rescope จะ merge เข้า |
| `test_geofs` | **20/20** (ฟื้น 2026-08-16) | **เดิมล้ม 19/20 → สืบสวนแล้วเป็นเทสต์ stale ไม่ใช่ bug ในโค้ด**: T14 ส่ง `slot=500/20735` ซึ่งล้น field 8 บิต (`CELL_SLOT_BITS=8`) ของ bit-packed bijection → ฟิก slot เป็น 0..255 + เพิ่ม identity sweep `forward∘reverse` [0,16384) + OOB check (slot≥324 reject) + ปรับ create/delete → summon/unsummon (corrected architecture ตาม test_geo_fs.c) — เพิ่ม coverage ที่ test_geo_fs ไม่มี: entropy/tier, alloc/exhaustion, reuse, visualize, frame_enc |
| `test_tess_codec` | **14/14** (ฟื้น 2026-08-16) | **เดิมล้ม 11/12 → สืบสวนแล้วเป็นเทสต์ stale**: T4 คาด "cubes 12-23 = CODEBOOK" แต่ CODEBOOK ถูกตัดออกจาก classify แล้ว (2026-08-10 — พิสูจน์ว่า byte-per-index แพ้ raw เสมอ, dwgls_dynamic_codec.h) → แก้เป็น BITPACK (strategy จริง) + เพิ่ม DELTA check (cubes 24-35) + regression guard "ไม่มี cube ใดเลือก CODEBOOK" — container ที่ rescope ต้อง integration |

---

## 🏛️ Legacy — ผ่าน แต่เทสต์แนวคิดก่อน rescope (28 ตัว)

### กลุ่ม hyperbolic math (แทนที่ด้วย passive scale log) — 15 ตัว
เทสต์กลไก KIS↔Hyperbolic คณิตศาสตร์ (complex, angle, ratio, axis) — rescope เปลี่ยนนิยาม
"hyperbolic side" เป็น passive log + registry {id → home} 2B. **เทสต์ผ่าน ≠ ระบบปัจจุบัน**

`test_kis_hyper_3axis` · `test_kis_hyper_4d_distortion` · `test_kis_hyper_correct` ·
`test_kis_hyper_delta` · `test_kis_hyper_fast` · `test_kis_hyper_formula` ·
`test_kis_hyper_handoff_v2` (v1 ล้ม ถูกแทนที่ด้วยตัวนี้ — ดู ❌) · `test_kis_hyper_protocol` ·
`test_kis_hyper_pythagoras` (float-angle math — §9 ห้าม) · `test_kis_hyper_random_ratios` ·
`test_kis_hyper_ratios` · `test_kis_hyper_speed` · `test_kis_hyper_storage` ·
`test_kis_hyper_threshold` · `test_kis_hyper_unequal`

### กลุ่ม pyramid carrier — 3 ตัว
`geo_pyramid_carrier.h` เป็น component ที่ไม่ปรากฏใน model ปัจจุบัน (หน่วย = tesseract
8 cube × 144 + index frame) และนิยาม "hyperbolic = recurrence s(n+1)=s(n)×k" ขัดกับนิยามใหม่
(ยืนยันอีกทีก่อนเอากลับมาใช้)

`test_pyramid_3axis` · `test_pyramid_carrier` · `test_pyramid_real_gguf`

### กลุ่ม KIS 4D container — 2 ตัว
ใช้ `scale_factor` fixed-point 65536 (float scale — §9 ห้าม) + เก็บ address map array
(lookup table — AGENTS.md ห้าม)

`test_kis_4d_container` · `test_kis_4d_scale_all`

### กลุ่มอื่น — 8 ตัว
| เทสต์ | ผ่าน | หมายเหตุ |
|---|---|---|
| `test_codec_tess` | 10/10 | tess codec (dwgls_codec_tess.h) — ⚠️ ชื่อคล้าย `test_tess_codec` (ที่ล้ม) — ยังไม่ยืนยันตรง container รุ่น rescope |
| `test_dynamic_codec` | ผ่าน | เทสต์ CODEBOOK strategy — **CODEBOOK ถูกตัดออกจาก classify แล้ว** (bfs_persist: "พิสูจน์ว่าแพ้เสมอ") — dynamic codec ยังใช้เป็น value-axis optional ใน MDIM |
| `test_core_equation_decomposition` | ผ่าน | 128×162 = 144² = 20736 — ถูก **test_tess_sacred 27/27 (TIER1) แทนที่** แล้ว |
| `test_dual_balance` | ผ่าน | 3-axis dual balance — duality ico↔dodeca = §9 "ทางที่พัง" (geometry = template ไม่ใช่ตัวคำนวณ) |
| `test_kis_projection` | 21/21 | projection — §9 "ทรงกลม 2 ลูก → projection = many-to-one = collision = lossy" |
| `test_octant_selfinv` | ผ่าน | tesseract_container.h (Cayley octants) — container เดิมที่ rescope จะชนเข้ากับ (ยังไม่ทบทวน) |
| `test_z_axis_bridge` | ผ่าน | mirror_z ข้าม tesseract boundary (6ico) — โครงสร้างที่อาจกลับมาใช้ตอน implement 18tes |
| `test_geo_inference` | ผ่าน | GEO↔GGUF tensor mapping — ชั้นเก่า (Makefile คอมเมนต์ไว้ว่า cross-repo, hang on fail — รันผ่านใน env นี้) |

---

## ❌ ล้มจริง (เหลือที่ยังไม่ตัดสินใจ — 3 ตัว)

| เทสต์ | อาการ | การวินิจฉัย |
|---|---|---|
| `test_kis_hyper_pipeline` | exit 0 **หลอก** — ข้างใน 1/2 (roundtrip 13824 fail) | float-scale resolver drift — model ใหม่ **คาดการณ์ได้ว่าต้องล้ม** (§9) — ตัวอย่างของเทสต์ที่ "ผ่าน" ได้แต่ล้มจริงถ้าดู output → **แนะนำดรอป** |
| `kis_codec_v5_test` | T3/T4 FAIL + **hang** (exit 124) | v5 ถอดออกจาก timeline แล้ว (§9) — hang = เหตุผลที่ Makefile ตัดออก → **แนะนำดรอป** |
| `test_kis_hyper_handoff` | 70/138 roundtrip fail | **v1 ถูกแทนที่ด้วย v2 (ที่ผ่าน)** — ทิ้งได้ |

---

## 🗑️ ดรอปแล้ว (2026-08-16) — ตัดสินใจไม่ใช้ในระบบใหม่

| เทสต์ | เหตุผล |
|---|---|
| `test_kis_compression_pipeline` | เทสต์ "compression pipeline" (dedup 81x + KIS scale 2x) — **ขัดกับ model ปัจจุบันโดยตรง**: §3 "ถ้า dedupe เหลือตัวเดียว ตอบไม่ได้ว่าตำแหน่งไหนมีค่า" (identity แตก) + float scale (§9 ห้าม) + roundtrip ยอมรับ ≥99% (lossy) + include path พัง |
| `test_kis_lossless_pipeline` | hyperbolic complex creation-points (re/im + float ratio) — **ถูกแทนที่**โดย registry {id→home} 2B (test_tess_scale_dedup / test_tess_registry_gate ใน TIER1) + include path พัง |
| `test_hyper_scale_ratio` | angle-math scale (y=2x / y=x÷2) — **ล้ม 2/5 = หลักฐานว่า angle-math ใช้ไม่ได้** (§9: position-dependent transform → ค่าแตก) — model ใหม่ = base-2 shift + passive path log |
| `test_zerocopy` | mmap zero-copy รุ่นเก่า (gguf_index API เก่า + ต้อง GGUF จริง) — **ถูก `test_geo_zerocopy` (TIER2, ผ่าน) แทนที่** |
| `kis_codec_v6_test` | v6 codec **ถูก `kis_codec_v6_standalone_test` (TIER1, ผ่าน) ครอบแล้ว** + ใช้ GGUF_File API เก่า + ต้องโมเดลจริง — ถ้าจะเทสต์ v6 กับ GGUF จริงต้อง rewrite |

---

## ⏸️ พักไว้ (deferred — ตัดสินใจ 2026-08-16)

| เทสต์ | เหตุผล |
|---|---|
| `test_rail_hub` | กลุ่ม rail — API เปลี่ยน (geo_rail_hub_open → geo_rail_open) — **ผู้ใช้ตัดสินใจ: ซับซ้อน ยังไม่ fit ตรงๆ** — เป็นกลไก "หยุดเวลาแล้วประกอบไฟล์ใหม่" ต้องใช้ตอนมี **residual space + jet puller** → พักไว้ก่อน |
| `test_geo_fs_bench` | เรียก `geos_idle_compress` ที่ยังไม่มีใน geofs_core.h (ฟีเจอร์ idle-compress ยังไม่ implement) — พักจนกว่าฟีเจอร์จะมา |
| `test_real_gguf_microscope` | microscope บน GGUF จริง — พัก (กลุ่ม observation — รอ infrastructure) |

---

## 🔨 Build ไม่ผ่าน (เหลือ 3 ตัว)

| สาเหตุ | เทสต์ |
|---|---|
| **API เก่า** (`GGUF_File`/`gguf_open(path)` ไม่มีใน gguf_reader.h รุ่นใหม่) | `kis_map_roundtrip` · `test_qwen3_microscope` |
| ต้องการไฟล์ภายนอก | `test_gguf_graft_llama` (llama.h — มี target `make graft-llama` แล้ว) |

**geo_jump_explore / geo_jump_real_test — build ได้แล้ว** ด้วย
`gcc -O2 -DGEO_JUMP_INLINE -I../../FGLS_new/collection/geo_jump_module/include`
(รันผ่าน exit 0 — cross-repo explorer ต้องมี `I:/FGLS_new`; คอนเซปถูกดูดเข้า
`core/geo_sync_bridge.h` แล้ว: test_geo_sync_bridge 7/7 + test_tess_geo_jump_walks 15/15 ใน TIER1)

---

## 🔌 ต้องมีไฟล์ GGUF/โมเดลจริง (11 ตัว — ส่วนใหญ่เป็น demo ไม่ใช่ test)

> โมเดลจริงมีอยู่ที่ `I:/model/` (Qwen2.5-0.5B, Qwen3-0.6B, LFM, Kokoro) —
> แต่เทสต์/demo เหล่านี้ส่วนใหญ่ใช้ **path เก่าที่ฮาร์ดโค้ด** (เช่น `I:/model/qwen25_q8.gguf`)
> หรือ API เก่า → ต้องอัปเดต path/API ก่อนถึงจะรันได้

`dyn_real_gguf` · `gcube_geometry_pipeline` · `geometry_address_demo` ·
`gguf_geometry_complete` · `safetensors_geometry_demo` · `test_geo_sid_loader`
(หา `I:/model/Qwen2.5-0.5B-Instruct-Q8_0.geo`) · `test_geo_sid_verify` (skip เงียบๆ) ·
`test_kis_hyper_real_gguf` (skip: "Tensor not found") · `test_kis_mirror_symmetry`
(รับ arg `model.gguf`) · `test_kis_v4_real_gguf` · `kis_real_gguf_test`

---

## 🔬 Explorer / benchmark / demo (8 ตัว — ไม่ใช่ verification test)

ใช้สำรวจ/วัดผล — มีคุณค่าเป็นงานวิจัย แต่ห้ามตีความว่า "ระบบผ่าน"

`capo_seeker_mix` (Capo+Seeker mixing) · `frame_index_demo` (demo frame-as-index —
แนวคิดใกล้ model ใหม่ แต่เป็น demo ไม่ใช่ test) · `hyper_pierce_test` (cost 12x vs frame_seek) ·
`mod_order_sweep` (stride order บน 20736) · `no_trig_benchmark` (LUT+Cayley vs trig) ·
`radius_access_test` (radius vs angle) · `spike_offset_sweep` (offset sweep) ·
`trig_alternatives_test` (LUT roundtrip 0% — ข้อสรุปคือ LUT แพ้)

---

## ⚠️ ข้อควรระวัง

1. **exit code หลอกได้** — `test_kis_hyper_pipeline` exit 0 แต่ข้างใน FAIL ครึ่งหนึ่ง. ถ้าจะฟื้นเทสต์ใด → ต้องเช็ค output จริง (ตัวอย่าง: `test_geofs`/`test_tess_codec` ที่ฟื้นแล้ว ดู output ก่อน)
2. **ชื่อคล้ายกัน** — `test_codec_tess` (ผ่าน, legacy) vs `test_tess_codec` (ฟื้นแล้ว 14/14, TIER1) เป็นคนละไฟล์ — ระวังสับสน
3. **run_tests.ps1 มี TIER1 list ของตัวเอง** — ล้าสมัย (มีเทสต์ที่ Makefile ตัดออก, ไม่มีเทสต์ที่เพิ่มใหม่ 8 ตัว) — ถ้าใช้ควร sync กับ Makefile
4. ถ้าจะฟื้นเทสต์ legacy → ต้อง **rewrite ตาม model ใหม่** ก่อน ไม่ใช่เอาเข้า TIER1 ตามเดิม
   (ตัวอย่างที่ rewrite แล้ว: `test_kis_hyper_handoff_v2` แทนที่ v1)
