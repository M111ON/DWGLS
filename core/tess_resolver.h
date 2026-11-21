/* tess_resolver.h — Zero-copy pointer-arithmetic resolver for Tesseract cubes.
 *
 * Full system: combines stride-37, balanced ternary [-1,0,1], 3/4 tetra branch,
 * 111^3 walking engine, quadtree cascade (rewind), Voronoi mask (multi-pointer),
 * constant distort (hyperbolic two-sided), inside-out (pos24+neg24=48).
 *
 * Physical: each cube = contiguous block (mmap, zero-copy, owns_buf=0)
 * Logical:  slot → cell via stride-37 bijection + balanced ternary + 3/4 branch
 * No Gather, no Memcpy — offset computation IS the resolution.
 *
 * Sacred: 20736 = 144² = 12⁴. Stride-37 (coprime with 20736).
 * Balanced ternary: [-1,0,1] (two-sided, inside-out).
 * 3/4 tetra: 4 faces, 3 usable (1 blocked by octa).
 * 111^3: walking engine (1 unit/axis, deterministic).
 * 24 tetra: 8 octants × 3 axes. 3 square planes = identity anchor.
 * Inside-out: pos24 + neg24 = 48 (octahedral group, closed loop).
 */
#ifndef TESS_RESOLVER_H
#define TESS_RESOLVER_H

#include <stdint.h>
#include <stddef.h>

#define TESS_TOTAL_SLOTS  20736u
#define TESS_STRIDE       37u
#define TESS_CELL_Q8      34u
#define TESS_CASCADE_LEVELS 4u

/* ── Balanced Ternary (base 3: -1, 0, 1) ─────────────────────── */
static inline int bt_floor_div(int a, int b) {
    int q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}

/* Encode integer → balanced ternary digits. Returns 0 on success. */
static inline int bt_encode(int val, int *digits, int n) {
    int v = val;
    for (int i = 0; i < n; i++) {
        int rem = ((v % 3) + 3) % 3;
        int digit;
        if (rem == 0) digit = 0;
        else if (rem == 1) digit = 1;
        else digit = -1;
        digits[i] = digit;
        v = bt_floor_div(v - digit, 3);
    }
    return (v == 0) ? 0 : -1;
}

/* Decode balanced ternary digits → integer. */
static inline int bt_decode(const int *digits, int n) {
    int val = 0, pow3 = 1;
    for (int i = 0; i < n; i++) {
        val += digits[i] * pow3;
        pow3 *= 3;
    }
    return val;
}

/* ── 3/4 Tetra Branch ─────────────────────────────────────────── */
#define TETRA_FACES  4
#define TETRA_USABLE 3
#define TETRA_BLOCKED 3

static inline int tetra_can_branch(int face_idx) {
    return (face_idx != TETRA_BLOCKED) ? 1 : 0;
}

static inline int tetra_usable_count(void) { return 3; }

/* ── 111^3 Walking Engine ─────────────────────────────────────── */
typedef struct { int x, y, z; } WalkPos;

static inline WalkPos walk_step(WalkPos p, int dx, int dy, int dz) {
    WalkPos np = { p.x + dx, p.y + dy, p.z + dz };
    if (np.x > 111) np.x = 111; if (np.x < -111) np.x = -111;
    if (np.y > 111) np.y = 111; if (np.y < -111) np.y = -111;
    if (np.z > 111) np.z = 111; if (np.z < -111) np.z = -111;
    return np;
}

/* ── Constant Distort (hyperbolic two-sided) ─────────────────── */
static inline uint32_t tess_distort(uint32_t slot, uint32_t pointer_pos, uint32_t total) {
    uint32_t d = (slot > pointer_pos) ? (slot - pointer_pos) : (pointer_pos - slot);
    if (d > total / 2) d = total - d;
    return d;
}

/* ── Stride-37 scatter (bijection, lossless) ───────────────────── */
static inline uint32_t tess_slot_to_cell(uint32_t slot) {
    return (slot * TESS_STRIDE) % TESS_TOTAL_SLOTS;
}

static inline uint32_t tess_cell_to_slot(uint32_t cell) {
    for (uint32_t s = 0; s < TESS_TOTAL_SLOTS; s++) {
        if ((s * TESS_STRIDE) % TESS_TOTAL_SLOTS == cell) return s;
    }
    return cell;
}

/* ── Pointer-arithmetic resolver (zero-copy) ───────────────────── */
typedef struct {
    const uint8_t *base;
    uint32_t        cell_size;
    uint32_t        n_cells;
    uint32_t        capo_id;
} TessCubeView;

