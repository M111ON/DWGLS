/*
 * geo_ghost_lift.h — Ghost Lift: hyperbolic track wired into residual_space
 * ═══════════════════════════════════════════════════════════════════════════
 * Rescope working model (§5.2 passive log + §15.2 ghost):
 *
 *   Block requests scale beyond its envelope (ROI cliff at k ≥ 4–5) →
 *   instead of expanding the field (ทุกอย่างถูกลาก — global scale เดียว),
 *   FREEZE the block's data into residual_space and let the hyperbolic
 *   log TRACK the ghost.  Time/scale is cut from the data (freeze) —
 *   access goes through the ghost via delta.
 *
 *   ┌────────────────────────────────────────────────────────────┐
 *   │  bond = BIRTH IDENTITY   (block_id, from_scale)            │
 *   │  log  = ROUTE            {block_id, from→to} = 5 B/event   │
 *   └────────────────────────────────────────────────────────────┘
 *
 * Mapping rule (the "address") — RDH mixed-radix (§15.38, replaces FNV):
 *   origin_seed = rdh_addr(block_id, from_scale)   = block×256 + from
 *   piece       = RDH piece: geo_key = rdh_addr, bond_L = rdh_addr (row),
 *                 bond_R = rdh_addr_twin (column) — no hash, reversible
 *   bond_key    = pogls_bond_key(piece) = bond_L XOR bond_R
 *
 *   - from_scale is PART of the address → เสาเข็มห้ามขยับ: read with a
 *     different from_scale → bond_key changes → thaw/verify fail
 *     automatically.  No lookup table, coordinate-bound by construction.
 *   - to_scale (the route) is NOT part of the bond → same frozen data can
 *     be reached by several routes (วางครั้งเดียว ไม่เคยเขียนซ้ำ — T8b).
 *   - axis(to_scale) = route flavor (I/O/T/S/Z/L/J); shape does NOT affect
 *     bond_key — bond stays pure birth identity.
 *
 * Re-attach (เสาเข็มใหม่ + reroute link, §15.2):
 *   block re-homes at the target scale → OLD pile's ghosts die via
 *   rs_expire_by_origin (data freed, log entries kept as audit trail,
 *   flagged GHOST_FLAG_EXPIRED — same idea as residual tombstones).
 *
 * Delta ∝ events, not data: one 5 B entry per lift; telescope — one entry
 * covers ANY distance on the scale axis (from 3 → to 140 is 1 entry,
 * not 137 steps).
 *
 * All header-only, static inline, int + fibo mixing only (no float,
 * no trig, no table — MAP not COMPRESS).
 */

#ifndef GEO_GHOST_LIFT_H
#define GEO_GHOST_LIFT_H

#include <stdint.h>
#include <string.h>
#include "residual_space.h"
#include "pogls_bond.h"
#include "geo_ghost_envelope.h"
#include "geo_rdh_addr.h"
#include "hyp_fusion.h"      /* S2 GATE: hyp_gate — wang integrity + tantrix */

/* ── Constants ─────────────────────────────────────────────── */
#define GHOST_LOG_MAX      4096u    /* max routes in one log — สมมาตร
                                     * กับ RS_DEFAULT_CAPACITY 4096    */
#define GHOST_SEED_MAGIC   UINT64_C(0x0D0C0A1100000000) /* "DWGLS"  */
#define GHOST_AXIS_SHAPES  7u       /* I O T S Z L J (fold 1..7)    */

/* ── Entry flags ───────────────────────────────────────────── */
#define GHOST_FLAG_LIFT    0x01u    /* live lift (frozen + tracked) */
#define GHOST_FLAG_EXPIRED 0x02u    /* re-attached — audit trail    */

/* ════════════════════════════════════════════════════════════
   GHOST LOG ENTRY — the passive scale-change record (5 B)
   ════════════════════════════════════════════════════════════
   entries ถูกเก็บ SORTED โดย (block_id, from_scale) — b-bond principle
   (§15.53/15.54): (block, from) เป็นคู่เดียวในโลก → ค้นด้วย binary search
   บนคู่ (deterministic, ไม่มี hash, ไม่มี linear scan) แล้ว scan เฉพาะ
   routes ของ pile นั้น (เล็ก) — "chunk ไม่ต้องรันเลข"
   ════════════════════════════════════════════════════════════ */
