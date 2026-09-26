/* geo_vol6.h — 6-axis volume address: xyz + ijk, 144 each.
 *
 * 144^6 = 8,916,100,448,256 (~8.9T) = same scale as 20736^3, but every
 * axis is exactly one 144: no cell decomposition, no new constants, and
 * each axis value indexes directly into a 144-unit (BFS lives inside).
 * xyz = square/cube lane, ijk = triangle lane (GBA axis order).
 * Rank fits u64 (max < 2^44). Pure integer, header-only.
 */
#ifndef GEO_VOL6_H
#define GEO_VOL6_H

#include <stdint.h>

#define V6_AXES  6u
#define V6_SPAN  144u
/* 144^6, computed by loop in test; value: 8916100448256 */
#define V6_SPACE 8916100448256ull

typedef struct { uint8_t v[V6_AXES]; } V6Addr; /* x,y,z,i,j,k each < 144 */

static inline int v6_valid(const V6Addr *a) {
    if (!a) return 0;
    for (int k = 0; k < 6; k++) if (a->v[k] >= V6_SPAN) return 0;
    return 1;
}

/* lexicographic rank, x most significant. */
static inline uint64_t v6_rank(const V6Addr *a) {
    uint64_t r = 0;
    for (int k = 0; k < 6; k++) r = r * V6_SPAN + a->v[k];
    return r;
}

static inline V6Addr v6_unrank(uint64_t r) {
    V6Addr a;
    for (int k = 5; k >= 0; k--) { a.v[k] = (uint8_t)(r % V6_SPAN); r /= V6_SPAN; }
    return a;
}

/* lane split: xyz cube lane + ijk triangle lane, each 144^3 = 2985984. */
static inline uint32_t v6_cube(const V6Addr *a) {
    return ((uint32_t)a->v[0] * V6_SPAN + a->v[1]) * V6_SPAN + a->v[2];
}
static inline uint32_t v6_tri(const V6Addr *a) {
    return ((uint32_t)a->v[3] * V6_SPAN + a->v[4]) * V6_SPAN + a->v[5];
}
static inline V6Addr v6_join(uint32_t cube, uint32_t tri) {
    V6Addr a;
    a.v[2] = (uint8_t)(cube % V6_SPAN); cube /= V6_SPAN;
    a.v[1] = (uint8_t)(cube % V6_SPAN); a.v[0] = (uint8_t)(cube / V6_SPAN);
    a.v[5] = (uint8_t)(tri % V6_SPAN); tri /= V6_SPAN;
    a.v[4] = (uint8_t)(tri % V6_SPAN); a.v[3] = (uint8_t)(tri / V6_SPAN);
    return a;
}

/* stride-37 step on one axis: bijective (gcd(37,144)=1). */
static inline uint8_t v6_step(uint8_t p) { return (uint8_t)((p * 37u) % V6_SPAN); }

/* ── bridge: one linear offset over three rulers ──
 * rank (0..144^6) == cell key (x,y,z,i) + local (j,k)
 *               == KIS chunk (o/20736) + slot (o%20736)
 *               == CLIM base k + slot s (window slides A+1,B+1, mapping holds).
 * Full space covered exactly: 144^4 cells x 20736. -1 on OOB. */
static inline uint64_t v6_cell_index(const uint8_t key[4]) {
    return (((uint64_t)key[0] * V6_SPAN + key[1]) * V6_SPAN + key[2]) * V6_SPAN + key[3];
}

static inline int v6_stripe(uint64_t o, uint8_t key[4], uint8_t *j, uint8_t *k) {
    if (o >= V6_SPACE || !key || !j || !k) return -1;
    uint64_t c = o / 20736ull;
    uint32_t s = (uint32_t)(o % 20736ull);
    for (int d = 3; d >= 0; d--) { key[d] = (uint8_t)(c % V6_SPAN); c /= V6_SPAN; }
    *j = (uint8_t)(s / V6_SPAN);
    *k = (uint8_t)(s % V6_SPAN);
    return 0;
}

static inline int v6_clim_locate(uint64_t base, uint32_t s, uint8_t key[4],
                                 uint8_t *j, uint8_t *k) {
    if (s >= 20736u) return -1;
    return v6_stripe(base + s, key, j, k);
}

#endif /* GEO_VOL6_H */
