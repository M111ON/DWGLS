/*
 * tw_capture_int.h — Integer-Only Hierarchical Sparse Triwheel
 *
 * SCALE = 12^4 * 10 = 207360  (dodecahedron-aligned fixed-point unit)
 * All coordinates, residuals, and angle tests are int32/int64.
 * No float, no trig at runtime. Pure integer relationships (POGLS rule).
 *
 * 207360 = 288 * 720 (TRing). Divisible by TRing=720, SHELL_TOTAL=288,
 * DODECA_FACES=12, TW_SLOTS=60, GEO_FULL/6=3456. Clean alignment.
 * 12^4 structure matches dodeca geometry.
 *
 * Geometry: 1 base edge + {36deg, 60deg} rules
 *   10 sectors (pentagon-pair, parent, 36deg each)
 *   each sector -> 6 child slots (hex-cast, 60deg) = 60 total
 *
 * Sector lookup: cross-product sign test against 10 boundary direction
 * vectors (no atan2). O(10) sign tests -> primary sector.
 *
 * Drain/bundle: margin band near a sector boundary activates BOTH
 * adjacent sectors. Margin expressed as integer cross-product threshold
 * (proportional to |v|, scale-invariant via squared-magnitude compare).
 *
 * Lossless: residual = v - slot_centroid, exact int64 subtraction.
 */

#ifndef TW_CAPTURE_INT_H
#define TW_CAPTURE_INT_H

#include "coord_spine.h"    /* TW_* constants (single source of truth) */
#include "shadow_zone.h"    /* ShadowZone, shadow_write */
#include <stdint.h>

/* Unit boundary direction vectors * TW_SCALE, at angles (90 - 36*k) deg */
static const int32_t TW_BOUNDARY_DIR[TW_N_SECTORS][2] = {
    {      0, 207360},
    { 121883, 167758},
    { 197211,  64078},
    { 197211, -64078},
    { 121883,-167758},
    {      0,-207360},
    {-121883,-167758},
    {-197211, -64078},
    {-197211,  64078},
    {-121883, 167758},
};

/* 6 slot centroids per sector, int32 * TW_SCALE (hexagon centroids, 0° grid) */
/*
 * Tri centroids: R_{-30} of hex centroids = independent triangle centroid grid.
 * These sit at the centers of equilateral triangles formed by edge/2
 * subdivision of the hexagon tiling. Together with TW_SLOT_LOCAL_I they
 * provide 120 physical positions per face — hex (0°) and tri (30° rotated).
 *
 * Computed as: tx = (hx*COS30 + hy*SIN30)/TW_SCALE
 *              ty = (-hx*SIN30 + hy*COS30)/TW_SCALE
 * where COS30=179580, SIN30=103680 (matches original rotation constants).
 *
 * No rotation of input needed — just use this table directly.
 */
static const int32_t TW_TRI_SLOT_LOCAL_I[TW_N_SECTORS][TW_SLOTS_PER][2] = {
  {{ 119232, 206517},{  88127, 206517},{  72575, 179580},{  88128, 152643},{ 119232, 152643},{ 134784, 179580}},
  {{ 217848,  96993},{ 192684, 115274},{ 164269, 102624},{ 161018,  71690},{ 186181,  53407},{ 214597,  66059}},
  {{ 233253, -49580},{ 223642, -19998},{ 193218, -13532},{ 172404, -36646},{ 182016, -66228},{ 212441, -72695}},
  {{ 159564,-177213},{ 169176,-147632},{ 148363,-124517},{ 117938,-130984},{ 108328,-160566},{ 129140,-183681}},
  {{  24926,-237160},{  50090,-218877},{  46839,-187942},{  18424,-175292},{  -6740,-193574},{  -3488,-224507}},
  {{-119232,-206517},{ -88128,-206517},{ -72576,-179580},{ -88128,-152643},{-119233,-152643},{-134785,-179580}},
  {{-217849, -96994},{-192685,-115275},{-164270,-102625},{-161019, -71691},{-186182, -53408},{-214598, -66060}},
  {{-233254,  49579},{-223643,  19997},{-193219,  13531},{-172405,  36645},{-182017,  66227},{-212442,  72694}},
  {{-159565, 177212},{-169177, 147631},{-148364, 124516},{-117939, 130983},{-108329, 160565},{-129141, 183680}},
  {{ -24927, 237159},{ -50091, 218876},{ -46840, 187941},{ -18425, 175291},{   6739, 193573},{   3487, 224506}}
};

