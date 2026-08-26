# FAN24 GEAR SYNC — เฟืองเชื่อมสองโลก (KIS × Hyperbolic)

> **สถานะ: PROVEN (2026-08-26)** — probes ALL GREEN + mutation-red verified
> นี่ไม่ใช่ภาษาที่ #10 — เป็น **กลไก sync** ระหว่างฝั่ง KIS (timeline/int) กับฝั่ง Hyperbolic (scale-change log)

---

## 1. แนวคิด

```
        ฝั่ง KIS                        ฝั่ง HYPERBOLIC
   8 cubes (tesseract)              scale-change log
   อ่าน remainder mod 8              อ่าน remainder mod 3
        │                                  │
        └───────── ring-24 = ขอบเฟือง ─────┘
                  24 = 8 cubes × 3 axes
```

- **24 มาจากไหน:** 8 cube ของ tesseract บน KIS × 3 axis → `24 = 8·3` หลั่งลงมาเป็นขอบเฟือง
- **อัตราทด (user):** การแยกตัวประกอบคู่ของ 24 — `(8,3)` `(6,4)` `(12,2)`
  ฝั่งละตัว หมุนสวนกัน 1 ฟัน = 1 ก้าว `s`
- **CRT theorem:** `s ↦ (s mod D, s mod d)` bijection บน Z_{D·d} ⟺ `gcd(D,d)=1`
  → **มีคู่เดียวที่ lossless: (8,3)** — ตรงกับโครงสร้าง KIS พอดี (ไม่ใช่บังเอิญ)

## 2. ผลพิสูจน์

### 2.1 Mesh identity — `tools/fan24_gear_probe.c` (G1–G7, 12/12)

| ข้อ | ผล |
|---|---|
| G1 | `s ↦ (s%8, s%3)` bijection ครบ 24 cells — lossless |
| G2 | inverse brute-force: ทุก (cube,axis) กลับเป็น `s` เดียว |
| G3 | fold `s→24−s` = negation **ทั้งสองเฟือง** = gears counter-rotate |
| G4 | ก้าวเฟือง cube นิ่ง = {8,16} (= inscribed △ strides F5f) · axis นิ่ง = ทวีคูณ 3 (รวม s=12 diameter hinge) |
| G5 | fence: (6,4),(12,2) image = lcm = 12 < 24 = half-density LOSSY เห็นชัด |
| G6 | `s=24 ≡ (0,0)` home — fan เริ่ม s=1 (F9) |
| G7 | chord partition C(24,2)=276 = 23×12 · contact/รอบ = 11 คู่ mirror + 1 diameter |

### 2.2 Gear-synced delta log — `tools/fan24_gear_sync_probe.c` (M1–M7, 10/10)

ฐานเทียบ: `tests/test_tess_scale_log.c` — event เดิม `{from:u8, to:u8}` = 16 บิต

```
Δ = (to − from + 144) % 144  ;  Δ = 24q + r  ;  r ≡ (dc mod 8, dx mod 3)
event = { q:3b, dc:3b, dx:2b }  =  8 บิต/event
ฝั่ง KIS  อ่านเฉพาะ dc (ล้อ cube) · ฝั่ง Hyper อ่านเฉพาะ dx (ล้อ axis)
→ sync โดยไม่มี shared clock — แต่ละฝั่งถือ remainder ตัวเอง
```

| ข้อ | ผล |
|---|---|
| M1 | encode→decode คืน w-chain ตรงเป๊ะ (69 hops: fixed 5 + random 64) |
| M2 | ล้อ (c,x) อัปเดตจากฟิลด์ dc/dx ล้วน == ground truth ทุกก้าว |
| M3 | CRT inverse brute force: 24 cells unique ครบ |
| M4 | read @ final scale ผ่าน gear-log replay → **lossless 1008 slots** · ไม่ replay → 1008/1008 mismatch |
| M5 | ขนาด: **8b vs 16b (50%)** · RIM mode (Δ คูณ 24 ทั้งหญิง) → **3b/event** (header ประกาศโหมด 1b โปร่งใสทั้งสองฝั่ง) |
| M6 | fence จริง: (6,4) collision 12 pairs (0~12 ฯลฯ) · (8,3) ศูนย์ sweep เต็ม |
| M7 | Δ=0 → all-zero (home tooth ไม่กินพื้นที่ใน log) |

### 2.3 Mutation check (เทส fail ได้จริง)

- gear_probe: แงะ G3 (ลบ negation ล้อ d) → 11/12 RED exit=1
- gear_sync_probe: แงะ decoder (ใช้ dc ตรง แทน CRT inverse) → 8/10 RED exit=1

## 3. ไฟล์ในระบบ

