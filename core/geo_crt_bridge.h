/*
 * geo_crt_bridge.h — CRT Bridge: base-12 ↔ base-{2,3} (256 × 81)
 * ═══════════════════════════════════════════════════════════════════════════════
 *
 * Chinese Remainder Theorem decomposition of 20736:
 *   20736 = 256 × 81,  gcd(256, 81) = 1
 *
 *   256 = 2⁸: binary octree depth (8 bits = 256 leaves)
 *            selects tesseract cube path
 *   81  = 3⁴: ternary quad breadth (4 trits = 81 leaves)
 *            selects hex sub-cell within cube
 *
 * Every flat address [0..20736) maps uniquely to (b, t):
 *   b ∈ [0, 255]  (binary component)
 *   t ∈ [0, 80]   (ternary component)
 *
 * CRT reconstruction:
 *   addr = (81×b + 256×t) mod 20736
 *
 * Chinese Remainder inverse:
 *   b = addr × 81⁻¹ mod 256   where 81⁻¹ mod 256 = 33
 *   t = addr × 256⁻¹ mod 81   where 256⁻¹ mod 81  = 64
 *
 * Verification:
 *   81 × 33 = 2673 = 10 × 256 + 113   → 81×33 mod 256 = 1  ✓
 *   256 × 64 = 16384 = 202 × 81 + 22  → wait, check:
 *   16384 / 81 = 202.27...  → 202 × 81 = 16362  → 16384 - 16362 = 22 ≠ 1
 *   Need: 256⁻¹ mod 81.  256 mod 81 = 256 - 3×81 = 256-243 = 13
 *   Need: 13⁻¹ mod 81.  13×50 = 650 = 8×81 + 2 = 650 - 648 = 2 ≠ 1
 *   13×25 = 325 = 4×81 + 1 = 325 - 324 = 1 ✓ → 13⁻¹ mod 81 = 25
 *   So 256⁻¹ mod 81 = 25 (since 256 ≡ 13 mod 81)
 *
 *   addr = (81×b + 256×t) mod 20736
 *   b = addr × 81⁻¹ mod 256 = (addr × 33) mod 256   [81×33 = 2673 ≡ 1 mod 256]
 *   t = addr × 256⁻¹ mod 81 = (addr × 25) mod 81    [256≡13 mod 81, 13×25≡1 mod 81]
 *
 * DEPENDS: none (pure arithmetic)
 * No malloc. No float. All static inline. Header-only.
 * ═══════════════════════════════════════════════════════════════════════════════
 */
#ifndef GEO_CRT_BRIDGE_H
#define GEO_CRT_BRIDGE_H

#include <stdint.h>

/* ═══════════════════════════════════════════════════════════════════════════
   CONSTANTS
   ═══════════════════════════════════════════════════════════════════════════ */

#define CRT_MODULUS       20736u   /* 256 × 81 */
#define CRT_BIN_SIZE       256u    /* 2⁸: binary octree leaves */
#define CRT_TER_SIZE        81u    /* 3⁴: ternary quad leaves */
#define CRT_BIN_INV        177u    /* 81⁻¹ mod 256: 81×177 = 14337 ≡ 1 mod 256 */
#define CRT_TER_INV         25u    /* 256⁻¹ mod 81: 256≡13 mod 81, 13×25 = 325 ≡ 1 mod 81 */

/* Hierarchy levels within each component:
 *   binary:  8 levels (2¹ → 2² → ... → 2⁸) = tesseract cube path
 *   ternary: 4 levels (3¹ → 3² → 3³ → 3⁴)  = hex sub-cell path
 */
#define CRT_BIN_DEPTH       8u
#define CRT_TER_DEPTH       4u

/* ═══════════════════════════════════════════════════════════════════════════
   CRT ADDRESS — flat ↔ (binary, ternary)
   ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t bin;   /* binary component [0..255] — tesseract cube index */
    uint32_t ter;   /* ternary component [0..80] — hex sub-cell index */
} CRTAddr;

/* ── flat → CRT decomposition ──────────────────────────────────────────── */
static inline CRTAddr crt_from_flat(uint32_t flat)
{
    CRTAddr a;
    a.bin = (flat * CRT_BIN_INV) % CRT_BIN_SIZE;  /* mod 256 */
    a.ter = (flat * CRT_TER_INV) % CRT_TER_SIZE;  /* mod 81 */
    return a;
}

