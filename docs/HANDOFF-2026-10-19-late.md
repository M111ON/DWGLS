# HANDOFF 2026-10-19 (late) — ANN kernel fix deployed + handoff numbers corrected

**DWGLS repo:** `I:\DWGLS-native-fs` · `feat/geo-native-fs` · HEAD `f3d5799` → see §6
**ANN repo:** `I:\tools\ann` (git root `I:\tools`, branch `main`, remote `M111ON/FS_tools`)
**Live:** ann `:8096` healthy — entries **61,727**, `routed:1`, `embed ok`, `fog {replayed:7, shut:7}`

Run `git log --oneline -1` in each repo for the current sha (do not freeze — this file is a
record of a session, not a live pointer).

---

## 0. TL;DR — session นี้ทำอะไร

1. **แก้ตัวเลขที่ผิดใน handoff เดิม** — "query ≈ 16 ms" ไม่ reproduce → จริงคือ **32.5 ms**
   และ "scan ≈ 2 ms" เป็น **ต้นทุนของการไม่ทำอะไร** (idle server)
2. **merge kernel fix เข้า server จริง** — variant G (4 accumulators, exact, pure-double)
   → kernel-level **1.5–2.0×**, ผลเหมือนเดิมทุกประการ
3. **refactor เป็น helper** `lz_dot()` — comment 22 บรรทัด → 8, hot loop เหลือ 1 บรรทัด
4. **push ทั้ง 2 repo** (DWGLS 12 commits + ANN 43 commits)

---

## 1. ⚠️ ตัวเลขที่ผิด และที่ถูกแล้ว

### 1.1 "query ≈ 16 ms" → **32.5 ms** (ไม่ reproduce)
วัดด้วย protocol เดียวกัน: **32.5 ms** mean (28.2 ms p50) ที่ `topb=2`
ที่มา: `experiments/ann-hnsw-vs-anchor-2026-10-14/RESULTS-ROUTE-COST-2026-10-19.md` §0

### 1.2 "scan ≈ 2 ms" → **34.3 ms** (เป็นต้นทุนของการไม่ทำอะไร)
`bench_latency_split.py` เดิมส่ง **zero vector** — คอมเมนต์เดิมเขียนว่า "the scan loop runs
regardless of values" ซึ่ง **ไม่จริง**: zero vector ไม่เข้า route bucket → server ให้คะแนน
0 รายการ → **ไม่ scan เลย**

```
zero vec : p50  1.5 ms  items_scored=0     mode=brute
real vec : p50 34-60 ms  items_scored=24000 mode=anchor
```
ของจริง: **embed 10.5 ms + scan 34.3 ms** (8 queries × 5 reps)
แก้แล้วใน `e495151` (ส่งผ่าน sidecar :18095 ตัวเดียวกับ server + **abort ถ้า vec-only ให้คะแนน 0**)
→ แต่ **doc** `ann/docs/IMPACT-ANALYSIS-2026-10-14-vec-format.md` ยังมีตัวเลขเก่า → แก้แล้ว session นี้

### 1.3 "float/no-norm 4.5–4.8×" → เป็นการเทียบกับ baseline ที่ทำให้ช้าลง
A คำนวณ norm ใหม่ในลูป แต่ **server โหลด norm ครั้งเดียวตอน index-load** (`lz_entry_parse`)
→ A เป็น *pessimised baseline ที่ไม่มีใน production*
ตัวเลขที่อ้างได้คือ **H→G = 1.62×/1.78×/1.73× routed** (3 runs)

---

## 2. ✅ Kernel fix — merged และ deployed

**อะไร:** dot product จาก 1 accumulator (serial chain) → 4 independent chains
```c
static inline double lz_dot(const float *a, const float *b, int d) {
    double s0=0, s1=0, s2=0, s3=0;  int j = 0;
    for (; j + 4 <= d; j += 4) {
        s0 += (double)a[j]     * (double)b[j];
        s1 += (double)a[j + 1] * (double)b[j + 1];
        s2 += (double)a[j + 2] * (double)b[j + 2];
        s3 += (double)a[j + 3] * (double)b[j + 3];
    }
    double s = (s0 + s1) + (s2 + s3);
    for (; j < d; j++) s += (double)a[j] * (double)b[j];
    return s;
}
```
ยัง `double` → ไม่มี precision loss, ไม่ต้องการ unit-norm · แต่ **ไม่ bit-identical**
(reassociating เปลี่ยนการปัดเศษ, `max_rel_diff = 2.5e-12`)