typedef struct __attribute__((packed)) {
    uint16_t block_id;    /* which pile this ghost belongs to      */
    uint8_t  from_scale;  /* birth scale w0 (envelope floor)       */
    uint8_t  to_scale;    /* requested scale — why it was lifted   */
    uint8_t  flags;       /* GHOST_FLAG_*                          */
} GhostLogEntry;          /* 5 B total — delta ∝ events, not data  */

typedef struct GhostPairTable GhostPairTable;   /* forward — defined below */

typedef struct {
    GhostLogEntry entries[GHOST_LOG_MAX];
    uint32_t      count;
    FrameWangLayer wang;    /* integrity gate (fusion S2) — ตรวจ timeline */
    GhostPairTable *pair;   /* optional O(1) accelerator — attach แล้ว
                               lift/expire ทำ dirty, read refresh เอง (§15.57) */
} GhostLog;

/* ════════════════════════════════════════════════════════════
   BOND_KEY MAPPING — ghost log entry → piece → bond_key
   ════════════════════════════════════════════════════════════ */

/* origin seed: birth pile identity — RDH mixed-radix address (no hash)
   ring=block_id, wedge=from_scale → collision-free + reversible */
static inline uint64_t ghost_origin_seed(uint16_t block_id, uint8_t from_scale) {
    return rdh_addr(block_id, from_scale);
}

/* fold axis = route flavor of the destination (1..7 → I..J);
   does NOT affect bond_key — bond is birth identity only */
static inline uint8_t ghost_fold_axis(uint8_t to_scale) {
    return (uint8_t)((to_scale % GHOST_AXIS_SHAPES) + 1u);
}

/* piece of the ghost — RDH addressing, same (block_id, from_scale) → same piece
   bond_L = addr (ครึ่งล่าง), bond_R = addr<<24 (ครึ่งบน) → L^R = interleave bijection */
static inline PoglsPiece ghost_piece(uint16_t block_id, uint8_t from_scale,
                                     uint8_t to_scale) {
    PoglsPiece p;
    uint64_t a = rdh_addr(block_id, from_scale);
    p.geo_key = a;                          /* coordinate = address (reversible) */
    p.shape   = POGLS_AXIS_SHAPE[ghost_fold_axis(to_scale)];
    /* offset +1: rdh_addr(0,0)=0 → bond 0 collides with RS_BOND_KEY_RESERVED
       (reserved = 0 = "no entry" sentinel). +1 keeps bijection (1..2^24)
       และ bond 0 กลายเป็นค่าว่าง — (block,from) → bond เดียว ไม่ชน */
    p.bond_L  = a + 1u;
    p.bond_R  = (a + 1u) << 24;
    return p;
}

/* bond_key = deterministic address of the ghost's birth pile */
static inline uint64_t ghost_bond_key(uint16_t block_id, uint8_t from_scale,
                                      uint8_t to_scale) {
    PoglsPiece p = ghost_piece(block_id, from_scale, to_scale);
    return pogls_bond_key(&p);
}

/* ════════════════════════════════════════════════════════════
   LOG ACCESS
   ════════════════════════════════════════════════════════════ */

static inline void ghost_log_init(GhostLog *log) {
    if (!log) return;
    memset(log, 0, sizeof(GhostLog));
    fwang_init(&log->wang);   /* integrity layer — deterministic (S2) */
}

/* binary search lower bound ของ pile (block_id, from_scale) — O(log n)
   (entries sorted โดย (block, from) — b-bond: คู่เดียวในโลก → ไม่ต้อง scan) */
static inline int _ghost_pile_lo(const GhostLog *log, uint16_t block_id,
                                 uint8_t from_scale) {
    uint32_t lo = 0, hi = log->count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) >> 1;
        const GhostLogEntry *e = &log->entries[mid];
        if (e->block_id < block_id ||
            (e->block_id == block_id && e->from_scale < from_scale))
            lo = mid + 1;
        else
            hi = mid;
    }
    return (int)lo;
}

/* find a LIVE route (block_id, from→to). Returns index or -1.
   binary search ถึง pile (block, from) แล้ว scan เฉพาะ routes ของ pile
   (เล็ก — 1..ไม่กี่) — Expired ไม่ match */
