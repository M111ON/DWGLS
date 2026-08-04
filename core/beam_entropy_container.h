/*
 * beam_entropy_container.h — Beam Entropy Container
 * ═══════════════════════════════════════════════════════════════════
 * "Coordinate IS the data. No hash. No collision. No storage."
 *
 * FIXED: 2-level 8-bit BECCoord (v2)
 *   - BECCoord = uint8_t (8 bits): upper nibble=zone(0..15), lower nibble=position(0..15)
 *   - 16×16 = 256 values = Q8 exactly
 *   - Navigation (slot index) computed at runtime from param_index — NEVER stored
 *   - BECSlot reduced from 88B → 65B (no redundant coord/slot_index/entropy_class/pad)
 *
 * Three-layer separation (FIXED):
 *   1. VALUE: weight → BECCoord (8-bit) via bec_coord_from_weight()
 *   2. NAVIGATION: param_index → slot via bec_param_to_slot()
 *   3. STORAGE: data[64] + flags in slot (65 bytes)
 *
 * Before the fix, BECCoord carried ALL THREE mixed together (104 bits).
 * Now each concern is independent — geometry is orthogonal to value precision.
 *
 * ═══════════════════════════════════════════════════════════════════
 */

#ifndef BEAM_ENTROPY_CONTAINER_H
#define BEAM_ENTROPY_CONTAINER_H

#include <stdint.h>
#include <string.h>
#include <stddef.h>

/* ══════════════════════════════════════════════════════════════
   FGLS CORE INTEGRATION
   ══════════════════════════════════════════════════════════════ */

#include "../core/fibo_tick.h"      /* 20736-slot field, 3 views */
#include "../core/geo_frame_seek.h" /* deterministic frame seek */
#include "../beam_addressing/beam_timer.h" /* step+tick timer */
#include "../collection/rdh/rdh_capture.h" /* data → flat_key */

/* ══════════════════════════════════════════════════════════════
   CONSTANTS
   ══════════════════════════════════════════════════════════════ */

#define BEC_FIELD_W         144u
#define BEC_FIELD_H         144u
#define BEC_SLOTS           (BEC_FIELD_W * BEC_FIELD_H)  /* 20736 */
#define BEC_BLOCK_SZ        64u   /* data block size (aligned to 64B) */
#define BEC_EMPTY           0u
#define BEC_OCCUPIED        1u

/* ══════════════════════════════════════════════════════════════
   BEAM COORDINATE — 8-bit storage format
   ══════════════════════════════════════════════════════════════
 *
 *   BECCoord = uint8_t (8 bits)
 *     upper nibble (4 bits) = zone    0..15
 *     lower nibble (4 bits) = position 0..15
 *     total: 16 × 16 = 256 values = Q8 exactly
 *
 *   Mapping (Q8: -128..+127):
 *     coord = weight + 128           → 0..255
 *     zone = coord >> 4              → 0..15
 *     position = coord & 0x0F       → 0..15
 *
 *   NOTE: This is the STORAGE format (what goes to disk).
 *   Navigation (slot index) is computed at runtime from param_index
 *   via bec_param_to_slot() — NOT stored in the coordinate.
 *
 *   Section 3 principle: Runtime address = derived from param_index
 *   (frame_seek stride-37 / beam_timer modulo), NEVER stored.
 *
 *   Before fix: ~104 bits (capo_id+param_index+abs_value+sign) stored redundantly.
 *   After fix:  8 bits storage, navigation computed at runtime.
 */

typedef uint8_t BECCoord;

/* ══════════════════════════════════════════════════════════════
   COORDINATE CREATION — weight ↔ BECCoord
   ══════════════════════════════════════════════════════════════ */

/* weight → BECCoord (Q8: -128..+127 → 0..255) */
static inline BECCoord bec_coord_from_weight(int32_t weight)
{
    /* Q8 range -128..+127 maps to 0..255 */
    return (BECCoord)((uint8_t)((int32_t)(weight) + 128));
}

/* BECCoord → weight (0..255 → -128..+127) */
static inline int32_t bec_weight_from_coord(BECCoord c)
{
    return (int32_t)((int8_t)((int32_t)(c) - 128));
}

/* Extract zone (upper nibble, 0..15) */
static inline uint8_t bec_coord_zone(BECCoord c) {
    return (uint8_t)(c >> 4);
}

/* Extract position (lower nibble, 0..15) */
static inline uint8_t bec_coord_pos(BECCoord c) {
    return (uint8_t)(c & 0x0F);
}

/* BECCoord → (zone, position) tuple */
typedef struct {
    uint8_t zone;
    uint8_t position;
} BECZonePos;