**Commits (repo `I:\tools`, `main`):**
| commit | อะไร |
|---|---|
| `be52bc7` | ใส่ 4-accumulator kernel |
| `b6b40a9` | ยกออกเป็น helper `lz_dot()` (DRY · comment สั้นลง) |

**ผลวัดสด (2026-10-19):**
| | |
|---|---|
| kernel-level | **1.50× / 1.85× / 2.04×** (3 passes, tight loop, OLD/NEW สลับใน process เดียว) |
| end-to-end | 1.11–1.44× — **noisy** (เครื่อง 2 core + มีงานอื่นวิ่ง) |
| ผลลัพธ์ | top-5 sids + **ลำดับ** เหมือนเดิม · 0 mismatch ใน 50/50 query-reps |
| ระบบ | binary ก่อน/หลัง บน **index เดียวกัน** → **13/13 queries identical** |
| ช่องทางอื่น | `ANN_GRAFT=1` (graft bias path) → ผลเหมือน graft-off |

> **ตัวเลขที่ควรอ้าง = kernel-level** — end-to-end บนเครื่องนี้แกวนงอนมาก
> (วัดสอง server พร้อมกันบน 2 core → ได้ 0.95× ซึ่งเป็นแค่การแข่งกัน cache)

**ทำไมไม่เอา H→C (2.04–2.24× ที่เร็วกว่า):** ตัวนั้นตัด norm ทิ้ง ต้องการ corpus เป็น unit-norm
เป๊ะ — วัด live index ได้ `max |‖x‖−1| = 1.053e-06` ซึ่ง **อยู่นอก 1e-6** ที่ experiment ตั้งไว้
และ norm ไม่ใช่ต้นทุนต่อ query อยู่แล้ว (precompute ตอน load) → **norm ต้องอยู่**

---

## 3. ✅ Verification

**Canonical suite ของ ANN repo:** `python run_tests.py` → **ALL PASS, exit 0**
> แต่ suite นี้ **ไม่ได้ทดสอบ kernel นี้เลย** → ต้องมี ad-hoc เพิ่ม

**Ad-hoc verification** (เก็บไว้ให้รันซ้ำได้):
`%TEMP%\hermes-verify-ann-kernel.py` — มี **GUARD** ที่เทียบ mirror ในสคริปต์กับ source จริง
→ ถ้าแก้ kernel ใน server สคริปต์จะ FAIL แทนที่จะผ่านเงียบ ๆ บน copy เก่า

```
[1] GUARD — mirrors vs source                     PASS
[2] harness built                                 PASS
[3] loaded 3000 real vectors from the live index  PASS
[4] TOPK set_mismatch=0 order_mismatch=0 max_rel_score_diff=1.459e-015
    EDGE dims=24 mismatch_beyond_1e-9=0 worst_rel=1.953e-013 at dim=9
    TIE  near_duplicate_cases=5 top10_order_differences=0
    TIME 1.50x / 1.85x / 2.04x
    ALL AD-HOC CASES BEHAVED (ad-hoc verification, not a project suite)
```

**DWGLS suite:** `make test` ไม่ได้รันใน session นี้ — **ไม่ได้แตะ core header** เลย
(แก้แค่ `docs/HANDOFF-2026-10-14d.md`) → ไม่มีผลกับ build

---

## 4. ⚠️ ปัญหาที่เจอระหว่างทาง (ด่วนมาก)

