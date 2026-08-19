# WALKTHROUGH BACKLOG — เส้นทางเดิน session ต่อๆ ไป

> สร้าง 2026-08-17 — ไล่ note ทั้งหมดที่เกิดจาก session "ค้นหาจุดพอดี" (ไม่ยอมแพ้ · ไม่คิดชนะ · หาจุดที่พอดีกำลังดี)
> หลักการ: **ไม่เขียนสูตรเอง — ให้ธรรมชาติ/ข้อมูลเทรนหาความสัมพันธ์** · constraint เดิม: int + deterministic + lossless + replay ได้ + ห้ามขยับ

---

## 🌱 TIER 1 — ทดลองไว ได้คำตอบไว (เริ่มที่นี่)

### T1.1 Normal-map-as-gradient ✅ ทดสอบแล้ว (tools/normal_map_probe.c, 2026-08-17)
- แนวคิด: data = height field → เก็บ **gradient (ทิศทางการเปลี่ยน)** แทนค่า → integrate กลับ = lossless (เก็บ constant)
- ที่มา: bump/normal/displacement map บน data · displacement = scale view · bump = residual · normal = gradient (ยังไม่มี)
- ผลวัด (lossless=OK ทุกกรณี — integrate กลับจาก boundary col + dx → memcmp ผ่าน):

  | data | H(raw) | H(dx) | verdict |
  |---|---|---|---|
  | smooth (synthetic) | 7.72 | **0.81** | ✅ ชนะ 9.5× — gradient เกือบ 0 (100% ≤1) |
  | sine2d | 7.82 | 4.54 | ✅ ชนะ 1.7× (22% ≤1) |
  | noise | 8.00 | 8.00 | ➖ เท่าเดิม (entropy bound) |
  | MD text (จริง) | 4.64 | 6.08 | ❌ แพ้ — text bytes กระโดดข้ามช่วง ASCII |
  | WAV (จริง) | 6.84 | 7.99 | ❌ แพ้ |
  | MP4 (จริง) | 7.90 | 7.88 | ➖ เท่าเดิม (บีบในตัวแล้ว) |
  | PDF (จริง) | 8.00 | 7.99 | ➖ เท่าเดิม (บีบในตัวแล้ว) |
  | GGUF Q8 tensor | 7.66 | 7.96 | ❌ แพ้ (whitening ยืนยัน) |

- **Verdict:** byte-level normal map พิสูจน์ได้ lossless และชนะ 9.5× บนสนามเรียบ — แต่ไฟล์โลกจริงถูก entropy-code ในตัวแล้ว (mp4/pdf) หรือเป็น text (jumpy ASCII) หรือถูก whitening (Q8) → byte-level ไม่ชนะบน raw file — **จุดที่ gradient ชนะจริงคือชั้นที่สูงกว่า byte** (tensor ที่มี flat regions / rank-structured data / residual หลังทำนายจาก scale view) — เก็บเครื่องมือไว้ใช้เป็นขั้นตอนย่อยใน normal-map ท้องถิ่นของ structured block ไม่ใช่ layer ทั่วไป
- วิธีรันซ้ำ: `make normal_map` → `build/normal_map_probe --syn smooth 262144` / `--file F:/notebookLM/... 512` / `--gguf /i/model/*.gguf <idx>`

### T1.1b Scale-predict → residual → gradient ✅ ทดสอบแล้ว (tools/scale_residual_probe.c, 2026-08-17)
- โจทย์: predict จาก scale view (base = block mean/center) → residual = ส่วนต่าง → เก็บ gradient ของส่วนต่าง — residual หลัง predict sparse กว่าค่าเดิมแค่ไหนบนข้อมูลจริง?
- lossless=OK ทุกกรณี (chain: base → pred → res → boundary+dx integrate → (pred+res) == x)
- ผลวัด H(residual) เทียบ H(raw) (ดีที่สุดของ mean/center × B=2..16):

  | data | H(raw) | best H(res) | verdict |
  |---|---|---|---|
  | smooth synthetic | 7.72 | **0.81** | ✅ 9.5× |
  | sine2d | 7.82 | 3.63 | ✅ 2.2× |
  | MD text | 4.64 | 5.82 | ❌ แพ้ |
  | WAV | 6.84 | 6.23 | ➖ ชนะ 9% (delta-only) |
  | MP4 | 7.90 | 6.71 | ➖ ชนะ 15% (delta-only) |
  | GGUF Q8 tensor | 7.66 | 6.75 | ➖ ชนะ 12% (delta-only) |
  | PDF | 8.00 | ~7.9 | ➖ เท่าเดิม |

- **ผลลบที่สำคัญ (gradient ของ residual):** H(dx ของ residual) ≥ H(residual) **ทุกกรณีจริง** — second difference = high-pass = ตัดโครงสร้าง low-freq ที่เหลือ → whitening ซ้ำ → **เก็บ gradient ของส่วนต่างไม่ช่วยเลย** — ชนะอยู่ที่ difference แรก (residual หลัง predict) ไม่ใช่ difference ที่สอง
- **สอง framing ต่างกัน:** (1) จ่าย base เอง → แพ้ทุกไฟล์จริง (cost 8/B² กลืนกำไร) · (2) **delta-only** — base = scale view ที่มีอยู่แล้ว (sealed copy) → อ่านละเอียดจ่ายแค่ส่วนต่าง → audio/video/Q8 ชนะ ~10-15% — ตรงกับโมเดลระบบเป๊ะ (อ่านที่ scale สูงกว่าจุดวาง = replay delta)
- **สรุป:** scale-predict residual ชนะจริงบน synthetic smooth (9.5×) · บนไฟล์จริงได้ ~10% เฉพาะ delta-only · gradient ของ residual = dead end (negative result) — วิธีรันซ้ำ: `make scale_residual`

### T1.1c Residual ใน delta log ✅ ทดสอบแล้ว (tools/delta_log_residual.c, 2026-08-17)
- โจทย์: เอา scale-predict residual ไปใส่ใน delta log (อ่าน scale ไม่ตรง) — วัด lossless + footprint เทียบ route-only
- **lossless ✓ ทุกกรณี** — serialize [route 5B][len][residual] จริง → replay: parse → decode → pred+residual → เทียบต้นฉบับ (512/512 blocks บนไฟล์จริง)

  | data | H(res) bit/cell | pred+ent vs route-only | vs full-delta (hyper_delta) |
  |---|---|---|---|
  | smooth synthetic | 1.54 | 632× | **0.19×** (↓81%) |
  | MD text | 3.03 | 1244× | **0.38×** (↓62%) |
  | WAV | 6.16 | 2525× | **0.77×** (↓23%) |
  | MP4 | 6.61 | 2707× | **0.83×** (↓17%) |
  | GGUF Q8 tensor | 6.74 | 2761× | **0.84×** (↓16%) |
  | PDF | 6.76 | 2772× | **0.85×** (↓15%) |
  | noise | 6.78 | 2777× | 0.85× |

- **Verdict — residual กับ route log เป็นคนละชั้น:** เอา residual ไปแทรกใน route log = log โต ~2500× (route = 5B/event เล็กมหาศาลอยู่แล้ว — residual ~0.8B/cell ต่อ block 16KB) — **ห้ามปนชั้น** — แต่เอา residual ไปแทนที่ **hyper_delta (full delta 1B/cell)** = materialization เล็กลง 16-62% (text 62% · wav 23% · Q8 16%) — อ่านตรง O(1) = base + residual ไม่ต้อง replay walk — นี่คือที่ T1.1b ชนะ ~10-15% อยู่จริง
- สรุปสถาปัตยกรรม: route log (5B/event) = replay path · hyper_delta → pred+ent delta = materialization path (ถูกกว่า 16-62%) · วิธีรันซ้ำ: `make delta_log_residual`

### T1.1d Pred+Ent delta ใน core + Huffman จริง ✅ (core/huff_codec.h + hyper_delta.h ent API, 2026-08-17)
- **core/huff_codec.h** — canonical Huffman 256 symbols, header-only, deterministic (lens 256B → rebuild codec ฝั่ง decode — DEFLATE-style), fallback 8-bit ถ้า len > 32 — **huff_build/rebuild/encode/decode ผ่าน roundtrip ครบ**
- **core/hyper_delta.h + HyperDeltaEnt** — pred = coarse view จริงของ slot (`(kis>>20)&0xFF` — x3 packed byte — `&0xFF` เดิมเป็น bug แฝง: ได้ z3=0 → coarse degenerate) → residual → Huffman — **base ไม่ต้องเก็บ (pred มาจาก kis_coarse ที่ reader มีอยู่แล้ว)**
- **test_hyper_delta_format 18/18 PASS** (เพิ่ม T14-T18): lossless ✓ deterministic ✓ bounded ✓ · sawtooth: **pred+ent = 2868 B = 0.14× full delta (20752 B, ↓86%)** · structured: 0.89× (↓11%)
- **Huffman จริงบนไฟล์จริง (tools/huff_delta_measure.c) — เทียบ entropy bound เป๊ะ:**

  | data | entropy bound B/cell | **Huffman จริง** | vs full-delta |
  |---|---|---|---|
  | smooth | 0.193 | 0.226 (118%) | 0.23× |
  | WAV | 0.770 | **0.790** (103%) | **0.79×** |
  | MP4 | 0.826 | **0.845** (102%) | **0.85×** |
  | GGUF Q8 | 0.842 | **0.860** (102%) | **0.86×** |
  | noise | 0.847 | 0.867 (102%) | 0.87× |