static const int32_t TW_SLOT_LOCAL_I[TW_N_SECTORS][TW_SLOTS_PER][2] = {
  {{      0, 238464},{ -26937, 222912},{ -26937, 191808},{      0, 176256},{  26937, 191808},{  26937, 222912}},
  {{ 140166, 192922},{ 109232, 196172},{  90949, 171009},{ 103601, 142594},{ 134534, 139342},{ 152817, 164507}},
  {{ 226792,  73689},{ 203678,  94502},{ 174097,  84890},{ 167629,  54466},{ 190744,  33653},{ 220326,  43265}},
  {{ 226792, -73689},{ 220326, -43265},{ 190744, -33653},{ 167629, -54466},{ 174097, -84890},{ 203678, -94502}},
  {{ 140166,-192922},{ 152817,-164507},{ 134534,-139342},{ 103601,-142594},{  90949,-171009},{ 109232,-196172}},
  {{      0,-238464},{  26937,-222912},{  26937,-191808},{      0,-176256},{ -26937,-191808},{ -26937,-222912}},
  {{-140166,-192922},{-109232,-196172},{ -90949,-171009},{-103601,-142594},{-134534,-139342},{-152817,-164507}},
  {{-226792, -73689},{-203678, -94502},{-174097, -84890},{-167629, -54466},{-190744, -33653},{-220326, -43265}},
  {{-226792,  73689},{-220326,  43265},{-190744,  33653},{-167629,  54466},{-174097,  84890},{-203678,  94502}},
  {{-140166, 192922},{-152817, 164507},{-134534, 139342},{-103601, 142594},{ -90949, 171009},{-109232, 196172}},
};

typedef struct {
    uint8_t zone;
    uint8_t slot;
    int64_t resid_x;
    int64_t resid_y;
    uint8_t drain;
    uint8_t drain_zone;
    uint8_t drain_slot;
    int64_t drain_resid_x;
    int64_t drain_resid_y;
} TWCaptureInt;

/* cross product (z component) of two 2D vectors, int64 to avoid overflow */
static inline int64_t _tw_cross(int32_t ax, int32_t ay, int64_t bx, int64_t by)
{
    return (int64_t)ax*by - (int64_t)ay*bx;
}

static inline int64_t _tw_dot(int32_t ax, int32_t ay, int64_t bx, int64_t by)
{
    return (int64_t)ax*bx + (int64_t)ay*by;
}

/* squared magnitude */
static inline int64_t _tw_mag2(int64_t x, int64_t y)
{
    return x*x + y*y;
}

/* abs for int64 */
static inline int64_t _tw_abs64(int64_t v) { return v < 0 ? -v : v; }

/* Grid-aware pick slot: uses specified centroid table */
static inline void _tw_pick_slot_on(int64_t vx, int64_t vy, int sector,
                                     const int32_t centroids[TW_N_SECTORS][TW_SLOTS_PER][2],
                                     uint8_t *slot_out, int64_t *rx, int64_t *ry)
{
    int best = 0;
    int64_t bd = -1;
    for (int j = 0; j < TW_SLOTS_PER; j++) {
        int64_t dx = vx - centroids[sector][j][0];
        int64_t dy = vy - centroids[sector][j][1];
        int64_t d = dx*dx + dy*dy;
        if (bd < 0 || d < bd) { bd = d; best = j; }
    }
    *slot_out = (uint8_t)(sector*TW_SLOTS_PER + best);
    *rx = vx - centroids[sector][best][0];
    *ry = vy - centroids[sector][best][1];
}

