/*
 * beam_value_dll.c — Shared library for Python ctypes bridge
 * ═══════════════════════════════════════════════════════════════════
 *
 * Compile: gcc -O2 -shared -o beam_value.dll beam_value_dll.c \
 *          -I../core -I../collection -I../collection/rdh \
 *          -I../collection/dgls/geo/include
 *
 * Python: ctypes.CDLL('beam_value.dll')
 * ═══════════════════════════════════════════════════════════════════
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Include the core beam_value implementation */
#include "beam_value.c"

/* ══════════════════════════════════════════════════════════════
   DLL EXPORTS — flat C API for ctypes
   ══════════════════════════════════════════════════════════════ */

#ifdef _WIN32
  #define EXPORT __declspec(dllexport)
#else
  #define EXPORT __attribute__((visibility("default")))
#endif

/* Store weights → coords (returns count stored) */
EXPORT uint32_t beam_store(const int32_t *weights, uint32_t count,
                           uint32_t *capo_ids, uint32_t *param_indices,
                           uint32_t *abs_values, uint8_t *signs)
{
    uint32_t stored = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t capo_id = i / BEAM_PARAMS_PER_CAPO;
        BeamCoord c = beam_weight_to_coord(capo_id, i, weights[i]);
        capo_ids[stored] = c.capo_id;
        param_indices[stored] = c.param_index;
        abs_values[stored] = c.abs_value;
        signs[stored] = c.sign;
        stored++;
    }
    return stored;
}

/* Recover weights from coords */
EXPORT void beam_recover(const uint32_t *capo_ids, const uint32_t *param_indices,
                         const uint32_t *abs_values, const uint8_t *signs,
                         uint32_t count, int32_t *weights)
{
    for (uint32_t i = 0; i < count; i++) {
        BeamCoord c;
        c.capo_id = capo_ids[i];
        c.param_index = param_indices[i];
        c.abs_value = abs_values[i];
        c.sign = signs[i];
        weights[i] = beam_coord_to_weight(c);
    }
}

/* Batch: weight → fibo_slot */
EXPORT void beam_to_fibo_slots(const int32_t *weights, uint32_t count,
                                uint32_t *slots)
{
    for (uint32_t i = 0; i < count; i++) {
        uint32_t capo_id = i / BEAM_PARAMS_PER_CAPO;
        BeamCoord c = beam_weight_to_coord(capo_id, i, weights[i]);
        slots[i] = beam_to_fibo_slot(c);
    }
}

/* Batch: weight → frame (face, slot) */
EXPORT void beam_to_frames(const int32_t *weights, uint32_t count,
                           uint8_t *faces, uint8_t *frame_slots)
{
    for (uint32_t i = 0; i < count; i++) {
        uint32_t capo_id = i / BEAM_PARAMS_PER_CAPO;
        BeamCoord c = beam_weight_to_coord(capo_id, i, weights[i]);
        DualFrame f = beam_to_frame(c);
        faces[i] = f.face;
        frame_slots[i] = f.slot;
    }
}

/* Batch: weight → spherical (az, el) */
EXPORT void beam_to_sphericals(const int32_t *weights, uint32_t count,
                               uint16_t *azimuths, uint16_t *elevations)
{
    for (uint32_t i = 0; i < count; i++) {
        uint32_t capo_id = i / BEAM_PARAMS_PER_CAPO;
        BeamCoord c = beam_weight_to_coord(capo_id, i, weights[i]);
        beam_to_spherical(c, &azimuths[i], &elevations[i]);
    }
}

/* Verify roundtrip (returns 1=PASS, 0=FAIL) */
EXPORT int beam_verify(const int32_t *weights, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        uint32_t capo_id = i / BEAM_PARAMS_PER_CAPO;
        BeamCoord c = beam_weight_to_coord(capo_id, i, weights[i]);
        int32_t recovered = beam_coord_to_weight(c);
        if (recovered != weights[i]) return 0;
    }
    return 1;
}

/* Stats */
EXPORT void beam_stats(const int32_t *weights, uint32_t count,
                       int32_t *min_val, int32_t *max_val,
                       uint32_t *pos_count, uint32_t *neg_count)
{
    if (count == 0) {
        *min_val = 0; *max_val = 0;
        *pos_count = 0; *neg_count = 0;
        return;
    }
    *min_val = weights[0]; *max_val = weights[0];
    *pos_count = 0; *neg_count = 0;
    for (uint32_t i = 0; i < count; i++) {
        int32_t w = weights[i];
        if (w < *min_val) *min_val = w;
        if (w > *max_val) *max_val = w;
        if (w >= 0) (*pos_count)++;
        else        (*neg_count)++;
    }
}

/* Verify FGLS core (returns 0=PASS) */
EXPORT int beam_verify_fgls(void)
{
    int r1 = fibo_tick_verify();
    int r2 = geo_frame_seek_verify();
    int r3 = beam_value_verify();
    return r1 + r2 + r3;
}
