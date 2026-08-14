/*
 * beam_value.c — Beam Addressing + FGLS Core Integration
 * ═══════════════════════════════════════════════════════════════════
 *
 * "Coordinate IS the data. No hash. No storage. No collision."
 *
 * FIXED (v2): Two-layer separation
 *   1. RUNTIME (BeamCoord) — transient for computation, carries capo_id+param_index
 *      for slot navigation. Created on the fly, NEVER stored.
 *   2. STORAGE  (BeamCode)  — 8-bit value encoding (uint8_t)
 *      upper nibble=zone(0..15), lower nibble=position(0..15)
 *      16×16 = 256 values = Q8 exactly
 *
 * Before fix: BeamCoord mixed navigation + value and was stored (104 bits).
 * After fix:  Navigation computed at runtime from param_index.
 *             Storage = 8-bit BeamCode per weight = same as Q8_0 byte overhead.
 *
 * No malloc in hot path. No float. Stateless O(1).
 * ═══════════════════════════════════════════════════════════════════
 */

#ifndef BEAM_VALUE_C
#define BEAM_VALUE_C

#include <stdint.h>
#include <string.h>

/* ══════════════════════════════════════════════════════════════
   FGLS CORE INTEGRATION
   ══════════════════════════════════════════════════════════════ */

/* fibo_tick: 20736-slot field with 3 views */
#include "../core/fibo_tick.h"

/* geo_frame_seek: deterministic frame seek on 1440 timeline */
#include "../core/geo_frame_seek.h"

/* beam_timer: step+tick based on fibo_spine (1728×12=20736) */
#include "beam_timer.h"

/* ══════════════════════════════════════════════════════════════
   CONSTANTS
   ══════════════════════════════════════════════════════════════ */

#define BEAM_MAX_CAPOS       256u     /* max capo partitions */
#define BEAM_PARAMS_PER_CAPO 1000000u /* params per capo */
#define BEAM_TOTAL_CAPACITY  (BEAM_MAX_CAPOS * BEAM_PARAMS_PER_CAPO)

/* ══════════════════════════════════════════════════════════════
   BEAM CODE — 8-bit storage format (uint8_t)
   ══════════════════════════════════════════════════════════════
 *
 *   upper nibble = zone    0..15
 *   lower nibble = position 0..15
 *   total: 16 × 16 = 256 values = Q8 exactly
 *
 *   Mapping (Q8: -128..+127):
 *     code = weight + 128           → 0..255
 *     zone = code >> 4              → 0..15
 *     position = code & 0x0F       → 0..15
 *
 *   This is the ON-DISK representation — 1 byte per weight.
 *   Navigation (slot index) computed at runtime from param_index.
 */
typedef uint8_t BeamCode;

/* ══════════════════════════════════════════════════════════════
   BEAM COORDINATE — runtime transient (NOT stored)
   ══════════════════════════════════════════════════════════════
 *
 *   Carries capo_id + param_index for runtime slot navigation.
 *   Created on the fly, used immediately, NEVER stored.
 *   For storage, convert to BeamCode (8-bit).
 */

typedef struct {
    uint32_t capo_id;      /* parameter partition (0..255) */
    uint32_t param_index;  /* position in weight array */
    uint32_t abs_value;    /* weight magnitude (cached) */
    uint8_t  sign;         /* polarity: 1=ceiling(+), 0=ground(-) */
} BeamCoord;

/* ══════════════════════════════════════════════════════════════
   BEAM CODE (STORAGE) — weight ↔ BeamCode (8-bit)
   ══════════════════════════════════════════════════════════════ */

/* weight → BeamCode (Q8: -128..+127 → 0..255) */
static inline BeamCode beam_code_from_weight(int32_t weight)
{
    return (BeamCode)((uint8_t)((int32_t)(weight) + 128));
}

/* BeamCode → weight (0..255 → -128..+127) */
static inline int32_t beam_weight_from_code(BeamCode c)
{
    return (int32_t)((int8_t)((int32_t)(c) - 128));
}