/* Hex-grid wrapper (backward compat) */
static inline void _tw_pick_slot(int64_t vx, int64_t vy, int sector,
                                  uint8_t *slot_out, int64_t *rx, int64_t *ry)
{
    _tw_pick_slot_on(vx, vy, sector, TW_SLOT_LOCAL_I, slot_out, rx, ry);
}

/*
 * Sector lookup via cross-product sign against boundary rays.
 * Boundary ray k separates sector (k-1) and sector k (going clockwise
 * from the +Y axis in the same convention as the original construction).
 * For each k, sign(cross(boundary_k, v)) tells which side v is on.
 * We find the sector by counting how many boundary rays v is "ahead of".
 */
static inline int _tw_find_sector(int64_t vx, int64_t vy, int64_t *out_mincross_abs)
{
    /* signed cross with each boundary direction */
    int64_t cross[TW_N_SECTORS];
    for (int k = 0; k < TW_N_SECTORS; k++)
        cross[k] = _tw_cross(TW_BOUNDARY_DIR[k][0], TW_BOUNDARY_DIR[k][1], vx, vy);

    /* sector k is the region between boundary_k and boundary_{k+1}
     * (going clockwise): v is in sector k if cross(boundary_k, v) <= 0
     * and cross(boundary_{k+1}, v) >= 0  (clockwise winding) */
    int sector = 0;
    int64_t mincross = -1;
    for (int k = 0; k < TW_N_SECTORS; k++) {
        int kn = (k+1) % TW_N_SECTORS;
        if (cross[k] <= 0 && cross[kn] >= 0) {
            sector = k;
        }
        int64_t a = _tw_abs64(cross[k]);
        if (mincross < 0 || a < mincross) mincross = a;
    }
    *out_mincross_abs = mincross;
    return sector;
}

/* Sparse hierarchical capture on any centroid grid (hex or tri) */
static inline void tw_capture_int_on_grid(int64_t vx, int64_t vy,
                                           const int32_t centroids[TW_N_SECTORS][TW_SLOTS_PER][2],
                                           TWCaptureInt *out)
{
    int64_t mincross;
    int primary = _tw_find_sector(vx, vy, &mincross);

    out->zone = (uint8_t)primary;
    _tw_pick_slot_on(vx, vy, primary, centroids, &out->slot, &out->resid_x, &out->resid_y);

    /* drain test: |cross| / |v||boundary| ~ sin(angle_to_boundary)
     * boundary dirs are unit*TW_SCALE, so |boundary|=TW_SCALE.
     * sin(angle) = |cross| / (|v| * TW_SCALE)
     * drain if sin(angle) < MARGIN_NUM/MARGIN_DEN
     * => |cross| * MARGIN_DEN < |v| * TW_SCALE * MARGIN_NUM  */
    int64_t vmag2 = _tw_mag2(vx, vy);
    /* compare mincross^2 * DEN^2  vs  vmag2 * SCALE^2 * NUM^2  (avoid sqrt) */
    __int128 lhs = (__int128)mincross * mincross * TW_MARGIN_DEN * TW_MARGIN_DEN;
    __int128 rhs = (__int128)vmag2 * TW_SCALE * TW_SCALE * (int64_t)TW_MARGIN_NUM * TW_MARGIN_NUM;

    if (lhs < rhs) {
        /* near boundary -> find neighbor sector with smaller |cross| on the
         * other side; check both neighbors and pick whichever's boundary
         * produced mincross */
        int64_t cross_prev = _tw_cross(TW_BOUNDARY_DIR[primary][0], TW_BOUNDARY_DIR[primary][1], vx, vy);
        int64_t cross_next = _tw_cross(TW_BOUNDARY_DIR[(primary+1)%TW_N_SECTORS][0],
                                        TW_BOUNDARY_DIR[(primary+1)%TW_N_SECTORS][1], vx, vy);
        int secondary;
        if (_tw_abs64(cross_prev) < _tw_abs64(cross_next))
            secondary = (primary - 1 + TW_N_SECTORS) % TW_N_SECTORS;
        else
            secondary = (primary + 1) % TW_N_SECTORS;

        out->drain = 1;
        out->drain_zone = (uint8_t)secondary;
        _tw_pick_slot_on(vx, vy, secondary, centroids, &out->drain_slot, &out->drain_resid_x, &out->drain_resid_y);
    } else {
        out->drain = 0;
        out->drain_zone = 0;
        out->drain_slot = 0;
        out->drain_resid_x = 0;
        out->drain_resid_y = 0;
    }
}