static inline int ghost_log_find(const GhostLog *log, uint16_t block_id,
                                 uint8_t from_scale, uint8_t to_scale) {
    if (!log) return -1;
    int i = _ghost_pile_lo(log, block_id, from_scale);
    for (; i < (int)log->count; i++) {
        const GhostLogEntry *e = &log->entries[i];
        if (e->block_id != block_id || e->from_scale != from_scale) break;
        if (e->flags & GHOST_FLAG_EXPIRED) continue;
        if (e->to_scale == to_scale) return i;
    }
    return -1;
}

/* number of routes recorded for a block (live + expired = audit trail)
   = ช่วง [lower(block,0), lower(block+1,0)) — O(log n) */
static inline uint32_t ghost_route_count(const GhostLog *log, uint16_t block_id) {
    if (!log) return 0;
    int lo = _ghost_pile_lo(log, block_id, 0);
    int hi = _ghost_pile_lo(log, (uint16_t)(block_id + 1u), 0);
    return (uint32_t)(hi - lo);
}

/* ════════════════════════════════════════════════════════════
   PAIR TABLE — dense (block, from) → pile slot, O(1) route check
   ════════════════════════════════════════════════════════════
   user: "ต่อยอด route check เป็น O(1): dense pair table (block,from)
          → pile slot แบบ direct (memory trade)"

   entries ถูก sorted โดย (block_id, from_scale) → ทุก pile ของ block
   เดียวกันอยู่ติดกัน → ตาราง dense 2 ตัวให้ O(1):
     pile[(b<<8)|f]     = index ของ entry แรกของ pile (b,f) หรือ 0xFFFF
     block_lo[b]        = index ของ entry แรกที่มี block_id >= b
                          (block_lo[maxb] = count — ขอบปิด)
   route_count(b)       = block_lo[b+1] - block_lo[b]   — O(1)
   find(b,f,t)          = pile lookup → scan เฉพาะ pile (เล็ก) — O(1)

   Memory trade: pile = max_block×256×2B, block_lo = (max_block+1)×2B
   — dense ตาม block ที่เห็นจริง (max_block = block สูงสุด+1)
   วัด footprint จริงบน 4 GGUF ใน test_pair_table (เล็ก: ~N×514B)

   AUTO-REFRESH (§15.57): attach กับ log (`ghost_pair_attach`) →
   lift/expire ตั้ง dirty flag (O(1)) → read (ghost_read / pair_*)
   refresh เองเมื่อ dirty — caller ไม่ต้องเรียก build เอง ไม่มี stale read

   SIGNAL BEFORE COMPUTE (§15.59 — user: "geometry ให้สัญญาณมาก่อน compute"):
   ราคา rebuild ประมาณได้ O(1) ก่อนจ่ายจริง — hint_max_block ถูกอัปเดตตอน
   lift (O(1), ไม่ต้อง scan) → pred_bytes = hint_max_block×514 → ถ้าตาราง
   จะใหญ่เกินกว่าคุ้มเทียบกับ binary search บน log เล็ก → refresh คืน 1
   (= skip rebuild) → find/route_count fallback ไป ghost_log_find ทันที
   จ่ายราคาถูกกว่า ไม่ใช่ rebuild 32MB แล้วค่อยรู้ว่าไม่คุ้ม (§15.58 วัดจริง)
   ════════════════════════════════════════════════════════════ */
typedef struct GhostPairTable {
    uint16_t *pile;        /* [(max_block)<<8] — 0xFFFF = ไม่มี pile     */
    uint16_t *block_lo;    /* [max_block+1] — block_lo[max_block]=count  */
    uint32_t  max_block;   /* block สูงสุดที่เห็น + 1 (0 = ว่าง)          */
    uint32_t  hint_max_block; /* O(1) estimate — อัปเดตตอน lift  ไม่ต้อง scan */
    uint32_t  count_built; /* log->count ตอน build                      */
    uint32_t  reads_served;/* read ที่ตารางล่าสุดตอบก่อน dirty ครั้งถัดไป   */
    uint8_t   dirty;       /* lift/expire ตั้ง — read refresh เอง        */
} GhostPairTable;

/* signal knob (§15.59): rebuild ต่อเมื่อ history บอกว่าคุ้ม — ตารางล่าสุด
   ต้องตอบ read มากพอ (reads_served) ว่า amortize ค่า build (memset) ได้
   rule: rebuild_cost ≈ pred_bytes/8 cyc ≤ reads_served × (binary−pair) ≈
   reads_served × 300 cyc → pred_bytes ≤ reads_served × 2400 */
#define GHOST_PAIR_AMORT_CYC (2400u)