/* Extract zone (upper nibble, 0..15) */
static inline uint8_t beam_code_zone(BeamCode c) {
    return (uint8_t)(c >> 4);
}

/* Extract position (lower nibble, 0..15) */
static inline uint8_t beam_code_pos(BeamCode c) {
    return (uint8_t)(c & 0x0F);
}

/* BeamCode → (zone, position) tuple */
typedef struct {
    uint8_t zone;
    uint8_t position;
} BeamZonePos;

static inline BeamZonePos beam_code_to_zp(BeamCode c) {
    BeamZonePos zp;
    zp.zone = c >> 4;
    zp.position = c & 0x0F;
    return zp;
}

/* (zone, position) → BeamCode */
static inline BeamCode beam_code_from_zp(BeamZonePos zp) {
    return (BeamCode)((zp.zone << 4) | (zp.position & 0x0F));
}

/* ══════════════════════════════════════════════════════════════
   CORE: weight_to_coord (runtime) — O(1), no hash, no storage
   ══════════════════════════════════════════════════════════════ */

static inline BeamCoord beam_weight_to_coord(uint32_t capo_id,
                                              uint32_t param_index,
                                              int32_t weight)
{
    BeamCoord c;
    c.capo_id = capo_id;
    c.param_index = param_index;
    c.abs_value = (uint32_t)((weight < 0) ? -weight : weight);
    c.sign = (uint8_t)((weight >= 0) ? 1 : 0);
    return c;
}

/* ══════════════════════════════════════════════════════════════
   CORE: coord_to_weight — O(1) roundtrip
   ══════════════════════════════════════════════════════════════ */

static inline int32_t beam_coord_to_weight(BeamCoord c)
{
    return c.sign ? (int32_t)c.abs_value : -(int32_t)c.abs_value;
}

/* ══════════════════════════════════════════════════════════════
   Runtime → Storage conversion
   ══════════════════════════════════════════════════════════════ */

/* BeamCoord (runtime) → BeamCode (storage) */
static inline BeamCode beam_coord_to_code(BeamCoord c)
{
    int32_t w = beam_coord_to_weight(c);
    return beam_code_from_weight(w);
}

/* BeamCode (storage) + param_index → BeamCoord (runtime) */
static inline BeamCoord beam_code_to_coord(BeamCode code, uint32_t capo_id,
                                            uint32_t param_index)
{
    int32_t w = beam_weight_from_code(code);
    return beam_weight_to_coord(capo_id, param_index, w);
}

/* ══════════════════════════════════════════════════════════════
   FGLS INTEGRATION: fibo_tick slot mapping
   ══════════════════════════════════════════════════════════════ */

/* beam coord → fibo_tick slot index (0..20735) */
static inline uint32_t beam_to_fibo_slot(BeamCoord c)
{
    uint16_t enc = (uint16_t)(c.param_index % FT_FRAME_CYCLE);
    uint16_t pipe = ft_enc_to_pipe(enc);
    uint8_t tick = ft_enc_to_tick(enc);
    return ft_slot_index(pipe, tick);
}

/* beam coord → fibo_tick flower ID (0..1727) */
static inline uint16_t beam_to_fibo_flower(BeamCoord c)
{
    uint16_t enc = (uint16_t)(c.param_index % FT_FRAME_CYCLE);
    return ft_enc_to_flower(enc);
}

/* beam coord → texture (inner/outer) */
static inline uint8_t beam_to_fibo_texture(BeamCoord c)
{
    uint16_t enc = (uint16_t)(c.param_index % FT_FRAME_CYCLE);
    return ft_enc_to_texture(enc);
}

/* ══════════════════════════════════════════════════════════════
   FGLS INTEGRATION: geo_frame_seek mapping
   ══════════════════════════════════════════════════════════════ */

/* beam coord → DualFrame (face, slot, phase, ico_idx) */
static inline DualFrame beam_to_frame(BeamCoord c)
{
    uint16_t enc = (uint16_t)(c.param_index % FT_FRAME_CYCLE);
    return frame_at(enc);
}

