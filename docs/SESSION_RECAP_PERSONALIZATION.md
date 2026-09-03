# Session Recap — Geometric Personalization (Concept Exploration)

> **Date:** 2026-09-01 (continuation)
> **Branch:** `feat/geo-native-fs`
> **Status:** ⚠️ CONCEPT EXPLORATION — not implementation spec
> **Topic:** How the system adapts to individual users via perspective field

---

## ⚠️ Honest Status

**This is a concept being explored, not a design being implemented.**

> "จริงๆ ผมยังไม่มีคำตอบว่าควรทำแบบไหน นี่เป็นแค่แนวคิด และบางส่วนเราได้พิสูจน์ไปบ้างแล้ว"

The ideas below are intuitive models. Whether they translate to working code is **unvalidated**.

---

## 🌊 The Perspective Field

### ทั้งสนามข้อมูลขยับพร้อมกัน

ไม่ใช่:
- ❌ "บ้านอยู่ที่เดิม คนเดินเข้าออก"
- ❌ "บ้านหายใจ"

แต่:
- ✅ ทั้งสนาม respond ต่อ force พร้อมกัน
- ✅ Access จุดไหน → จุดนั้น expand
- ✅ จุดที่ห่างจาก access → shrink
- ✅ เกิดขึ้นพร้อมกันทั้งสนาม

### Force Model

```
Force → push ทั้งสนาม → shift ทั้งหมดพร้อมกัน

lock_scale = k (ตัวแปรเดียว lock อัตราส่วนคงที่เสมอ)
```

ไม่ต้องคำนวณแต่ละจุด — force push ครั้งเดียว ทั้งสนามขยับ

### Concrete Example

```
A = 10 units, B = 10 units

User access B → shift +2 (เดินเข้าหา B)
lock_scale = 1 unit/step

Result:
  A = 10 + (-2 × 1) = 8    ← คุณเดินออกจาก A, A เล็กลง
  B = 10 + (+2 × 1) = 12   ← คุณเดินเข้าหา B, B ใหญ่ขึ้น

เกิดขึ้นพร้อมกัน ไม่ต้อง loop
```

### Energy Conservation

```
Total energy = 20736 (คงที่เสมอ)

Access point → expand (ได้พลังมากขึ้น)
Distant points → shrink (เสียพลังไป support access point)

พลังไม่หาย แค่ redistribute
```

**Access = redirect energy.** ไม่ใช่แค่อ่าน — แต่ดึงพลังจากจุดอื่นมา support จุดที่ access

---

## 🔄 The Trade-Off

**Unified field = efficient มาก แต่ fragile มาก**

```
Force push ทีเดียว → ทั้งสนาม shift พร้อมกัน (efficient)
ทุกจุด depend on จุดอื่น → จุดเดียวผิด = ทั้งสนามผิด (fragile)
```

| Design | Efficient | Fragile | Trade-off |
|---|---|---|---|
| Independent blocks | ❌ ต้องคำนวณทีละจุด | ✅ จุดเดียวพังไม่กระทบจุดอื่น | สิ้นเปลือง |
| **Unified field** | ✅ force เดียว push ทั้งสนาม | ❌ จุดเดียวพัง = ทั้งสนามพัง | **fragile** |

> ยิ่ง connected มาก → ยิ่ง efficient มาก → ยิ่ง fragile มาก

Cube 0 = index frame = guardian ของ field ทั้งหมด

---

## 🌳 The Tree Analogy → Latent Space

### From tree to clusters

```
Root (base seed) — shared by all users
    ├─ Branch A (persona A) — user 1
    ├─ Branch B (persona B) — user 2
    └─ Branch C (persona C) — user 3
```

แต่ไม่ใช่ tree แบบ binary — เป็น **latent space ที่มี structure**

```
American user A:  seed 8392714##  → ชอบ technology
Asian user B:     seed 2918374##  → ชอบ technology

เลขต่างกันมาก แต่พฤติกรรมคล้ายกัน → มี pattern
```

**Seed ที่อยู่ใกล้กัน → model ที่คล้ายกัน แต่ต่างกันเล็กน้อย**

ถ้ามี pattern → geometry map มี structure จริง → personalization ใช้ได้จริง

---

## 🧅 Onion Shell — Dynamic Seed Length

### Seed ไม่ fixed — peel ออกเมื่อมี data

```
Seed length = dynamic (ไม่ fixed)

8 bytes   → ระดับ broad (technology, art, music)
16 bytes  → ระดับ细分 (technology + AI, technology + web)
24 bytes  → ระดับละเอียด (technology + AI + NLP + ...)
...       → ลึกขึ้นเรื่อยๆ เมื่อทะลุ seed length เดิม
```