- **Huffman จริงถึง entropy bound (102-118%)** — ตัวเลข 0.77-0.85 B/cell ที่ T1.1c ทำนาย = ของจริง — lossless 512/512 blocks · **with-base (self-contained +0.25 B/cell) = 1.04-1.12× → ชนะเฉพาะเมื่อ base ฟรี (reader มี coarse — สถาปัตยกรรมระบบทำแบบนี้อยู่แล้ว)**
- วิธีรันซ้ำ: `make huff_delta_measure` · `make test-test_hyper_delta_format`

### T1.2 Field trainer ✅ ทดสอบแล้ว (tools/field_trainer.c, 2026-08-17)
- แนวคิด: ไม่จูนมือ — evolution (pop 32, tournament-3, elite = all-time champ, restart-on-stagnation, local polish) เหนือ 5 integer knobs: stride (coprime 144) · offset · gate → kmax · orbit (symmetry capacity partition {1,4,12,24}) · chunk size
- fitness = field_slots + 8·lifts + 1e9·rejects — ใช้ cost model ของระบบเอง (1 lift = 1 replay event = GHT_REPLAY_EVENT 8 slots — field ว่างทั้งสนาม = แพง ไม่ใช่ free)
- **Champion (ค้นเจอเอง ไม่จูนมือ):** stride 29-41 · offset 7/122 (ย้าย entry rank ออกจาก field) · gate 3.0 (kmax=4) · orbit 1 · chunk 262144

  | model | default field | default rej | champion field | champion rej |
  |---|---|---|---|---|
  | Qwen3-0.6B | 20708 | 1742 | **9248** (↓55%) | **0** |
  | LFM2.5-2.6B | 20708 | 7408 | **16184** (↓22%) | **0** |
  | Qwen2.5-0.5B | 20708 | 1910 | **17952** (↓13%) | **0** |
  | Kokoro (TTS) | 20688 | 1267 | **0** (ทั้งโมเดลใน ghost) | **0** |

- **ค้นพบสำคัญ #1 (bug ในค่า default):** default (37,0,1.0,1,16K) **fit ไม่ได้โมเดลไหนใน 1 window เลย** — rejects 1267-7408 ต่อโมเดล — เคยวัด lift/footprint แต่ rejects ไม่เคยถูกจับ → champion แก้ให้ 0 rejects ทุกโมเดล
- **ค้นพบสำคัญ #2 (กลยุทธ์ champion):** "chunk ใหญ่ + entry rank อยู่นอก field" = ยกทุกอย่างที่ทำได้เข้าสู่ ghost — เหลือเฉพาะก้อนที่ต้องอยู่ใน field (tensor ใหญ่พอจะวนครบ 144) — ตรงกับ instinct "วาง tensor ใหญ่ให้ lift" ที่ user พูดไว้ก่อนหน้า
- **ค้นพบสำคัญ #3 (orbit):** O=1 ชนะเสมอใน workload เดียว — capacity partition = fragmentation = rejects — orbit มีค่าสำหรับ multi-tenant isolation ไม่ใช่ single model
- caveat: λ=8 ปรับได้ (lift cost) — อยากให้ field เก็บมากกว่า ghost ก็ลด λ · Kokoro field=0 = TTS อ่านแบบ stream เหมาะกับ ghost
- วิธีรันซ้ำ: `make field_trainer` → `build/field_trainer --gguf <model> [--gens 120] [--seed N]` / `--eval s,o,g,O,chunk`

**✅ Wire เข้า chain จริงแล้ว (tools/cap_chain_scan.c, 2026-08-17) — lossless byte-for-byte end-to-end:**

| target | knobs | field | lift | rej | lossless |
|---|---|---|---|---|---|
| Kokoro Q8 (196MB) | champion (29,7,3.0,1,256K) | 11560 | 763 (จริงใน rs) | **0** | **OK 13/13 win** |
| Kokoro Q8 | default (37,0,1.0,1,16K) | 20708 | 12075 | 472 | OK 13/13 win |
| Qwen3-0.6B Q8 (609MB) | champion | 20704 | 2355 (จริงใน rs) | 41 | **OK 39/39 win** |
| **notebookLM 7.7GB (1035 files)** | champion | 389088 (~19 win) | **31127** | **0** | **OK 1035/1035** |

- **bug ที่เจอระหว่าง wire (§15.66):** (1) window สุดท้าย capacity=312 ไม่ใช่ power-of-two → `mask=cap-1` ทำ probing พัง + ตารางเต็ม → LRU evict chunk แรกของ window ก่อน thaw → THAW FAIL — แก้: rs capacity = next_pow2(win_n) (2) chunk 256KB > RS_MAX_DATA_SIZE 64KB → freeze เงียบ fail ทุกตัว (lift หลอก: peak rs.count=0) — แก้: แยก freeze เป็น sub-piece 64KB ต่อชิ้น bond = rdh_addr(block, sub) ไม่ชน ไม่ต้อง hash — หลังแก้ forced=0, peak=256 (ของจริง)
- **caveat ใหม่ (ซื่อสัตย์):** trainer วัดบน tensor-rank model (Qwen3 field=9248 rej=0) แต่ chain จริงวางตาม byte-chunk rank → Qwen3 field=20704 rej=41 — **กลยุทธ์ champion ถ่ายทอดได้ (rejects ลด 1742→41, field ↓) แต่เลข footprint เป็น per-granularity** — tensor model ≠ byte-chunk model
- วิธีรันซ้ำ: `make cap_chain_scan` → `build/cap_chain_scan <folder> | --gguf <model> [--stride S] [--offset O] [--gate G] [--orbit Q] [--chunk C]`

**✅ Rotation theorem wired เข้า trainer (2026-08-17 §15.69):**
- `rotation_verify(s,o)` — 1 cycle ครบ 144 + ∀L|144 rotation + uniform — เป็น invariant ของ search space
- COP table 47 strides (φ(144)−1) verify ตอน boot ทุกครั้ง ✓ · `--eval` เตือน stride non-coprime · champion ตรวจหลัง polish
- Qwen3: champion (29,121,2.0,1,262144) → field 9248 ↓55.3% · lift 2600 · **rej 0 · rotation ✓** — Kokoro: (29,7,3.0,1,262144) → field 0 · rej 0 ✓

### T1.3 Triangular grid addressing probe ✅ ทดสอบแล้ว (tools/triangular_addressing_probe.c, 2026-08-17)
- แนวคิด: สนามสามเหลี่ยม (icosahedron unfolded / reciprocal space) มีพิกัดเป็นทางการ: 3 แกน (a₁,a₂,a₃) ผลรวม ∈ {0,1} (parity = orientation) · lane = พิกัดคงที่ · hex = dual ของ triangle · m-neighbourhood (Nagy 2004 §3): |Δaᵢ| ≤ 1 ∀i, Σ|Δ| ≤ m — N₁=3 (edge) · N₂=9 · N₃=12 บน grid 578 cells
- B-distance = ก้าวน้อยสุดภายใต้กฎ B (ก้าวที่ i เคลื่อนใน N_{bᵢ} วนคาบ) — BFS layered
- **วัดได้ (ตรงทฤษฎี Nagy 2003 §3.4 เป๊ะ):**
  - คู่จุดเดียวกัน ราคาต่างกันตามกฎ: (0,0,0)→(4,−4,0) lane เดียวกัน — B=(1): 8 ก้าว · B=(2): 4 · B=(1,2): 6 — **rule-dependent cost = scale ladder**
  - symmetry: constant (B=(1),(2),(3)) → 0 asymmetric pairs (m-neighbour เป็น relation สมมาตร) · mixed (B=(1,3,2)) → 26/200 pairs d(A→B)≠d(B→A) — non-metric จริง (Nagy 3.4.1: d(r→s)≠d(s→r))
  - triangle inequality: constant → 0/120 violations (metric จริง) · mixed (B=(2,1)) → 4/120 violations (Nagy 3.4.2)