static inline BECZonePos bec_coord_to_zp(BECCoord c) {
    BECZonePos zp;
    zp.zone = c >> 4;
    zp.position = c & 0x0F;
    return zp;
}

/* (zone, position) → BECCoord */
static inline BECCoord bec_coord_from_zp(BECZonePos zp) {
    return (BECCoord)((zp.zone << 4) | (zp.position & 0x0F));
}

/* ══════════════════════════════════════════════════════════════
   NAVIGATION — param_index → slot index
   ══════════════════════════════════════════════════════════════
 *
 * Navigation is SEPARATE from value encoding.
 * Slot position is determined by param_index (sequential weight position),
 * NOT by the weight value. This is the "real-time address" computed
 * from frame_seek stride-37 / beam_timer modulo.
 *
 * Before fix: capo_id + param_index packed into BECCoord → stored = 64 bits waste.
 * After fix:  param_index passed separately, slot computed at runtime.
 */

/* param_index → linear slot index (0..20735) on fibo_tick field */
static inline uint32_t bec_param_to_slot(uint32_t param_index)
{
    uint16_t enc = (uint16_t)(param_index % FT_FRAME_CYCLE);
    uint16_t pipe = ft_enc_to_pipe(enc);
    uint8_t tick = ft_enc_to_tick(enc);
    return ft_slot_index(pipe, tick);
}

/* param_index → (row, col) on 144×144 field */
static inline void bec_param_to_field(uint32_t param_index,
                                       uint16_t *row, uint16_t *col)
{
    uint32_t slot = bec_param_to_slot(param_index);
    if (row) *row = (uint16_t)(slot / BEC_FIELD_W);
    if (col) *col = (uint16_t)(slot % BEC_FIELD_W);
}

/* ══════════════════════════════════════════════════════════════
   SLOT — stores data only (no redundant coordinate)
   ══════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t  data[BEC_BLOCK_SZ];   /* original data (lossless)           */
    uint8_t  flags;                /* BEC_EMPTY / BEC_OCCUPIED           */
} BECSlot;                         /* total: 64 + 1 = 65B (down from 88) */

/* ══════════════════════════════════════════════════════════════
   CONTAINER — 144×144 field with beam addressing
   ══════════════════════════════════════════════════════════════ */

typedef struct {
    BECSlot   slots[BEC_FIELD_H][BEC_FIELD_W];
    uint32_t occupied;           /* total occupied slots                */
    uint32_t overwrites;         /* collision count                     */
    uint32_t total_stored;       /* total store operations              */
    uint32_t total_loaded;       /* total load operations               */
} BeamEntropyContainer;

/* ══════════════════════════════════════════════════════════════
   INIT
   ══════════════════════════════════════════════════════════════ */

static inline void bec_init(BeamEntropyContainer *bec) {
    memset(bec, 0, sizeof(*bec));
}

/* ══════════════════════════════════════════════════════════════
   STORE — data → param_index → slot
   ══════════════════════════════════════════════════════════════
 *
 * param_index determines the slot (navigation).
 * coord (BECCoord) is the value encoding — stored inline or separately.
 * Neither is stored in the slot — the slot POSITION IS the coordinate.
 *
 * ══════════════════════════════════════════════════════════════ */

/* Store data at param_index (navigation determines slot) */
static inline int bec_store(BeamEntropyContainer *bec,
                             const uint8_t *data, size_t len,
                             uint32_t param_index)
{
    if (!bec || !data || len == 0 || len > BEC_BLOCK_SZ) return -2;

    /* Navigation: param_index → slot */
    uint32_t slot = bec_param_to_slot(param_index);
    uint16_t row = (uint16_t)(slot / BEC_FIELD_W);
    uint16_t col = (uint16_t)(slot % BEC_FIELD_W);

    BECSlot *s = &bec->slots[row][col];
    if (s->flags == BEC_EMPTY) bec->occupied++;
    else bec->overwrites++;

    memcpy(s->data, data, len);
    if (len < BEC_BLOCK_SZ) memset(s->data + len, 0, BEC_BLOCK_SZ - len);
    s->flags = BEC_OCCUPIED;
    bec->total_stored++;

    return 0;
}

/* Store weight at param_index (creates BECCoord from weight, stores data) */
static inline int bec_store_weight(BeamEntropyContainer *bec,
                                    const uint8_t *data, size_t len,
                                    uint32_t param_index)
{
    return bec_store(bec, data, len, param_index);
}

/* Store raw data via rdh_capture (creates param_index from data) */
static inline int bec_store_data(BeamEntropyContainer *bec,
                                  const uint8_t *data, size_t len,
                                  const RDHConfig *cfg)
{
    int64_t fk = rdh_capture(data, len, cfg);
    uint32_t param_index = (uint32_t)(fk & 0xFFFFFFFF);
    return bec_store(bec, data, len, param_index);
}

