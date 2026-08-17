/* hyp_fusion.h — Hyperbolic Section Fusion
 * ═══════════════════════════════════════════════════════════════════════════
 * user: "merge / fusion เป็น section ได้ไหม — ถ้าแยกกันแบบนี้ทุกอย่างผมสร้าง
 *        มาก็ดีหมดล่ะ แก้ปัญหาทีละจุด แต่ก็ต้องเลือกที่ดีแล้วไม่ฉุดกำลังด้วย"
 *
 * หลักการ fusion:
 *   - แต่ละ SECTION = หนึ่งหน้าที่ ใช้ candidate ที่เร็วสุดตัวเดียว
 *   - section เรียกของเดิมที่พิสูจน์แล้ว (include) — ไม่ duplicate logic
 *   - static inline ทั้งหมด → compiler inline → ไม่มี runtime overhead
 *   - verify/decision เดียวต่อ section แทน N module แยก
 *
 * ── S1 ADDRESS — bond = RDH + face_id (GeoSeed) + cylinder route + mirror ──
 *     hyp_bond(block, topo, from)  → 64-bit (rdh_bond_key + face tag 48..51)
 *     hyp_bond_face / hyp_bond_core → reversible (address IS data)
 *     hyp_route(slot)               → (axis, half, spoke, slot_in) cylinder
 *     hyp_mirror_slot(slot)         → อีกครึ่งของ axis (KIS ↔ hyperbolic)
 *
 * ── S2 GATE — wang integrity + tantrix route = หนึ่ง decision ─────────────
 *     hyp_gate(wl, enc, incoming)   → OPEN / SKIP / CLOSED / TAMPER
 *     (เดิมต้องเรียก fwang_seek_gate แล้ว tantrix_route แยก 2 ชั้น state)
 *     hyp_log_validate(wl)          → 1 loop ตรวจครบ (แทน 6 loop ของ verify)
 *
 * ── S3 WEIGHT — Hosoya F ladder (scale weight ต่อตำแหน่ง) ─────────────────
 *     hyp_fibo(n) / hyp_weight(n)   → F(n) ≈ φⁿ — scale ladder ของระบบ
 */

#ifndef HYP_FUSION_H
#define HYP_FUSION_H

#include <stdint.h>
#include "geo_rdh_addr.h"        /* RDH bond (พิสูจน์แล้ว bijection 2^24)   */
#include "geo_frame_seek_wang.h" /* wang edge + tamper (FIX แล้ว §15.50)     */
#include "lc_tantrix.h"          /* tantrix route (fabric switch)            */

/* ═══════════════════════════════════════════════════════════════════════════
   S1 — ADDRESS
   ═══════════════════════════════════════════════════════════════════════════ */

/* face_id จาก GeoSeed topology — shift+mask (~2 cycles): 12 หน้า dodeca
 * (ต้นทาง geo_thirdeye.h: (topo >> 11) & 0xF) */
static inline uint8_t hyp_face(uint64_t topo) {
    return (uint8_t)((topo >> 11) & 0xFu);
}

/* bond = RDH(block, from) + face tag ที่ bits 48..51 — identity ของ lifted
 * block (block, face, from_scale) อยู่ใน address เอง — reversible */
static inline uint64_t hyp_bond(uint32_t block, uint64_t topo, uint8_t from) {
    uint64_t core = rdh_bond_key(block, from);
    return core | ((uint64_t)(hyp_face(topo) & 0xFu) << 48);
}

/* กู้ face กลับจาก bond */
static inline uint8_t hyp_bond_face(uint64_t bond) {
    return (uint8_t)((bond >> 48) & 0xFu);
}

/* กู้ (block, from) กลับจาก bond — ครึ่งล่าง 48 bits = rdh_bond_key
 * (low 24 bits = addr เพราะ interleave a ^ a<<24 → bit 0..23 = a) */
static inline void hyp_bond_core(uint64_t bond, uint32_t *block, uint8_t *from) {
    uint32_t addr = (uint32_t)(bond & 0xFFFFFFu);
    *from  = (uint8_t)(addr % RDH_N_WEDGES);
    *block = addr / RDH_N_WEDGES;
}

/* cylinder route — slot ∈ [0,20736) → (axis 0..2, half 0..1, spoke 0..5,
 * slot_in_spoke 0..575) — bijection พิสูจน์แล้ว (hyp_candidate_map A)
 * half 0 = KIS cylinder · half 1 = hyperbolic cylinder (mirror) */
typedef struct {
    uint8_t  axis;
    uint8_t  half;
    uint8_t  spoke;
    uint16_t slot_in_spoke;
} HypRoute;