| ไฟล์ | บทบาท |
|---|---|
| **`core/fan24_gear.h`** | ★ WIRED (2026-08-26) — event format เป็น core header: `fg_crt` (closed form), `fg_enc/fg_dec`, `FGLog` + FREE/RIM mode, `fg_reconstruct` backward walk · ★ FULL FIELD (2026-08-26): `fgx_enc/fgx_dec` + `FGXLog` + `fgx_reconstruct` บน [0,20736) — ฟันเดิม q ขยาย 6b→10b |
| `tests/test_tess_scale_log_gear.c` | TIER1 — rerun T3–T8 บน gear format (19/19) + oracle อิสระ O1–O6, mutation-red ยืนยันแล้ว |
| `tests/test_tess_gear_full.c` | TIER1 — full field [0,20736): X1a/X1b roundtrip (tooth-exhaustive + 200k random) · X2 hand event q=863,dc=7,dx=2 · X3 translation-invariance · X4 bridge bijection · X5 RIM replay · X8 mutation drill same-length swap+restore-proof (10/10) |
| `tools/gear_microscope.c` | ★ MICROSCOPE end-state B (2026-08-26): observation on the gear skeleton — zoom-out EVENT→TOOTH→RIM→FIELD, wheel census + entropy + RIM share; M1–M6 oracle (uniform = exact residue expectations 4σ; rim drift = entropy 0 vs uniform 4.583); จับ LCG low-bit bias ได้ตั้งแต่รันแรก |
| `core/geo_ghost_gear_adapter.h` | ★ SWAP (2026-08-26): gear wire ลง core จริง — `ghost_gear_lift/expire` พัน `ghost_lift/ghost_expire`, flag bit 0x08 + wire side-table, replay block-scoped จาก Δ-wire, serialize ต่อท้าย GHST (backward compatible) |
| `tests/test_ghost_gear_adapter.c` | TIER1 — A1 lift==core bond_key · A2 hand event · A3 wire-only replay · A4 bond/Delta-only · A5 seal · A6 persist+mismatch −2 · A7 GHST prefix (12/12) |
| `docs/fan24_start.html` | visualizer — ขยับ start + aa อิสระ (census label s=4/8/12, tie-break s=12 deterministic) |
| `tools/fan24_probe.c` | Construction C: vertex-fan กฎ F1–F9 (census/equilateral/choice/chord/apex/slot/stroke/fence/mutation) |
| `tools/fan24_gear_probe.c` | Construction G: mesh identity G1–G7 |
| `tools/fan24_gear_sync_probe.c` | Construction GS: gear delta-log vs baseline M1–M7 |
| `tools/ghost_gear_probe.c` | consumer probe (2026-08-26): route ของ GhostLog (`geo_ghost_lift.h`) ผ่าน gear wire — 7/7 + mutation-red · `{from,to}`=2B → **1B/event** (50%) · bond (block,from) ไม่ถูกแตะ (P4: wire อยู่บน Δ ล้วน) — ตอนนี้ SWAP แล้วผ่าน adapter จริงใน test_ghost_gear_adapter |

## 4. กฎที่ล็อกแล้ว

1. **aa ∈ {3,4,6,8,12}** — divisor ของ 24 · aa=24 ≡ host ring (degenerate ตัดทิ้ง) · {5,7,9,10,11} fail-loud
2. **slot formula** `24+23(aa−2)`: aa=3→46 · 4→70 · 6→116 · **8→162 (sacred dual-place)** · 12→254
3. **split (8,3) เท่านั้น** ที่ใช้เป็น addressing — split อื่น = visible fence
4. **hinge s=12**: apex สองฝั่งรัศมีเท่ากัน → ต้อง tie-break ด้วยทิศ chord (deterministic) ห้าม FP noise

## 5. ขอบเขต / งานต่อ

- ✅ **Wired (2026-08-26):** `core/fan24_gear.h` + `test_tess_scale_log_gear.c` (19/19)
  - rerun T3–T8 บน format ใหม่ → lossless เท่าเดิมทุกข้อ
  - FREE = {q:3b,dc:3b,dx:2b} = ครึ่งของ baseline (16b→8b/event)
  - RIM = 3b/event; wire bytes (header 12B รวม): n=68 → 38B (27% ของ baseline), n=1000 → 19% (amortize → floor 18.75%)
  - **ENTER ANYWHERE จริง**: log เก็บ Δ ล้วน ไม่มี absolute w แม้แต่ seed — reader ถือ current_w ตัวเองแล้ว `fg_reconstruct` เดินย้อนหา append scale เอง (T7b + neg-ctrl + T7c late-joiner)
  - home tooth Δ=0 ไม่ถูก push ลง log (encoder skip)
  - mutation drill: พัง fg_crt → 7 checks RED (O1,O2,T6,T7,T7b,T7c,T8e)
- ✅ **Full field (2026-08-26):** rim เต็ม [0,20736) — `test_tess_gear_full` 10/10
  - W ≡ q·24 + s · q ∈ [0,864) (10b) · FREE 15b vs baseline 28b (53.6%) · RIM 10b (35.7%)
  - bridge `fg_to_full(frame,local)` bijection 144×144 → bitmap-proven
  - events TRANSLATION-INVARIANT: wire = f(Δ) เท่านั้น, position อยู่ที่ reader
- ✅ **Microscope end-state B (2026-08-26):** `gear_microscope` 6/6
  - zoom-out EVENT→TOOTH→RIM→FIELD บน skeleton เดียวกับ wire
  - uniform noise: entropy 4.583/4.585 bits; rim drift: 0.000 bits + RIM 100%
  - จับ LCG low-bit bias จริง (ฟันเดียว 19200/19200) → แก้ RNG mix high-bits
- ✅ **SWAP adapter (2026-08-26):** `geo_ghost_gear_adapter.h` + `test_ghost_gear_adapter` 12/12
  - ghost_lift/expire ถูก wrap ด้วย gear wire append/seal จริง
  - replay block-scoped จาก Δ-wire · serialize ต่อท้าย GHST (reader เก่าใช้ได้)
  - corruption → −2 loud; bond identity ไม่ถูกแตะ (A4)
- rim local [0,144) ↔ full [0,20736): bridge frame-invariant (144 ≡ 0 mod 24)

---
*Proven 2026-08-26 · feat/geo-native-fs · oracle อิสระทุกข้อ (brute force / number theory) ไม่มี expected จาก implementation*