/* Combined grid (hamburger): hex(0..5) + tri(6..11) baked into 12/sector.
 * Usage: slot_local ∈ {0..5}=hex, {6..11}=tri. is_tri = slot_local >= 6.
 * The zone and slot local (0..5) within each sub-grid are identical to the
 * individual hex/tri tables — just combined for single-call nearest search. */
#define TW_COMBINED_SLOTS 12u
static const int32_t TW_COMBINED_GRID[TW_N_SECTORS][TW_COMBINED_SLOTS][2] = {
  {{      0, 238464},{ -26937, 222912},{ -26937, 191808},{      0, 176256},{  26937, 191808},{  26937, 222912},{ 119232, 206517},{  88127, 206517},{  72575, 179580},{  88128, 152643},{ 119232, 152643},{ 134784, 179580}},
  {{ 140166, 192922},{ 109232, 196172},{  90949, 171009},{ 103601, 142594},{ 134534, 139342},{ 152817, 164507},{ 217848,  96993},{ 192684, 115274},{ 164269, 102624},{ 161018,  71690},{ 186181,  53407},{ 214597,  66059}},
  {{ 226792,  73689},{ 203678,  94502},{ 174097,  84890},{ 167629,  54466},{ 190744,  33653},{ 220326,  43265},{ 233253, -49580},{ 223642, -19998},{ 193218, -13532},{ 172404, -36646},{ 182016, -66228},{ 212441, -72695}},
  {{ 226792, -73689},{ 220326, -43265},{ 190744, -33653},{ 167629, -54466},{ 174097, -84890},{ 203678, -94502},{ 159564,-177213},{ 169176,-147632},{ 148363,-124517},{ 117938,-130984},{ 108328,-160566},{ 129140,-183681}},
  {{ 140166,-192922},{ 152817,-164507},{ 134534,-139342},{ 103601,-142594},{  90949,-171009},{ 109232,-196172},{  24926,-237160},{  50090,-218877},{  46839,-187942},{  18424,-175292},{  -6740,-193574},{  -3488,-224507}},
  {{      0,-238464},{  26937,-222912},{  26937,-191808},{      0,-176256},{ -26937,-191808},{ -26937,-222912},{-119232,-206517},{ -88128,-206517},{ -72576,-179580},{ -88128,-152643},{-119233,-152643},{-134785,-179580}},
  {{-140166,-192922},{-109232,-196172},{ -90949,-171009},{-103601,-142594},{-134534,-139342},{-152817,-164507},{-217849, -96994},{-192685,-115275},{-164270,-102625},{-161019, -71691},{-186182, -53408},{-214598, -66060}},
  {{-226792, -73689},{-203678, -94502},{-174097, -84890},{-167629, -54466},{-190744, -33653},{-220326, -43265},{-233254,  49579},{-223643,  19997},{-193219,  13531},{-172405,  36645},{-182017,  66227},{-212442,  72694}},
  {{-226792,  73689},{-220326,  43265},{-190744,  33653},{-167629,  54466},{-174097,  84890},{-203678,  94502},{-159565, 177212},{-169177, 147631},{-148364, 124516},{-117939, 130983},{-108329, 160565},{-129141, 183680}},
  {{-140166, 192922},{-152817, 164507},{-134534, 139342},{-103601, 142594},{ -90949, 171009},{-109232, 196172},{ -24927, 237159},{ -50091, 218876},{ -46840, 187941},{ -18425, 175291},{   6739, 193573},{   3487, 224506}}
};