/* Single-cube resolve (slot must be < TESS_TOTAL_SLOTS) */
static inline const uint8_t *tess_resolve_ptr(const TessCubeView *cube, uint32_t slot) {
    uint32_t cell = tess_slot_to_cell(slot % TESS_TOTAL_SLOTS);
    return cube->base + (size_t)cell * cube->cell_size;
}

/* ── Multi-cube resolver (zero-copy, no Gather) ────────────────── */
typedef struct {
    TessCubeView *caps;       /* array of capo views */
    uint32_t      n_caps;     /* number of capos */
    uint32_t      cell_size;  /* bytes per cell (uniform) */
    uint64_t      total_cells; /* total cells across all capos */
} TessMultiCubeView;

/* Resolve weight index → zero-copy pointer (multi-cube) */
static inline const uint8_t *tess_resolve_multi(const TessMultiCubeView *mc, uint64_t weight_idx) {
    uint32_t cube_id = (uint32_t)(weight_idx / TESS_TOTAL_SLOTS);
    uint32_t local_slot = (uint32_t)(weight_idx % TESS_TOTAL_SLOTS);
    if (cube_id >= mc->n_caps) return NULL;
    if (!mc->caps[cube_id].base) return NULL;
    uint32_t cell = tess_slot_to_cell(local_slot);
    return mc->caps[cube_id].base + (size_t)cell * mc->caps[cube_id].cell_size;
}

/* ── Balanced ternary coordinate → slot ────────────────────────── */
/* 3 ternary digits → 27 positions → mapped to 3/4 branch → slot */
static inline uint32_t bt_coord_to_slot(int d0, int d1, int d2) {
    /* balanced ternary → index [0,26] → stride-37 → slot */
    int idx = (d0 + 1) * 9 + (d1 + 1) * 3 + (d2 + 1);
    /* idx in [0,26], map to slot via 3/4 branch (skip blocked face 3) */
    /* Simple: idx * 768 ≈ slot in 20736 (27 * 768 = 20736) */
    uint32_t slot = (uint32_t)idx * (TESS_TOTAL_SLOTS / 27);
    if (slot >= TESS_TOTAL_SLOTS) slot = TESS_TOTAL_SLOTS - 1;
    return slot;
}

/* ── Quadtree cascade (bottom-up OR propagation = rewind) ──────── */
typedef struct {
    uint8_t leaf[64];
    uint8_t l16[16];
    uint8_t l4[4];
    uint8_t root;
    int     active;
} TessCascade;

static inline void tess_cascade_update(TessCascade *c) {
    for (int i = 0; i < 16; i++)
        c->l16[i] = c->leaf[i*4] | c->leaf[i*4+1] | c->leaf[i*4+2] | c->leaf[i*4+3];
    for (int i = 0; i < 4; i++)
        c->l4[i] = c->l16[i*4] | c->l16[i*4+1] | c->l16[i*4+2] | c->l16[i*4+3];
    c->root = c->l4[0] | c->l4[1] | c->l4[2] | c->l4[3];
}

static inline void tess_cascade_activate(TessCascade *c, int leaf_idx) {
    if (leaf_idx < 0 || leaf_idx >= 64) return;
    c->leaf[leaf_idx] = 1;
    tess_cascade_update(c);
    c->active = 1;
}

static inline void tess_cascade_deactivate(TessCascade *c, int leaf_idx) {
    if (leaf_idx < 0 || leaf_idx >= 64) return;
    c->leaf[leaf_idx] = 0;
    tess_cascade_update(c);
    int any = 0;
    for (int i = 0; i < 64; i++) if (c->leaf[i]) { any = 1; break; }
    c->active = any;
}

/* ── Voronoi mask (temporary unfold for multi-pointer read) ────── */
typedef struct {
    uint8_t mask[TESS_TOTAL_SLOTS];
    int     applied;
} TessVoronoiMask;

static inline void tess_voronoi_apply(TessVoronoiMask *m, uint32_t slot) {
    m->mask[slot] = 1; m->applied = 1;
}
static inline void tess_voronoi_clear(TessVoronoiMask *m, uint32_t slot) {
    m->mask[slot] = 0;
    int any = 0;
    for (uint32_t i = 0; i < TESS_TOTAL_SLOTS; i++) if (m->mask[i]) { any = 1; break; }
    m->applied = any;
}
static inline int tess_voronoi_is_unfolded(const TessVoronoiMask *m, uint32_t slot) {
    return m->mask[slot];
}

/* ── Inside-out constants ──────────────────────────────────────── */
#define POS_COUNT 24
#define NEG_COUNT 24
#define OCTAHEDRAL_GROUP 48

/* ── 24 Tetra identity ─────────────────────────────────────────── */
#define TETRA_PER_AXIS 8
#define TETRA_TOTAL 24
#define TETRA_AXES 3

#endif /* TESS_RESOLVER_H */