### 4.1 `ann-sync` เขียน index ทับ server ที่กำลังรัน
ระหว่าง session index เปลี่ยน **254 MB → 231 MB** → server ค้าง (`/health` timeout
ทั้งที่ port ยัง LISTENING) เพราะถือไฟล์เก่า + anchors/order เปลี่ยนไปใต้มือ
**อาการ:** `netstat` เห็น port แต่ `/health` ไม่ตอบ
**แก้:** รอให้ index mtime นิ่ง แล้ว restart
**ควรทำ (ยังไม่ทำ):** atomic swap (เขียน `.tmp` → rename) — `ann_index.jsonl.tmp` มีอยู่แล้ว
น่าจะตั้งใจเป็นแบบนั้นแต่ยังไม่ถูกใช้

### 4.2 server ที่ start โดยไม่ตั้ง env จะโหลด index ว่าง **แต่ตอบว่า ok**
`ANN_INDEX_PATH` default = `data/ann_index.jsonl` แต่ corpus จริงอยู่ `data/memcore/`
→ `entries: 0`, `items_scored: 0`, และ **ตอบเร็วขึ้น** → อ่านเป็น "speedup ดีมาก"
**ต้องเช็ค `entries` + `items_scored` ทุกครั้งก่อนวัดอะไร**
(นิยาม path อยู่ที่ `ann_keepalive.py:_index_paths()`)

### 4.3 เครื่องนี้มี 2 core — ห้ามวัดสอง server พร้อมกัน
ได้ 0.95× (ไม่มี speedup เลย) เทียบกับ 1.44× ของ binary เดิมกับ index เก่า
→ ถ้าตัวเลขขยับเมื่อเปลี่ยนไฟล์ที่ไม่เกี่ยวกัน แปลว่า **การวัดมั่ว** ไม่ใช่แพตช์พัง

---

## 5. บทเรียน 2 ข้อ (ทั้งคู่เกิดวันเดียวกัน)

1. **"baseline ที่เรียกว่า server อาจไม่ใช่ server"** — variant A เป็น baseline ที่ทำให้ช้าลง
   ตัวจริงโหลด norm ครั้งเดียวตอน load
2. **"ผลลัพธ์ต่างจากเดิม" ≠ "แพตช์พัง"** — corpus เปลี่ยน (ann-sync) ทำให้ reference sid
   หายไป → ต้องเช็คก่อนว่า reference **ยังมีอยู่ใน corpus ไหม** แล้ว A/B binary ทั้งสอง
   บน corpus เดียวกัน (ได้ 13/13 identical)

→ ทั้งสองข้อถูกบันทึกเป็น pitfall ใน skill `dwgls-development` (#71, #77)

---

## 6. สถานะ ณ ตอนจบ session

```
DWGLS-native-fs  feat/geo-native-fs   HEAD  f3d5799  (+ handoff fix)  → pushed
I:\tools         main                 HEAD  b6b40a9                   → pushed
```

**งานที่ยังค้าง (จาก session นี้):**
| # | งาน | หมายเหตุ |
|---|---|---|
| 1 | **atomic index swap** | `ann-sync` เขียนทับ server ที่รันอยู่ — อันตรายกว่าที่คิด |
| 2 | **kernel fix ใน `I:\tools\ann` ยังไม่ถูก push ไปที่อื่น** | ตอนนี้ push แล้ว |
| 3 | **f16/f32 index format** | ยังรอ user เลือก (ผมแนะนำ f16) |
| 4 | **DB cleanup** (v1-migrate 38 MB, exchange) | ยังรอ user ตอบ 2 ข้อ |
| 5 | **H1 cold-path watcher** | งานเดียวที่แตะ live แล้วยังไม่เริ่ม |
| 6 | **CORE-CONSTANT manifest** (DWGLS) | ยังไม่ทำ — ป้องกันการแก้เลขแล้วสาขาหนึ่งพังเงียบ |
| 7 | **orphan 37 core headers** → `deprecated/` | ยังไม่ทำ |

**กันไว้ (ไม่ลบ):** `I:\tools\ann\build\ann_search.exe.pre-kernelG` = binary ก่อนแก้

---

*Session 2026-10-19 late · DWGLS-native-fs `feat/geo-native-fs` · I:\tools `main`*
