/*
 * b_fs_components.h — b_fs Component Adapters (fan24, gear, language, 18tes)
 * ════════════════════════════════════════════════════════════════════
 * Docks the existing DWGLS components into b_fs as pure-arithmetic transforms:
 *   - fan24 : CRT bijection (dc mod 8, dx mod 3) on ring-24
 *   - gear   : gear tooth (pos mod 24) + frame
 *   - language: 9-language identity (pos mod 9)
 *   - 18tes  : 18 tesseracts × 8 cells × 144 slots = 20736
 * All transforms are bijective/identity-preserving and store results in b_fs.
 * ════════════════════════════════════════════════════════════════════
 */
#ifndef B_FS_COMPONENTS_H
#define B_FS_COMPONENTS_H

#include <stdint.h>
#include <string.h>
#include "b_fs_base.h"
#include "b_fs_ann.h"
#include "fan24_gear.h"

/* ── 18tes (6ico compound): 18 tess × 8 cells × 144 slots ─────── */
#define BFC_TESS_COUNT   18u
#define BFC_CELL_COUNT    8u
#define BFC_SLOT_COUNT  144u
#define BFC_TESS_SIZE   (BFC_CELL_COUNT * BFC_SLOT_COUNT)  /* 1152 */

static inline void bfc_18tes_decode(uint32_t flat, uint32_t *tess, uint32_t *cell, uint32_t *slot) {
    flat %= BFS_PAIR_N;
    if (tess) *tess = flat / BFC_TESS_SIZE;
    if (cell) *cell = (flat % BFC_TESS_SIZE) / BFC_SLOT_COUNT;
    if (slot) *slot = flat % BFC_SLOT_COUNT;
}

static inline uint32_t bfc_18tes_encode(uint32_t tess, uint32_t cell, uint32_t slot) {
    return (tess % BFC_TESS_COUNT) * BFC_TESS_SIZE
         + (cell % BFC_CELL_COUNT) * BFC_SLOT_COUNT
         + (slot % BFC_SLOT_COUNT);
}

/* ── fan24: CRT bijection (ring-24) ───────────────────────────── */
static inline uint8_t bfc_fan24_crt(uint8_t dc, uint8_t dx) {
    return fg_crt(dc, dx);
}

static inline void bfc_fan24_decode(uint32_t w, uint8_t *dc, uint8_t *dx) {
    uint32_t r = w % FG_RING;
    if (dc) *dc = (uint8_t)(r % FG_WHEEL_KIS);
    if (dx) *dx = (uint8_t)(r % FG_WHEEL_HYP);
}

/* ── gear: tooth + frame ───────────────────────────────────────── */
static inline uint32_t bfc_gear_tooth(uint32_t w) { return w % FG_RING; }
static inline uint32_t bfc_gear_frame(uint32_t w) { return w / FG_LOCAL; }

/* ── language: 9-identity ──────────────────────────────────────── */
static inline uint32_t bfc_language(uint32_t pos) { return pos % 9u; }

/* ── KIS: stride-37 frame on 1440 cycle ───────────────────────── */
static inline uint32_t bfc_kis_frame(uint32_t flat) {
    /* frame = (flat * 37) % 1440 mapped back; use frame_enc-style */
    return (flat * 37u) % 1440u;
}

/* ── Hyperbolic: axis (0..2) from slot (hyperbolic_axis_of) ───── */
static inline uint8_t bfc_hyperbolic_axis(uint32_t slot) {
    /* hyperbolic_axis_of: slot / HYP_AXIS_SLOTS, clamped to 0..2 */
    if (slot >= 20736u) slot = 20735u;
    uint32_t a = slot / 6912u;  /* HYP_AXIS_SLOTS = 6912 (3 axes) */
    if (a > 2) a = 2;
    return (uint8_t)a;
}

/* ── b_fs docking: store a component value as a BFS entry ──────── */
static inline int bfc_store_18tes(BfsAnnIndex *idx, uint32_t flat) {
    uint32_t t, c, s; bfc_18tes_decode(flat, &t, &c, &s);
    /* encode as 4-byte payload: t,c,s,flat */
    uint8_t buf[4] = { (uint8_t)t, (uint8_t)c, (uint8_t)s, (uint8_t)(flat & 0xFF) };
    char name[32]; 
    /* build name from flat */
    name[0]='t'; name[1]='e'; name[2]='s'; 
    /* simple unique name via flat value suffix */
    uint32_t v = flat;
    int i = 3;
    /* decimal encode */
    char tmp[16]; int ti = 0;
    if (v == 0) tmp[ti++] = '0';
    else { uint32_t x = v; char rev[16]; int ri=0; while (x>0){rev[ri++]='0'+(x%10);x/=10;} while(ri>0) tmp[ti++]=rev[--ri]; }
    tmp[ti]=0;
    for (int j=0; tmp[j]; j++) name[i++]=tmp[j];
    name[i]=0;
    return bfs_ann_put(idx, name, buf, 4, flat);
}

#endif /* B_FS_COMPONENTS_H */
