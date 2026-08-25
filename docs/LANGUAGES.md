# LANGUAGES — 8 ภาษาบน Carrier เดียว
## 2026-08-24 · branch feat/geo-native-fs

> **นิยาม:** ภาษา = **provable bijection + grammar + oracle** บน carrier เดียวกัน
> — ไม่ใช่ codec (bytes นิ่งเป๊ะ) ไม่ใช่ walk formula (โครงสร้างอยู่นิ่ง ไม่มี walker)
> ภาษา**ค้นพบ** ไม่ได้ออกแบบ (พิสูจน์: คู่ snubL/snubR enantiomorph เกิดจาก parity
> solve ไม่มีทางออกแบบโดยบังเอิญได้)
>
> **Carrier:** RID slots 60 ช่อง (`addr = layer·60 + viewpos(slot)`)
> — rhombicosidodecahedron V=60/E=120/F=62 จาก int-dodeca labeling R(u,F)=(vertex,face)

---

## ตารางรวม 8 ภาษา

| # | ชื่อ | Grammar (หัวใจคณิตศาสตร์) | Oracle อิสระ | Probe |
|---|---|---|---|---|
| 1 | **pent** | dodecahedron face-cycle: 12 faces × 5 consecutive | Euler χ=2, adjacency 2 กฎ | `geo_archimedean_test.c` |
| 2 | **tri** | icosahedron dual: 20 vertices × 3 star | degree-5 vertex figure | `geo_rid_serve.c` |
| 3 | **snubL** | RID squares split diagonal (parity solution A) | union-find path-parity, F=92=GEO_GOLDBERG_92 | `geo_snub_test.c` |
| 4 | **snubR** | mirror enantiomorph (complement diagonal bits) | bitwise complement ≠ L, 30/30 squares | `geo_snub_test.c` |
| 5 | **hosoya** | stride F(7)=13 mod 60 (multiplicative permutation) | gcd(13,60)=1 · inverse 37≡13⁻¹ · T(6,0)=13 · φ≈13/8 · mutation red | `hosoya_view_probe.c` |
| 6 | **zeck** | Zeckendorf code reversed-bits ordering | existence+non-consec 1..4000 · uniqueness brute-force · mutation red 3998/4000 | `zeckendorf_probe.c` |
| 7 | **pascal** | zig-zag diagonal stream → `A(n)=Σ(−1)^k C(n−k,k)` period-6 | Fibonacci identity · period-6 {1,1,0,−1,−1,0} | `pascal_zigzag_probe.c` |
| 8 | **hexagram** | hex-distance rank (axial coords, x+y+z=0) | hexagon ≡ cube-in-2D · MacMahon count 20 | `hexagram_cubes_probe.c` |

ทุกภาษา = structural bijection บน slot เดียวกัน → **XOR ทุก view ต้องเท่ากับ source**
(verify แล้วบน GGUF 675.7MB · 5156 parts · commit `76aee0c`, Colab T4 `99109c4`)

---

## เหตุผลเชิงสถาปัตยกรรม — ทำไมต้องหลายภาษา

**1. Multi-view summon (จุดเชื่อมหลายมุมมอง)**
ข้อมูลผ่าน address เดียวแต่ summon ได้หลายภาษา — consumer ต่างกันอ่านต่าง pattern
โดยไม่ copy (llama sequential / microscope square-tile / router stride)

**2. Verify = consensus**
views ต้องเห็นพ้อง: XOR(pent)=XOR(tri)=...=source — ถ้า view ไหนไม่ match
= mapping ผิด จับได้ทันทีโดยไม่ต้อง decode

**3. Damage localization + surgical restore**
flip byte → per-part checksum localize → re-bake **เฉพาะ part เดียว**
(R3 drill: part 2578 → restore → lossless) — summon ผ่าน address เดียว ไม่ต้อง backup

**4. Dedup ข้ามภาษา**
chunk duplicate 21.6% ของ Qwen payload (`8ee6aac`) — structure tied detection
ทำงานได้เพราะมองหลายภาษาพร้อมกัน

**5. Chirality = free state bit**
snubL/snubR ให้ runtime toggle ฟรี — 1 bit สลับ enantiomorph ทั้งระบบ lossless

---

## Gate การเป็นภาษา (ทุกภาษาต้องผ่าน)

```
G1 bijection sweep ครบ domain (counting/bitset)
G2 inverse มีจริง (deterministic roundtrip)
G3 int-only (ห้าม float — Elser-Sloane lesson: order-5 rotation impossible)
G4 oracle อิสระ (hand-computed / number theory / brute force — ห้าม expected จาก impl)
G5 mutation red (แก้ logic 1 บรรทัด → probe ต้องแดง)
G6 real-GGUF roundtrip lossless (byte-identical ทุก part)
```

## ความสัมพันธ์ข้ามภาษา

- **13 = F(7)** — stride hosoya (ภาษา 5) = hexagram cell count (Metatron topology, สาย 4)
- **pascal period-6 ↔ hex face-cycle** — ทั้งคู่วนบน {6}
- **snubL/snubR** — enantiomorph pair จาก parity solve global
- **zeck W₆ = W₅∥W₄** — Fibonacci recurrence ในรูป word-concatenation

## ที่มาของภาษา 7–8 (drawing-derived)

สูตร 7–8 decode จากภาพวาดผู้ใช้โดยตรง — รายละเอียด:
`docs/DRAWING-DERIVED-STRUCTURES.md` (สาย 1 zigzag→pascal · สาย 2 rhombus grammar→hexagram
· สาย 3 seven-construction→index-frame · สาย 4 ring-24 gearbox)

## Next candidates (ยังไม่เปิด)

| ตระกูล | ตัวอย่าง | หมายเหตุ |
|---|---|---|
| Archimedean เหลือ | truncated icosa, icosidodecahedron | 13 ตัว ใช้แค่ 2 |
| Catalan dual | ★ rhombic triacontahedron (dual ของ RID) | mirror semantics ใหม่ |
| A₅ group | Cayley action 60 elements = 60 slots | natural fit แต่ต้อง probe |
| sequences | Lucas/Pell/Jacobsthal/tribonacci/Thue-Morse/quadratic residues | int-only check ก่อน |
| Euclidean rhythm | E(k,n) necklaces | bijection โครงสร้างเดียวกับ stride views |

**เกณฑ์เลือก:** ไม่ใช่ "เก็บให้ครบ" — แต่ "ภาษาไหนให้ property ที่ขาด"
(dual-of-RID ให้ isolation semantics ต่างจากทุก view ปัจจุบัน = มีค่ากับ microscope)

## Sources

- `docs/ARCHIMEDEAN-STOCK-2026-08-22.md` — RID/snub/hosoya/zeck/circle-config proofs
- `docs/HANDOFF-RID-PIPE-2026-08-23.md` — direct pipe 4 layers
- `docs/DRAWING-DERIVED-STRUCTURES.md` — pascal/hexagram origins
- `docs/REPORT-COLAB-DWGLS-2026-08-24.md` — 8-view cloud verification