/* Combined capture: searches all 12 centroids/sector (hex+tri) in one pass.
 * Eliminates separate hex-vs-tri comparison — the nearest of all 12 wins.
 * is_tri_out: 0=hex centroid, 1=tri centroid.
 * slot: 0..59 (local = slot % 6, same as hex-only for backward compat). */
static inline void tw_capture_int_combined(int64_t vx, int64_t vy,
                                            TWCaptureInt *out, uint8_t *is_tri_out)
{
    int64_t mincross;
    int primary = _tw_find_sector(vx, vy, &mincross);

    out->zone = (uint8_t)primary;

    /* Search 12 centroids (6 hex + 6 tri) in one pass */
    int best = 0, best_is_tri = 0;
    int64_t bd = -1;
    for (int j = 0; j < (int)TW_COMBINED_SLOTS; j++) {
        int64_t dx = vx - TW_COMBINED_GRID[primary][j][0];
        int64_t dy = vy - TW_COMBINED_GRID[primary][j][1];
        int64_t d = dx*dx + dy*dy;
        if (bd < 0 || d < bd) { bd = d; best = j; best_is_tri = (j >= TW_SLOTS_PER); }
    }

    int local = best % TW_SLOTS_PER;
    out->slot = (uint8_t)(primary * TW_SLOTS_PER + local);
    out->resid_x = vx - TW_COMBINED_GRID[primary][best][0];
    out->resid_y = vy - TW_COMBINED_GRID[primary][best][1];
    *is_tri_out = (uint8_t)best_is_tri;

    /* Drain test */
    int64_t vmag2 = _tw_mag2(vx, vy);
    __int128 lhs = (__int128)mincross * mincross * TW_MARGIN_DEN * TW_MARGIN_DEN;
    __int128 rhs = (__int128)vmag2 * TW_SCALE * TW_SCALE * (int64_t)TW_MARGIN_NUM * TW_MARGIN_NUM;

    if (lhs < rhs) {
        int64_t cross_prev = _tw_cross(TW_BOUNDARY_DIR[primary][0], TW_BOUNDARY_DIR[primary][1], vx, vy);
        int64_t cross_next = _tw_cross(TW_BOUNDARY_DIR[(primary+1)%TW_N_SECTORS][0],
                                        TW_BOUNDARY_DIR[(primary+1)%TW_N_SECTORS][1], vx, vy);
        int secondary;
        if (_tw_abs64(cross_prev) < _tw_abs64(cross_next))
            secondary = (primary - 1 + TW_N_SECTORS) % TW_N_SECTORS;
        else
            secondary = (primary + 1) % TW_N_SECTORS;

        out->drain = 1;
        out->drain_zone = (uint8_t)secondary;
        /* For drain, search combined grid in secondary sector too */
        int d_best = 0;
        int64_t d_bd = -1;
        for (int j = 0; j < (int)TW_COMBINED_SLOTS; j++) {
            int64_t dx = vx - TW_COMBINED_GRID[secondary][j][0];
            int64_t dy = vy - TW_COMBINED_GRID[secondary][j][1];
            int64_t d = dx*dx + dy*dy;
            if (d_bd < 0 || d < d_bd) { d_bd = d; d_best = j; }
        }
        int d_local = d_best % TW_SLOTS_PER;
        out->drain_slot = (uint8_t)(secondary * TW_SLOTS_PER + d_local);
        out->drain_resid_x = vx - TW_COMBINED_GRID[secondary][d_best][0];
        out->drain_resid_y = vy - TW_COMBINED_GRID[secondary][d_best][1];
    } else {
        out->drain = 0;
        out->drain_zone = 0;
        out->drain_slot = 0;
        out->drain_resid_x = 0;
        out->drain_resid_y = 0;
    }
}

/* Hex-grid capture (backward compat, 0° hexagon centroids) */
static inline void tw_capture_int(int64_t vx, int64_t vy, TWCaptureInt *out)
{
    tw_capture_int_on_grid(vx, vy, TW_SLOT_LOCAL_I, out);
}

