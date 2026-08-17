# HANDOFF — 2026-08-17: Entropy-Bound Verdict + Fibo Clock Checkpoint (test + sweep)

Session ที่วัด "MAP not COMPRESS" ด้วยข้อมูลจริงจนได้คำตอบเป็นตัวเลขทุกประเภท:
Q8 weights / PCM wav / whisper mel / beam addressing / silk-screen — ลงเอยด้วย
การพิสูจน์แนวคิด user เรื่อง deterministic field + checkpoint + tick วนกี่รอบก็ได้
(test 22/22 + sweep 27/27)

## 🎯 คำตัดสินหลักของวัน (ทุกข้อวัดด้วยของจริง)

1. **ระดับค่า (values) ปิดแล้วทุกหมวด** — Q8 ถึง bound ~96% (ค่า 7.65/8, scale 15.3/16,
   รู้ max|q| ก็ลดแค่ 0.2-0.3 bits), PCM delta 1.5×, whisper mel bound แล้ว (3.68 b/v,
   delta แย่กว่า raw 0.55×), silk-screen blocks/map = 1.0 (rot+rev ไม่มีเนื้อให้จับ)
2. **ของจริงที่ dedup ได้ = ระดับโครงสร้างไฟล์** — tied embeddings: Qwen2.5-0.5B
   `output.weight == token_embd.weight` byte-identical **137 MB = 35% ของไฟล์**
3. **capability ชนะ ไม่ใช่ compression** — N scale views จาก 1 copy (5B/event) —
   นี่คือสิ่งที่ "MAP" มอบให้ ไม่ใช่การบีบอัด

## 🛠️ สิ่งที่สร้าง (6 ตัววัด + 1 เทสต์ + 1 sweep — ทั้งหมด manual/TIER1 ยกเว้นที่ระบุ)

| layer | ไฟล์ | วัดอะไร / ผล |
|---|---|---|
| silk screen | `tools/silk_screen_scan.c` (`make silk_scan`) | canonicalize rot+rev → นับ unique maps บน Q8_0 จริง 4 โมเดล (40M blocks): blocks/map = **1.0** ทุกตัว, ratio 0.97-0.99× แพ้ raw → **ปิด block-level**; เจอ tied embeddings |
| entropy | ใน `silk_screen_scan.c` | H(value)=7.65/8, H(scale)=15.3/16, H(scale\|maxq-bin)=15.1 — conditional ลดแค่ 0.2-0.3b; **🐛 เจอ int8 −128 → abs=128 → index เกิน array → segfault** (Qwen3 มี −128, SmolLM ไม่มี) |
| beam audit | `collection/beam_addressing/` (อ่านอย่างเดียว) | claim 0.59× Q8_0 → **ล้ม: roundtrip ตัวเอง 0/100 PASS, avg_err 18%** — decode ได้แค่ sorted order (lossy + ทำลายลำดับ); `beam_7bit.c` ซื่อสัตย์; beam addressing เอง (336M ops/s, 0 collision, 0 bytes) ใช้ได้เป็น addressing |
| perm cost | `tools/beam_cost_probe.c` | sort ประหยัด 106 bits แต่ permutation บีบไม่ได้ (117.0 vs uniform 117.7) → **NET −11 bits/block ทุกโมเดล** — "cost ไม่คุ้ม" พิสูจน์เป็นตัวเลข |
| wave/mel | `tools/wave_delta_probe.c`, `tools/mel_delta_probe.c` | PCM: podcast 12.97→10.64 (1.5×), TTS 11.93→10.33 (1.5×); mel quantized → delta แพร่ (0.55×) — representation เปลืองเท่านั้นที่ได้ |
| two-gap | `tools/two_gap_fill.c` (`make two_gap_fill`) | transform gap (ฟรี, replay 5B) + detail gap (residual = entropy): sine 2Hz 1.70× (smooth จริงชนะ), real wav 1.05× **แพ้ entropy-raw 1.34×**, random 0.70× จ่ายเพิ่ม |
| fibo checkpoint | `tests/test_fibo_checkpoint.c` (TIER1) | **22/22**: spine wrap (tick 11→bridge→13), round ใน bond (เสาเข็มห้ามขยับ) / to_scale ใน route, checkpoint @round 72 → restart → reload → lossless ข้ามรอบ, telescope 137 steps + ข้ามรอบ 1 route, overhead **+1.05%** (log 5B/route ∝ events) |
| fibo sweep | `tools/fibo_checkpoint_sweep.c` (`make fibo_sweep`) | **27/27** ทุก custom config: ตาราง (pipes×ticks), สนาม (cycles 8..255), ระยะ (dist, wrap วนข้าม 0), ปริมาณ (chunks×size ≤64KB), รูปแบบ (scatter/cluster/allone/wrap/random); overhead ตามขนาด: 8MB→+0.06%, เล็กมาก→+9.38% (จุดไม่คุ้ม) |