/* beam coord → frame range with entropy tolerance */
static inline FrameRange beam_to_frame_range(BeamCoord c, uint8_t entropy_class)
{
    uint16_t enc = (uint16_t)(c.param_index % FT_FRAME_CYCLE);
    return frame_range(enc, entropy_class);
}

/* ══════════════════════════════════════════════════════════════
   FGLS INTEGRATION: beam_timer (step+tick based on fibo_spine)
   ══════════════════════════════════════════════════════════════ */

/* beam coord → BeamTimer (step, tick, slot) */
static inline BeamTimer beam_to_timer(BeamCoord c)
{
    return bt_from_param(c.param_index);
}

/* beam coord → step (pipe) */
static inline uint16_t beam_to_pipe(BeamCoord c)
{
    return bt_param_to_pipe(c.param_index);
}

/* beam coord → tick */
static inline uint8_t beam_to_tick(BeamCoord c)
{
    return bt_param_to_tick(c.param_index);
}

/* ══════════════════════════════════════════════════════════════
   FGLS INTEGRATION: angular mapping (spherical coords)
   ══════════════════════════════════════════════════════════════ */

/* beam coord → spherical (azimuth, elevation) */
static inline void beam_to_spherical(BeamCoord c,
                                      uint16_t *azimuth,
                                      uint16_t *elevation)
{
    uint32_t idx = c.param_index;
    *azimuth = (uint16_t)(idx % 360);
    *elevation = (uint16_t)((idx / 360) % 360);
}

/* beam coord → full 5D coordinate (capo, azimuth, elevation, abs_value, sign) */
typedef struct {
    uint32_t capo_id;
    uint16_t azimuth;
    uint16_t elevation;
    uint32_t abs_value;
    uint8_t  sign;
} BeamCoord5D;

static inline BeamCoord5D beam_to_5d(BeamCoord c)
{
    BeamCoord5D c5;
    c5.capo_id = c.capo_id;
    c5.abs_value = c.abs_value;
    c5.sign = c.sign;
    beam_to_spherical(c, &c5.azimuth, &c5.elevation);
    return c5;
}

/* ══════════════════════════════════════════════════════════════
   BATCH OPERATIONS — process weight arrays
   ══════════════════════════════════════════════════════════════ */

/* Store weight array → BeamCode array (storage format) */
static inline uint32_t beam_store_codes(const int32_t *weights,
                                         uint32_t count,
                                         BeamCode *codes,
                                         uint32_t max_codes)
{
    uint32_t stored = 0;
    for (uint32_t i = 0; i < count && stored < max_codes; i++) {
        codes[stored] = beam_code_from_weight(weights[i]);
        stored++;
    }
    return stored;
}

/* Verify roundtrip: weight → code → weight */
static inline int beam_verify_code_roundtrip(const int32_t *weights,
                                              const BeamCode *codes,
                                              uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        int32_t recovered = beam_weight_from_code(codes[i]);
        if (recovered != weights[i]) return 0; /* FAIL */
    }
    return 1; /* PASS */
}

/* Old-style batch (BeamCoord runtime) — returns BeamCoord array */
static inline uint32_t beam_store_weights(const int32_t *weights,
                                           uint32_t count,
                                           BeamCoord *coords,
                                           uint32_t max_coords)
{
    uint32_t stored = 0;
    for (uint32_t i = 0; i < count && stored < max_coords; i++) {
        uint32_t capo_id = i / BEAM_PARAMS_PER_CAPO;
        coords[stored] = beam_weight_to_coord(capo_id, i, weights[i]);
        stored++;
    }
    return stored;
}

/* Verify roundtrip: weight → coord → weight */
static inline int beam_verify_roundtrip(const int32_t *weights,
                                         const BeamCoord *coords,
                                         uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        int32_t recovered = beam_coord_to_weight(coords[i]);
        if (recovered != weights[i]) return 0; /* FAIL */
    }
    return 1; /* PASS */
}

