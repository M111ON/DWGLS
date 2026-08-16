/*
 * geo_belt.h — the +37 belt as the SERIAL ORDER of the 20736-slot field
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * The "belt" is the stride-37 serial walk through the 144×144 torus:
 * gcd(37, 20736)=1 → one full cycle visits all 20736 slots exactly once,
 * so the belt is a collision-free permutation of the linear layout that
 * still closes back to its start (no origin, enter anywhere).
 *
 *   WIN         = 20736  (one belt cycle = 144² = GSW_FULL)
 *   BELT_STRIDE = 37     (coprime with 20736)
 *   37⁻¹ mod 20736 = 16813  (extended Euclid: 37·16813 ≡ 1)
 *
 * Used by: tools/gguf_graft_belt.c, tests/test_tess_belt.c,
 *          tests/test_tess_tensor_belt.c, tests/test_tess_full_cycle.c
 *          (single source of truth — was duplicated as #defines).
 *
 * BUILD: header-only, no .c; include "geo_belt.h" — needs <stdint.h>.
 */

#ifndef GEO_BELT_H
#define GEO_BELT_H

#include <stdint.h>

#define BELT_WIN         20736u   /* field window = one belt cycle (144²)   */
#define BELT_STRIDE      37u      /* the +37 walk — gcd(37,20736)=1         */
#define BELT_INV         16813u   /* 37⁻¹ mod 20736 (extended Euclid)      */

/* belt address — serial position k of a window, starting at slot `start`
 * (start=0 → the canonical layout; the walk is shift-invariant by design) */
static inline uint32_t belt_addr(uint32_t start, uint32_t k) {
    return (start + BELT_STRIDE * k) % BELT_WIN;
}

/* inverse mapping: given a belt address, return its serial position k.
 * belt_addr(start, k) = a  ⟺  (a − start)·37⁻¹ ≡ k (mod 20736). */
static inline uint32_t belt_serial_of(uint32_t start, uint32_t addr) {
    int32_t d = (int32_t)(addr) - (int32_t)(start);
    if (d < 0) d += (int32_t)BELT_WIN;
    /* BELT_INV = 16813; (d * BELT_INV) mod 20736 */
    return (uint32_t)(((uint64_t)(uint32_t)d * BELT_INV) % BELT_WIN);
}

/* return the rotation offset (in slots) when reading from `start_read`
 * a stream that was embedded at `start_write`.
 * delta = (start_read − start_write) · 37⁻¹ mod 20736. */
static inline uint32_t belt_rotation(uint32_t start_write, uint32_t start_read) {
    int32_t d = (int32_t)(start_read) - (int32_t)(start_write);
    if (d < 0) d += (int32_t)BELT_WIN;
    return (uint32_t)(((uint64_t)(uint32_t)d * BELT_INV) % BELT_WIN);
}

#endif /* GEO_BELT_H */