static inline HypRoute hyp_route(uint32_t slot) {
    HypRoute r;
    uint32_t rem = slot % 6912u;      /* HYP_AXIS_SLOTS */
    uint32_t rem2 = rem % 3456u;      /* 1 cylinder     */
    r.axis  = (uint8_t)(slot / 6912u);
    r.half  = (uint8_t)(rem / 3456u);
    r.spoke = (uint8_t)(rem2 % 6u);
    r.slot_in_spoke = (uint16_t)(rem2 / 6u);
    return r;
}

/* กระจก: อีกครึ่งของ axis เดียวกัน (KIS ↔ hyperbolic) — 3 ops int */
static inline uint32_t hyp_mirror_slot(uint32_t slot) {
    uint32_t axis = slot / 6912u;
    uint32_t rem  = slot % 6912u;
    uint32_t half = rem / 3456u;
    return axis * 6912u + ((half == 0u) ? rem + 3456u : rem - 3456u);
}

/* ═══════════════════════════════════════════════════════════════════════════
   S2 — GATE (wang integrity + tantrix route = หนึ่ง decision)
   ═══════════════════════════════════════════════════════════════════════════ */

typedef enum {
    HYP_SEEK_OPEN,    /* wang OK + tantrix FORWARD — อ่านตรงได้           */
    HYP_SEEK_SKIP,    /* 369 Tesla loop — ขอบเขตที่ตั้งใจ (cpair candidate) */
    HYP_SEEK_CLOSED,  /* tantrix DROP หรือ wang MISMATCH — ปิด → replay   */
    HYP_SEEK_TAMPER,  /* chord ของชั้นเก็บพัง — log เสีย → หยุด            */
} HypSeek;

/* หนึ่ง decision แทน (fwang_seek_gate + tantrix_route) — ประมาณ 10 ops:
 * tamper(ชั้นเก็บ) → 369 → edge continuity → entry match (tantrix) */
static inline HypSeek hyp_gate(const FrameWangLayer *wl, uint16_t enc,
                               uint8_t incoming_gate) {
    uint16_t win = (enc / WANG_WIN_SIZE) % WANG_WIN_COUNT;
    if (!fwang_tamper_check(&wl->wins[win])) return HYP_SEEK_TAMPER;
    if (_fwang_is_369(enc))                  return HYP_SEEK_SKIP;
    if (!fwang_edge_valid(wl, win))          return HYP_SEEK_CLOSED;
    /* tantrix: entry = chord_a(enc) บน 4-bit gate; ไม่ตรง = DROP (ปิด) */
    uint8_t entry = (uint8_t)(_fwang_chord_a(enc) & 3u);
    if (entry != (incoming_gate & 3u))       return HYP_SEEK_CLOSED;
    return HYP_SEEK_OPEN;
}

/* validate ทั้ง log — 1 loop (แทน 6 loop ของ fwang_verify):
 * ทุก window: valid + continuity + wrap + tamper + skip==3 + tile_id<12 */
static inline int hyp_log_validate(const FrameWangLayer *wl) {
    for (uint16_t w = 0; w < WANG_WIN_COUNT; w++) {
        const FrameWangWindow *win = &wl->wins[w];
        if (!win->valid) return -1;
        if (__builtin_popcount(win->skip_mask) != 3u) return -2;
        if (win->tile_id > 11u) return -3;
        if (!fwang_tamper_check(win)) return -4;
        if (w > 0u && !fwang_edge_valid(wl, w)) return -5;
    }
    if (!fwang_edge_valid_wrap(wl, 0u)) return -6;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
   S3 — WEIGHT (Hosoya F ladder — scale weight ต่อตำแหน่ง)
   ═══════════════════════════════════════════════════════════════════════════ */

/* F(n): 0,1,1,2,3,5,... — F(12) = 144 = "100" ของโลกฐาน 12 (§15.46) */
static inline uint32_t hyp_fibo(uint32_t n) {
    uint32_t a = 0, b = 1;
    for (uint32_t i = 0; i < n; i++) { uint32_t t = a + b; a = b; b = t; }
    return a;
}

/* Hosoya cell: H(n,k) = F(k+1) × F(n-k+1) — ตารางคูณของบันได scale */
static inline uint32_t hyp_hosoya(uint32_t n, uint32_t k) {
    return hyp_fibo(k + 1u) * hyp_fibo(n - k + 1u);
}

/* weight ladder สำหรับ w (scale position 0..143): น้ำหนัก deterministic
 * ต่อตำแหน่ง — ใช้ตั้งราคา predict/residual (hex_tile) */
static inline uint32_t hyp_weight(uint32_t w) {
    return hyp_fibo((w % 12u) + 1u) * (1u + (w / 12u));
}

#endif /* HYP_FUSION_H */