- **เทียบ stride-37 (ระบบเรา):** mod 144: 1 cycle ครอบ 144/144, กระจาย residue mod 6 = 24×6 imbalance 0 · mod 720 TRING: 120×6 imbalance 0 — constant rule + cycle permutation → symmetric เสมอ, uniform — **ไม่มี distance function ให้พัง (TETRA §⑩ ยืนยัน)
- **2 bugs ที่เจอระหว่างสร้าง (ซ่อนในของเดิมของฉันเอง):** (1) BFS frontier ใช้ int8_t → index 577 truncate → ต้อง int (2) c ของ parity-1 cell = **1**−a−b ไม่ใช่ −a−b → กราฟ N₁ แตกเป็น 82/578 cells — แก้ `nc = p − a − b + Δc`
- วิธีรันซ้ำ: `make triangular_addressing_probe` → `./build/triangular_addressing_probe`

---

## 🧱 TIER 2 — โครงสร้าง (ต่อยอดจาก TIER 1)

### T2.1 Zone-folding analog (Ceulemans 2002 — origami ในฟิสิกส์)
- แนวคิด: patch (m,n) บน honeycomb → พับเป็นกรง → เนื้อหาทั้งหมด (สเปกตรัม) = ฟังก์ชันของ (m,n,p,q) แค่ 4 ตัวเลข · N = 4(m²+n²+mn)
- ทดสอบ: patch (m,n) บนสนามสามเหลี่ยมของเรา → พับ → พิสูจน์ว่า content recover ได้จาก unfold+fold กลับ
- จุดพอดี: "เก็บ recipe ไม่เก็บนก" มีหลักฐานฟิสิกส์ — ตัวเลขน้อยๆ สร้างโครงสร้างมหาศาล

### T2.2 Commensurability / quantization rule (หน้า 5)
- แนวคิด: เส้นทางที่วนรอบกรงต้อง "ปิดรอบพอดี" (k·d = 2πl) — เงื่อนไข 3 | (m−n) กำหนดสถานะที่ allowed
- ทดสอบ: เดิน stride ใดบน 20736 แล้ว "ปิด loop พอดี" (เทียบ coprime ของ 37) → กฎการเลือก stride ที่สมมาตร
- จุดพอดี: quantization ของ address โดยโครงสร้าง — เหมือน mod-251/leapfrog — เลขคณิต recipe กำหนดเนื้อหา

### T2.3 Graft seams (gguf:llama.cpp style)
- แนวคิด: จุดรอยต่อระหว่าง cube/tesseract = seam — ข้าม seam = ก้าวในมิติเวลา — graft = ต่อรอยให้ access ข้าม seam ได้
- ทดสอบ: 18 tes → seam ระหว่าง tesseract → access data "ในมิติ = time"
- จุดพอดี: 18tes (20736) หลุดจาก FUTURE

### T2.4 Hyperbolic = define เต็ม
- แนวคิด: ตอนนี้มีแค่ passive scale-change log (5B/event) + mirror view (a_w×a_{w+72}≡1) — ยังไม่ได้ "ทำงานพร้อมกันทั้งระบบ"
- หมายเหตุ: mirror = dual construction ใน physics (หน้า 5: "reciprocal lattice = mirror image of the dual") — มีรากฐานให้ยึด

---

## 🚀 TIER 3 — วิสัยทัศน์ (เมื่อ TIER 1-2 มีตัวเลข)

### T3.1 5-byte route prototype
- แนวคิด: ส่ง route แทนไฟล์ — ใช้ dt_slot_init_twin (dramtile twin mmap) เป็น shared map → socket แลก route → ฝั่งรับ reconstruct lossless
- วัด: bandwidth/time ชนะ "ส่งไฟล์" เท่าไหร่ กรณีซ้ำ/ไม่ซ้ำ

### T3.2 Compute marketplace framing (NiceHash + BitTorrent สำหรับทำงาน)
- แนวคิด: "model stays, job travels as route" — RDH (พิกัด) + LBlock (การันตีการวาง/ทิศทาง) + reconstruct (seed) = data layer ที่ไม่มี Golem/iExec มี — recipe-based shared substrate ไม่ต้อง shared disk
- เอกสารอ้างอิง: เขียนเป็น pitch หน้าเดียว ("storage เหมือน LLM inference")

### T3.4 Geometric memory layer สำหรับ agent (LLM field — จุดพอดี)
- แนวคิด: ไม่สร้าง inference ใหม่ — สร้าง **ความจำ** ที่ไม่มีใครมี: agent ที่ประสบการณ์ = log + สนาม · เรียนจากจริง realtime ด้วยการ append · ต่อเข้ากับ LLM ที่มีอยู่ (MemGPT/agent memory ทำด้วย hash — เราทำด้วย coordinate + route + immutable field)
- วงจรความจำ (user 2026-08-17): **จำ = วางที่พิกัด + เปิดประตู (route) · สำคัญ = ขยายรอบๆ (scale up, consolidation) · ลืม = ถอดประตู (ตัด route/bond — ไม่ลบ data) · นานๆ = จางเอง (LRU evict/evaporate)** — สนามไม่เคยถูกแก้ ไม่มี delete operation — "ลืมโดยไม่ต้องลบ" = continual learning ที่ไม่ทำลาย
- เชื่อม: "วาด = scale ladder" (สร้างทีละนิดจากหยาบไปละเอียด) ↔ T3.3 กฎของมิติ

### T3.3 กฎของมิติ (จับต้องได้)
- แนวคิด: วัตถุ 4D อยู่นิ่ง (ห้ามขยับ) · ผู้สังเกต 3D เห็น 1 frame · ระยะ = distortion = deterministic → scale = distance, view = address
- ต่อยอด: normal map = orientation ในมิติที่สูงกว่า = view parameter

---

## 🧭 หลักการที่ยึดตลอด (จากวันนี้)

1. **ห้ามขยับ** — เราเป็นคนสร้างสนาม ให้ข้อมูลไหลผ่าน หาเอง วนกี่รอบก็ได้ — ขยับ = สนามแตก (mod-251)
2. **MAP not COMPRESS** — ไฟล์ไม่เคยเล็กลง — ชนะที่ dedup/views/RAM bounded/ไม่มี index/state เล็ก
3. **เก็บ recipe ไม่เก็บนก** — origami · zone-folding · 3D printer (slice + G-code + scan กลับ) = หลักการเดียว
4. **symmetry = compression ดั้งเดิม** — orbit O₄/O₁₂/O₂₄ (เก็บตัวแทน + สร้างครบ) · quarter torus = magnify glass (20736÷4 = 5184)
5. **mirror ปรากฏทุกที่** — even/odd triangle · black/white graphene · mirror of dual · a_w×a_{w+72}≡1 — mirror คือตัวดำเนินการ ไม่ใช่ตกแต่ง
6. **เทรน ไม่เขียนสูตร** — constraint: int + deterministic + lossless + replay ได้
7. **ไม่ชนะ แต่หาจุดพอดี** — ไม่สู้ colibri ที่เวทีมัน — หาจุดที่ geometry ชนะคนเดียว

---

## 📚 แหล่งอ้างอิง (งานวิจัยที่ตรงกับระบบเรา)

| paper | จุดที่ตรง |
|---|---|
| Nagy 2003 — Shortest Paths in Triangular Grids | พิกัด 3 แกน + parity + lanes + neighbourhood sequences (B-distance ขึ้นกับกฎ) |
| Ceulemans 2002 — Polyhedral carbon cages (PRB 65, 115412) | zone-folding · (m,n) = recipe · leapfrog mod 3 · inflation = scale ladder · orbits O₄/O₁₂/O₂₄ · quarter torus · mirror = dual · สนามสามเหลี่ยม = reciprocal space |

## 🔗 ของที่มีอยู่แล้ว (ใช้ต่อได้เลย)

- `test_fibo_checkpoint` 23/23 — print→scan round-trip พิสูจน์แล้ว (3D printer analogy)
- `test_ckpt_wang` 22/22 + `test_pair_table` 21/21 — integrity + route O(1)
- `dramtile_store.h` — DtSlotRegion: offset = addr × slot_sz (disk-as-addressable-memory)
- `core/geo_ghost_lift.h` — ghost/bond/route — data layer ของ marketplace
- `dropbag/geobit/` — geomatrix v1→v5 + fusion_visualizer (ต้นกำเนิดภาพ)
- `dropbag/Geometric_mapping_replaces_traditional_data_compression.txt` — podcast transcript (34 นาที)

