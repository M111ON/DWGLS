/*
 * b_fs_magnifier.h — Magnifier Pointer (yin-yang two-sided) for b_fs
 * ════════════════════════════════════════════════════════════════════
 * สนามทั้งหมด = [0, 20736] บนเส้น infinity
 * ข้อมูลวางจากฝั่ง 20736 (ceiling) ก่อนเสมอ → เดินเข้าหา 0 = ย่อ (contract)
 * ถ้าวางจาก >0 → เดินออกห่าง = ขยาย (expand)
 *
 * Magnifier (pointer) มีสองฝั่ง (yin-yang):
 *   - ฝั่งหนึ่ง ขยาย (expand)  — เห็นข้อมูลมากขึ้น
 *   - อีกฝั่ง  ย่อ (contract) — เห็นข้อมูลน้อยลง
 * กลไก: อ่านไฟล์ที่จุดที่เขียนครั้งแรก → lossless เสมอ
 * (reader_pos == write_pos ⇒ lossless)
 * ════════════════════════════════════════════════════════════════════
 */
#ifndef B_FS_MAGNIFIER_H
#define B_FS_MAGNIFIER_H

#include <stdint.h>
#include <string.h>
#include "b_fs_base.h"

/* ── Magnifier: center + scale + two sides ─────────────────────── */
typedef struct {
    uint32_t center;        /* where the magnifier stands (flat addr) */
    double   scale;         /* current scale (expand/contract)         */
    uint32_t write_pos;     /* where data was originally written       */
    uint32_t expand_side;   /* width of expand (magnify) side          */
    uint32_t contract_side; /* width of contract (shrink) side         */
    int      anchored_ceiling; /* 1 = data anchored at 20736 (ceiling) */
} BfsMagnifier;

static inline void bfmag_init(BfsMagnifier *m, uint32_t write_pos) {
    if (!m) return;
    memset(m, 0, sizeof(*m));
    m->center = write_pos % BFS_PAIR_N;
    m->scale = 1.0;
    m->write_pos = write_pos % BFS_PAIR_N;
    m->expand_side = BFS_PAIR_N / 2;
    m->contract_side = BFS_PAIR_N / 2;
    m->anchored_ceiling = 1;
}

/* Move magnifier to new center at given scale.
 * Data stays anchored at write_pos (ceiling). Movement = scale change. */
static inline void bfmag_move(BfsMagnifier *m, uint32_t new_center, double new_scale) {
    if (!m) return;
    if (new_scale < 1e-6) new_scale = 1e-6;
    m->center = new_center % BFS_PAIR_N;
    m->scale = new_scale;
    /* yin-yang: one side expands, other contracts based on direction */
    if (new_center < m->write_pos) {
        /* moving toward 0 → contract side grows */
        m->contract_side = (uint32_t)((double)BFS_PAIR_N / new_scale);
        m->expand_side = BFS_PAIR_N - m->contract_side;
        if (m->expand_side < 1) m->expand_side = 1;
    } else {
        /* moving away → expand side grows */
        m->expand_side = (uint32_t)((double)BFS_PAIR_N / new_scale);
        m->contract_side = BFS_PAIR_N - m->expand_side;
        if (m->contract_side < 1) m->contract_side = 1;
    }
}

/* Lossless iff magnifier center == write point */
static inline int bfmag_is_lossless(const BfsMagnifier *m) {
    if (!m) return 0;
    return (m->center == m->write_pos) ? 1 : 0;
}

/* Scale factor: how much data is visible (expand side) */
static inline uint32_t bfmag_visible(const BfsMagnifier *m) {
    if (!m) return 0;
    return m->expand_side;
}

#endif /* B_FS_MAGNIFIER_H */