/* Tri-grid capture (30° triangle centroids, independent physical grid) */
static inline void tw_capture_int_tri(int64_t vx, int64_t vy, TWCaptureInt *out)
{
    tw_capture_int_on_grid(vx, vy, TW_TRI_SLOT_LOCAL_I, out);
}

/* Reconstruct from primary half on any grid (lossless, exact integers) */
static inline void tw_reconstruct_int_on_grid(const TWCaptureInt *in,
                                               const int32_t centroids[TW_N_SECTORS][TW_SLOTS_PER][2],
                                               int64_t *vx, int64_t *vy)
{
    int sector = in->zone;
    int local  = in->slot - sector*TW_SLOTS_PER;
    *vx = centroids[sector][local][0] + in->resid_x;
    *vy = centroids[sector][local][1] + in->resid_y;
}

/* Hex-grid reconstruct (backward compat) */
static inline void tw_reconstruct_int(const TWCaptureInt *in, int64_t *vx, int64_t *vy)
{
    tw_reconstruct_int_on_grid(in, TW_SLOT_LOCAL_I, vx, vy);
}

/* Tri-grid reconstruct */
static inline void tw_reconstruct_int_tri(const TWCaptureInt *in, int64_t *vx, int64_t *vy)
{
    tw_reconstruct_int_on_grid(in, TW_TRI_SLOT_LOCAL_I, vx, vy);
}

/* Reconstruct from combined capture: uses is_tri to select hex or tri table */
static inline void tw_reconstruct_int_combined(const TWCaptureInt *in,
                                                uint8_t is_tri,
                                                int64_t *vx, int64_t *vy)
{
    if (is_tri)
        tw_reconstruct_int_tri(in, vx, vy);
    else
        tw_reconstruct_int(in, vx, vy);
}

/* ══════════════════════════════════════════════════════════════
   SHADOW ROUTING — boundary/drain → shadow zone
   ══════════════════════════════════════════════════════════════
   If capture has drain=1, routes the drain capture to a shadow
   zone instead of the secondary sector. Returns node_id for
   retrieval. */
typedef struct {
    TWCaptureInt capture;
    uint8_t      shadow_zone;       /* SHADOW_ZONE_A or B, or 0 if no drain */
    uint8_t      shadow_slot;       /* slot index in shadow zone */
    uint32_t     shadow_node_id;    /* GEO_FULL address in shadow zone */
    uint8_t      is_tri;
} TWShadowCapture;

static inline void tw_capture_shadow(int64_t vx, int64_t vy,
                                      ShadowZone *shadow,
                                      TWShadowCapture *out)
{
    uint8_t is_tri;
    tw_capture_int_combined(vx, vy, &out->capture, &is_tri);
    out->is_tri = is_tri;

    if (!out->capture.drain || !shadow) {
        out->shadow_zone = 0;
        out->shadow_slot = 0;
        out->shadow_node_id = 0;
        return;
    }

    /* route drain to shadow zone instead of neighbor sector */
    uint64_t bond_key = (uint64_t)(uint32_t)vx ^ ((uint64_t)(uint32_t)vy << 32);
    uint32_t tick = out->capture.drain_zone;  /* reuse as tick */

    uint8_t drain_data[DIAMOND_BLOCK_SIZE];
    memset(drain_data, 0, DIAMOND_BLOCK_SIZE);
    drain_data[0] = out->capture.drain_zone;
    drain_data[1] = (uint8_t)(out->capture.drain_slot);

    uint32_t node_id;
    if (shadow_write(shadow, bond_key, tick, BERMUDA_SHADOW_COLD,
                      drain_data, DIAMOND_BLOCK_SIZE, &node_id) == SHADOW_OK)
    {
        out->shadow_zone = shadow->zone_id;
        out->shadow_slot = node_id % SHADOW_N_SLOTS;
        out->shadow_node_id = node_id;
    } else {
        out->shadow_zone = 0;
        out->shadow_slot = 0;
        out->shadow_node_id = 0;
    }
}

#endif /* TW_CAPTURE_INT_H */