/* ══════════════════════════════════════════════════════════════
   STATISTICS
   ══════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t count;
    int32_t  min_value;
    int32_t  max_value;
    uint64_t sum_abs;
    uint32_t positive_count;
    uint32_t negative_count;
} BeamStats;

static inline BeamStats beam_compute_stats(const int32_t *weights, uint32_t count)
{
    BeamStats s = {0};
    if (count == 0) return s;

    s.count = count;
    s.min_value = weights[0];
    s.max_value = weights[0];
    s.sum_abs = 0;
    s.positive_count = 0;
    s.negative_count = 0;

    for (uint32_t i = 0; i < count; i++) {
        int32_t w = weights[i];
        if (w < s.min_value) s.min_value = w;
        if (w > s.max_value) s.max_value = w;
        s.sum_abs += (uint64_t)((w < 0) ? -w : w);
        if (w >= 0) s.positive_count++;
        else        s.negative_count++;
    }

    return s;
}

/* ══════════════════════════════════════════════════════════════
   BEAM CODE HISTOGRAM — for variable-length tuning
   ══════════════════════════════════════════════════════════════ */

typedef struct {
    uint64_t counts[256];     /* per-code counts */
    uint64_t total;
    uint32_t non_zero;        /* distinct codes seen */
    uint32_t zero_count;      /* count of code=0 (weight=-128) */
    uint32_t max_count;       /* max count in any bin */
    uint8_t  max_code;        /* code with max count */
} BeamHistogram;

static inline BeamHistogram beam_compute_histogram(const int32_t *weights,
                                                    uint32_t count)
{
    BeamHistogram h = {0};
    h.total = count;

    for (uint32_t i = 0; i < count; i++) {
        BeamCode code = beam_code_from_weight(weights[i]);
        if (h.counts[code] == 0) h.non_zero++;
        h.counts[code]++;
    }

    /* Find max */
    for (int i = 0; i < 256; i++) {
        if (h.counts[i] > h.max_count) {
            h.max_count = (uint32_t)h.counts[i];
            h.max_code = (uint8_t)i;
        }
    }

    h.zero_count = (uint32_t)h.counts[0];
    return h;
}

/* ══════════════════════════════════════════════════════════════
   VERIFY — call once at init, returns 0 on pass
   ══════════════════════════════════════════════════════════════ */