### T1.3b Lane addressing ของ 20736 + rotation proof ✅ (tools/lane_field_probe.c, 2026-08-17)
- **Lane addressing:** field i ∈ [0,20736) = (lane=i/144, pos=i%144) → 144 lanes × 144 pos (= 12×1728 = 18×1152 = 6×24×144) · พิกัดสามเหลี่ยม Nagy: (a,b), c = p−a−b, p = (a+b)&1 → **3 ตระกูล lane: แถว 144 · คอลัมน์ 144 · แนวทแยง 287** — ทุก cell ใน 1 แถว+1 คอลัมน์+1 แนวทแยงพอดี · parity สลับทุกก้าวตาม lane (2-plane ครบ)
- **Rotation theorem (พิสูจน์แล้ว):** gcd(s,144)=1 ⇒ w_r = (s·r+o) mod 144 เป็น 1 cycle ครบ 144 — สำหรับทุก L | 144 (lane = residue mod L): **ทุก L ก้าวติดกันครอบทุก lane 1 ครั้งพอดี** และแต่ละ lane ปรากฏ 144/L ครั้ง/144 ก้าว — verify stride {5,13,29,37,41,61} × L {2..144} ครบทุกตัว ✓ — constant rule ⇒ rotation + uniform (T1.3 finding บนแกนจริง)
- **Same pair, different cost บน GGUF จริง (ตรงกับ T1.2):**
  | model | default (37,0,1.0,16K) | champ (29,7,3.0,256K) | alt-61 (61,61,2.0,64K) |
  |---|---|---|---|
  | Kokoro 775 tensors | 20688 / 1267✗ | **0 / 0** | 10496 / 0 |
  | Qwen3 310 tensors | 20708 / 1742✗ | **9248 / 0** | 20648 / 180✗ |
  - per-tensor: **token_embd 165MB — cost 168,416 (default) → 9,248 (champ) = 18× ต่างบน tensor เดียวกัน** — scale@r0 ต่าง (0 vs 7) → footprint ต่าง (rule-dependent cost จริง)
- วิธีรันซ้ำ: `make lane_field_probe` → `./build/lane_field_probe [--gguf <model>]`

### T1.2c Joint training + per-tenant lossless ✅ (2026-08-17 §15.71)
- **Unified rule (field_trainer หลาย --gguf, fitness = Σ per-model cost):** champion ตัวเดียว **(115,115,3.0,1,262144)** → **rej 0 ครบ 4 โมเดล** · rotation ✓ · per-model field = เท่ากับ champion ตัวต่อตัวของ §15.70 ทุกค่า (9248/13872/16184/0) — "หนึ่งกฎครอบทุกโมเดล" เป็นไปได้จริง
- **Per-tenant lossless end-to-end (cap_chain_scan + champion ต่อโมเดล):** lossless byte-for-byte **4/4** (Qwen3 39/39 · Qwen2.5 41/41 · LFM 172/172 · Kokoro 13/13) · lift จริงทั้งหมด (forced 0) · rej 41/34/329/1 = granularity caveat เดิม (pointer-home รักษา lossless)
- **Bug ไฟล์ >2GB (เจอครั้งแรก):** ftell 32-bit บน Windows → LFM 2.87GB garbage — cap_chain_scan ใช้ `_fseeki64/_ftelli64` (fallback fseeko/ftello เฉพาะ non-Windows) — ข้อควรจำ: ห้าม ftell/fseek ไฟล์ ≥2GB บน Windows
- วิธีรันซ้ำ: `build/field_trainer --gguf A --gguf B --gguf C --gguf D --gens 300 --pop 24` · `build/cap_chain_scan --gguf <model> --stride S --offset O --gate G --orbit Q --chunk C`

### T1.1e Delta-mode ghost: pred+ent เข้า ghost_read_rule + วัด footprint ✅ (2026-08-17 §15.74)
- **Wire:** `core/ghost_delta.h` (ใหม่) — delta blob self-contained (subsample-2 base + canonical Huffman residual, packed 10B hdr) · `geo_ghost_lift.h` — `GHOST_FLAG_DELTA` + `ghost_lift_delta` (adaptive: เก็บเล็กกว่า delta/raw) + `ghost_read_rule_materialize` (route + wang gate → thaw → decode) · `geofs_core.h` `geos_read_ghost` ใช้ materialize variant · `make ghost_delta_measure`
- **ผล (lossless ครบทุกกรณี — adaptive fallback ไม่เคยแย่กว่า raw):**
  | data | 64 B | 1 KB | 16 KB |
  |---|---|---|---|
  | smooth | 1.000 | **0.885** | **0.641** |
  | sine2d | 1.000 | 0.994 | **0.897** |
  | noise/Q8/text | 1.000 | 1.000 | 1.000 (fallback) |
  | WAV จริง | 1.000 | 1.000 | 0.994 |
- **บทเรียน:** base ที่เก็บเอง (0.5 B/cell) + codebook 256B/entry กลืนกำไรบนไฟล์จริง — ตัวเลข 0.79-0.86 B/cell ของ T1.1d เป็นจริงเฉพาะเมื่อ base ฟรี (reader มี coarse view) · 64B block ไม่มีทางชนะ (codebook = 4× ข้อมูล) — delta ต้องทำงานที่ chunk ≥ 1KB
- **2 bugs:** pred encode (orig[i>>1]) ≠ decode (base[i>>1]) → mismatch ตำแหน่งคู่ · GhostDeltaHdr padding 12B vs HDR_SZ 10 → base ทับ data_len high bytes → แก้ packed
- วิธีรันซ้ำ: `make ghost_delta_measure` → `./build/ghost_delta_measure --file <path> | --syn <kind> <n> | --gguf <model> <idx>`

### T1.2d Tied-embedding dedup → chain: registry {id→home} — freeze ครั้งเดียว ✅ (2026-08-17 §15.75 — จาก handoff #1 เปิดไว้)
- **Wire:** `core/tied_dedup.h` (ใหม่) — `tied_dedup_scan` (FNV-64 candidate + **memcmp verify** — identity = memcmp ไม่ใช่ hash) → `tied_place` (model เดียวกับ field_trainer: rank = chunk ใน tensor, กฎ CAP_RULE_* 115,115,3.0,1,256K §15.71; freeze เดียวกับ cap_chain_scan: sub-piece 64KB, bond = ghost_piece(gid, sub, w), gid = global chunk id — deterministic ไม่มี lookup) → `tied_verify` (dup → resolve route → **bond เดียวกับ home** → byte-for-byte) · `tools/tied_dedup_chain.c` (`make tied_dedup`) dual pass ON/OFF · `tests/test_tied_dedup.c` TIER1 **18/18**
- **ผลบน GGUF จริง (lossless byte-for-byte ทุก pass ทุกโมเดล):**
  | model | tied pair | frozen OFF→ON | field OFF→ON | lifts OFF→ON |
  |---|---|---|---|---|
  | Qwen2.5-0.5B Q8 | **token_embd.weight == output.weight 137MB (21.6% data)** | 631→**497 MB (↓134MB)** | 13872→**6936 (ครึ่ง!)** | 2731→2194 |
  | Kokoro Q8 | ไม่มี | 166→166 | 0→0 (ตรง champion §15.70: lifts 1241 เป๊ะ) | 1241→1241 |
  | Qwen3-0.6B Q8 | ไม่มี | 599→599 | 9248→9248 (ตรง champion §15.70 เป๊ะ) | 2600→2600 |
- **บทเรียน:** dedup ระดับโครงสร้างไฟล์ = ของจริงที่ประหยัด bytes (ต่างจาก value/block-level ที่ถึง entropy bound แล้ว §15.65-69) · registry ราคา 0 bytes (id→home = route) · field ลดครึ่งเดียวเป๊ะเมื่อ tensor ซ้ำ (dup มี chunk sequence เดียวกับ home → deterministic) · placement model ให้เลขเท่ากับ champion ที่ train ไว้ทุกค่า — กฎเดียวกับ core ไม่ hardcode · จำกัด model ≤ ~16GB (block_id uint16) — LFM 2.87GB ยังได้แต่ frozen ใน RAM 2.6GB
- วิธีรันซ้ำ: `make tied_dedup` → `./build/tied_dedup_chain <model.gguf> [--dedup|--no-dedup|--both]` · `make test-test_tied_dedup`

