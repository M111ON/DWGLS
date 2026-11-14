/* geo_shifts.h — Lossless transforms: Ladder / Orthogonal / Positional Add.
 *
 * All integer-only. Each transform is lossless roundtrip.
 * Self-contained: only <stdint.h>.
 */
#ifndef GEO_SHIFTS_H
#define GEO_SHIFTS_H

#include <stdint.h>

/* Ladder Shift: capacity expand factor (16->64->256). Value unchanged. */
#define LADDER_FACTOR 4u

static inline void ladder_expand(const int32_t *in, int32_t *out, uint32_t n) {
    /* out has LADDER_FACTOR*n slots; first n copied, rest zero */
    for (uint32_t i = 0; i < n * LADDER_FACTOR; i++) out[i] = 0;
    for (uint32_t i = 0; i < n; i++) out[i] = in[i];
}

static inline void ladder_contract(const int32_t *in, int32_t *out, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) out[i] = in[i];
}

/* Orthogonal Shift: A = v + c, B = v - c. Lossless if v,c same parity. */
static inline void ortho_split(const int32_t *v, const int32_t *c, int32_t *A, int32_t *B, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) { A[i] = v[i] + c[i]; B[i] = v[i] - c[i]; }
}

static inline void ortho_merge(const int32_t *A, const int32_t *B, int32_t *v, int32_t *c, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) { v[i] = (A[i] + B[i]) / 2; c[i] = (A[i] - B[i]) / 2; }
}

/* Positional Add: A[n] = v[n] + (n + k). Lossless: v[n] = A[n] - (n + k). */
static inline void posadd_encode(const int32_t *v, int32_t *A, uint32_t n, int32_t k) {
    for (uint32_t i = 0; i < n; i++) A[i] = v[i] + (int32_t)(i + (uint32_t)k);
}

static inline void posadd_decode(const int32_t *A, int32_t *v, uint32_t n, int32_t k) {
    for (uint32_t i = 0; i < n; i++) v[i] = A[i] - (int32_t)(i + (uint32_t)k);
}

#endif /* GEO_SHIFTS_H */
