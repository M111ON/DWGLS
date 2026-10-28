# Design DNA — 3 Inspirations (owner)

**Date:** 2026-10-22 · **Status:** design-intent record (founder-level inspiration; not a spec, not a measurement)
**Provenance tags** (#7129): `[owner]` verbatim owner words · `[outside]` from an outside-model conversation, spot-checked · `[verified]` confirmed against a repo artifact this session · `[claim]` not re-checked — do not cite as proven
**Sources:** owner 2026-10-10 (project-memory #7540, caption #7535) + owner 2026-10-22 (this session) + section-1 mapping from the outside conversation (spot-checked below)

These three are the design spine the owner has held from the first versions to now. Every new design should be checkable against their flavor (§4).

---

## 1. The three (owner's words)

### (1) Shadow Clone — Naruto
- `[owner 2026-10-22]` "ทุกตัวเป็น clone ที่ unique, transfer experience back to user after dissolve."
- `[owner 2026-10-22]` "ทุกตัวแม้จะ unique แต่ก็รู้เหมือนแชร์ same db ตลอดเวลา เพียงแค่ exp จะเติมเข้าไปหลัง flushed สลายร่างแล้วเท่านั้น."
- `[owner 2026-10-22]` "นารูโตะไม่ต้องบอกร่างแยกของมันให้มาช่วยปั้นกระสุนวงจักร" — clones coordinate-free; but "ร่างแยกบางตัวก็มีกล้าๆกลัวๆ ตายโง่ๆเหมือนกัน เจ็บได้เหมือนกัน และสิ่งนี้มัน exponential มากเวลาเรียนรู้สกิลอะไร."
- `[owner 2026-10-22]` The honest limit, stated by owner: "ความจริงเกิดขึ้นไม่ได้ เพราะอะไร? เราไม่มี 9tails kurama chakra ที่ infinity… แต่เราเอาแนวคิดนี้มาใช้ได้ ถ้าใช้แค่ 2-3 instant."
- `[owner 2026-10-10]` (#7540) unique isolated massive duplicates, each perceiving separately, syncing at a high level, returning experience to the main body on dissolve (clones deliberately take hits / dissolve to send data back).

### (2) Venom — symbiote
- `[owner 2026-10-22]` "ส่งต่อประสบการณ์ให้กับ host ใหม่ skill memory transfer ไปด้วย เช่น spiderman venom ทำให้พ่นใยได้" — the skill travels with the host change.
- `[owner 2026-10-22]` "อีกอย่างคือมันสามารถสื่อสารกันข้ามจักรวาลแบบโทรจิตได้" — cross-universe telepathy.
- `[owner 2026-10-10]` (#7540) inheriting skill/talent/memory from a previous host into a new host; **"การวาง tombstone ก็คือ venom ทำงานแล้ว"** — laying a tombstone IS venom working: a tombstone is not merely a footprint, it is inheritance.

### (3) Infinity Castle — Demon Slayer (Nakime)
- `[owner 2026-10-22]` (quoting the article) "…ability to create a huge range of space that can be completely controlled by its own will. Forcibly push your opponent into a disadvantageous place without restrictions… When you are in infinity, you can say that Nakime is almost invincible."
- `[owner 2026-10-10]` (#7540) rooms without limit, overlapping — the infinity-castle shape: anchor → 6 direction → frustum quadtree ×4 per layer (1 < 4 < 16 < 64), overlapping and overflowing but always in different dimensions (memory #7535).

### Combined (the synthesis)
- `[owner 2026-10-22]` "ลองคิดดูว่าเอา 3 อย่างนี้รวมกัน — นี่ล่ะคือสิ่งที่ผมกำลังพยายามทำ: **container ที่ shuffles infinity ผ่าน tesseract dimension แล้วส่งต่อความรู้เรียนรู้ได้เรื่อยๆ**."
- `[owner 2026-10-10]` (#7540) the combined picture: a naruto in every room of the infinity castle, every room inheriting knowledge by devouring the clones. "จริงๆเราหยิบมาแค่ส่วนหนึ่งของทั้งหมดมาใช้เอง ถ้ามีครบหมดนี่จะขนาดไหน."

---

## 2. What each becomes in the system (mapping, spot-checked 2026-10-22)

| Inspiration | Rule it contributes | Repo anchor | Status |
|---|---|---|---|
| Clone unique + shared home | each instance has its own bond, home points to one place (tied_dedup + ghost) | `core/tied_dedup.h` (includes `geo_ghost_lift.h`) | `[verified]` exists; doctrine #7582/#7576 |
| Experience after dissolve, Δ ∝ events | route log is 5 B/route, **price independent of data size**: 66 routes = 330 B, overhead +1.05% @4KB | `docs/HANDOFF-2026-08-17-entropy-bound-fibo-checkpoint.md:28,41` | `[verified]` |
| Clone needs no coordination | address resolves from seed+method — walk clock `state = (seed, round, tick)`, field re-derivable from seed without index | `tests/test_fibo_checkpoint.c:1`, `core/fibo_walk.h` (WALKTHROUGH-BACKLOG:245) | `[verified]` |
| Clone can die stupidly → still learns | tombstone persists as audit trail; expire path exists | `core/geo_ghost_lift.h:655` `ghost_expire` | `[verified]` |
| Experience merge by replay, not copy | replay of the append-only log | `core/geo_wang_latch.h:142` `wl_replay`, `wl_replay_log_file` | `[verified]` |
| **Kurama = entropy wall** (the honest limit) | value axis ceiling: whitened Q8 all-coin-flip → lossless squeeze floor **1.077×** (~1.08×) | `docs/TIMELINE_WORKING_MODEL.md:262,268` (`probe_entropy`, H=7.686, planes 1.0000) | `[verified]` |
| Venom: skill = function, not payload | skill travels as seed+method that regenerates state; checkpoint-replay round-trips it | `test_fibo_checkpoint` (23/23 now; the outside session quoted 22/22 = pre-upgrade) | `[verified]` — number corrected |
| Venom telepathy = coprime sync | KIS wheel 8 teeth (`s mod 8`) × hyperbolic wheel 3 teeth (`s mod 3`), gcd(8,3)=1 → CRT bijection, lossless, no shared clock | `core/fan24_gear.h:10-12`, probe `tools/fan24_gear_sync_probe.c` (M1–M7, 10/10) | `[verified]` |
| Host compatibility = method agreement | CRT bijection requires coprime moduli; non-coprime pairs are structurally lossy | math (CRT); the specific (6,4)/(12,2) probes | `[claim]` — condition is math-certain, probes not re-checked |
| Infinity: space under own will | fixed 20736 field + capacity accounting + gate (admit/deflect/reject) | capacity accounting #7015; `core/ckpt_wang.h` | `[verified]` (existence) |
| Push opponent out | lift ill-fitting data into residual space | `core/geo_ghost_lift.h:2` "wired into residual_space", `ghost_lift:398` | `[verified]` |
| Invincible inside | wang gate + CRC: the system knows when it is corrupt and refuses — never silent corruption | `tests/test_ckpt_wang.c` (22/22, corrupt-1-byte detection) | `[verified]` |
| Shuffles infinity | unbounded timeline over bounded unit: `t × 20736 + local`; window (0, 20736) with scale log | project.md Rescope (2026-08-14); KIS timeline | `[verified]` |
| Tombstone = venom working | owner's own reading (2026-10-10), precedes this session | memory #7540 | `[owner]` |

**Synthesis restated over verified machinery:**
container = unit(20736) + timeline(t×20736) + registry + path + gate · shuffles = walk clock (seed+method) on the timeline · infinity = t unbounded while each unit stays bounded · through tesseract = 12⁴ = 20736 · transfers = route log (5 B/event, ∝events) + replay + seed+method.

---

## 3. Honest limits (the "Kurama problem" and friends)

1. **Kurama = entropy.** Infinity is not free: on whitened data (Q8) the lossless value-axis ceiling is ~1.08× (measured floor 1.077×). Without infinite chakra, clones run 2–3 instances, not ∞ — and owner already chose 2–3. `[verified]` number, `[owner]` choice: bounded instances + cheap transfer > infinite instances + expensive transfer.
2. **Telepathy requires coprime.** The CRT gear sync works when the moduli are coprime (8,3). A pair like (6,4) or (12,2) is lossy by construction — "Venom is not compatible with every host."
3. **Transfer-back needs a real append-only log.** If the log grows without bound it needs compaction (the route-log/handoff compaction is already open work).
4. **Same DB is not enough — same method too.** Clone A's experience replays at B only if both use the same seed+method; otherwise experience is not composable. Compatibility = method agreement.

---

## 4. Flavor checklist (use when designing something new)

- **Clone flavor:** prefer many small unique-but-shared-home instances over one monolith; merge state as **Δ at dissolve** (∝ events, never ∝ data); coordinate by **address (seed+method)**, not by message-passing; keep the count honest (bounded 2–3 > claimed infinity).
- **Venom flavor:** transfer by **inheriting a function/rule** (seed+method, tombstone = inheritance), not by copying payloads; every receiver declares a compatibility precondition (coprime/method agreement) and a gate that rejects cleanly when it fails.
- **Infinity-castle flavor:** each unit is bounded (20736) while the timeline is not (t×20736); the system **controls entry** (admit/deflect/reject); integrity is self-known (wang + CRC) — corruption is loud, never silent; displacement is a lift (residual_space), not a delete.

If a design cannot answer one line per flavor, it is not yet in the DNA.

## 5. Where this is perceived

1. **Every session** — project-memory: #7540 (the three, enriched) + the Design-DNA rule in PROJECT_RULES (both updated 2026-10-22).
2. **At design time (L2)** — `.agents/project.md` → "Design DNA — 3 inspirations" section, read when architecture/state is loaded.
3. **Full record** — this file. Companion caption: memory #7535 ("infinity castle ที่การดีดเครื่องดนตรีแล้ว climate change").
Deliberately **not** in `.agents/rules.md` — that file is prohibitions-only (L1); flavor belongs in the architecture layer, not in the override contract.
