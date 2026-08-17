/* ═══════════════════════════════════════════════════════════════════════════
 * tied_dedup.h — §15.75: registry {tensor_id → home} — byte-identical tensors
 * freeze ครั้งเดียว
 * ═══════════════════════════════════════════════════════════════════════════
 * MAP not COMPRESS: tensor ตัวที่สอง (byte-identical กับตัวแรก) ไม่ freeze
 * และไม่กิน field — registry เก็บ route → home (coordinate = address — home
 * bond เดียว). อ่าน dup tensor = resolve route → home → thaw จาก bond เดียว
 * กับที่ home ใช้ (ไม่มี copy ที่สอง — วางครั้งเดียว ไม่เคยเขียนซ้ำ T8b)
 *
 *   tied_dedup_scan : FNV-64 candidate filter + memcmp verify
 *                     (identity = memcmp — FNV เป็นแค่ตัวกรอง ไม่ใช่ address;
 *                      ไม่มี false positive — ต่างค่า → ไม่ merge)
 *   tied_place      : home tensor → chunk 256KB (CAP_RULE_CHUNK) → กฎ trained
 *                     (CAP_RULE_* §15.71): w > kmax → lift (rs_freeze, bond จาก
 *                     ghost_piece) · w ≤ kmax → admit (field footprint ght_fp)
 *                     · เกิน orbit capacity → reject (pointer-home — lossless)
 *                     dup tensor → ข้ามทั้งหมด (registry entry = route)
 *   tied_verify     : ทุก tensor → home ผ่าน route → reconstruct → byte-for-byte
 *
 * ใช้ placement model เดียวกับ field_trainer (rank = chunk ใน tensor, เริ่ม 0
 * ทุก tensor — §15.70/71) และ freeze/thaw เดียวกับ cap_chain_scan (sub-piece
 * 64KB, bond จาก ghost_piece(gid, sub, w)) — ตัวเลขเทียบกันได้โดยตรง
 *
 * block_id (uint16) = global chunk id (สะสมข้าม tensor) — unique ตลอดทั้ง
 * model → rdh_addr ไม่ชน; base_gid[t] = chunk แรกของ tensor t (deterministic —
 * ไม่มี lookup table)
 */
#ifndef TIED_DEDUP_H
#define TIED_DEDUP_H

#include <stdint.h>
#include <string.h>
#include "geo_cap_account.h"   /* CAP_RULE_* — trained placement rule §15.71 */
#include "geo_ghost_lift.h"    /* ghost_piece + pogls_bond_key (rs primitives) */
#include "residual_space.h"    /* ResidualSpace — rs_freeze / rs_thaw */

#define TIED_MAX_TENSORS 2048u

/* ── stats ของหนึ่ง pass ─────────────────────────────────────────────── */
typedef struct {
    uint64_t field_slots;   /* Σ used — field footprint ที่จองจริง      */
    uint64_t lifts;         /* chunks เข้า ghost                        */
    uint64_t rejects;       /* capacity reject → pointer-home (lossless) */
    uint64_t frozen_bytes;  /* rs.total_bytes — physical ที่ freeze จริง */
    uint32_t rs_count;      /* rs.count — entries ใน residual space     */
} TiedChainStats;

/* ── FNV-64 candidate filter (identity = memcmp — ไม่ใช่ address) ────── */
static inline uint64_t tied_fnv64(const uint8_t *p, uint64_t n) {
    uint64_t h = UINT64_C(14695981039346656037);
    for (uint64_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= UINT64_C(1099511628211);
    }
    return h;
}

/* ── scan: หา byte-identical tensor pairs → home_of[] ──────────────────
 * home_of[i]  = index ของ home (ตัวเอง = home) · -1 = ข้าม (ไม่มี data/ว่าง)
 * ตัวแรกที่เจอ = home — อ่าน dup → route → home เสมอ
 * returns: จำนวน dup bytes ทั้งหมด (เก็บ 1 copy ได้เท่านี้) */
static inline uint64_t tied_dedup_scan(const uint8_t *const *data,
                                       const uint32_t *sizes, uint32_t n,
                                       int32_t *home_of) {
    uint64_t dup_bytes = 0;
    uint64_t *fnv = NULL;
    if (n > TIED_MAX_TENSORS) n = TIED_MAX_TENSORS;
    if (n == 0) return 0;

    fnv = (uint64_t *)calloc(n, sizeof(uint64_t));
    if (!fnv) return 0;

    for (uint32_t i = 0; i < n; i++) {
        home_of[i] = (data[i] && sizes[i] > 0) ? (int32_t)i : -1;
        if (home_of[i] >= 0) fnv[i] = tied_fnv64(data[i], sizes[i]);
    }

    for (uint32_t i = 0; i < n; i++) {
        if (home_of[i] < 0) continue;
        for (uint32_t j = i + 1; j < n; j++) {
            if (home_of[j] < 0 || home_of[j] != (int32_t)j) continue; /* placed แล้ว */
            if (sizes[j] != sizes[i] || fnv[i] != fnv[j]) continue;
            if (memcmp(data[i], data[j], sizes[i]) != 0) continue;    /* verify */
            home_of[j] = (int32_t)i;      /* registry: j → home i */
            dup_bytes += sizes[j];
        }
    }
    free(fnv);
    return dup_bytes;
}

/* ── place: home tensor ผ่าน chain — dup ข้าม (registry = route) ───────
 * mirror field_trainer eval_one (rank = chunk ใน tensor) + cap_chain_scan
 * freeze (sub-piece 64KB, bond = ghost_piece(gid, sub, w)) — คืน 0 ok, -1 fail
 * base_gid[t] = global chunk id แรกของ tensor t (deterministic — ใช้ตอนอ่าน) */
