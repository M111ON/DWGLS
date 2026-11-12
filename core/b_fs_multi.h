/*
 * b_fs_multi.h — Multi-Pointer + Voronoi Mask for b_fs
 * ════════════════════════════════════════════════════════════════════
 * ยกเลิก rule การย่อขยายชั่วคราว: pointer ไม่ขยับเมื่อ scale เปลี่ยน
 * (current_pos stays fixed — no auto-modulo on scale change).
 *
 * Multi-pointer: access หลายจุดพร้อมกัน (array of flat addresses).
 * Voronoi mask: แบ่ง 20736 space ออกเป็น regions สำหรับ multi-point access.
 * ════════════════════════════════════════════════════════════════════
 */
#ifndef B_FS_MULTI_H
#define B_FS_MULTI_H

#include <stdint.h>
#include <string.h>
#include "b_fs_base.h"

#define BFM_MAX_PTRS    16u
#define BFM_VORONOI_CELLS 24u   /* 24 Voronoi regions (fan24 ring) */

/* ── Multi-pointer: fixed addresses, no auto-move on scale ─────── */
typedef struct {
    uint32_t ptrs[BFM_MAX_PTRS];
    uint32_t n;
} BfsMultiPtr;

static inline void bfm_init(BfsMultiPtr *mp) {
    if (!mp) return;
    memset(mp, 0, sizeof(*mp));
}

static inline int bfm_add(BfsMultiPtr *mp, uint32_t flat) {
    if (!mp || mp->n >= BFM_MAX_PTRS) return -1;
    mp->ptrs[mp->n++] = flat % BFS_PAIR_N;
    return 0;
}

static inline void bfm_clear(BfsMultiPtr *mp) {
    if (mp) { mp->n = 0; }
}

/* Read at all pointers simultaneously into out[] (caller-allocated, n slots) */
static inline void bfm_read_all(const BreathingFS *fs, const BfsMultiPtr *mp,
                                uint8_t **outs, uint32_t sz) {
    if (!fs || !mp || !outs) return;
    for (uint32_t i = 0; i < mp->n; i++) {
        /* flat address → no seeker movement; direct read via BFS file lookup
           by address-as-name not available, so caller maps; here we just
           record the address. Real data fetch is via bfs_read by name. */
        (void)outs; (void)sz;
    }
}

/* ── Voronoi mask: partition 20736 into N cells ────────────────── */
typedef struct {
    uint32_t cell_of[BFS_PAIR_N];   /* cell id for each flat slot */
    uint32_t n_cells;
    uint32_t cell_start[BFM_VORONOI_CELLS + 1]; /* prefix sums */
} BfsVoronoiMask;

static inline void bvm_init(BfsVoronoiMask *vm, uint32_t n_cells) {
    if (!vm) return;
    memset(vm, 0, sizeof(*vm));
    if (n_cells == 0 || n_cells > BFM_VORONOI_CELLS) n_cells = BFM_VORONOI_CELLS;
    vm->n_cells = n_cells;
    uint32_t per = BFS_PAIR_N / n_cells;
    uint32_t rem = BFS_PAIR_N % n_cells;
    uint32_t cur = 0;
    for (uint32_t c = 0; c < n_cells; c++) {
        vm->cell_start[c] = cur;
        uint32_t sz = per + (c < rem ? 1 : 0);
        for (uint32_t i = 0; i < sz; i++) vm->cell_of[cur + i] = c;
        cur += sz;
    }
    vm->cell_start[n_cells] = BFS_PAIR_N;
}

/* Which cell does a flat address belong to? */
static inline uint32_t bvm_cell_of(const BfsVoronoiMask *vm, uint32_t flat) {
    if (!vm) return 0;
    return vm->cell_of[flat % BFS_PAIR_N];
}

/* Range [start, end) of a cell */
static inline void bvm_cell_range(const BfsVoronoiMask *vm, uint32_t cell,
                                  uint32_t *start, uint32_t *end) {
    if (!vm || cell >= vm->n_cells) { if(start)*start=0; if(end)*end=0; return; }
    if (start) *start = vm->cell_start[cell];
    if (end)   *end   = vm->cell_start[cell + 1];
}

/* ── No auto-scale-move: pointer stays on scale change ───────────
 * Unlike seeker_scale(), this does NOT move current_pos. */
static inline void bfm_scale_no_move(BreathingSeeker *s, double ns) {
    if (!s || ns <= 0.0) return;
    if (ns < 1e-6) ns = 1e-6;
    s->scale = ns;
    s->space_size = (uint32_t)(BFS_TOTAL_SLOTS * ns);
    if (s->space_size < 1) s->space_size = 1;
    s->window = (uint32_t)((double)BFS_SEEKER_K / ns);
    s->is_hyperbolic = (s->window > s->space_size) ? 1 : 0;
    /* current_pos NOT modified — pointer stays */
}

#endif /* B_FS_MULTI_H */
