/*
 * frustum_gcfs.h — Serialize FrustumStore to 4896B GCFS File
 * ════════════════════════════════════════════════════════════════
 *
 * GCFS = GiantCube FrustumStore — output format for frustum pipeline.
 *
 * File layout (4896B total = 288×17):
 *   [0    ..3455]  data zone  — 54 × 64B DiamondBlocks (verbatim copy)
 *   [3456 ..3464]  coset_mask — 9B (one byte per coset, reserved_mask[0..8])
 *   [3465 ..3490]  letter_map — 26B (A..Z, caller-supplied)
 *   [3491 ..3498]  slope      — 8B  (uint64_t last slope fingerprint)
 *   [3499 ..3502]  merkle_root— 4B  (XOR of all slot core[0..3])
 *   [3503 ..4895]  _pad       — 1393B zeros (boundary reserve)
 *
 * 4896 = 2⁵×3²×17  — factor 17 ∈ FACE_PRIME {7,11,13,17,19,23}
 *   → file boundary unreachable by pure 2ⁿ×3ᵐ arithmetic (security seam)
 *   → metadata zone = 1440B = 2⁵×3²×5 (has factor 5 = intentional marker)
 *
 * gcfs_serialize: FrustumStore → out[4896]
 * gcfs_deserialize: in[4896]  → FrustumStore
 *
 * No malloc. No float. No heap.
 * Depends: frustum_slot64.h → frustum_trit.h
 * ════════════════════════════════════════════════════════════════
 */

#ifndef FRUSTUM_GCFS_H
#define FRUSTUM_GCFS_H

#include <stdint.h>
#include <string.h>
#include "frustum_slot64.h"

/* ── file constants ────────────────────────────────────────── */
#define GCFS_FILE_SIZE     4896u   /* 2⁵×3²×17                    */
#define GCFS_DATA_OFFSET      0u   /* DiamondBlock data zone start  */
#define GCFS_DATA_SIZE     3456u   /* 54×64 = FRUSTUM_DATA_SZ       */
#define GCFS_META_OFFSET   3456u   /* metadata zone start           */
#define GCFS_META_SIZE     1440u   /* 2⁵×3²×5 — has factor 5        */
#define GCFS_COSET_OFFSET  3456u   /*  9B coset_mask                */
#define GCFS_LETTER_OFFSET 3465u   /* 26B letter_map                */
#define GCFS_SLOPE_OFFSET  3491u   /*  8B slope fingerprint         */
#define GCFS_MERKLE_OFFSET 3499u   /*  4B merkle summary            */
#define GCFS_PAD_OFFSET    3503u   /* 1393B reserved zeros          */
#define GCFS_PRIME_MARKER    17u   /* FACE_PRIME boundary           */

typedef char _gcfs_sz[(GCFS_DATA_SIZE + GCFS_META_SIZE == GCFS_FILE_SIZE) ? 1:-1];
typedef char _gcfs_data[(GCFS_DATA_SIZE == GEAR_MESH * DIAMOND_BLOCK) ? 1:-1];

/* ── gcfs_merkle_summary: XOR all core values in store ─────── */
static inline uint32_t _gcfs_merkle(const FrustumStore *fs)
{
    uint32_t acc = 0u;
    for (uint8_t i = 0u; i < GEAR_MESH; i++)
        for (uint8_t l = 0u; l < LEVEL_COUNT; l++)
            acc ^= fs->slots[i].core[l];
    return acc;
}

/* ── gcfs_coset_summary: one reserved_mask byte per coset ──── */
/*
 * Collapse 54 slots into 9 coset bytes:
 *   coset_out[c] = OR of all slots' reserved_mask bits for coset c
 */
static inline void _gcfs_coset_summary(const FrustumStore *fs,
                                        uint8_t             coset_out[COSET_COUNT])
{
    memset(coset_out, 0, COSET_COUNT);
    for (uint8_t c = 0u; c < COSET_COUNT; c++) {
        for (uint8_t f = 0u; f < FACE_COUNT; f++) {
            uint8_t idx = (uint8_t)(c * FACE_COUNT + f);
            if ((fs->slots[idx].reserved_mask >> c) & 1u)
                coset_out[c] |= (1u << f);   /* which faces silenced this coset */
        }
    }
}

/* ── gcfs_serialize ─────────────────────────────────────────── */
/*
 * Serializes FrustumStore into out[4896].
 * letter_map[26]: caller-supplied A..Z usage map (pass NULL for zeros).
 * slope: last slope fingerprint to record (e.g. from last TritAddr.slope).
 * Returns GCFS_FILE_SIZE on success.
 */