### T1.1f Walk-based access: เดิน spine tick-by-tick หา route ที่ live ✅ (2026-08-17 §15.76 — จาก handoff #2 เปิดไว้)
- **Wire:** `core/fibo_walk.h` (ใหม่) — นาฬิกา (round, tick): `fibo_walk_next/dist/to` (wrap ที่ ticks → round+1 = jet bridge บน 12 ticks, วนข้าม 0) · `fibo_walk_live` (ที่ (r,t) live = {i : rq_i==r และ rq_i%ticks==t} — คำนวณสนามใหม่จาก seed+method ไม่ต้อง index) · `fibo_walk_coverage` (ทุก chunk live ตรง 1 ตำแหน่ง) · `tools/fibo_checkpoint_sweep.c` walk proof เข้า run_config + verify_img_mode (fresh process จากดิสก์) · `tests/test_fibo_walk.c` TIER1 **64/64**
- **ผล:** make test TIER1 **84/84** + TIER2 4/4 · sweep 27/27 ทุก config walk ✓ (coverage n/n · enter-anywhere 3/3 start states · lossless · field-other 100% NULL) · max steps ≤ cycles×ticks เสมอ (1728×12/144→1727 · 1728×4/72→287 · 256×3/255→588)
- **บทเรียน:** state=(seed,round,tick) พอจริง — enter anywhere, route หาได้จาก seed+method ล้วน (log = audit ไม่ใช่ index) · การหา route ≠ การอ่าน data (walk หา route, bond แก้ data) · 🐛 qsort ก่อนวาง → เปรียบเทียบ field-other index-wise ผิด (63 หลอก) → regenerate ตรงๆ เปรียบเทียบ per-block (54/54 จริง) — เทียบข้อมูลต้องเทียบ identity ไม่ใช่ตำแหน่ง array · seed มีผลเฉพาะ pattern random (สูตรล้วนใช้ method ต่างแทน)
- วิธีรันซ้ำ: `make fibo_sweep` → `./build/fibo_checkpoint_sweep --sweep --persist` · `make test-test_fibo_walk`