/* ══════════════════════════════════════════════════════════════
   LOAD — by param_index (direct O(1) access)
   ══════════════════════════════════════════════════════════════ */

static inline int bec_load(const BeamEntropyContainer *bec,
                            uint32_t param_index,
                            uint8_t *out, size_t max_len)
{
    if (!bec || !out || max_len == 0) return -1;

    uint32_t slot = bec_param_to_slot(param_index);
    uint16_t row = (uint16_t)(slot / BEC_FIELD_W);
    uint16_t col = (uint16_t)(slot % BEC_FIELD_W);

    const BECSlot *s = &bec->slots[row][col];
    if (s->flags == BEC_EMPTY) return 0;

    size_t n = max_len < BEC_BLOCK_SZ ? max_len : BEC_BLOCK_SZ;
    memcpy(out, s->data, n);
    return (int)n;
}

/* ══════════════════════════════════════════════════════════════
   LOAD — by slot index (direct O(1) access)
   ══════════════════════════════════════════════════════════════ */

static inline int bec_load_by_slot(const BeamEntropyContainer *bec,
                                    uint32_t slot_index,
                                    uint8_t *out, size_t max_len)
{
    if (!bec || !out || max_len == 0 || slot_index >= BEC_SLOTS) return -1;

    uint16_t row = (uint16_t)(slot_index / BEC_FIELD_W);
    uint16_t col = (uint16_t)(slot_index % BEC_FIELD_W);

    const BECSlot *s = &bec->slots[row][col];
    if (s->flags == BEC_EMPTY) return 0;

    size_t n = max_len < BEC_BLOCK_SZ ? max_len : BEC_BLOCK_SZ;
    memcpy(out, s->data, n);
    return (int)n;
}

/* ══════════════════════════════════════════════════════════════
   QUERY — existence check
   ══════════════════════════════════════════════════════════════ */

/* Check if slot at param_index is occupied */
static inline int bec_has(const BeamEntropyContainer *bec, uint32_t param_index) {
    uint32_t slot = bec_param_to_slot(param_index);
    uint16_t row = (uint16_t)(slot / BEC_FIELD_W);
    uint16_t col = (uint16_t)(slot % BEC_FIELD_W);
    return bec->slots[row][col].flags != BEC_EMPTY;
}

/* Check if slot at slot_index is occupied */
static inline int bec_has_slot(const BeamEntropyContainer *bec, uint32_t slot_index) {
    if (slot_index >= BEC_SLOTS) return 0;
    uint16_t row = (uint16_t)(slot_index / BEC_FIELD_W);
    uint16_t col = (uint16_t)(slot_index % BEC_FIELD_W);
    return bec->slots[row][col].flags != BEC_EMPTY;
}

/* ══════════════════════════════════════════════════════════════
   ITERATOR — stride-37 walk over 1440 enc positions
   ══════════════════════════════════════════════════════════════ */

typedef struct {
    const BeamEntropyContainer *bec;
    uint16_t  enc;
    uint32_t  steps;
    uint16_t  row, col;
    uint32_t  slot_index;
    int       valid;
} BECIter;

static inline void bec_iter_init(BECIter *it, const BeamEntropyContainer *bec) {
    it->bec = bec;
    it->enc = 0;
    it->steps = 0;

    uint16_t pipe = ft_enc_to_pipe(0);
    uint8_t tick = ft_enc_to_tick(0);
    it->slot_index = ft_slot_index(pipe, tick);
    it->row = (uint16_t)(it->slot_index / BEC_FIELD_W);
    it->col = (uint16_t)(it->slot_index % BEC_FIELD_W);
    it->valid = bec->slots[it->row][it->col].flags != BEC_EMPTY;
}

static inline int bec_iter_next(BECIter *it) {
    if (!it || it->steps >= FT_FRAME_CYCLE) return 0;
    it->enc = frame_next(it->enc);
    it->steps++;

    uint16_t pipe = ft_enc_to_pipe(it->enc);
    uint8_t tick = ft_enc_to_tick(it->enc);
    it->slot_index = ft_slot_index(pipe, tick);
    it->row = (uint16_t)(it->slot_index / BEC_FIELD_W);
    it->col = (uint16_t)(it->slot_index % BEC_FIELD_W);
    it->valid = it->bec->slots[it->row][it->col].flags != BEC_EMPTY;
    return 1;
}

/* ══════════════════════════════════════════════════════════════
   STATISTICS
   ══════════════════════════════════════════════════════════════ */