static inline int tied_place(ResidualSpace *rs,
                             const uint8_t *const *data, const uint32_t *sizes,
                             uint32_t n, const int32_t *home_of,
                             uint64_t *base_gid, TiedChainStats *st) {
    uint64_t used[24] = {0};
    uint32_t k_max   = ght_envelope_depth(CAP_RULE_GATE);     /* 3.0 → 4 */
    uint64_t cap_per = GHT_WIN / CAP_RULE_ORBIT;              /* 20736 / O */
    uint64_t gid = 0;
    memset(st, 0, sizeof(*st));

    for (uint32_t t = 0; t < n; t++) {
        base_gid[t] = gid;
        if (home_of[t] < 0) continue;            /* ไม่วาง (ไม่มี data) */
        if (home_of[t] != (int32_t)t) continue;  /* dup — freeze ครั้งเดียวที่ home */

        uint64_t sz = sizes[t];
        uint64_t nchunks = (sz + CAP_RULE_CHUNK - 1) / CAP_RULE_CHUNK;
        if (nchunks == 0) continue;

        for (uint64_t r = 0; r < nchunks; r++, gid++) {
            uint8_t w = (uint8_t)(((uint64_t)CAP_RULE_STRIDE * r + CAP_RULE_OFFSET) % 144u);
            if (w > k_max) {                     /* นอก envelope → ghost */
                st->lifts++;
                uint64_t off = r * CAP_RULE_CHUNK;
                uint32_t len = (uint32_t)(sz - off);
                if (len > CAP_RULE_CHUNK) len = CAP_RULE_CHUNK;
                for (uint32_t s = 0; s * RS_MAX_DATA_SIZE < len; s++) {
                    uint32_t sl = len - s * RS_MAX_DATA_SIZE;
                    if (sl > RS_MAX_DATA_SIZE) sl = RS_MAX_DATA_SIZE;
                    PoglsPiece p = ghost_piece((uint16_t)gid, (uint8_t)s, w);
                    uint64_t bk = rs_freeze(rs, &p, data[t] + off + s * RS_MAX_DATA_SIZE,
                                            sl, 0);
                    if (bk == RS_BOND_KEY_RESERVED) return -1;
                }
            } else {                             /* ใน envelope → field */
                uint64_t env = ght_fp(w);
                uint8_t b = (uint8_t)(r % CAP_RULE_ORBIT);
                if (used[b] + env > cap_per) { st->rejects++; continue; }
                used[b] += env;
                st->field_slots += env;
            }
        }
    }
    st->frozen_bytes = rs->total_bytes;
    st->rs_count     = rs->count;
    return 0;
}

/* ── verify: ทุก tensor (home + dup ผ่าน route) → byte-for-byte ────────
 * dup t → src = home; chunk r ของ t = chunk r ของ home (size เท่ากัน) →
 * bond เดียวกับที่ home วาง → thaw — แล้วเทียบกับต้นฉบับ
 * scratch: buffer ขนาด ≥ max tensor size (ชั่วคราว — ไม่ใช่ lookup) */
static inline int tied_verify(ResidualSpace *rs,
                              const uint8_t *const *data, const uint32_t *sizes,
                              uint32_t n, const int32_t *home_of,
                              const uint64_t *base_gid,
                              uint8_t *scratch, uint32_t scratch_cap) {
    uint32_t k_max = ght_envelope_depth(CAP_RULE_GATE);

    for (uint32_t t = 0; t < n; t++) {
        if (home_of[t] < 0) continue;
        int32_t src = (home_of[t] == (int32_t)t) ? (int32_t)t : home_of[t]; /* route */
        uint64_t len = sizes[t];
        if (len > scratch_cap) return -1;
        uint64_t nchunks = (len + CAP_RULE_CHUNK - 1) / CAP_RULE_CHUNK;

        for (uint64_t r = 0; r < nchunks; r++) {
            uint8_t w = (uint8_t)(((uint64_t)CAP_RULE_STRIDE * r + CAP_RULE_OFFSET) % 144u);
            uint64_t off = r * CAP_RULE_CHUNK;
            uint32_t clen = (uint32_t)(len - off);
            if (clen > CAP_RULE_CHUNK) clen = CAP_RULE_CHUNK;

            if (w > k_max) {                     /* lifted — thaw จาก bond ของ home */
                uint64_t gid = base_gid[src] + r;
                uint32_t got = 0;
                for (uint32_t s = 0; s * RS_MAX_DATA_SIZE < clen; s++) {
                    uint32_t sl = clen - s * RS_MAX_DATA_SIZE;
                    if (sl > RS_MAX_DATA_SIZE) sl = RS_MAX_DATA_SIZE;
                    PoglsPiece p = ghost_piece((uint16_t)gid, (uint8_t)s, w);
                    const uint8_t *chunk = (const uint8_t *)rs_thaw(rs, pogls_bond_key(&p), &got);
                    if (!chunk || got != sl) return -1;
                    memcpy(scratch + off + s * RS_MAX_DATA_SIZE, chunk, sl);
                }
            } else {                             /* admit/reject → pointer-home */
                memcpy(scratch + off, data[src] + off, clen);
            }
        }
        if (memcmp(scratch, data[t], (size_t)len) != 0) return -1;
    }
    return 0;
}

#endif /* TIED_DEDUP_H */