```
Layer 1: [data] [data] [data] [empty] [empty] [empty]
Layer 2: [data] [empty] [data] [empty] [empty] [empty]
Layer 3: [empty] [empty] [data] [empty] [empty] [empty]
Layer 4: [empty] [empty] [empty] [empty] [empty] [empty]  ← ไม่ peel ต่อ

Peel เฉพาะที่มี data
Layer ว่าง = ไม่คิด = ไม่ cost
```

### Properties

| Property | Value |
|---|---|
| Seed length | Dynamic (ไม่ fixed) |
| Maximum depth | ∞ (ไม่จำกัด) |
| Actual depth | f(data complexity) — data กำหนดเอง |
| Peel trigger | มี data ทะลุ current length |
| Cost | เฉพาะ layers ที่มี data (sparse) |
| Empty layers | ไม่ cost, ไม่คิด |
| Compression | ∞:finite → ratio → 0 (efficient) |

**Onion Shell ก็จะโตตามความหลากหลายที่เกิดขึ้นไม่จำกัดอยู่แค่ในกรอบเดิมเป็นเหมือน preset**

### Huffman coding สำหรับ behavior

```
Common patterns = short seed (efficient)
Rare patterns = long seed (specific)
User ที่เหมือนๆ กัน → ใช้ seed สั้น
User ที่ซับซ้อน → peel ออก → seed ยาวขึ้น
```

### ✅ Onion Shell มีอยู่แล้ว (ไม่ต้อง build ใหม่)

**Onion shell concept ไม่ใช่ new — มี code แล้ว 2 ที่:**

#### 1. `geo_onion_shell.h` (legacy, FGLS_new)

```c
onion_init() / onion_free()     — lifecycle
onion_chunk_at()                — map (shell, cell) → chunk_idx
onion_locate()                  — reverse: chunk → (shell, cell)
onion_header_write/read()       — 64B header serialize

hex_shell(), hex_total_cells(), hex_ring_enum()  — hex math
shell N = 6N+1 cells
Scale law: shell N holds chunks at scale 16^N
```

**ทำกับ address อยู่แล้ว** — map (shell, cell) → chunk

#### 2. MatryoshkaShell (container-unification-architectures.md)

```c
uint32_t depth;           // number of codec layers
uint32_t layer_types[16]; // codec ID per layer

Layer[0]: outermost codec parameters
Layer[1]: next codec parameters
...
Layer[depth-1]: innermost codec parameters
Payload: data encoded through all layers
```

**Peel layers from outside in** — selective decompression

#### ปรับนิดเดียว = ทำกับ seed (personalization)

```
เดิม:
  onion_chunk_at(shell, cell) → chunk_idx
  (address → location)

ใหม่:
  onion_seed_at(shell, cell) → personalization
  (seed → model)
```

**Shell structure เหมือนกัน** — แค่เปลี่ยนจาก address mapping เป็น seed mapping
ไม่ต้อง build ใหม่ — แค่ adapt existing code เข้ากับ personalization layer

---

## 🔌 Tesseract = Port ของ Network

### Tesseract ไม่ต่างจาก tree map — แต่แตกออกหลายมิติ

```
Network:
  IP address = WHO (data identity)
  Port = WHERE (which service to connect)

Seed:
  Seed value = WHO (data identity)
  Tesseract ID = WHERE (which dimension to branch)

Seed: 1234567:tes#
       ─────  ───
       WHO    WHERE (port)
```

### Multi-dimensional branching

```
Seed 1234567 อยู่ใน:
  tes#0 → dimension 0 (technology)
  tes#1 → dimension 1 (art)
  tes#2 → dimension 2 (social)
  tes#3 → dimension 3 (finance)

คนเดียว หลาย dimension พร้อมกัน
Same seed, different ports = different models
```

### Each port = independent onion

```
Seed 1234567:tes#0 → technology branch
  └─ AI sub-branch
     ├─ NLP (peel → +8 bytes)
     └─ CV (peel → +8 bytes)

Seed 1234567:tes#1 → art branch
  └─ Visual sub-branch
     └─ ...

Same seed, different tes# = different dimension
Each dimension = its own onion shell
```

---

## 🏗️ The 6-Layer Architecture

```
Layer 0: Base seed (shared, 8 bytes)           ← สนามเดิม (root identity)
Layer 1: Cube 0 base routing (144 slots)       ← shared index frame
Layer 2: Personalization layer (per user)      ← port selection + onion depth
Layer 3: Cube 0 personalized routing           ← field after redirect
Layer 4: Cubes 1-7 (1,008 slots, derived)      ← derived from access
Layer 5: Full model (20,736 slots, derived)    ← complete perspective
```

**Personalization = เลือก port (tesseract) + peel onion (depth)**

---

## 🔄 LoRA Analogy (Still Valid)