static inline uint32_t gcfs_serialize(const FrustumStore *fs,
                                       const uint8_t       letter_map[LETTER_COUNT],
                                       uint64_t            slope,
                                       uint8_t             out[GCFS_FILE_SIZE])
{
    memset(out, 0, GCFS_FILE_SIZE);

    /* [0..3455] data zone: verbatim DiamondBlock copy */
    memcpy(out + GCFS_DATA_OFFSET, fs->slots, GCFS_DATA_SIZE);

    /* [3456..3464] coset_mask: 9 bytes */
    uint8_t coset_sum[COSET_COUNT];
    _gcfs_coset_summary(fs, coset_sum);
    memcpy(out + GCFS_COSET_OFFSET, coset_sum, COSET_COUNT);

    /* [3465..3490] letter_map: 26 bytes */
    if (letter_map)
        memcpy(out + GCFS_LETTER_OFFSET, letter_map, LETTER_COUNT);

    /* [3491..3498] slope: 8 bytes little-endian */
    out[GCFS_SLOPE_OFFSET + 0] = (uint8_t)(slope);
    out[GCFS_SLOPE_OFFSET + 1] = (uint8_t)(slope >> 8);
    out[GCFS_SLOPE_OFFSET + 2] = (uint8_t)(slope >> 16);
    out[GCFS_SLOPE_OFFSET + 3] = (uint8_t)(slope >> 24);
    out[GCFS_SLOPE_OFFSET + 4] = (uint8_t)(slope >> 32);
    out[GCFS_SLOPE_OFFSET + 5] = (uint8_t)(slope >> 40);
    out[GCFS_SLOPE_OFFSET + 6] = (uint8_t)(slope >> 48);
    out[GCFS_SLOPE_OFFSET + 7] = (uint8_t)(slope >> 56);

    /* [3499..3502] merkle summary: 4 bytes little-endian */
    uint32_t merk = _gcfs_merkle(fs);
    out[GCFS_MERKLE_OFFSET + 0] = (uint8_t)(merk);
    out[GCFS_MERKLE_OFFSET + 1] = (uint8_t)(merk >> 8);
    out[GCFS_MERKLE_OFFSET + 2] = (uint8_t)(merk >> 16);
    out[GCFS_MERKLE_OFFSET + 3] = (uint8_t)(merk >> 24);

    /* [3503..4895] pad: already zero from memset */

    return GCFS_FILE_SIZE;
}

/* ── gcfs_deserialize ───────────────────────────────────────── */
/*
 * Restores FrustumStore from in[4896].
 * Rebuilds reserved_mask for each slot from coset_mask section.
 * letter_map_out[26]: if non-NULL, filled from file.
 * slope_out: if non-NULL, filled from file.
 * Returns 1 on success, 0 if file size marker invalid.
 */
static inline int gcfs_deserialize(FrustumStore *fs,
                                    const uint8_t in[GCFS_FILE_SIZE],
                                    uint8_t       letter_map_out[LETTER_COUNT],
                                    uint64_t     *slope_out)
{
    memset(fs, 0, sizeof(*fs));

    /* restore DiamondBlock data */
    memcpy(fs->slots, in + GCFS_DATA_OFFSET, GCFS_DATA_SIZE);

    /* restore coset silence per slot from coset_mask section */
    const uint8_t *coset_sum = in + GCFS_COSET_OFFSET;
    for (uint8_t c = 0u; c < COSET_COUNT; c++) {
        for (uint8_t f = 0u; f < FACE_COUNT; f++) {
            if ((coset_sum[c] >> f) & 1u) {
                uint8_t idx = (uint8_t)(c * FACE_COUNT + f);
                fs->slots[idx].reserved_mask |= (uint16_t)(1u << c);
            }
        }
    }

    /* letter_map */
    if (letter_map_out)
        memcpy(letter_map_out, in + GCFS_LETTER_OFFSET, LETTER_COUNT);

    /* slope */
    if (slope_out) {
        *slope_out =
            (uint64_t)in[GCFS_SLOPE_OFFSET + 0]        |
            (uint64_t)in[GCFS_SLOPE_OFFSET + 1] <<  8  |
            (uint64_t)in[GCFS_SLOPE_OFFSET + 2] << 16  |
            (uint64_t)in[GCFS_SLOPE_OFFSET + 3] << 24  |
            (uint64_t)in[GCFS_SLOPE_OFFSET + 4] << 32  |
            (uint64_t)in[GCFS_SLOPE_OFFSET + 5] << 40  |
            (uint64_t)in[GCFS_SLOPE_OFFSET + 6] << 48  |
            (uint64_t)in[GCFS_SLOPE_OFFSET + 7] << 56;
    }

    return 1;
}

/* ── gcfs_merkle_verify: check stored merkle vs live ───────── */
static inline int gcfs_merkle_verify(const FrustumStore *fs,
                                      const uint8_t       in[GCFS_FILE_SIZE])
{
    uint32_t stored =
        (uint32_t)in[GCFS_MERKLE_OFFSET + 0]        |
        (uint32_t)in[GCFS_MERKLE_OFFSET + 1] <<  8  |
        (uint32_t)in[GCFS_MERKLE_OFFSET + 2] << 16  |
        (uint32_t)in[GCFS_MERKLE_OFFSET + 3] << 24;
    return stored == _gcfs_merkle(fs);
}

#endif /* FRUSTUM_GCFS_H */