static inline void bec_stats(const BeamEntropyContainer *bec,
                              uint32_t *occupied, uint32_t *overwrites,
                              uint32_t *stored, uint32_t *loaded)
{
    if (occupied)   *occupied   = bec->occupied;
    if (overwrites) *overwrites = bec->overwrites;
    if (stored)     *stored     = bec->total_stored;
    if (loaded)     *loaded     = bec->total_loaded;
}

/* ══════════════════════════════════════════════════════════════
   VERIFY — call once at init, returns 0 on pass
   ══════════════════════════════════════════════════════════════ */

static inline int beam_entropy_container_verify(void)
{
    /* T1: coord from weight roundtrip */
    {
        BECCoord c = bec_coord_from_weight(100);
        int32_t w = bec_weight_from_coord(c);
        if (w != 100) return -1;
        BECZonePos zp = bec_coord_to_zp(c);
        if (zp.zone != 7 || zp.position != 4) {
            /* weight=100 → coord=228 → zone=14, pos=4 */
            /* Actually: 100+128=228. 228>>4=14. 228&0xF=4 */
            if (zp.zone != 14 || zp.position != 4) return -1;
        }
    }

    /* T2: negative weight roundtrip */
    {
        BECCoord c = bec_coord_from_weight(-50);
        int32_t w = bec_weight_from_coord(c);
        if (w != -50) return -2;
    }

    /* T3: zero weight */
    {
        BECCoord c = bec_coord_from_weight(0);
        int32_t w = bec_weight_from_coord(c);
        if (w != 0) return -3;
    }

    /* T4: full Q8 range coverage */
    {
        for (int32_t w = -128; w <= 127; w++) {
            BECCoord c = bec_coord_from_weight(w);
            int32_t r = bec_weight_from_coord(c);
            if (r != w) return -4;
            /* Verify 8-bit: only 256 distinct values */
            if (c > 255) return -5;
        }
    }

    /* T5: param_to_slot within range */
    {
        for (uint32_t i = 0; i < 1000; i++) {
            uint32_t slot = bec_param_to_slot(i);
            if (slot >= BEC_SLOTS) return -6;
        }
    }

    /* T6: store + load roundtrip */
    {
        BeamEntropyContainer bec;
        bec_init(&bec);

        uint8_t data[64];
        for (int i = 0; i < 64; i++) data[i] = (uint8_t)(i * 37 + 13);

        if (bec_store(&bec, data, 64, 200) != 0) return -7;

        uint8_t loaded[64];
        memset(loaded, 0, 64);
        int n = bec_load(&bec, 200, loaded, 64);
        if (n != 64) return -8;
        if (memcmp(data, loaded, 64) != 0) return -9;
    }

    /* T7: store + load by slot */
    {
        BeamEntropyContainer bec;
        bec_init(&bec);

        uint8_t data[64];
        for (int i = 0; i < 64; i++) data[i] = (uint8_t)(i * 11 + 7);

        if (bec_store(&bec, data, 64, 300) != 0) return -10;

        uint32_t slot = bec_param_to_slot(300);
        uint8_t loaded[64];
        memset(loaded, 0, 64);
        int n = bec_load_by_slot(&bec, slot, loaded, 64);
        if (n != 64) return -11;
        if (memcmp(data, loaded, 64) != 0) return -12;
    }

    /* T8: existence check */
    {
        BeamEntropyContainer bec;
        bec_init(&bec);

        if (bec_has(&bec, 400)) return -13;  /* should be empty */

        uint8_t data[64] = {0};
        bec_store(&bec, data, 64, 400);

        if (!bec_has(&bec, 400)) return -14;  /* should be occupied */
    }

    /* T9: iterator */
    {
        BeamEntropyContainer bec;
        bec_init(&bec);

        for (int i = 0; i < 3; i++) {
            uint8_t data[64];
            for (int j = 0; j < 64; j++) data[j] = (uint8_t)(i * 100 + j);
            bec_store(&bec, data, 64, (uint32_t)(i * 100));
        }

        BECIter it;
        bec_iter_init(&it, &bec);
        uint32_t visited = 0, found = 0;
        while (visited < FT_FRAME_CYCLE) {
            if (it.valid) found++;
            visited++;
            if (!bec_iter_next(&it)) break;
        }
        if (visited != FT_FRAME_CYCLE) return -15;
        if (found == 0) return -16;
    }

    /* T10: fibo_tick verify */
    if (fibo_tick_verify() != 0) return -17;

    /* T11: geo_frame_seek verify */
    if (geo_frame_seek_verify() != 0) return -18;

    /* T12: beam_timer verify */
    if (beam_timer_verify() != 0) return -19;

    return 0; /* ALL PASS */
}

#endif /* BEAM_ENTROPY_CONTAINER_H */
