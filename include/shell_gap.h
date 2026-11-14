/* shell_gap.h — gap/stride ledger for two sides that share a unit.
 *
 * Origin: the early ShellN file (pogls_shell.h) proposed
 *     size_162 = 162n,  size_128 = 128n,  delta = 34(n+1),  gate = delta % 18 == 0
 * That does not close. 162n - 128n = 34n, not 34(n+1), and n cancels out of both
 * the ratio (81/64) and the gap ratio (17/81) — so NO choice of n can align the
 * two sides. The n+1 was masking it: at n=8 the real delta is 272 and 272 % 18 = 2,
 * so the advertised "n=8 anchor" fails. Honest gate is n = 0 mod 9 (n=9, delta 306).
 *
 * Kept: the gap is a first-class checked object, not something to eliminate.
 * Fixed: the gap is gcd(side_a, side_b), and alignment comes from the tiling
 * PERIOD, not from the scale.
 *
 *     20736 = 2^8 * 3^4 = 128 * 162     field
 *     896   = 2^7 * 7   = 128 * 7       tensor row
 *     gcd   = 128 = 2^7                 -> 7 bits of frustum cost
 *     7 is a unit on the field          -> free rotation, costs nothing
 *     162 rows * 896 = 7 * 20736        -> period 162 rows, 7 pages
 *
 * Cost model, identical to the stride codec (verified on real Qwen2.5 Q8_0 bytes):
 *     cost(k, N) = log2(gcd(k, N)) bits per element;  free iff gcd(k, N) == 1
 * A stride that is a unit is a permutation: zero side information, exact inverse.
 * A stride with a 2 or 3 in it is a frustum: it collapses and must carry
 * exactly log2(gcd) bits, which is the same residual the fold's gate holds.
 */
#ifndef SHELL_GAP_H
#define SHELL_GAP_H

#include <stdint.h>

#define SG_LOG2_3 1.5849625007211562  /* log2(3) */

static inline uint64_t sg_gcd(uint64_t a, uint64_t b) {
    while (b) { uint64_t t = a % b; a = b; b = t; }
    return a;
}

/* The shared unit both sides can be measured in (bytes). */
static inline uint64_t sg_unit(uint64_t side_a, uint64_t side_b) {
    return sg_gcd(side_a, side_b);
}

/* Bits per element that must be carried because the two sides disagree.
 * Only gcd = 2^j * 3^l is representable on a 2^8*3^4 field; -1 otherwise. */
static inline double sg_gap_bits(uint64_t g) {
    int j = 0, l = 0;
    while (g % 2u == 0u) { g /= 2u; j++; }
    while (g % 3u == 0u) { g /= 3u; l++; }
    if (g != 1u) return -1.0;
    return (double)j + SG_LOG2_3 * (double)l;
}

/* Rows of row_bytes needed before the ring closes, and how many field pages
 * that many rows cover. period = field/gcd, pages = row/gcd. */
static inline uint64_t sg_period(uint64_t row_bytes, uint64_t field_bytes) {
    return field_bytes / sg_gcd(row_bytes, field_bytes);
}
static inline uint64_t sg_pages(uint64_t row_bytes, uint64_t field_bytes) {
    return row_bytes / sg_gcd(row_bytes, field_bytes);
}

/* First block (in units of gcd bytes) touched by row r, on a ring of
 * field/gcd blocks. Row r occupies blocks (R*r) .. (R*r + R - 1) mod F.
 * overflow: F*F must fit in uint64 — fine for any field below 2^32 bytes. */
static inline uint64_t sg_row_first_block(uint64_t r, uint64_t row_bytes,
                                         uint64_t field_bytes) {
    uint64_t g = sg_gcd(row_bytes, field_bytes);
    uint64_t R = (row_bytes / g) % (field_bytes / g);
    return (R * (r % (field_bytes / g))) % (field_bytes / g);
}

/* Modular inverse; -1 when it does not exist (gcd != 1). */
static inline int64_t sg_inv_mod(int64_t a, int64_t n) {
    int64_t t = 0, newt = 1, r = n, newr = a % n;
    if (newr < 0) newr += n;
    while (newr != 0) {
        int64_t q = r / newr, tmp;
        tmp = t - q * newt; t = newt; newt = tmp;
        tmp = r - q * newr; r = newr; newr = tmp;
    }
    if (r != 1) return -1;
    if (t < 0) t += n;
    return t;
}

static inline uint32_t sg_stride_enc(uint32_t x, uint32_t k, uint32_t n) {
    return (uint32_t)(((uint64_t)x * (uint64_t)k) % n);
}
static inline uint32_t sg_stride_dec(uint32_t y, uint32_t k, uint32_t n) {
    int64_t i = sg_inv_mod((int64_t)k, (int64_t)n);
    return i < 0 ? y : (uint32_t)(((uint64_t)y * (uint64_t)i) % n);
}
static inline int sg_stride_free(uint32_t k, uint32_t n) {
    return sg_gcd(k, n) == 1u;
}

#endif /* SHELL_GAP_H */