/* build — O(count) หนึ่งรอบ: เก็บ pile start + block start */
static inline int ghost_pair_build(GhostLog *log, GhostPairTable *t) {
    if (!log || !t) return -1;
    if (t->pile) { free(t->pile); t->pile = NULL; }
    if (t->block_lo) { free(t->block_lo); t->block_lo = NULL; }
    t->max_block = 0; t->reads_served = 0;
    t->count_built = log->count; t->dirty = 0;
    if (log->count == 0) return 0;

    uint32_t maxb = 0;
    for (uint32_t i = 0; i < log->count; i++)
        if ((uint32_t)log->entries[i].block_id + 1u > maxb)
            maxb = (uint32_t)log->entries[i].block_id + 1u;
    t->max_block = maxb;
    t->hint_max_block = maxb;   /* sync — hint = ค่าจริงหลัง build */
    t->pile = (uint16_t *)malloc((size_t)maxb * 256u * sizeof(uint16_t));
    t->block_lo = (uint16_t *)malloc(((size_t)maxb + 1u) * sizeof(uint16_t));
    if (!t->pile || !t->block_lo) {
        free(t->pile); free(t->block_lo);
        t->pile = NULL; t->block_lo = NULL;
        return -1;
    }
    memset(t->pile, 0xFF, (size_t)maxb * 256u * sizeof(uint16_t));
    for (uint32_t b = 0; b <= maxb; b++) t->block_lo[b] = 0xFFFFu;

    /* one pass — entries sorted: entry แรกของ (b,f) คือ index ปัจจุบัน */
    for (uint32_t i = 0; i < log->count; i++) {
        uint16_t b = log->entries[i].block_id;
        uint32_t key = ((uint32_t)b << 8) | log->entries[i].from_scale;
        if (t->pile[key] == 0xFFFFu) t->pile[key] = (uint16_t)i;
        if (t->block_lo[b] == 0xFFFFu) t->block_lo[b] = (uint16_t)i;
    }
    t->block_lo[maxb] = (uint16_t)log->count;   /* ขอบปิด */
    /* backward fill: block ที่ไม่มี entry → ชี้ไป block ถัดไปที่มี */
    for (uint32_t b = maxb; b-- > 0; ) {
        if (t->block_lo[b] == 0xFFFFu) t->block_lo[b] = t->block_lo[b + 1u];
    }
    return 0;
}

static inline void ghost_pair_free(GhostPairTable *t) {
    if (!t) return;
    free(t->pile); free(t->block_lo);
    t->pile = NULL; t->block_lo = NULL;
    t->max_block = 0; t->hint_max_block = 0;
    t->count_built = 0; t->reads_served = 0; t->dirty = 0;
}

/* attach — log จะดูแลความ fresh ให้ (lift/expire ตั้ง dirty, read refresh)
   hint เริ่มจาก log ปัจจุบัน (O(count) ครั้งเดียวตอน attach — ก่อน rebuild ใดๆ) */
static inline void ghost_pair_attach(GhostLog *log, GhostPairTable *t) {
    if (!log) return;
    log->pair = t;
    if (!t) return;
    t->dirty = 1;                          /* ต้อง build ก่อนใช้ครั้งแรก */
    t->reads_served = 0;                   /* history เริ่มว่าง */
    t->hint_max_block = 0;
    for (uint32_t i = 0; i < log->count; i++)
        if ((uint32_t)log->entries[i].block_id + 1u > t->hint_max_block)
            t->hint_max_block = (uint32_t)log->entries[i].block_id + 1u;
}

static inline void ghost_pair_detach(GhostLog *log) {
    if (log) log->pair = NULL;
}

/* fresh = attached + สร้างแล้ว + log ยังไม่เปลี่ยนไปหลัง build */
static inline int ghost_pair_fresh(const GhostLog *log) {
    const GhostPairTable *t = log ? log->pair : NULL;
    return (t && t->pile && !t->dirty && log->count == t->count_built);
}

/* refresh — สัญญาณมาก่อน compute (§15.59): ประเมินราคา rebuild จาก
   hint_max_block (O(1)) ก่อนจ่ายจริง — ถ้าตารางจะใหญ่เกินคุ้มเทียบกับ
   binary search บน log เล็ก → คืน 1 (skip, ใช้ binary) ไม่ rebuild
   คืน 0 = ใช้ตารางได้ (fresh หรือ rebuild แล้ว) · -1 = ไม่มีตาราง */
