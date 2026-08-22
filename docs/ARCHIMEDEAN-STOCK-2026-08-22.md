# ARCHIMEDEAN STOCK — RID · Snub · RID-Serve (2026-08-22)

> สถานะ: **ปิดสาขาแล้ว — เก็บเป็น stock** ก่อนลุยท่อตรง (GeoFS/llama graft)
> ทุกอย่างในเอกสารนี้ proven บน oracle จริง / GGUF จริง ไม่มี expected แบบ circular

## 1. RID — Rhombicosidodecahedron จาก int dodeca (`tools/geo_archimedean_test.c`)

- Labeling **R(u,F) = (vertex, face)** — ห้าม directed-edge labeling (ผิด, เรียนรู้แพง)
- V=60 · E=120 (=GEO_COMP_SPIKE_120) · F = 20△+30□+12⬠ · degree 4 · Euler χ=2 — ALL ORACLE PASS
- Adjacency = 2 กฎเท่านั้น: pent-side (consecutive in face F) + tri-side (other faces G ∋ u)
- int dodeca coords: S=104, H=64, P=169 (rational φ = 13/8 ×104)

**RID = interchange hub**: tri→icosa / square→edge-world / pent→dodeca

## 2. Snub dodecahedron = chiral diagonal split (`tools/geo_snub_test.c`)

- RID squares (30) split ด้วย diagonal 1 เส้นต่อ square → 80△+12⬠ = **F=92 = GEO_GOLDBERG_92**
- V=60 · E=120+30=150 · Euler 2 ✓ · vertex figure 3.3.3.3.5
- **วิธีพิสูจน์ที่ถูกต้อง: constraint solve ไม่ใช่ local rule**
  - naive rule ("succ-face เลือก diagonal") FAIL — pentagon วงคี่ทำ coverage สลับ 2/0 ไม่ยูนิฟอร์ม
  - ระบบ parity: vertex ทุกตัวต้องเป็น endpoint ของ diagonal พอดี 1 ครั้ง (จาก 2 squares ของมัน)
  - union-find with path-parity → system consistent, **solution มีแค่ 2 ตัว bitwise-complement**
  - = enantiomorph pair (ซ้าย/ขวา) = chirality โดยนิยาม
- Gate ที่ผ่าน: deg5 uniform · exactly 80 triangles (ไม่มี spurious) · per-vertex tri incidence 4 · 12 pents intact · M1: mutants 30/30 แดง

## 3. Data-plane (`tools/geo_rid_serve.c`) — PASSED บน GGUF จริง

```
model : Qwen2.5-0.5B-Instruct-Q8_0.gguf · 675.7 MB · 291 tensors
bake  : part f → layer f/60 · slot f%60 (slot = R(u,F) label จาก int dodeca)
verify: 5305 parts byte-identical · lossless
views : pent(12×5 face-cycle) / tri(20×3 vertex-star) / snub(30×2 diag-matching)
        ทุก view = structural bijection over 60 slots
XOR   : pent = tri = snub = 516e6420ec09614d == source ✓✓✓
damage: flip 1 byte → localize slot R(v,f)+layer (per-part XOR) → re-bake 1 part → lossless
```

**ความหมาย (multi-view semantics)** — ตอบโจทย์ "จุดเชื่อมหลายมุมมอง":
- ช่องเชื่อม = hub; ข้อมูลผ่าน address เดียวแต่ summon ได้หลายภาษา
- verify = views ต้องเห็นพ้อง (XOR match); localize = per-part checksum; restore = summon ผ่าน address เดียว ไม่ต้อง copy สำรอง
- ขีดจำกัดจริง: byte ต้นทางพัง → ทุก view เห็นพร้อมกัน ต้องมี checksum layer (CRC-64/wang digest) ต่างหาก

## 4. STOCKED (พักไว้ก่อน — ห้ามเปิดก่อนท่อตรงเสร็จ)

| สาขา | สาระ | จุดเริ่ม |
|---|---|---|
| สาขา | สาระ | จุดเริ่ม |
|---|---|---|
| ~~ภาษาที่ 4: Fibonacci/circle view~~ | **OPENED+PASSED (2026-08-23)** — golden-spiral stride F(7)=13 mod 60 · probe 11/11 (`tools/hosoya_view_probe.c`: Euclid gcd oracle + inverse 37 + Hosoya cell T(6,0) + φ-convergent 13/8 + mutation red) · real-GGUF 5156 parts lossless via `gguf_roundtrip` view "hosoya" | เสร็จแล้ว |
| ~~snub-mode switch~~ | **OPENED+PASSED (2026-08-23)** — mirror enantiomorph runtime toggle: complement ทุก diagonal bit → view "snubR" · snubL≠snubR 30/30 squares · full GGUF lossless ทั้งคู่ (gguf_roundtrip ×5 languages: pent/tri/snubL/snubR/hosoya) | เสร็จแล้ว |
| Zeckendorf decomposition | ไม่มีใน repo เลย — word→Fib index | probe ใหม่ ถ้าจำเป็น |
| circle-config catalog (degree 4/5/6) | classify table ↔ geo_cell_classify / tied_dedup | mapping study |
| blueprint-compression (7 centroids/block) | validated แต่ encode 60B/block แพ้ Q8_0 raw 34B/block — ใช้เป็น observation layer เท่านั้น | — |

## 5. ท่อตรง (MAINLINE) — GeoFS/llama graft

1. wire RID slots เข้า DtSlotRegion / graft belt path
2. llama.cpp อ่านจริงผ่าน slot addressing (link b9733 llama.dll — llama-server slot-save broken)
3. gate: inference output ต้องเทียบเท่า baseline ก่อน graft
