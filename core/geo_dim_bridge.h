/* geo_dim_bridge.h — Variable<->Value swap (Dimensional Bridge).
 *
 * Cross-dimensional routing: the same field element can be read as a
 * Variable (coordinate/position) or as a Value (letter/payload). The swap
 * is a bijective permutation of the field (20736), so it is lossless and
 * invertible: swap_val_to_var(swap_var_to_val(x)) == x for all x.
 *
 * Self-contained: only <stdint.h>.
 */
#ifndef GEO_DIM_BRIDGE_H
#define GEO_DIM_BRIDGE_H

#include <stdint.h>

#define DIM_FIELD 20736u

/* multiplier coprime to DIM_FIELD (gcd(37,20736)=1) -> bijection */
#define DIM_A 37u

/* modular inverse of 37 mod 20736: 37*? = 1 mod 20736.
 * 20736 = 37*560 + 16; ... extended euclid -> inv = 16813 (check: 37*16813=622081; 622081 mod 20736 = 1) */
#define DIM_AINV 16813u
#define DIM_B 12345u

static inline uint32_t dim_swap_var_to_val(uint32_t var) {
    return (var * DIM_A + DIM_B) % DIM_FIELD;
}

static inline uint32_t dim_swap_val_to_var(uint32_t val) {
    /* inverse: (val - B) * Ainv mod F */
    uint32_t v = (val >= DIM_B) ? (val - DIM_B) : (val + DIM_FIELD - DIM_B);
    return (v * DIM_AINV) % DIM_FIELD;
}

static inline int dim_bridge_roundtrip_ok(uint32_t x) {
    return dim_swap_val_to_var(dim_swap_var_to_val(x)) == x;
}

#endif /* GEO_DIM_BRIDGE_H */