static inline int ghost_pair_refresh(GhostLog *log) {
    if (!log || !log->pair) return -1;
    GhostPairTable *t = log->pair;
    if (t->pile && !t->dirty && log->count == t->count_built)
        return 0;                       /* fresh — ไม่ต้องทำอะไร */

    /* ── SIGNAL: ราคา rebuild ≈ pred_bytes (memset) รู้ก่อน build ──
       ใช้ HISTORY: ตารางล่าสุดตอบ reads_served reads ก่อน dirty ครั้งถัดไป
       ถ้า amortize ไม่ได้ (build แพงกว่า reads × saving) → skip → binary
       (เราวัด §15.58: 32MB table แพ้ binary 276 cyc — อย่าจ่าย rebuild
       ที่ไม่คุ้ม; reads_served ต่ำ = workload เขียนถี่ = binary ถูกกว่า) */
    uint64_t pred_bytes = (uint64_t)t->hint_max_block * 514u + 8u;
    uint64_t served = t->reads_served ? t->reads_served : 4u;  /* ครั้งแรกสมมติ 4 */
    if (pred_bytes > served * GHOST_PAIR_AMORT_CYC)
        return 1;                       /* skip — caller ใช้ ghost_log_find */
    t->reads_served = 0;                /* rebuild — นับรอบใหม่ */
    return ghost_pair_build(log, t);    /* rebuild — ล้าง dirty ด้วย */
}

/* O(1) find — เทียบเท่า ghost_log_find แต่ผ่านตาราง (auto refresh)
   ถ้า refresh คืน 1 (ตารางไม่คุ้ม) → delegate ไป binary search ทันที
   history: reads_served นับทุก read (table + binary) — ตารางเรียนรู้ว่า
   workload อ่านมากพอจะ rebuild คุ้มหรือไม่ (§15.59) */
static inline int ghost_pair_find(GhostLog *log, uint16_t block_id,
                                  uint8_t from_scale, uint8_t to_scale) {
    if (!log || !log->pair) return -1;
    int r = ghost_pair_refresh(log);
    if (r == 1) {
        log->pair->reads_served++;         /* history — binary ยังนับ */
        return ghost_log_find(log, block_id, from_scale, to_scale);
    }
    if (r != 0) return -1;
    GhostPairTable *t = log->pair;
    if (block_id >= t->max_block) return -1;
    uint32_t key = ((uint32_t)block_id << 8) | from_scale;
    uint16_t start = t->pile[key];
    if (start == 0xFFFFu) return -1;
    t->reads_served++;                     /* history: ตารางตอบ 1 read */
    for (uint32_t i = start; i < log->count; i++) {
        const GhostLogEntry *e = &log->entries[i];
        if (e->block_id != block_id || e->from_scale != from_scale) break;
        if (e->flags & GHOST_FLAG_EXPIRED) continue;
        if (e->to_scale == to_scale) return (int)i;
    }
    return -1;
}

/* O(1) route count — ช่วงของ block ใน entries (ข้าม from ทั้งหมด)
   refresh คืน 1 → delegate ไป ghost_route_count (binary) */
static inline uint32_t ghost_pair_route_count(GhostLog *log, uint16_t block_id) {
    if (!log || !log->pair) return 0;
    int r = ghost_pair_refresh(log);
    if (r == 1) {
        log->pair->reads_served++;         /* history — binary ยังนับ */
        return ghost_route_count(log, block_id);
    }
    if (r != 0) return 0;
    GhostPairTable *t = log->pair;
    if (block_id >= t->max_block) return 0;
    t->reads_served++;                     /* history: ตารางตอบ 1 read */
    return (uint32_t)t->block_lo[block_id + 1u] - t->block_lo[block_id];
}

/* ════════════════════════════════════════════════════════════
   LIFT — freeze block data as ghost + record the route
   ════════════════════════════════════════════════════════════
   Returns bond_key on success, 0 (RS_BOND_KEY_RESERVED) on failure.
   Data is frozen ONCE per birth pile; lifting to another to_scale
   appends a route to the SAME frozen data (ไม่เคยเขียนซ้ำ). */