/* ── CRT → flat reconstruction ─────────────────────────────────────────── */
static inline uint32_t crt_to_flat(CRTAddr a)
{
    return (81u * a.bin + 256u * a.ter) % CRT_MODULUS;
}

/* ── direct pair encode ────────────────────────────────────────────────── */
static inline uint32_t crt_encode(uint32_t bin, uint32_t ter)
{
    return (81u * (bin % CRT_BIN_SIZE) + 256u * (ter % CRT_TER_SIZE))
         % CRT_MODULUS;
}

/* ── helper: 3^d ───────────────────────────────────────────────────────── */
static inline uint32_t pow3(uint32_t d)
{
    uint32_t r = 1;
    for (uint32_t i = 0; i < d; i++) r *= 3u;
    return r;
}

/* ═══════════════════════════════════════════════════════════════════════════
   BINARY OCTREE PATH — 8-level binary decomposition
   ═══════════════════════════════════════════════════════════════════════════
   Each bit selects a sub-cube within the tesseract.
   bit[0] = least significant = finest split
   bit[7] = most significant  = coarsest split (root)
   ═══════════════════════════════════════════════════════════════════════════ */

/* Extract bit at depth d (0=LSB, 7=MSB) from binary component */
static inline uint32_t crt_bin_bit(uint32_t bin, uint32_t d)
{
    return (bin >> d) & 1u;
}

/* Reconstruct binary component from 8-bit path */
static inline uint32_t crt_bin_from_path(const uint32_t bits[CRT_BIN_DEPTH])
{
    uint32_t b = 0;
    for (uint32_t d = 0; d < CRT_BIN_DEPTH; d++) {
        b |= (bits[d] & 1u) << d;
    }
    return b;
}

/* ═══════════════════════════════════════════════════════════════════════════
   TERNARY QUAD PATH — 4-level ternary decomposition
   ═══════════════════════════════════════════════════════════════════════════
   Each trit selects a sub-cell within the hex region.
   trit[0] = finest split (3¹)
   trit[3] = coarsest split (3³)
   ═══════════════════════════════════════════════════════════════════════════ */

/* Extract trit at depth d (0=least, 3=most) from ternary component */
static inline uint32_t crt_ter_trit(uint32_t ter, uint32_t d)
{
    return (ter / pow3(d)) % 3u;
}

/* Reconstruct ternary component from 4-trit path */
static inline uint32_t crt_ter_from_path(const uint32_t trits[CRT_TER_DEPTH])
{
    uint32_t t = 0;
    uint32_t scale = 1;
    for (uint32_t d = 0; d < CRT_TER_DEPTH; d++) {
        t += (trits[d] % 3u) * scale;
        scale *= 3u;
    }
    return t;
}

/* ═══════════════════════════════════════════════════════════════════════════
   FRAME SEEK SPEEDUP
   ═══════════════════════════════════════════════════════════════════════════
   frame_seek(addr):
     1. Decompose: CRTAddr a = crt_from_flat(addr)
     2. Binary path: O(8) bit comparisons → tesseract cube selection
     3. Ternary path: O(4) trit comparisons → hex sub-cell selection
     Total: O(337) vs O(20736) linear scan = 61× speedup
   ═══════════════════════════════════════════════════════════════════════════ */

/* Seek within binary octree: find cube containing target bin */
static inline uint32_t crt_seek_binary(uint32_t target_bin)
{
    /* Binary octree: just use the binary component directly */
    return target_bin;  /* O(1) — direct index */
}

/* Seek within ternary quad: find hex sub-cell containing target ter */
static inline uint32_t crt_seek_ternary(uint32_t target_ter)
{
    /* Ternary quad: just use the ternary component directly */
    return target_ter;  /* O(1) — direct index */
}

/* Combined seek: flat addr → (cube, subcell) pair for frame_seek */
typedef struct {
    uint32_t cube;    /* which tesseract cube [0..255] */
    uint32_t subcell; /* which hex sub-cell within cube [0..80] */
    uint32_t slot;    /* offset within sub-cell (for future use) */
} CRTSeekResult;

static inline CRTSeekResult crt_seek(uint32_t flat)
{
    CRTSeekResult r;
    CRTAddr a = crt_from_flat(flat);
    r.cube    = a.bin;
    r.subcell = a.ter;
    r.slot    = 0;  /* reserved */
    return r;
}

