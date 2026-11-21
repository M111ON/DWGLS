/* tess_jump.h — Jump module: integrates geo_jump + hyper_jump + frame_seek + fan24 + jet
 * into the tess_resolver system. Carries positive-negative (two-sided).
 *
 * Architecture (outer → inner):
 *   geo_jump (outer)     — basic jump types (Hilbert, Peano, Pentagon, MOD, Invert, Ground)
 *     ↓
 *   hyper_jump (dims)    — 4D tes-compound towers (HJ3: 48×3, HJ4: 36×4)
 *     ↓
 *   frame_seek (fishbone) — deterministic frame seek on Fibo 1440 timeline
 *     ↓
 *   fan24 (CRT)          — ring-24 bijection (dc mod 8, dx mod 3)
 *     ↓
 *   jet (bridge)         — discrete leads, continuous chases (ambulance, not highway)
 *     ↓
 *   144×144 (20736)     — snap deterministic pattern onto format
 *
 * Two-sided: positive (outside) + negative (inside-out). Carries both.
 * Lazy evaluation: only compute when pointer reads (no wasted work).
 */
#ifndef TESS_JUMP_H
#define TESS_JUMP_H

#include <stdint.h>
#include <stddef.h>
#include "tess_resolver.h"

/* ── Jump types (from geo_jump) ────────────────────────────────── */
typedef enum {
    JUMP_HILBERT  = 0,
    JUMP_PEANO    = 1,
    JUMP_PENTAGON = 2,
    JUMP_MOD      = 3,
    JUMP_INVERT   = 4,
    JUMP_GROUND   = 5,
    JUMP_FRAME    = 6,   /* frame_seek (fishbone snap) */
    JUMP_FAN24    = 7,   /* fan24 CRT (ring-24) */
    JUMP_HYPER    = 8,   /* hyper_jump (4D) */
    JUMP_JET      = 9,   /* jet bridge (discrete/continuous) */
} JumpType;

/* ── Jump result ───────────────────────────────────────────────── */
typedef struct {
    uint32_t slot;       /* resolved slot in 144×144 (0..20735) */
    uint32_t cell;       /* stride-37 cell */
    uint32_t cube_id;    /* which cube (capo) */
    uint32_t local_slot; /* slot within cube */
    int      side;       /* +1 = positive, -1 = negative (inside-out) */
    uint32_t distort;    /* constant distort value */
    int      computed;   /* 1 = was computed (lazy), 0 = not */
} JumpResult;

/* ── Jump context (carries state across jumps) ─────────────────── */
typedef struct {
    uint32_t pointer_pos;     /* current pointer position (slot) */
    int      side;            /* +1 = positive, -1 = negative */
    uint32_t frame_enc;       /* frame_seek encoded position (1440 cycle) */
    uint8_t  fan24_state;     /* fan24 ring-24 state */
    int      cascade_active;   /* 1 = cascade (rewind) active */
    TessCascade cascade;       /* quadtree cascade (rewind) */
    TessVoronoiMask mask;     /* voronoi mask (multi-pointer read) */
} JumpContext;

/* ── Initialize jump context ───────────────────────────────────── */
static inline void jump_init(JumpContext *ctx) {
    ctx->pointer_pos = 0;
    ctx->side = 1;  /* start on positive side */
    ctx->frame_enc = 0;
    ctx->fan24_state = 0;
    ctx->cascade_active = 0;
    memset(&ctx->cascade, 0, sizeof(ctx->cascade));
    memset(&ctx->mask, 0, sizeof(ctx->mask));
}

/* ── Frame seek (fishbone snap): deterministic 1440 cycle ──────── */
/* frame_enc(t) = (t * 37) % 1440 (stride-37 on 1440 cycle) */
static inline uint32_t frame_seek_enc(uint32_t t) {
    return (t * 37u) % 1440u;
}