static inline uint64_t ghost_lift(GhostLog *log, ResidualSpace *rs,
                                  uint16_t block_id, uint8_t from_scale,
                                  uint8_t to_scale,
                                  const void *data, uint32_t size) {
    if (!log || !rs || !data || size == 0)
        return RS_BOND_KEY_RESERVED;
    if (log->count >= GHOST_LOG_MAX)
        return RS_BOND_KEY_RESERVED;    /* log full — reserve first */
    if (ghost_log_find(log, block_id, from_scale, to_scale) >= 0)
        return RS_BOND_KEY_RESERVED;    /* route already exists */

    PoglsPiece p = ghost_piece(block_id, from_scale, to_scale);
    uint64_t bk = rs_freeze(rs, &p, data, size, 0);
    if (bk == RS_BOND_KEY_RESERVED)
        return RS_BOND_KEY_RESERVED;

    /* SORTED INSERT โดย (block, from) — คง sortedness สำหรับ binary search
       (memmove ≤ GHOST_LOG_MAX×5B ต่อ lift — lift หายาก, read บ่อย) */
    int pos = _ghost_pile_lo(log, block_id, from_scale);
    for (uint32_t i = log->count; i > (uint32_t)pos; i--)
        log->entries[i] = log->entries[i - 1u];
    GhostLogEntry *e = &log->entries[pos];
    e->block_id   = block_id;
    e->from_scale = from_scale;
    e->to_scale   = to_scale;
    e->flags      = GHOST_FLAG_LIFT;
    log->count++;
    if (log->pair) {
        log->pair->dirty = 1;              /* อัปเดต log → ตาราง stale */
        /* O(1) signal: อัปเดต hint ขนาดตารางตอน lift — ไม่ต้อง scan */
        if ((uint32_t)block_id + 1u > log->pair->hint_max_block)
            log->pair->hint_max_block = (uint32_t)block_id + 1u;
    }
    return bk;
}

/* ════════════════════════════════════════════════════════════
   READ — derive bond from (block_id, from_scale), require a live
   route to (to_scale), then thaw.  Two-layer truth:
     wrong from_scale → bond breaks (NULL)
     wrong to_scale   → route not found (NULL)
   ════════════════════════════════════════════════════════════ */
static inline const void *ghost_read(GhostLog *log, const ResidualSpace *rs,
                                     uint16_t block_id, uint8_t from_scale,
                                     uint8_t to_scale, uint32_t *out_size) {
    if (!log || !rs) return NULL;
    /* route check: pair table (ถ้า attach — O(1) auto refresh)
       หรือ binary search (fallback — ไม่มีตาราง) */
    if (log->pair) {
        if (ghost_pair_find(log, block_id, from_scale, to_scale) < 0)
            return NULL;
    } else if (ghost_log_find(log, block_id, from_scale, to_scale) < 0) {
        return NULL;                     /* route is the authority  */
    }

    /* integrity gate (fusion S2) — เส้นทางต้องเปิด:
       wang edge + tamper ตรวจ timeline ของ route (CLOSED/TAMPER → NULL)
       incoming = chord ของตำแหน่งเอง (self-consistent — tantrix DROP
       เป็น routing decision ตอน seek, พิสูจน์ใน test_wang_tantrix) */
    uint16_t enc = (uint16_t)((ghost_origin_seed(block_id, from_scale)
                              + to_scale) % FRAME_CYCLE);
    HypSeek d = hyp_gate(&log->wang, enc,
                         (uint8_t)(_fwang_chord_a(enc) & 3u));
    if (d != HYP_SEEK_OPEN && d != HYP_SEEK_SKIP)
        return NULL;                     /* timeline เสีย → ปิดเส้นทาง  */

    PoglsPiece p = ghost_piece(block_id, from_scale, to_scale);
    return rs_thaw(rs, pogls_bond_key(&p), out_size);
}

/* ════════════════════════════════════════════════════════════
   AUTO LIFT — envelope gate (§11.6 — decided)
   ════════════════════════════════════════════════════════════
   Requested expansion depth > envelope_depth(gate) → FREEZE data as
   ghost + record the route (instead of expanding the pile — ย่อฟรี
   ขยายจ่าย; ROI cliff at k=4-5, knob = gate).

   Returns GHOST_AUTO_PLACE (within envelope — normal placement),
   GHOST_AUTO_LIFT (lifted), or GHOST_AUTO_ERR. */
#define GHOST_AUTO_PLACE 0
#define GHOST_AUTO_LIFT  1
#define GHOST_AUTO_ERR   (-1)