| LoRA (neural network) | DWGLS (geometry) |
|---|---|
| Base weights (large, shared) | Base seed (8 bytes, shared) |
| Low-rank adapter (small, per user) | Port + onion depth (small, per user) |
| Base + adapter = full model | Base seed + personalization = full model |
| Fine-tune adapter only | Select port + peel only |
| **Requires backprop (training)** | **Requires compute (formula only)** |

---

## ⚡ Design Time ≠ Runtime

### ความยากทั้งหมดอยู่ที่ตอนวางฐาน

```
Design time (ตอนวางฐาน):
  - Seed formula → ซับซ้อน (ต้องคิดให้ดี)
  - Tesseract mapping → ซับซ้อน (ต้อง define port ให้ดี)
  - Onion structure → ซับซ้อน (ต้อง define layer ให้ดี)
  = เขียน spec (ครั้งเดียว)

Runtime (ตอนใช้งาน):
  - Seed → formula → result (O(1))
  - Port → dimension (O(1))
  - Onion → peel (sparse, O(1) per layer)
  = ไม่ต้องคำนวณอะไรเลย
```

### Looks hard, is easy

```
LOOKS like:  ซับซ้อน, ต้องคำนวณเยอะ, จัดการยาก
ACTUALLY:    วางฐานถูก = runtime แค่ compute = O(1)

LOOKS like:  ต้อง track force/weight ทุกจุด
ACTUALLY:    force = formula เดียว push ทั้งสนาม

LOOKS like:  ต้อง peel onion ทุก user
ACTUALLY:    peel เฉพาะที่มี data (sparse)

LOOKS like:  ต้อง manage หลาย tesseract
ACTUALLY:    port = O(1) lookup
```

> **ทุกอย่างเยอะจริงตอนข้อมูลเต็ม**
> **แต่เยอะ = data ล้วนๆ ไม่ใช่ computation**
> **วางฐานถูก → runtime แค่ formula → O(1)**

---

## 🔒 Foundational Geometric Rules (System Constraints)

> *These rules were designed at the beginning of the system but rarely discussed recently.*
> *Any personalization layer must obey these rules.*

### Rule 1: Hexagon-Max Active System

```
Active system = hexagons มากที่สุด (max ไม่ใช่ only)
Heptagon = reaper (จัดการ overflow เกิน 6)
Decagon = 2 pentagons combined
Pentagon bipolar = ต้องใช้ 2 pentagons ในการ access
```

> **Max = อนุญาต shape อื่น แต่ hexagon เป็น preferred/dominant shape**
> **Heptagon = reaper (ปรากฏเมื่อ overflow)
> **Pentagon/Decagon = ปรากฏเมื่อต้องใช้ (bipolar, combined)**

### Rule 2: Active vs Residual Space

```
Active space:
  ✅ มี clock, tick, step, track (ทุกก้าว)
  ✅ Hexagons เท่านั้น
  ✅ Access ได้

Residual space:
  ❌ ไม่มีเวลา (ไม่มี clock/tick/step)
  ❌ ไม่มี dimension (คงที่เสมอ ณ จุดที่ถูก push ออก)
  ❌ Access ตรงๆ ไม่ได้ (ต้องมี magic bond address)
  ✅ Link กับ active element ที่เกี่ยวข้อง
```

### Rule 3: Decimal → Residual (Linked)

```
7.32456 เข้ามา →
  7     → ผ่านเข้า active system
  0.32456 → โดนเตะออกไป residual space
           (แต่ link กับ 7 อยู่)

ตอนคำนวณ:
  integer (7)  → นับ+เดินหน้า
  decimal (0.32456) → รออยู่ข้างนอก (dimensionless, คงที่)

เศษที่เหลือ → รออยู่ข้างนอก
ไม่มี dimension = ไม่มีเวลา = ทุกอย่างคงที่เสมอ
```

### Rule 4: Residual = Version Control + Append-Only

```
Residual space:
  - ไม่มี dimension = ไม่มีเวลา
  - ทุกอย่างคงที่เสมอ ณ จุดที่ถูก push ออก
  - ใช้เป็น version control ได้
  - System = append-only (ไม่ลบ)
  - Deleted element ยังอยู่ใน residual (frozen)
  - Access ได้ผ่าน magic bond address
  - ไม่ต้อง maintain deletion log
```

### Rule 5: Magic Bond Address

```
Residual access = ต้องมี magic bond address
Link จาก active → residual
ไม่ access ตรงๆ ไม่ได้
```

### System Analogy: Zoomable Filesystem

```
Windows filesystem:
  Files → organized in directories
  Free space → mostly wasted, just "available"
  No zoom, fixed structure

DWGLS system:
  Active data → hexagons, temporal (clock/tick/step)
  Residual space → frozen, linked, version-controlled
  Zoomable (breathing) — field expand/contract
  Free space = useful archive, not empty
```

