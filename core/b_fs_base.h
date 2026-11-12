/*
 * b_fs_base.h — Unified b_fs Foundation (Phase 1)
 * ════════════════════════════════════════════════════════════════════
 * Single base that every DWGLS subsystem docks to:
 *   - canonical storage API: bfs_write / bfs_read / bfs_move_seeker
 *     (re-exported from breathing_fs.h — the only storage primitive)
 *   - Twin: hot (RAM/mmap) + cold (storage) views at ONE address
 *   - Pair rule: bijective cross-dimension address transfer
 *
 * Nothing here allocates. Twin views are caller-owned pointers.
 * Pair transfer is pure arithmetic (affine-modular) — no lookup, no data.
 * ════════════════════════════════════════════════════════════════════
 */
#ifndef B_FS_BASE_H
#define B_FS_BASE_H

#include <stdint.h>
#include <string.h>
#include "breathing_fs.h"
#include "geo_frame_seek.h"

/* ── Canonical dimensions ─────────────────────────────────────── */
typedef enum {
    BFS_DIM_FLAT  = 0,   /* 20736 flat address space       */
    BFS_DIM_TESS  = 1,   /* tesseract 18-tower view        */
    BFS_DIM_KIS   = 2,   /* KIS timeline 1440-cycle view   */
    BFS_DIM_COUNT = 3
} BfsDim;

/* ── Twin: hot + cold views at one address ───────────────────── */
typedef struct {
    uint8_t *hot;        /* RAM / mmap view (may == cold)   */
    uint8_t *cold;       /* storage / file view              */
    size_t    size;      /* bytes of the twin                */
    int       separate;  /* 1 if hot != cold, 0 if aliased   */
} BfsTwin;

static inline void bfs_twin_init(BfsTwin *t, uint8_t *hot, uint8_t *cold, size_t size) {
    if (!t) return;
    t->hot = hot; t->cold = cold; t->size = size;
    t->separate = (hot != NULL && cold != NULL && hot != cold) ? 1 : 0;
}

/* Sync hot → cold (the "breath" that parks data in storage). */
static inline void bfs_twin_sync(BfsTwin *t) {
    if (!t || !t->hot || !t->cold || t->hot == t->cold) return;
    memcpy(t->cold, t->hot, t->size);
}

/* Verify twin roundtrip: original == cold after sync. Returns 1 if ok. */
static inline int bfs_twin_roundtrip_ok(const uint8_t *orig, const BfsTwin *t) {
    if (!orig || !t || !t->cold) return 0;
    return (memcmp(orig, t->cold, t->size) == 0) ? 1 : 0;
}

/* ── Pair rule: bijective cross-dimension transfer ─────────────
 * Affine-modular: f(x) = (a*x + b) mod N, gcd(a,N)=1.
 * Cross-dim uses stride 37 (proven coprime with 20736).
 * Same-dim is identity. Inverse uses modular inverse.           */
#define BFS_PAIR_N       BFS_TOTAL_SLOTS
#define BFS_PAIR_A       37u

static inline uint32_t bfs_pair_modinv(uint32_t a, uint32_t m) {
    /* extended euclid, small inputs */
    int64_t t = 0, newt = 1;
    int64_t r = (int64_t)m, newr = (int64_t)a;
    while (newr != 0) {
        int64_t q = r / newr;
        int64_t tmp = t - q * newt; t = newt; newt = tmp;
        tmp = r - q * newr; r = newr; newr = tmp;
    }
    if (r > 1) return 0;       /* not invertible */
    if (t < 0) t += (int64_t)m;
    return (uint32_t)t;
}

/* Transfer address from src dim to dst dim. Pure, no data. */
static inline uint32_t bfs_pair_transfer(BfsDim src, BfsDim dst, uint32_t addr) {
    if (src == dst) return addr % BFS_PAIR_N;
    /* cross-dimension: affine step */
    return (uint32_t)(((uint64_t)addr * BFS_PAIR_A) % BFS_PAIR_N);
}

/* Inverse: dst → src. */
static inline uint32_t bfs_pair_inverse(BfsDim src, BfsDim dst, uint32_t addr) {
    if (src == dst) return addr % BFS_PAIR_N;
    uint32_t inv = bfs_pair_modinv(BFS_PAIR_A, BFS_PAIR_N);
    return (uint32_t)(((uint64_t)addr * inv) % BFS_PAIR_N);
}

/* Roundtrip check helper (returns 1 if identity). */
static inline int bfs_pair_roundtrip_ok(BfsDim a, BfsDim b, uint32_t addr) {
    uint32_t t = bfs_pair_transfer(a, b, addr);
    uint32_t r = bfs_pair_inverse(a, b, t);
    return (r == (addr % BFS_PAIR_N)) ? 1 : 0;
}

#endif /* B_FS_BASE_H */