## 📊 ตัวเลขสำคัญ

```
make test: TIER1 75/75 + TIER2 4/4 ✅
Q8_0: H(value)=7.65/8 (96%), H(scale)=15.3/16, H(scale|maxq)=15.1
silk: blocks/map = 1.0 ทุกโมเดลทุก mode (rot+rev = identity)
beam 0.59× = lossy จริง (0/100 roundtrip) — ละเมิดกฎ "ratio<1.0 ต้อง prove ด้วย decode"
sort+perm: กำไร 106b − perm 117b = NET −11 bits/block
PCM delta 1.5× · mel bound แล้ว · two-gap ชนะเมื่อ detail≈0 เท่านั้น
tied embeddings: Qwen2.5 137MB = 35% ← ทางที่ลด bytes ได้จริง
fibo checkpoint: overhead +1.05% @4KB, 66 routes = 330B log, ราคาไม่ขึ้นกับข้อมูล
```

## 🧭 คำศัพท์/หลักการ (ต่อ §15.35-15.37)

- **Entropy-bound rule (ยืนยันทั้งวัน)**: engine แก้ addressing/collision/order ได้หมด
  แต่สร้าง redundancy ที่ข้อมูลไม่มีไม่ได้ — ข้อมูลถึง bound แล้ว → ไม่มีอะไรให้ map แชร์
- **MAP not COMPRESS**: ชนะที่ระดับค่าเมื่อ data IS address หรือ smooth/structured (FLAT
  tiles) — นอกนั้น win อยู่ที่ระดับโครงสร้างไฟล์ (tied/shared tensor) + capability
  (N scale views จาก 1 copy)
- **sort ทำลาย order = ต้องจ่าย permutation** — order ฟรีเฉพาะ data ที่ order เป็น
  ธรรมชาติ (audio/time/image) — นั่นคือเหตุผลที่ beam → audio ถูกทาง
- **hyperbolic เติมเต็ม lossy**: transform gap ฟรี (deterministic replay, 5B/event —
  ∝ events ไม่ใช่ data) + detail gap จ่าย entropy จริง — สอง-gap ชนะต่อเมื่อ
  inter-scale detail ≈ 0
- **state = (seed, round, tick)** — checkpoint image = seed(8)+round(8)+tick(4)+ver(8)
  + log(5B/event) + payload — วนกี่รอบก็ได้ ราคา "jump anywhere" ไม่ขึ้นกับขนาดข้อมูล
- **fibo sweep**: ตาราง (pipes×ticks)/สนาม (cycles ≤255)/ระยะ (dist)/ปริมาณ
  (chunks×size ≤64KB)/รูปแบบ (5 patterns) — custom ได้ทุกมิติ พิสูจน์ lossless ทุกตัว

## ⏭️ ขั้นต่อไป (เปิดไว้)

1. **Wire tied-embedding dedup เข้า chain** — registry {id→home}: tensor byte-identical
   → bond เดียว → freeze ครั้งเดียว (Qwen2.5 พิสูจน์ 137MB = 35%)
2. **Walk-based access ใน fibo sweep** — เดิน spine tick-by-tick แล้วหา route ที่ live
   → พิสูจน์ state=(seed,round,tick) พอสำหรับทุกตำแหน่งทุกตาราง
3. **Persist checkpoint images ลงดิสก์ใน sweep** — แต่ละ config เขียน image → reload
   ใน process ใหม่ → lossless (ตอนนี้ใน-memory พิสูจน์แล้ว)