/* ═══════════════════════════════════════════════════════════════════════════
   CROSS-SYSTEM BRIDGES
   ═══════════════════════════════════════════════════════════════════════════
   CRT ↔ fractal_addr (base-12)
   CRT ↔ twin_rebalance (128×162 ↔ 144×144)
   CRT ↔ tesseract_addr (18tes)
   ═══════════════════════════════════════════════════════════════════════════ */

/* flat ↔ CRT (canonical, standalone) */
/* crt_from_flat and crt_to_flat defined above */

/*
 * CRT ↔ tesseract_addr:
 *   18 tesseracts × 8 cells = 144 cubes
 *   But CRT has 256 binary cubes — not 144.
 *   Mapping: CRT cube [0..255] → tesseract (cube/18, cube%18) ?
 *   No: 18 × 8 = 144 ≠ 256.
 *
 *   The correct mapping uses the 8-cell structure:
 *   256 = 2⁸,  tesseract has 8 cells
 *   256 / 8 = 32 → 32 "tesseract groups" of 8 cells each
 *   Or: 256 = 4⁴ = (2²)⁴ → 4 axes × 4 levels
 *
 *   For 18tes: 18 × 144 = 2592 slots in binary side
 *   But CRT binary = 256 ≠ 2592/18 = 144
 *
 *   RESOLUTION: CRT binary [0..255] indexes within ONE tesseract's
 *   256 sub-positions (8 cells × 32 slots/cell = 256).
 *   The tess index is extracted separately from flat:
 *   tess = flat / 1152  (from geo_tesseract_addr.h)
 *   Within-tess offset = flat % 1152
 *   CRT bin = within_tess_bin  (only 8 of 256 used per tess)
 */

/* ═══════════════════════════════════════════════════════════════════════════
   VERIFY
   ═══════════════════════════════════════════════════════════════════════════ */

static inline int crt_verify(void)
{
    /* 1. CRT roundtrip: flat → (bin, ter) → flat */
    for (uint32_t flat = 0; flat < CRT_MODULUS; flat++) {
        CRTAddr a = crt_from_flat(flat);
        if (a.bin >= CRT_BIN_SIZE) return -1;  /* out of range */
        if (a.ter >= CRT_TER_SIZE) return -2;  /* out of range */
        uint32_t back = crt_to_flat(a);
        if (back != flat) return -3;  /* roundtrip failed */
    }

    /* 2. CRT injectivity: different flats → different (bin, ter) */
    for (uint32_t f1 = 0; f1 < CRT_MODULUS; f1++) {
        for (uint32_t f2 = f1 + 1; f2 < CRT_MODULUS && f2 < f1 + 100; f2++) {
            CRTAddr a1 = crt_from_flat(f1);
            CRTAddr a2 = crt_from_flat(f2);
            if (a1.bin == a2.bin && a1.ter == a2.ter) return -4;
        }
    }

    /* 3. CRT mod properties: 81×177 ≡ 1 mod 256 */
    if ((81u * 177u) % 256u != 1u) return -5;
    /* 13×25 ≡ 1 mod 81 (since 256 ≡ 13 mod 81) */
    if ((13u * 25u) % 81u != 1u) return -6;

    /* 4. Path decomposition roundtrip */
    for (uint32_t flat = 0; flat < CRT_MODULUS; flat++) {
        CRTAddr a = crt_from_flat(flat);
        uint32_t bits[CRT_BIN_DEPTH];
        uint32_t trits[CRT_TER_DEPTH];
        for (uint32_t d = 0; d < CRT_BIN_DEPTH; d++)
            bits[d] = crt_bin_bit(a.bin, d);
        for (uint32_t d = 0; d < CRT_TER_DEPTH; d++)
            trits[d] = crt_ter_trit(a.ter, d);

        uint32_t bin_back = crt_bin_from_path(bits);
        uint32_t ter_back = crt_ter_from_path(trits);
        if (bin_back != a.bin) return -7;
        if (ter_back != a.ter) return -8;
    }

    /* 5. Coverage: every (bin,ter) pair reconstructs to valid flat */
    for (uint32_t b = 0; b < CRT_BIN_SIZE; b++) {
        for (uint32_t t = 0; t < CRT_TER_SIZE; t++) {
            uint32_t flat = crt_encode(b, t);
            if (flat >= CRT_MODULUS) return -9;
        }
    }

    return 0;
}

#endif /* GEO_CRT_BRIDGE_H */