/* frame_next: advance by one stride */
static inline uint32_t frame_seek_next(uint32_t enc) {
    return (enc + 37u) % 1440u;
}

/* frame_prev: step back by one stride */
static inline uint32_t frame_seek_prev(uint32_t enc) {
    return (enc + 1440u - 37u) % 1440u;
}

/* ── Fan24 CRT (ring-24 bijection) ─────────────────────────────── */
/* fg_crt(dc, dx) = dc + 8 * (((dx + 3 - (dc % 3)) % 3) * 2 % 3) */
static inline uint8_t fan24_crt(uint8_t dc, uint8_t dx) {
    uint8_t k = (uint8_t)(((dx + 3u - (dc % 3u)) % 3u) * 2u % 3u);
    return (uint8_t)(dc + 8u * k);
}

/* ── Hyper jump (4D: tower + local) ────────────────────────────── */
/* HJ3: 48×3 towers (span=48, 3 towers) */
/* HJ4: 36×4 towers (span=36, 4 towers) */
#define HJ_TOTAL 20736u
#define HJ_TES48 432u   /* 48 * 9 */
#define HJ_TES36 576u   /* 36 * 16 */

static inline uint32_t hj3_tower(uint32_t pos) { return (pos % HJ_TOTAL) / HJ_TES48; }
static inline uint32_t hj3_local(uint32_t pos) { return (pos % HJ_TOTAL) % HJ_TES48; }
static inline uint32_t hj4_tower(uint32_t pos) { return (pos % HJ_TOTAL) / HJ_TES36; }
static inline uint32_t hj4_local(uint32_t pos) { return (pos % HJ_TOTAL) % HJ_TES36; }

/* hyper jump: mirror local (invert) */
static inline uint32_t hj_jump(uint32_t pos, int use_hj4) {
    if (use_hj4) {
        uint32_t t = hj4_tower(pos);
        uint32_t l = hj4_local(pos);
        return (t * HJ_TES36) + (HJ_TES36 - 1 - l); /* invert local */
    } else {
        uint32_t t = hj3_tower(pos);
        uint32_t l = hj3_local(pos);
        return (t * HJ_TES48) + (HJ_TES48 - 1 - l); /* invert local */
    }
}

/* ── Jet bridge (discrete leads, continuous chases) ────────────── */
/* Jet is "ambulance, not highway": discrete decides, continuous chases. */
/* For jump: discrete (jump type) decides direction, continuous (frame) chases. */
static inline uint32_t jet_snap(uint32_t pos, uint32_t target) {
    /* Snap pos to nearest multiple of target (deterministic) */
    if (target == 0) return pos;
    uint32_t rem = pos % target;
    if (rem <= target / 2) return pos - rem;
    return pos + (target - rem);
}

/* ── Cross boundary (positive ↔ negative) ──────────────────────── */
static inline int jump_cross_boundary(JumpContext *ctx) {
    ctx->side = -ctx->side;  /* flip side */
    return ctx->side;
}