4. **Auto-flag "ไม่คุ้ม"** — overhead% เทียบ threshold ขนาดข้อมูล → verdict ต่อ config
5. **18tes (GEO_COMPOUND_144)** — ตัวเอก ยัง FUTURE (test_6ico_tesseract ผ่านเป็น base)
6. **กลุ่ม rail** — พักไว้ รอ residual space + jet puller (bond จาก FGLS_new มีเยอะ)

## ⚠️ ข้อควรระวัง

- **กฎ "ratio < 1.0 ต้องพิสูจน์ด้วย decode"** — beam_codec 0.59× เป็น lossy ที่ไม่เคย
  verify; ตรวจ claims เก่าเสมอด้วย roundtrip ของตัวเอง
- bug ที่เจอวันนี้: (1) int8 −128 → abs → index เกิน array (Qwen3 เท่านั้น), (2)
  `(uint32_t)` cast ก่อน modulo → product เกิน 2³² ทำลาย golden-ratio spread —
  วัด metric ต้องตรวจ generator/ขอบเขตด้วย ไม่ใช่แค่ผลลัพธ์
- branch `feature/geo-audio-codec` = classifier (word match 73.7%) ไม่ใช่ lossless
  codec — mel ถึง bound แล้ว; voice bridge (96%) ใช้ได้จริง
- เทสต์ tuning ใช้ไฟล์จริง (I:/model, F:/notebookLM) — ข้ามเงียบๆ ถ้าไม่มี
- `.freebuff/` = client metadata — อย่า commit
- `master` ที่ I:/DWGLS มี gguf_box.h เก่า (hdr_size=16) — งาน fix อยู่ branch
  `feat/geo-native-fs` ระวัง merge ทับ

## ➕ เพิ่มหลังเขียน (00:45) — fibo sweep: disk persist + fresh-process restore + economy verdict

- `tools/fibo_checkpoint_sweep.c` ขยาย: `--sweep` เขียน image+manifest ลง `build/ckpt/` แล้ว spawn
  ตัวมันเองเป็น fresh process (`--verify-img`) reload จากไฟล์จริง → พิสูจน์ lossless — **27/27 RESTORE PASS**
- negative case พิสูจน์: corrupt 2 bytes → 26/27 เจอตัวนั้นเป๊ะ; `--verify-all [DIR]` ตรวจทีหลังได้
- economy verdict ต่อ config (`--economy X.X`, default 2.0): EXCELLENT/WORTH/MARGINAL/NOT WORTH —
  thr 1.0% → 6 NOT WORTH (ข้อมูลเล็ก), thr 5.0% → 21 EXCELLENT — จุด "ไม่คุ้ม" auto-detect
- 🐛 2 bug ที่เจอ: (1) spawn ส่ง args แบบแยกแต่ parser รับแต่ `=` → **false positive** (child รัน
  default config ใน-memory แทน verify จากไฟล์) — จับได้จาก output ที่ child พิมพ์ "single config";
  (2) verify-all path image ผิด ("build/c.img") — ทั้งคู่แก้ + negative test ยืนยัน
- `make test`: TIER1 75/75 + TIER2 4/4 (ยังเขียว)

## ➕ เพิ่มหลังเขียน (01:00) — RDH แทน hash/fnv-1a ในสาย ghost/bond (§15.38)

- ตาม user: `collection/rdh` (Ring-Wedge-Mirror mixed-radix) แทนที่ FNV-1a — ใหม่
  `core/geo_rdh_addr.h`: rdh_addr = block×256+from (collision-free + reversible), ghost
  origin/piece ใช้ RDH ล้วน, bond = interleave addr|addr<<24 (bijection 48-bit)
- 🐛 พิสูจน์ด้วย bitset ทั้ง 2^24 keys จับ bug การออกแบบแรก (L^R row⊕column ไม่เป็น
  bijection — image เหลือ 2^16) → แก้เป็น interleave
- `tests/test_rdh_addr.c` TIER1 15/15 · `make test` TIER1 **76/76** + TIER2 4/4 ·
  sweep persist/restore 27/27 ยัง lossless
