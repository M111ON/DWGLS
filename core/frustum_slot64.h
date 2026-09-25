#ifndef FRUSTUM_SLOT64_H
#define FRUSTUM_SLOT64_H

#include <stdint.h>
#include "frustum_trit.h"

/* ── 64B DiamondBlock (FrustumSlot64) ── */
#define DIAMOND_BLOCK       64u
#define FRUSTUM_DATA_SZ     3456u   /* GEAR_MESH * DIAMOND_BLOCK = 54 * 64 */

typedef struct {
    uint32_t  core[LEVEL_COUNT];  /* 16B: merkle roots per level (0..3) */
    uint16_t  reserved_mask;      /*  2B: coset silence bitmap (9 bits used) */
    uint16_t  write_count;        /*  2B: writes into this slot */
    uint32_t  slope_lo;           /*  4B: last slope fingerprint (low 32 bits) */
    uint8_t   _pad[40];           /* 40B: reserved for future use */
} FrustumSlot64;                  /* exactly 64B = DIAMOND_BLOCK */

/* ── FrustumStore = 54 slots (GEAR_MESH) ── */
typedef struct {
    FrustumSlot64 slots[GEAR_MESH];  /* 54 × 64B = 3456B = FRUSTUM_DATA_SZ */
    uint32_t      total_writes;
    uint32_t      total_silenced;
} FrustumStore;

/* ── compile-time size assertions ── */
typedef char _frustum_slot64_sz[(sizeof(FrustumSlot64) == DIAMOND_BLOCK) ? 1 : -1];
typedef char _frustum_store_sz  [(sizeof(FrustumStore)  == FRUSTUM_DATA_SZ + 8) ? 1 : -1];

/* ── FrustumStore initialization ── */
static inline void frustum_store_init(FrustumStore *fs)
{
    for (uint8_t i = 0u; i < GEAR_MESH; i++) {
        for (uint8_t l = 0u; l < LEVEL_COUNT; l++) {
            fs->slots[i].core[l] = 0u;
        }
        fs->slots[i].reserved_mask = 0u;
        fs->slots[i].write_count   = 0u;
        fs->slots[i].slope_lo      = 0u;
    }
    fs->total_writes   = 0u;
    fs->total_silenced = 0u;
}

/* ── FrustumSlot64 write (merges core, tracks coset silence) ── */
static inline void frustum_slot_write(FrustumSlot64 *slot,
                                       uint8_t        level,
                                       uint32_t       core_val,
                                       uint8_t        coset)
{
    slot->core[level] = core_val;
    slot->write_count++;
    if (coset < COSET_COUNT) {
        slot->reserved_mask |= (uint16_t)(1u << coset);
    }
}

/* ── FrustumStore slot access by trit decomposition ── */
static inline FrustumSlot64 *frustum_store_slot(FrustumStore *fs, const TritAddr *t)
{
    uint8_t idx = (uint8_t)(t->coset * FACE_COUNT + t->face);
    return &fs->slots[idx];
}

/* ── FrustumStore total coset silence summary ── */
static inline void frustum_store_coset_summary(const FrustumStore *fs,
                                                uint8_t             coset_out[COSET_COUNT])
{
    for (uint8_t c = 0u; c < COSET_COUNT; c++) {
        coset_out[c] = 0u;
        for (uint8_t f = 0u; f < FACE_COUNT; f++) {
            uint8_t idx = (uint8_t)(c * FACE_COUNT + f);
            if ((fs->slots[idx].reserved_mask >> c) & 1u) {
                coset_out[c] |= (1u << f);
            }
        }
    }
}

/* ── FrustumStore merkle root (XOR of all core[0..3]) ── */
static inline uint32_t frustum_store_merkle(const FrustumStore *fs)
{
    uint32_t acc = 0u;
    for (uint8_t i = 0u; i < GEAR_MESH; i++) {
        for (uint8_t l = 0u; l < LEVEL_COUNT; l++) {
            acc ^= fs->slots[i].core[l];
        }
    }
    return acc;
}

#endif /* FRUSTUM_SLOT64_H */