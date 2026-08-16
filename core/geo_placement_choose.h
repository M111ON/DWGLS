/*
 * geo_placement_choose.h — Adaptive placement scheme (per-file vs global)
 * ═══════════════════════════════════════════════════════════════════════════
 * Question (user): ไฟล์เริ่มเยอะ → ทุกไฟล์จ่าย "ค่าแรกเข้า" (chunk แรกที่
 * w=0 ราคาเต็ม) → ไม่คุ้ม → ควรสลับไป chain ทั้ง folder เป็น rank ลำดับเดียว
 * + targeted assignment (§15.33) ซึ่งเอา chunk เล็กสุดไปวางที่ field ranks
 *
 * Two schemes (size model — real bytes, 1 byte = 1 slot):
 *
 *   PER_FILE: each file restarts ranks at 0 → its chunk at rank 0 has
 *             w=0 → stays in field at FULL price (no shrink).  Cost
 *             grows with the NUMBER of files (entry fee each).
 *             O(1) per file — no sort needed.
 *
 *   GLOBAL:   all chunks in ONE rank sequence; field ranks (w ≤ k_max)
 *             get the SMALLEST chunks (targeted — §15.33).  Cost = Σ
 *             view_of(smallest_i, w_i).  Needs one sort of all sizes.
 *
 * Decision (pc_choose): GLOBAL iff per_file is meaningfully worse:
 *             per_file × 100 > global × (100 + margin_pct)
 *   margin default 50 → switch when global saves ≥ ⅓ of per-file cost.
 *   For a single file both schemes coincide (same field ranks) → stays
 *   PER_FILE (locality: file chunks together — §15.30 belt ordering).
 *
 * Pure + deterministic — same input → same scheme (replay ได้).
 */

#ifndef GEO_PLACEMENT_CHOOSE_H
#define GEO_PLACEMENT_CHOOSE_H

#include <stdint.h>
#include <stdlib.h>

#define PC_SCHEME_PER_FILE 0
#define PC_SCHEME_GLOBAL   1

/* field ranks within one 144-cycle: rank r with w_r = (37r)%144 ≤ k_max.
   For k_max = 5 (gate 1.0): r = {0,4,39,74,109,113} → w = {0,4,3,2,1,5} */
static const uint16_t PC_FR[6] = { 0, 4, 39, 74, 109, 113 };
static const uint8_t  PC_FW[6] = { 0, 4,  3,  2,   1,   5 };

static inline uint8_t pc_scale_w(uint32_t rank) {
    return (uint8_t)(((uint64_t)rank * 37u) % 144u);
}

static inline uint64_t pc_view_of(uint64_t s, uint32_t k) {
    if (k >= 63) return s ? 1 : 0;
    uint64_t d = 1ull << k;
    return (s + d - 1) / d;
}

/* ════════════════════════════════════════════════════════════
   PER-FILE cost — O(1): a file of n chunks (chunk_slots each),
   ranks 0..n−1 → field ranks present within the file's own sequence.
   k_max = ght_envelope_depth(gate) (5 @ gate 1.0).              */
static inline uint64_t pc_per_file_cost(uint32_t n_chunks,
                                        uint64_t chunk_slots,
                                        uint32_t k_max) {
    uint64_t cost = 0;
    uint32_t full = n_chunks / 144u;
    uint32_t rem  = n_chunks % 144u;
    for (int b = 0; b < 6; b++) {
        if (PC_FW[b] > k_max) continue;
        uint64_t v = pc_view_of(chunk_slots, PC_FW[b]);
        cost += (uint64_t)full * v;
        if (PC_FR[b] < rem) cost += v;
    }
    return cost;
}

/* ════════════════════════════════════════════════════════════
   GLOBAL cost — targeted: field ranks over the whole sequence get
   the SMALLEST chunks (sizes ascending + ranks by w ascending).
   sizes_asc: ALL chunk sizes sorted ascending.  Returns cost.    */
static inline uint64_t pc_global_cost(const uint64_t *sizes_asc, uint32_t N,
                                      uint32_t k_max) {
    /* field ranks r < N with w_r ≤ k_max */
    uint32_t *fr = (uint32_t *)malloc(N * sizeof(uint32_t));
    uint32_t nf = 0;
    for (uint32_t r = 0; r < N; r++)
        if (pc_scale_w(r) <= k_max) fr[nf++] = r;
    /* sort ranks by w ascending */
    for (uint32_t i = 0; i < nf; i++)
        for (uint32_t j = i + 1; j < nf; j++)
            if (pc_scale_w(fr[j]) < pc_scale_w(fr[i])) {
                uint32_t t = fr[i]; fr[i] = fr[j]; fr[j] = t;
            }
    uint64_t cost = 0;
    for (uint32_t i = 0; i < nf; i++)
        cost += pc_view_of(sizes_asc[i], pc_scale_w(fr[i]));
    free(fr);
    return cost;
}

/* ════════════════════════════════════════════════════════════
   DECISION — pure: GLOBAL iff per_file is meaningfully worse
   (margin_pct default 50 → switch when saving ≥ ⅓).              */
static inline int pc_choose(uint64_t per_file, uint64_t global,
                            uint32_t margin_pct) {
    if (global == 0) return PC_SCHEME_GLOBAL;
    return (per_file * 100u > global * (100u + margin_pct))
           ? PC_SCHEME_GLOBAL : PC_SCHEME_PER_FILE;
}

#endif /* GEO_PLACEMENT_CHOOSE_H */