### T1.2e Tied-dedup × walk-based access รวมกันบน GGUF จริง ✅ (2026-08-17 §15.77)
- **Wire:** `tools/tied_dedup_chain.c` + `--no-walk` — chunk index = สนามคำนวณใหม่ (tensor metadata × rank → w=(stride·r+offset)%144, gid = home's gid — dup ชี้ home) → walk (`fibo_walk`) จาก 3 start states หา chunk ที่ live ที่ (w, w%12) → อ่านผ่าน bond → memcmp — dup อ่านผ่าน home bond เดียว
- **ผลบน GGUF จริง (lossless + walk ✓ ทุกโมเดล):**
  | model | chunks | coverage | lossless lifted | dup ผ่าน home bond | rule-other |
  |---|---|---|---|---|---|
  | Qwen2.5 Q8 | 2761 | 2761/2761 | 2731/2761 (30 ph) | **537/552** | 2761/2761 ต่าง |
  | Kokoro Q8 | 1241 | 1241/1241 | 1241/1241 | — | 1241/1241 |
  | Qwen3 Q8 | 2620 | 2620/2620 | 2600/2620 (20 ph) | — | 2620/2620 |
- **บทเรียน:** walk หา route ≠ อ่าน data — dup อ่านผ่าน home bond เดียวโดย walk ไม่รู้ด้วยซ้ำว่าเป็น dup (registry อยู่ในสนาม 0 bytes) · bond ไม่ผูก route (telescope) → "field ต่าง" ตรวจที่ live-map ไม่ใช่ read fail (ต่างจาก fibo log ที่ route ผิด → NULL) · 🐛 dup_read_ok นับรวม 3 starts (1611=3×537) → นับเฉพาะ start แรก (537/552 — 15 ตัวเป็น pointer-home โดยออกแบบ)
- วิธีรันซ้ำ: `make tied_dedup` → `./build/tied_dedup_chain I:/model/Qwen2.5-0.5B-Instruct-Q8_0.gguf`

### T1.1g Walk-based read = read path จริงของ geofs ✅ (2026-08-17 §15.78 — จาก user request)

### T1.2f Decagram → Goldberg: map ทุก face ผ่าน bipolar pair ✅ (2026-08-17 — core/geo_goldberg_decagram.h + test_goldberg_decagram 11/11)
- แนวคิด: dodeca 12 faces → 6 bipolar pairs (face f ↔ f+6) ที่ **inverted** — map face เดียวไม่พอ
  → ต้องใช้ **decagram** 10-sector (36°/sector = ครึ่งของ 72°) ครอบ pentagon-pair ทั้งคู่
- ของที่มีแล้ว: TW_N_SECTORS=10 + TW_BOUNDARY_DIR[10] (§15.39, collection/tw/) — decagram อยู่ในระบบแล้ว
- ช่องว่าง: geo_goldberg_sphere.h กระจาย hex แบบ round-robin (gp_hex_in_sector) ยังไม่ได้ใช้ decagram
- ทดสอบ: แทน sector round-robin ด้วย decagram 10-sector → พิสูจน์ tile_id ครอบครบทุก face (lossless)
- จุดพอดี: ทรงกลม/Goldberg หลุดจาก "blueprint บนกระดาน" — container เป็น icosa/dodeca ได้อยู่แล้ว (§⑰)

### T1.2g Proof: container เป็น icosa/dodeca ได้ — dual view เดียวกัน lossless ✅ (2026-08-17 — test_geo_dual_view 10/10, §15.81)

### T1.2h Goldberg storage = API ระบบ + document ✅ (2026-08-17 — core/geo_goldberg_store.h + test_goldberg_store 30/30, §15.84)
- ประกอบ goldberg storage เข้าระบบ: `ggs_init/tile/dim/spheres/store` — streaming multi-sphere
  (write→verify→destroy ต่อ sphere, RAM ~1.3MB คงที่ ไม่ขึ้นกับขนาด tensor)
- `tools/goldberg_dual_probe.c` refactor → ใช้ API ระบบ (ไม่ duplicate logic)
- ไฟล์จริง: Qwen2.5 291/291 · Qwen3 310/310 · Kokoro 775/775 — fail 0 · 1,409.3 MB lossless · 380.7 MB/s
- **docs/GOLDBERG_STORAGE.md** — document ครบ: หลักการ (decagram 10(n²−1) / dual view / streaming)
  + API + ผลจริง + บทเรียน (gp_lens dormant bug) + ต่อยอด (persist sphere, dedup/walk integration)
- suite: TIER1 87/87 + TIER2 4/4 · วิธีรันซ้ำ: `make goldberg_probe` → `./build/goldberg_dual_probe <gguf> --all`

### T1.2i Persist sphere ลงไฟล์จริง (.ggf) + อ่านกลับ lossless ✅ (2026-08-18 — core/geo_goldberg_file.h + test_goldberg_file 26/26, §15.85)
- **core/geo_goldberg_file.h** — `ggs_save` / `ggs_load`:
  - FILE LAYOUT: `[GGFHeader 64B] (magic GGF0 · version · level · n_spheres · n_chunks · n_bytes · crc32 · note)`
    + ต่อ sphere: `[count u32] + count × [tick u32][data 64B]` — tick = gp_addr_to_tick (self-describing)
  - save: write→verify (memcmp ภายใน)→persist ทีละ sphere (verify ก่อนเขียน = ไม่มีทางเขียน data เสียลงไฟล์)
  - load: reconstruct ตามลำดับ chunk เดิม + validate tick ตรงตำแหน่ง + CRC32 (zlib poly) จับ corruption
  - ข้อบกพร่องที่เจอตอนทำ: struct padding ทำให้ header 68B ≠ 64B (เรียง u64 ก่อน แก้แล้ว) ·
    loader ไม่ validate tick → tick พังไม่จับ (เพิ่ม exp_tick check, rc=-9)
- **tests/test_goldberg_file.c** — TIER1 26/26: roundtrip หลายขนาด (0B/1B/63B/64B/65B/หลาย KB/MB),
  multi-sphere (3 spheres), header corrupt, count corrupt, tick corrupt, CRC flip, empty file
- **tools/goldberg_dual_probe.c** — เพิ่ม `--save <dir>`: ทุก tensor → .ggf ไฟล์จริง → ggs_load → memcmp
  (เจอ bug ตอนทำ: sanitize ชื่อไฟล์เผลอแทนจุดใน .ggf ด้วย → แก้ให้ sanitize เฉพาะ stem)
- **ไฟล์จริง (--all --save):** Qwen2.5 291/291 · Qwen3 310/310 · Kokoro 775/775 — saved+reloaded lossless fail 0
- **Corruption proof บนไฟล์จริง:** flip 1 byte กลาง .ggf → ggs_load rc=-10 (CRC mismatch) จับได้ ✅
- suite: TIER1 88/88 + TIER2 4/4 · วิธีรันซ้ำ: `./build/goldberg_dual_probe <gguf> --all --save build/ggf_out`

### T1.2j Lazy read .ggf — seek ต่อ node ไม่โหลดทั้งไฟล์ ✅ (2026-08-18 — GGFReader ใน geo_goldberg_file.h + test_goldberg_lazy 52/52, §15.86)
- **GGFReader API:** `ggf_open` (header + index sphere_off[], RAM ≈ 12B × n_spheres คงที่
  ไม่ขึ้นกับขนาด data) · `ggf_chunk(k)` seek O(1) + ตรวจ tick ตรงตำแหน่ง · `ggf_read(off,n)`
  byte range ตามออฟเซ็ตต้นฉบับ (ข้าม chunk, เศษได้) · `ggf_verify` CRC lazy (RAM 64B) · `ggf_close`
- พิสูจน์: lazy full == ggs_load byte-for-byte (สอง path ให้ผลเท่ากัน) · random access ข้าม
  3 spheres · unaligned ranges · per-node tick detect (node 0 พัง node 1 อ่านได้) · 4MB
  เปิดโดยไม่ alloc data buffer
- ไฟล์จริง: lazy == full + verify PASS บน .ggf Kokoro (4KB / 1.1MB / 512B)
- suite: TIER1 89/89 + TIER2 4/4 · วิธีรันซ้ำ: `./build/test_goldberg_lazy`

### T1.2k Single read path: walk clock + dedup registry + GGFReader ✅ (2026-08-18 — core/geo_ggf_walk.h + test_ggf_walk 29/29, §15.87)
- **core/geo_ggf_walk.h** — รวม 3 ชิ้น: walk clock (state = seed/round/tick → live tensor,
  rq_t deterministic จาก (seed,t) — ไม่ต้อง index) + dedup registry {tensor_id → home}
  (dup → resolve → home .ggf — dedup ระดับไฟล์) + GGFReader (lazy open-on-demand, RAM คงที่)
- API: ggf_walk_init/pos/live/to/home/read/dedup_bytes/coverage — enter-anywhere ผ่าน fibo_walk_to
- test_ggf_walk 29/29: registry จาก tied_dedup_scan จริง (3 dups + 1 ว่าง), coverage Σ==n,
  read by state 11/11, dup → home เปิดไฟล์ home เท่านั้น (lazy), read_at ทุกตำแหน่ง,
  corrupt → verify จับ, multi-sphere, deterministic replay
- **ไฟล์จริง (--walk):** Qwen2.5 291/291 · Qwen3 310/310 · Kokoro 775/775 — fail 0
  · **dedup จับ tied pair จริง: output.weight == token_embd.weight (137.9 MB — เก็บ 290 ไฟล์แทน 291)**
- 🐛 Windows 512 open-file limit → fopen ล้มที่ ~509 (Kokoro 775) — แก้ _setmaxstdio(2048)
- suite: TIER1 90/90 + TIER2 4/4 · วิธีรันซ้ำ: `./build/goldberg_dual_probe <gguf> --all --save <dir> --walk`

### T1.2l ggf_mmap — อ่าน .ggf ตรงจากเพจ (zero-copy) ✅ (2026-08-18 — GGFMap ใน geo_goldberg_file.h + test_goldberg_mmap 48/48 + tools/ggf_mmap_bench, §15.88)
- **GGFMap API:** `ggf_map` (mmap ทั้งไฟล์ Windows/POSIX + index จาก mapping — ไม่มี seek)
  · `ggf_map_node(k)` คืน POINTER ตรงเข้า data (zero-copy, ตรวจ tick, NULL ถ้าพัง)
  · `ggf_map_chunk/read` drop-in แทน lazy · `ggf_map_verify` CRC เหนือ mapping · `ggf_unmap`
- test 48/48: zero-copy pointer ตรง + คงที่, random 1/3 spheres, unaligned, verify + corrupt,
  tick flip → NULL, mmap == lazy ทุก chunk (drop-in), tail partial, empty/bad magic/L5, unmap ปลอดภัย
- **bench ไฟล์จริง:** lazy ~28 MB/s seq / 11-14 MB/s rand vs **mmap ~1.3-1.8 GB/s seq / 123-1006 MB/s rand**
  (~45× seq, ~10× rand) · `make ggf_bench` → `./build/ggf_mmap_bench <file.ggf>`
- suite: TIER1 91/91 + TIER2 4/4 · ต่อยอด: ใช้ ggf_map_node ใน ggf_walk_read (walk clock + zero-copy)

### T1.2m Walk clock + mmap zero-copy + save ผ่าน mmap view ✅ (2026-08-18 — ggf_save_map + walk-read ผ่าน GGFMap, test_ggf_walk_mmap 38/38, §15.89)
- **ggf_save_map** — เขียน .ggf ผ่าน mmap VIEW (แทน fwrite): verify จาก view เองก่อน flush
  → อ่านด้วย GGFMap ได้ทันที · deterministic == ggs_save byte-for-byte
- **geo_ggf_walk.h + GGFMap read path**: ggf_walk_map_open (open-on-demand ต่อ home file)
  · ggf_walk_read_map (drop-in) · **ggf_walk_node_map (zero-copy pointer เข้า mapping ของ home)**
- test 38/38: save_map → GGFMap อ่านทันที lossless + CRC, dup → home map (เปิด 8 maps จาก 11),
  zero-copy pointer ตรง, read_map == read (lazy) ทุก byte, enter-anywhere, corrupt จับ
- **benchmark ใหม่ (probe --walk ใช้ mmap): Qwen2.5 291/291 @ 938.1 MB/s · Qwen3 310/310 @ 378.7 ·
  Kokoro 775/775 @ 875.7 — fail 0 · เทียบ lazy 27.5 MB/s = เร็วขึ้น ~14-34×**
- suite: TIER1 92/92 + TIER2 4/4

### T1.2n Checkpoint/replay ของ .ggf — save + manifest → fresh-process restore ผ่าน walk clock ✅ (2026-08-18 — core/geo_ggf_ckpt.h + test_ggf_ckpt_replay 17/17 + tools/ggf_checkpoint_replay, §15.90)
- **geo_ggf_ckpt.h**: manifest .mfp (GGRP · seed/ticks/cycles/n/dup_bytes/data_bytes + entry name[128]/size/home_of)
  — เก็บแค่ seed+method (rq คำนวณใหม่ ไม่ต้องเก็บ) · paths derive จาก name · ggf_ckpt_write/read/replay
- **tools/ggf_checkpoint_replay.c**: checkpoint (GGUF จริง → dedup → save เฉพาะ home ผ่าน ggf_save_map
  + manifest) → **spawn fresh process** → replay (manifest + reopen GGUF + GGFMap zero-copy) → lossless
- test 17/17: manifest roundtrip, replay ใน structures ใหม่ 11/11 lossless, corrupt manifest/.ggf จับ,
  home หาย จับ, deterministic, ว่างข้าม
- **ไฟล์จริง (fresh process): Qwen2.5 291/291 @ 758.5 MB/s (dedup 137.9 MB) · Qwen3 310/310 @ 694.1 ·
  Kokoro 775/775 @ 600.8 — fail 0**
- 🐛 Kokoro ล้ม 24/775: ชื่อ tensor ~63 ตัวถูกตัดที่ NAME_LEN=64 → derive path ไม่ตรงไฟล์จริง
  → แก้เป็น 128
- suite: TIER1 93/93 + TIER2 4/4 · วิธีรันซ้ำ: `make ggf_ckpt` → `./build/ggf_checkpoint_replay <gguf> --ckpt-dir <dir>`

### T1.2o Manifest v3: provenance + CRC64 checksum ✅ (2026-08-18 — geo_ggf_ckpt.h v3 + test_ggf_ckpt_replay 24/24, §15.91)
- GgfCkptHeader 312B: created_utc (เมื่อไหร่) · note[64] (ใคร/อะไร) · model[192] (โมเดลไหน) · crc64
- **crc64 ECMA-182 (poly เดียวกับ kis_crc64, seedable chain ได้)** ครอบ header (ยกเว้น crc64 field)
  + ทุก entry — แก้ manifest ตรงไหนก็จับได้ (rc=-7)
- test 24/24 (+7): provenance ตรง, tamper 6 จุดจับ (ชื่อ/size/home_of/seed/note/model) · T5 แก้ junk 64→512B
- **ไฟล์จริง:** replay 291/291 @ 737.8 MB/s ผ่าน v3 · แก้ manifest จริง 1 byte → replay ปฏิเสธทันที
- suite: TIER1 93/93 + TIER2 4/4

### T1.2p --verify (ไม่มีโมเดลต้นทาง) + checkpoint กลางรอบ ✅ (2026-08-18 — manifest v4 + test_ggf_ckpt_replay 35/35, §15.92)
- manifest v4 (320B): + ckpt_round/ckpt_tick (ครอบ crc64) — replay เริ่มจากจุดนั้น อ่านเฉพาะ
  tensor ที่ live ตั้งแต่ (round,tick), ก่อน checkpoint ข้าม (out_skip)
- ggf_ckpt_verify(dir): manifest crc64 + ทุก home .ggf (map + n_bytes ตรง + CRC32 ต่อไฟล์)
  — ไม่ต้องมีโมเดลต้นทาง
- tool: `--verify <dir>` (แสดง provenance) · checkpoint `--ckpt-round R --ckpt-tick T`
- test 35/35 (+11): verify ดี/corrupt/หาย/n_bytes ผิด/manifest แก้จับ · mid-round (72,0)
  pending 6/skip 5/fail 0 · dup (t4) pending แม้ home (t0) skip
- ไฟล์จริง: mid-round replay 146/146 @ 612.7 MB/s (skip 145) · verify 290/290 ไม่มีโมเดล
  · flip 1 byte จริง → 289/290 จับ
- suite: TIER1 93/93 + TIER2 4/4

### T1.2q Delta checkpoint — เก็บเฉพาะที่เปลี่ยนจาก base ✅ (2026-08-18 — manifest v5 + test_ggf_ckpt_replay 52/52, §15.93)- manifest v5 (584B): header + base_dir[256] · entry + status u8 (140B): STORED / SAME
- ggf_ckpt_cmp_base — diff ระดับไฟล์: เทียบ CRC32 ของ data region กับ base .ggf → SAME/STORED
- replay/verify merge: SAME → base_dir · STORED → dir นี้ (chain ต่อได้) · SAME ไม่มี base → rc=-11
- tool: checkpoint `--delta <base_dir>` · replay/verify อ่าน base_dir จาก manifest อัตโนมัติ
- test 52/52 (+17): diff จับถูก (เปลี่ยน 3/ไม่เปลี่ยน 5) · dup สถานะตาม home · delta เก็บเฉพาะ STORED
  · verify merge base+delta 8/8 · base/delta corrupt จับ · base หาย → เก็บเอง · tamper status/base_dir จับ
  · SAME ไม่มี base → ปฏิเสธ
- ไฟล์จริง: delta ไม่เปลี่ยน same 290 stored 0 (44K = manifest) → merge 291/291 · mixed (ลบ base 9)
  stored 9 same 281 → 291/291 · tamper base → replay FAIL 290/291 + verify FAIL 289/290
- suite: TIER1 93/93 + TIER2 4/4

### T1.2r Delta chain multi-level + GC ✅ (2026-08-18 — GgfCkptChain + ggf_ckpt_gc + test_ggf_ckpt_replay 74/74, §15.94)
- chain: manifest ของแต่ละระดับอ้าง base_dir ต่อกัน (base → delta1 → delta2 → ...) · tail = เต็ม
- ggf_ckpt_chain_open — เดิน chain ตรวจ crc64 ทุกระดับ (provenance chain) · จับวน (-3)/ลึกเกิน (-2)/n ไม่ตรง (-4)
- ggf_ckpt_chain_path — resolve ไฟล์จริง: เดิน head → ลงลึกจนเจอระดับที่ STORED · cmp_base เทียบผ่าน chain (ไม่เก็บซ้ำ)
- replay/verify ใช้ chain resolution · tool: `--gc <head> [--ckpt-dir <new>]`
- GC: คัดลอก home .ggf ทั้งหมดลง snapshot (ตรวจขนาด+CRC หลังคัดลอก) + manifest เต็ม self-contained → ลบ chain เดิมได้
- test 74/74 (+22): chain 3 ระดับ resolve/replay/verify · corrupt ไฟล์ลึกจับ · manifest กลางพังจับ · chain วนจับ · GC snapshot ลำพังหลังลบ chain
- ไฟล์จริง: c1→c2→c3 (chain) replay 291/291 lossless @ 737 MB/s · tamper ไฟล์ลึก verify จับ · GC → ลบ chain → replay snapshot ลำพัง 291/291

### T1.2s Auto-GC — chain ลึกเกิน threshold → รวม snapshot อัตโนมัติ ✅ (2026-08-18 — ggf_ckpt_auto_gc + --max-chain + test_ggf_ckpt_replay 81/81, §15.95)
- ggf_ckpt_auto_gc(base, new, max_chain): depth ≥ max → ggf_ckpt_gc รวมเป็น base ใหม่ (chain ลึก 2 เสมอ) · < max → ใช้ base เดิม
- ggf_ckpt_gc สร้าง dir เอง (ggf_ckpt_mkdirs — Windows-safe) · tool: --delta <base> --max-chain N (default 4) + พิมพ์ [CKPT AUTO-GC]
- test 81/81 (+7): ไม่ GC เมื่อลึก 3 < 4 · GC เมื่อ 3 ≥ 3 (home 8) · d3 บน GC base ลึก 2 · replay d3 11/11 lossless
- ไฟล์จริง Qwen2.5 --max-chain 3: ag1→ag2→ag3 (ลึก 3) → ag4 AUTO-GC → ag4_base (290 home · 525 MB) → delta 0 ไฟล์ · replay 291/291 @ 698 MB/s · verify 290/290 · ลบ chain → replay ลำพัง 291/291 · ag5 ต่อ (ลึก 2) ผ่าน

### T1.2t GGFS — checkpoint dir เป็น geometric filesystem ✅ (2026-08-18 — geo_ggf_fs.h + ggf_fs_probe + test_ggf_fs 28/28, §15.96)
- GgfsMount: mount (manifest+chain) → walk clock + dedup registry + paths (chain resolve) + zero-copy mmap + CRC verify-on-open
- ggfs_mount/unmount · count/find/stat (rq/tick/home/dup/status/level) · read (enter-anywhere) · read_by_name · node (zero-copy) · walk_steps สะสม
- tool: make ggf_fs → ./build/ggf_fs_probe --mount <dir> [--read <name> [r t]] [--sweep r1 t1 r2 t2]
- test 28/28 (TIER1 94/94): mount/find/stat · 3 states bytes เหมือน + steps ต่าง · dup → home · ว่าง -1 · corrupt -4 · deterministic · zero-copy · chain mount (level 0/1) · mid-round pending
- ไฟล์จริง Qwen2.5: sweep 3 คู่ states (เต็ม + delta chain 2 ระดับ) 291/291 byte-for-byte fail 0 · steps 250,860/251,985/248,820/253,995 · tied pair token_embd→output.weight crc32 เดียวกัน

- แนวคิด: 20736 = 12 pent × 1728 (dodeca) = 20 tri (icosa) — spike = dual transform (face ↔ vertex)
- ทดสอบ: วางข้อมูลลงสนาม → อ่านผ่าน view dodeca (GeoType 12) และ view icosa (GeoType 20) → byte-for-byte
  เหมือนกัน — พิสูจน์ "container เลือก GeoType ได้โดยข้อมูลไม่ต้องย้าย"
- จุดพอดี: ยืนยัน rescope "geometry = template เท่านั้น" ด้วยหลักฐานรันจริง
- **Wire:** `core/geo_ghost_lift.h` — `ghost_read_rule_walk` (เดินนาฬิกาจาก state ไปตำแหน่ง live (to, to%12) → thaw ผ่าน bond ตรง — แทน pile lookup; delta self-describing) · `core/geofs_core.h` — GeosVolume + walk_round/walk_tick/walk_steps (runtime clock) · `geos_read_ghost` ใช้ walk variant + state เดินหน้า · `tools/rule_e2e.c` พิสูจน์ enter-anywhere 3 start states
- **ผล:** rule_e2e lossless 6/6 + ไฟล์จริง (5084 B) ทุก start state ✓ · rule-mismatch 137/137 blocked ✓ · steps (0,0)=1,243,806 / (72,2)=1,244,668 / (143,11)=1,243,807 — state ต่าง → เส้นทางต่าง แต่ข้อมูลเดียวกัน · make test TIER1 84/84 + TIER2 4/4
- **บทเรียน:** delta detection จาก payload ปลอดภัยใน geofs (blob ≥ 266B > block 64B → raw ไม่ผ่าน validation) · wang gate ไม่ใช่ lookup — ยังอยู่ครบ · walk state ไม่ serialize (deserialize = enter anywhere ที่ (0,0)) · materialize เดิมยังอยู่ (ghost_delta_measure ใช้)
- วิธีรันซ้ำ: `make rule_e2e` → `./build/rule_e2e [file ≤4MB]`

### T1.2u กฎ ×2 ของ 12-gon — dodecagon ทุกชั้นมาเป็นคู่ ✅ (2026-08-18 — test_dodeca_x2 28/28, §15.97)
- stride-2 → 2 hexagons พอดี: odd {1,3,5,7,9,11} + even {2,4,6,8,10,12} (หมุน 30°) — sim hex-construction.html วาดแค่ตัวคี่ = ×2 ที่ขาด
- divisor law: stride k → gcd(k,12) cycles × ยาว 12/gcd(k,12) (k=1..6): 1 dodecagon · 2 hex · 3 sq · 4 Δ · 1 star · 6 diameters
- 6 diameters ทุกเส้นผ่าน center = 12 rays ถึง 1 ศูนย์กลางเดียว (ชั้นที่ 6 = 2 ชุด)
- hexagon คู่ไขว้กัน 12 จุด = regular inner 12-gon (radius = √3/(2·cos15°) = 0.896575…, spacing 30°, offset 15°) — โครงสร้างซ้อนตัวเอง (self-similar nesting)
- fan triangles 12 ชิ้น share center: ทุกคู่ติดกันอยู่คนละข้าง radial edge ร่วม (สลับ ∧∨ — sawtooth) · 2 parity orbits ละ 6 (6+6) · วงปิด tri11↔tri0 สลับด้วย
- test 28/28 (TIER1 95/95): D1 stride-2 = 2 hex · D2 gcd law k=1..6 + diameters ผ่าน center · D3 inner 12-gon (analytic radius + spacing + offset) · D4 fan ∧∨ 6+6 + ครบรอบ · D5 3 squares = orbit R120° (3 lanes Rail_sync) + R30 สลับ hexagon parity · D6 4 Wang Δ = tetrahedron (equilateral · partition · centroid=center · 3+1 complement · C3 axis)

### T1.2v Walk = Sync — docs วิเคราะห์ stride-37 ร่วม 3 layer ✅ (2026-08-18 — docs/WALK-SYNC.md, §15.98)
- เดิมเข้าใจว่า rail_sync/phase_rail เป็นของ FGLS_new (memory) — จริงๆ `core/infra/geo_rail_ring.h` + `core/geo_frame_seek.h` อยู่ใน repo ปัจจุบัน
- 3 lanes A/B/C = สำเนา walk เดียวกัน offset 120° = 480 ticks (A=base, B=base+480, C=base+960) · stride-37 เดียวกับ tring_walk/frame_seek
- walk = addressing (ที่ไหน) · rail = sync (เมื่อไหร่) — เลขคณิตเดียวกัน (37·i mod N) — bijection → deterministic O(1) ทั้งคู่
- PARK (XOR=0) → freeze ที่ tick-12 boundary (geo_bfs_hub P5HRibcage) → ต้นตระกูล checkpoint (geo_ggf_ckpt)
- 1440 = 2×720 (กฎ ×2 hex+tri) · 480 = 1440/3 (3-fold) · 12 = FS_TICKS = WANG_WIN_SIZE
- บทเรียน memory: XOR ≠ angular distance (aliasing) → modular · ring 512 ไม่ครบรอบ (35.6%) → 1440 — sync ต้องอยู่บน ring เดียวกับ addressing

### T1.2w test_walk_sync — พิสูจน์ Walk = Sync บน ring จริง ✅ (2026-08-18 — 12/12, TIER1 96/96, §15.99)
- rail_ring build → verify: ทุก lane A/B/C ครอบ 1440 enc ครบ (bijection) — lane lock B=A+480, C=A+960 ทุก i (120° คงที่)
- stride-37 bijection บน 1440 + 720 (tring) — เดินครบกลับจุดเริ่ม · modular distance ระหว่าง lane = 480 = 1440/3 เสมอ (sync ไม่ drift)
- freeze tick-12 = 120 จุด = WANG_WIN_COUNT (หนึ่ง freeze ต่อ Wang window) · θ=enc/4 integer ที่ freeze point (3 lanes บน 1° grid)
- geo_frame_seek_verify() == 0 · รวมเป็น TIER1 (96/96)

### T1.2x กฎ N-gon ทั่วไป — generalization ของกฎ ×2 ✅ (2026-08-18 — test_dodeca_x2 28→67/67, §15.100)
- verify_n_gon(N) สำหรับ N = {6,8,10,12,16,24}: stride-2 → 2 cycles ของ N/2 (24-gon → 2 dodecagon) · divisor law gcd(k,N) ทุก k
- inner ring: N จุดไขว้ = regular inner N-gon — radius analytic cos(2π/N)/cos(π/N) (12: 0.896575 · 16: 0.941979 · 24: 0.974261) · spacing 2π/N · offset π/N
- fan N triangles สลับ ∧∨ = N/2+N/2 (24 → 12+12) — กฎ ×2/3-fold = กรณีเฉพาะ N=12

### T1.2y ring = กฎ N-gon 2 สเกล — 720 = 6×120 · 1440 = 12×120 ✅ (2026-08-18 — test_walk_sync 12→24/24, §15.101)
- sector indices (enc/120) obey divisor law (6 spokes → gcd(k,6) · 12 faces → gcd(k,12)) — เชื่อม G-section ของ test_dodeca_x2
- กฎ ×2 ระดับ ring: 12 faces → 2 hexagons ของ faces (คี่/คู่) — D1 เดียวกันบน faces ของ 1440-ring · 6 spokes → 2 triangles
- กฎ ×2 ระดับ slot: polarity 60/60 ภายในทุก sector (ROUTE/GROUND = ∧∨) · ทุก 120-step window → 60/60 (sawtooth balance)
- stride-37: gcd กับ ring และ sector (120) = 1 → bijection 2 สเกล (walk เยี่ยมทุก sector 120 ครั้ง + slots ครบไม่ซ้ำ)
- สรุป: 720 = 6-gon × vertex 120 (parity ใน) · 1440 = 12-gon × vertex 120 — กฎเดียวกันคนละสเกล

### T1.2z benchmark walk ครบรอบ ✅ (2026-08-18 — test_walk_bench 12/12, §15.102, TIER1 97/97)
- stride-37 ครอบครบ 720/1440 ครบ 1 ครั้ง (bijection) vs stride-36/42 ครอบ n/gcd(n,k) (วนซ้ำ)
- 720: stride-37 9.93 ns/step · random 12.50 — bijection ไม่ช้าเท่ากับ random access, เร็วกว่าด้วยซ้ำ
- random access n ก้าวครอบ < n (ไม่การันตี) → ทำไม walk ต้องเป็น bijection (address ครบทุกพิกัด)

### T1.2aa nesting ซ้อนต่อ ✅ (2026-08-18 — test_dodeca_x2 67→90/90 H-section, §15.103)
- scale factor คงที่ s = cos(π/6)/cos(π/12) ≈ 0.89658 ทุกระดับ — r: 0.8966→0.8038→0.7207→0.6462→0.5793 (5 ระดับ)
- self-similar: 2 hex ไขว้ → inner 12-gon → ไขว้ซ้ำ → inner-inner … ลู่เข้าศูนย์กลาง · offset +15°/ระดับ
- บทเรียน: scale กลับด้านจับได้จาก radius จริง · ต้อง sort จุดไขว้ตามมุม (cyclic order) ก่อนป้อนระดับถัดไป

### T1.2ab ring จริง = กฎ N-gon ที่ N=1440 ✅ (2026-08-18 — test_walk_sync 24→34/34 S-section, §15.104)
- stride-2 บน 1440 slots → 2 cycles ของ 720 (กฎ ×2 ระดับ slot) + parity สลับทุกก้าว frame_enc
- s(N) = cos(2π/N)/cos(π/N): s(1440) ≈ 0.99999286 — (1−s)·N² → 3π²/2 ≈ 14.8044 (analytic)
- s¹²: 12-gon 0.2698 vs 1440-ring 0.99991 — สเกลเล็กหดชัด, สเกลใหญ่แบน (พื้นเรียบ)

### T1.2ac: test_parity_sector — เชื่อม rules x2 กับระบบจัดเก็บจริง ✅
- **วันที่:** 2026-08-19
- **ไฟล์:** tests/test_parity_sector.c (43/43)
- **Makefile:** TIER1 98/98 + TIER2 4/4 เขียว
- **พิสูจน์:** parity สลับ → cross-parity exclusion → sector balance → cache locality → dedup parity
- **§** 15.105

### T1.2ad: test_cache_locality — cache simulation scatter vs sorted ✅
- **วันที่:** 2026-08-19
- **ไฟล์:** tests/test_cache_locality.c (18/18)
- **Makefile:** TIER1 99/99 + TIER2 4/4 เขียว
- **พิสูจน์:** scatter O(1) lookup 7-9x เร็วกว่า sorted O(log n) · stripe ชนะsorted · uniform distribution
- **§** 15.106