static inline int beam_value_verify(void)
{
    /* T1: roundtrip positive weight */
    {
        BeamCoord c = beam_weight_to_coord(0, 42, 100);
        if (beam_coord_to_weight(c) != 100) return -1;
    }

    /* T2: roundtrip negative weight */
    {
        BeamCoord c = beam_weight_to_coord(0, 43, -50);
        if (beam_coord_to_weight(c) != -50) return -2;
    }

    /* T3: roundtrip zero weight */
    {
        BeamCoord c = beam_weight_to_coord(0, 44, 0);
        if (beam_coord_to_weight(c) != 0) return -3;
    }

    /* T4: roundtrip max Q8 values */
    {
        BeamCoord c1 = beam_weight_to_coord(0, 45, 127);
        if (beam_coord_to_weight(c1) != 127) return -4;
        BeamCoord c2 = beam_weight_to_coord(0, 46, -128);
        if (beam_coord_to_weight(c2) != -128) return -5;
    }

    /* T5: BeamCode roundtrip — full Q8 range */
    {
        for (int32_t w = -128; w <= 127; w++) {
            BeamCode code = beam_code_from_weight(w);
            int32_t r = beam_weight_from_code(code);
            if (r != w) return -6;
            if (code > 255) return -7;
        }
    }

    /* T6: BeamCode zone/position split */
    {
        BeamCode c = beam_code_from_weight(100);
        uint8_t zone = beam_code_zone(c);
        uint8_t pos = beam_code_pos(c);
        /* weight=100 → code=228 → zone=14, pos=4 */
        if (zone != 14 || pos != 4) return -8;
    }

    /* T7: BeamCode roundtrip for edge cases */
    {
        BeamCode c1 = beam_code_from_weight(0);
        BeamCode c2 = beam_code_from_weight(127);
        BeamCode c3 = beam_code_from_weight(-128);
        if (beam_weight_from_code(c1) != 0) return -9;
        if (beam_weight_from_code(c2) != 127) return -10;
        if (beam_weight_from_code(c3) != -128) return -11;
    }

    /* T8: BeamCoord → BeamCode conversion */
    {
        BeamCoord c = beam_weight_to_coord(0, 42, 100);
        BeamCode code = beam_coord_to_code(c);
        if (beam_weight_from_code(code) != 100) return -12;
    }

    /* T9: BeamCode → BeamCoord conversion */
    {
        BeamCoord c = beam_code_to_coord(beam_code_from_weight(-50), 0, 100);
        int32_t w = beam_coord_to_weight(c);
        if (w != -50) return -13;
        if (c.param_index != 100) return -14;
    }

    /* T10: fibo_tick integration — slot index within range */
    {
        BeamCoord c = beam_weight_to_coord(0, 100, 50);
        uint32_t slot = beam_to_fibo_slot(c);
        if (slot >= FT_GEO_FULL) return -15; /* 0..20735 */
    }

    /* T11: geo_frame_seek integration — face within range */
    {
        BeamCoord c = beam_weight_to_coord(0, 200, 75);
        DualFrame f = beam_to_frame(c);
        if (f.face > 11) return -16;
        if (f.slot > 119) return -17;
    }

    /* T12: spherical integration */
    {
        BeamCoord c = beam_weight_to_coord(0, 300, 25);
        uint16_t az, el;
        beam_to_spherical(c, &az, &el);
        if (az >= 360) return -18;
        if (el >= 360) return -19;
    }

    /* T13: batch BeamCode store + roundtrip */
    {
        int32_t weights[] = {10, -20, 30, -40, 50};
        BeamCode codes[5];
        beam_store_codes(weights, 5, codes, 5);
        if (!beam_verify_code_roundtrip(weights, codes, 5)) return -20;
    }

    /* T14: old batch store + verify roundtrip */
    {
        int32_t weights[] = {10, -20, 30, -40, 50};
        BeamCoord coords[5];
        beam_store_weights(weights, 5, coords, 5);
        if (!beam_verify_roundtrip(weights, coords, 5)) return -21;
    }

    /* T15: stats computation */
    {
        int32_t weights[] = {10, -20, 30, -40, 50};
        BeamStats s = beam_compute_stats(weights, 5);
        if (s.count != 5) return -22;
        if (s.min_value != -40) return -23;
        if (s.max_value != 50) return -24;
        if (s.positive_count != 3) return -25;
        if (s.negative_count != 2) return -26;
    }

    /* T16: histogram */
    {
        int32_t weights[512];
        for (int i = 0; i < 512; i++) {
            weights[i] = (int32_t)((i % 256) - 128);
        }
        BeamHistogram h = beam_compute_histogram(weights, 512);
        if (h.total != 512) return -27;
        if (h.non_zero != 256) return -28; /* all 256 codes used */
        /* Each code appears exactly 2 times */
        if (h.zero_count != 2) return -29;
    }

    /* T17: fibo_tick verify */
    if (fibo_tick_verify() != 0) return -30;

    /* T18: geo_frame_seek verify */
    if (geo_frame_seek_verify() != 0) return -31;

    /* T19: beam_timer verify */
    if (beam_timer_verify() != 0) return -32;

    /* T20: beam_to_timer integration */
    {
        BeamCoord c = beam_weight_to_coord(0, 500, 25);
        BeamTimer t = beam_to_timer(c);
        if (t.pipe >= BT_STEPS) return -33;
        if (t.tick >= BT_TICKS) return -34;
    }

    /* T21: beam_to_pipe/tick integration */
    {
        BeamCoord c = beam_weight_to_coord(0, 600, 30);
        uint16_t step = beam_to_pipe(c);
        uint8_t tick = beam_to_tick(c);
        if (step >= BT_STEPS) return -35;
        if (tick >= BT_TICKS) return -36;
    }

    /* T22: BeamCode storage = 1 byte per weight */
    {
        /* Verify sizeof(BeamCode) == 1 */
        if (sizeof(BeamCode) != 1) return -37;
    }

    return 0; /* ALL PASS */
}

#endif /* BEAM_VALUE_C */