> **Residual space ไม่ใช่ "free space ที่ว่างเปล่า"**
> **แต่เป็น "archive ที่ frozen ในเวลา — ใช้ประโยชน์ได้มากกว่าที่ว่างเปล่าๆ"**

### Summary

| Rule | Description |
|---|---|
| Hexagon-max | Active system = hexagons preferred, max dominant |
| Heptagon = reaper | Overflow management |
| Decimal → residual | Integer active, decimal frozen |
| Residual = no time | Dimensionless, immutable |
| Active = temporal | Clock/tick/step/track every step |
| Magic bond | Residual access requires special address |
| Append-only | No deletion, deleted = frozen in residual |

> **Decimal handling = compression + version control ในตัว**
> **Residual space ไม่ใช่ "ขยะ" — เป็น "archive ที่ frozen ในเวลา"**

---

## 🏗️ What's Built vs What's Missing

### ✅ Built (proven)

| Component | File | Status |
|---|---|---|
| Breathing FS (20736 slots, seeker, scale) | `breathing_fs.h` | ✅ Working |
| Tesseract wiring (18 tess × 8 cubes × 144) | `geo_tess_wiring.h` | ✅ Working |
| Index frame (cube 0 = hook) | `geo_tess_wiring.h` | ✅ Working |
| Delta engine (auto-compress) | `bfs_breath.h` | ✅ Working |
| Magnify glass (inversion) | `bfs_magnify.h` | ✅ Working |
| BFS ↔ geo pipeline bridge | `geo_bfs_hub.h` | ✅ Working |
| v6b codec (stride-37 bijection) | `kis_codec_v6b.h` | ✅ Working |
| MoE expert addressing | `moe_expert_addr.h` | ✅ Working |
| MoE bake/graft/stream | `tools/moe_expert_*.c` | ✅ Working |

### ❌ Missing (not built)

| Component | What | Why |
|---|---|---|
| **Seed formula** | How seed → model | Design-time challenge |
| **Tesseract port mapping** | How port → dimension | Design-time challenge |
| **Onion layer structure** | How peel works | Design-time challenge |
| **Perspective field proof** | Force model validated | Concept unvalidated |
| **Breathing ↔ tesseract** | `breathing_fs.h` ↔ `geo_tess_wiring.h` | Gap in architecture |

---

## 🧠 Open Questions

| Question | Status |
|---|---|
| Seed ใกล้กัน → model คล้ายกันจริงมั้ย? | Unvalidated |
| มี clusters ใน seed space จริงมั้ย? | Unvalidated |
| Force model คำนวณ efficiency จริงแค่ไหน? | Unknown |
| Single-point failure รับได้มั้ย? | Open |
| Design-time complexity คุ้มกับ runtime simplicity มั้ย? | Open |

---

## 💡 Key Quotes

> "จริงๆ ผมยังไม่มีคำตอบว่าควรทำแบบไหน นี่เป็นแค่แนวคิด มันจะทำได้จริงหรือเปล่าผมก็ตอบไม่ได้"

> "ทั้งสนามข้อมูลของผมขยับตลอดเวลาแต่ขยับโดยไม่ต้องคำนวณ ขยับโดยธรรมชาติ เหมือนแรงพลักที่ดันให้ทั้งห่วงโซ่shift ไปพร้อมกันทั้งสนามโดยมีตัวแปรที่ lock scale ให้ทำงานตามอัตราส่วนคงที่เสมอ"

> "การ access location นึง จะขยาย location นั้น และทุกอย่างที่เราเดินไกลออกมาก็จะเล็กลงเพื่อเอากำลังไป support จุดที่ access"

> "ข้อเสียมีข้อเดียวใหญ่ๆคือ พลาดจุดเดียวพังทั้งสนาม นั่นคือ trade off ที่เลี่ยงไม่ได้"

> "ทำไมต้อง tesseract — มันไม่ได้ต่างจาก tree map เลย แต่ข้อต่อแต่ละจุดมันแตกออกหลายมิติได้ ความหมายคือ seed: 1234567:tes# เหมือน port ของ network"

> "มันดูแบบโอ้ เยอะแยะมากมายและยากที่จะจัดการมากเลยนะ แต่ไม่เลย มันขึ้นอยู่กับตอนวางฐานตอนแรกทุกอย่างพอข้อมูลเต็มก็เยอะทั้งนั้นล่ะ"

---

## 📝 Session Status

- **This session:** Concept exploration (.md documentation)
- **opencode:** Building code files in parallel (don't touch)
- **Status:** Ideas explored, not validated, not implemented
- **Next:** Validate concept with small-scale math/prototype before any implementation