/* ── Main jump function (carries positive-negative) ────────────── */
static inline JumpResult jump_execute(JumpContext *ctx, JumpType type, uint32_t param) {
    JumpResult res;
    memset(&res, 0, sizeof(res));
    res.side = ctx->side;
    res.computed = 0;

    uint32_t pos = ctx->pointer_pos;

    switch (type) {
        case JUMP_HILBERT:
            /* geo_jump Hilbert: node + param (mod GEO_FULL) */
            pos = (pos + param) % TESS_TOTAL_SLOTS;
            break;
        case JUMP_PEANO:
            pos = (pos + param * 2) % TESS_TOTAL_SLOTS;
            break;
        case JUMP_PENTAGON:
            pos = (pos + param * 5) % TESS_TOTAL_SLOTS;
            break;
        case JUMP_MOD:
            pos = (pos * param) % TESS_TOTAL_SLOTS;
            break;
        case JUMP_INVERT:
            pos = TESS_TOTAL_SLOTS - 1 - pos;
            break;
        case JUMP_GROUND:
            pos = (pos + param) % TESS_TOTAL_SLOTS;
            break;
        case JUMP_FRAME:
            /* frame_seek (fishbone): advance by stride-37 on 1440 cycle */
            if (ctx->frame_enc == 0) ctx->frame_enc = 37; /* first step */
            else ctx->frame_enc = frame_seek_next(ctx->frame_enc);
            pos = (ctx->frame_enc % TESS_TOTAL_SLOTS);
            break;
        case JUMP_FAN24:
            /* fan24 CRT: ring-24 bijection */
            ctx->fan24_state = fan24_crt(ctx->fan24_state % 8, ctx->fan24_state % 3);
            pos = (pos + ctx->fan24_state) % TESS_TOTAL_SLOTS;
            break;
        case JUMP_HYPER:
            /* hyper_jump: 4D tower + local invert */
            pos = hj_jump(pos, (param > 0) ? 1 : 0);
            break;
        case JUMP_JET:
            /* jet bridge: snap to nearest boundary */
            pos = jet_snap(pos, param > 0 ? param : 144);
            break;
    }

    /* Cross boundary if needed (inside-out) */
    /* (boundary crossing is determined by the jump type and position) */

    /* Update pointer */
    ctx->pointer_pos = pos;

    /* Resolve slot → cell (stride-37) */
    res.slot = pos;
    res.cell = tess_slot_to_cell(pos % TESS_TOTAL_SLOTS);
    res.cube_id = pos / TESS_TOTAL_SLOTS;
    res.local_slot = pos % TESS_TOTAL_SLOTS;
    res.side = ctx->side;
    res.distort = tess_distort(pos % TESS_TOTAL_SLOTS, ctx->pointer_pos % TESS_TOTAL_SLOTS, TESS_TOTAL_SLOTS);

    /* Cascade (rewind): activate if pointer near */
    if (res.distort < 5) {
        tess_cascade_activate(&ctx->cascade, (int)(pos % 64));
        ctx->cascade_active = 1;
        res.computed = 1;
    } else {
        ctx->cascade_active = 0;
        res.computed = 0;
    }

    /* Voronoi mask: if multi-pointer read requested */
    /* (handled separately via tess_voronoi_apply) */

    return res;
}

/* ── Jump sequence: outer → inner → snap ───────────────────────── */
/* geo_jump (outer) → hyper_jump (dims) → frame_seek (fishbone) → fan24 → jet → 144x144 */
static inline JumpResult jump_full_sequence(JumpContext *ctx, uint32_t param) {
    /* Step 1: geo_jump (outer) — basic jump */
    JumpResult r1 = jump_execute(ctx, JUMP_HILBERT, param);

    /* Step 2: hyper_jump (through dims) — 4D tower */
    JumpResult r2 = jump_execute(ctx, JUMP_HYPER, 1);

    /* Step 3: frame_seek (fishbone snap) — deterministic 1440 cycle */
    JumpResult r3 = jump_execute(ctx, JUMP_FRAME, 0);

    /* Step 4: fan24 (CRT ring-24) — small node anchors */
    JumpResult r4 = jump_execute(ctx, JUMP_FAN24, 0);

    /* Step 5: jet (snap to 144x144) — discrete leads */
    JumpResult r5 = jump_execute(ctx, JUMP_JET, 144);

    /* Final: snap to 144x144 format */
    uint32_t final_slot = r5.slot % TESS_TOTAL_SLOTS;
    JumpResult final_res = r5;
    final_res.slot = final_slot;
    final_res.cell = tess_slot_to_cell(final_slot);
    final_res.cube_id = final_slot / TESS_TOTAL_SLOTS;
    final_res.local_slot = final_slot % TESS_TOTAL_SLOTS;

    return final_res;
}

#endif /* TESS_JUMP_H */