static inline int ghost_lift_auto(GhostLog *log, ResidualSpace *rs,
                                  double gate, uint16_t block_id,
                                  uint8_t from_scale, uint8_t to_scale,
                                  const void *data, uint32_t size) {
    if (!log || !rs || !data || size == 0)
        return GHOST_AUTO_ERR;
    if (ght_scale_depth(from_scale, to_scale) <= ght_envelope_depth(gate))
        return GHOST_AUTO_PLACE;    /* within envelope — field placement */
    uint64_t bk = ghost_lift(log, rs, block_id, from_scale, to_scale,
                             data, size);
    return (bk != RS_BOND_KEY_RESERVED) ? GHOST_AUTO_LIFT : GHOST_AUTO_ERR;
}

/* ════════════════════════════════════════════════════════════
   EXPIRE — re-attach: kill the OLD pile's ghosts
   ════════════════════════════════════════════════════════════
   Uses rs_expire_by_origin with the derived geo_key (birth pile
   identity).  Frozen data is tombstoned; log entries stay as audit
   trail flagged GHOST_FLAG_EXPIRED.  Returns number of routes expired. */
static inline uint32_t ghost_expire(GhostLog *log, ResidualSpace *rs,
                                    uint16_t block_id, uint8_t from_scale) {
    if (!log || !rs) return 0;

    PoglsPiece p = ghost_piece(block_id, from_scale, 0);
    rs_expire_by_origin(rs, p.geo_key);   /* data dies by birth pile */

    uint32_t expired = 0;
    int i = _ghost_pile_lo(log, block_id, from_scale);
    for (; i < (int)log->count; i++) {
        GhostLogEntry *e = &log->entries[i];
        if (e->block_id != block_id || e->from_scale != from_scale) break;
        if (!(e->flags & GHOST_FLAG_EXPIRED)) {
            e->flags |= GHOST_FLAG_EXPIRED;
            expired++;
        }
    }
    if (expired && log->pair) log->pair->dirty = 1;  /* flags เปลี่ยน → stale */
    return expired;
}

/* ════════════════════════════════════════════════════════════
   LOG PERSISTENCE — routes survive the restart (§15.34)
   ════════════════════════════════════════════════════════════
   The ghost log is the durable AUDIT TRAIL (live + EXPIRED routes).
   All entries are persisted verbatim — 5 B each, delta ∝ events.
   Format (little-endian, packed):
     [0..3]   magic "GHST" (4B)
     [4..5]   version u16 = 1
     [6..7]   reserved u16 = 0
     [8..11]  count u32
     [12..]   GhostLogEntry records (5B each)
   */
#define GHOST_LOG_MAGIC "GHST"

static inline uint64_t ghost_log_serialize_size(const GhostLog *log) {
    if (!log) return 0;
    return 12u + (uint64_t)log->count * sizeof(GhostLogEntry);
}

static inline uint64_t ghost_log_serialize(const GhostLog *log, void *buf,
                                           uint64_t cap) {
    if (!log || !buf) return 0;
    uint64_t need = ghost_log_serialize_size(log);
    if (cap < need) return 0;
    uint8_t *p = (uint8_t *)buf;
    memcpy(p, GHOST_LOG_MAGIC, 4); p += 4;
    p[0] = 1; p[1] = 0;
    p[2] = 0; p[3] = 0;
    p += 4;
    memcpy(p, &log->count, 4); p += 4;
    if (log->count > 0)
        memcpy(p, log->entries,
               (size_t)(log->count * sizeof(GhostLogEntry)));
    return need;
}

static inline int ghost_log_load(GhostLog *log, const void *buf, uint64_t size) {
    if (!log || !buf) return -1;
    const uint8_t *p = (const uint8_t *)buf;
    if (size < 12) return -1;
    if (memcmp(p, GHOST_LOG_MAGIC, 4) != 0) return -1;
    uint16_t ver;
    memcpy(&ver, p + 4, 2);
    if (ver != 1) return -1;
    uint32_t count;
    memcpy(&count, p + 8, 4);
    if (count > GHOST_LOG_MAX) return -1;
    if (12u + (uint64_t)count * sizeof(GhostLogEntry) > size) return -1;
    ghost_log_init(log);
    if (count > 0)
        memcpy(log->entries, p + 12,
               (size_t)(count * sizeof(GhostLogEntry)));
    log->count = count;
    return 0;
}

#endif /* GEO_GHOST_LIFT_H */
