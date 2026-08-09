# Pyramid Swing Carrier v1 — ผลทดสอบ (Aug 10)

## สรุป
| Component | สถานะ |
|---|---|
| `core/geo_pyramid_carrier.h` | ✅ ใหม่ — carrier pyramid swing |
| `tests/test_pyramid_carrier.c` | ✅ **30/30 PASS** (-Wall -Wextra 0 warnings) |
| `make test` | ✅ 25/25 (22/22 TIER1 + 3/3 TIER2) |
| Coverage 20736 | ✅ Python verify: 20736/20736 ครบ |

## แนวคิดที่พิสูจน์ (จาก user design)
```
square (4) → spike → pyramid (5) → seal → square (4) → loop
```
- **self-dual**: V=5 == F=5 — ไม่ต้องมี dual pair (ต่างจาก dodeca↔icosa)
- **swing 2-cycle**: sealed→spiked→sealed = identity
- **recurrence**: `stateA(n+1) = stateB(n) + 9` — pair = 4+5
- **octahedron fill**: 2 pyramids ก้นต่อก้น = V6/F8/E12 — honeycomb no-gap
- **no-zero infinity**: 4608 layers → 20736 เต็มเป๊ะ; address เติบโตไม่จำกัด

## เลขจริงที่ลงตัวเอง (ไม่ได้ฝืน)
```
20736 / 9 = 2304 = 48²        ← tower width เดิม (48 addr)
4608 layers (2304 pairs × 9)  เติม 20736 พอดี 0 overhang
90336 = 12⁴ = 20736 (ท้ายสุดของ pyramid ตาม parity)
```

## KIS timeline ที่เหลือแก่น
- เดิม: dodeca(12) ↔ icosa(20) — 2 shapes + geometry เฉพาะทาง
- ตอนนี้: pyramid(4↔5) — 1 shape, self-dual, minimal state
- **ข้อแม้**: pyramid ไม่ใช่ sphere → subdivide ได้จำกัดถ้าต้อง infinite growth
  (จำได้จากหัวข้อ "Pyramid (Tetra) vs Ico Trade-off" — แต่ KIS swing ไม่ต้อง sphere
  คือ carrier เดิน timeline ไม่ใช่ grow subdivision)

## ถัดไป (ตาม T8)
- Hyperbolic: s(n+1) = s(n)×k — k=9 per pair ใช้ได้แล้วใน test
- ต่อได้: capo/octant ภายใน pyramid addressing, ผูกกับ frame_seek

## Impact Analysis (Aug 10) — ผลกระทบต่อระบบเดิม
**ZERO — pyramid เป็นเลเยอร์แยก ไม่แตะ core เดิม:**
- 0 macro clash — compile รวมกับ codec_v4/frame_seek/projection/tess/param_grid/gguf/
  gear_lock/fibo_spine ใน TU เดียวผ่าน (0 error, ทดสอบจริง)
- 0 ไฟล์เดิมถูกแก้ — เพิ่มแค่ geo_pyramid_carrier.h + 3 tests
- make test 25/25 regression ผ่าน
- **ข้อควรระวัง**: ยังไม่แทนที่ kis_layer (ICO 20 / DEC 12) หรือ dwgls_codec_* (10 ไฟล์ 20736)
  — ถ้าจะแทนที่จริง = แตะ container/format ชั้นลึก ในจุดนั้น impact จะเกิด