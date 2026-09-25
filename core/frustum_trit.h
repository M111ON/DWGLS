#ifndef FRUSTUM_TRIT_H
#define FRUSTUM_TRIT_H

#include <stdint.h>

/* ── trit dimension ── */
#define TRIT_MOD        27u       /* 3^3 — trit space */
#define FACE_COUNT      6u        /* cube faces / half-axes */
#define COSET_COUNT     9u        /* GiantCube cosets (TRIT_MOD / FACE_COUNT) */
#define LEVEL_COUNT     4u        /* core depth levels */
#define LETTER_COUNT    26u       /* A..Z letter pairs */
#define GEAR_MESH       54u       /* COSET_COUNT * FACE_COUNT = 9 * 6 */

typedef struct {
    uint8_t  trit;      /* 0..26 — primary key */
    uint8_t  coset;     /* 0..8  — GiantCube zone (trit / 3) */
    uint8_t  face;      /* 0..5  — cube direction (trit % 6) */
    uint8_t  level;     /* 0..3  — core depth (trit % 4) */
    uint8_t  letter;    /* 0..25 — LetterPair A..Z */
    uint64_t slope;     /* fibo_seed ^ addr — apex fingerprint */
} TritAddr;

/* ── TritAddr decomposition / recomposition ── */
static inline void trit_decompose(uint8_t trit, TritAddr *out)
{
    out->trit   = trit;
    out->coset  = trit / FACE_COUNT;          /* 0..8 */
    out->face   = trit % FACE_COUNT;          /* 0..5 */
    out->level  = trit % LEVEL_COUNT;         /* 0..3 */
    out->letter = trit % LETTER_COUNT;        /* 0..25 */
    out->slope  = 0;                          /* caller sets */
}

static inline uint8_t trit_compose(const TritAddr *t)
{
    return t->trit;
}

/* ── slope fingerprint ── */
static inline uint64_t trit_slope(uint64_t fibo_seed, uint32_t addr)
{
    return fibo_seed ^ (uint64_t)addr;
}

#endif /* FRUSTUM_TRIT_H */