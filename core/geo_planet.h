/* ═══════════════════════════════════════════════════════════════════════════
 * geo_planet.h — Detached frame + entangle tail (design 2026-09-13, §134-142)
 * ═══════════════════════════════════════════════════════════════════════════
 * DETACH: a planet owns its frame (home, birth_W, digest). Main-field
 *   motion never touches it (bytes don't move; force hits pointer views).
 * ENTANGLE (safety): idle-zero tail. No bytes while healthy; error records
 *   on verify mismatch; replay walks main's fan24 FGLog on demand
 *   (planet holds birth_W + log ref, not a copy).
 * BIRTH-MAX: planet is born at its biggest (birth_W = main W at birth);
 *   W_now >= birth_W forever. Shrink/re-widen within bounds OK; expansion
 *   beyond birth REJECTED. Need bigger? Spawn, don't stretch.
 * OVERFLOW LIFECYCLE: tail full (8) + new mismatch -> AUTO-REANCHOR
 *   (adopt current bytes as new baseline, tail cleared, reanchors++,
 *   overflow scar persists). Mirrors breath-engine precedent (anchor
 *   follows data); audit preserved via counters, not silent.
 * TOMBSTONE: one 32B plate on retire {id,birth,death,home,origin,digest}
 *   (origin = birth key, digest = final key: audit across reanchor epochs) —
 *   region becomes self-describing (deposit vs graveyard = caller's call;
 *   default severed: reads after retire return -2).
 * V2 (RDH blueprint): 64-bit capture keys (bond-key shape), origin/final
 *   tomb pair (RDH origin_key precedent), fail-closed keyed thaw; policies
 *   (birth-max, idle-zero, detach, replay) unchanged.
 *
 * Scale unit = fan24 W tooth [0,144) (int-only, s = 2^(-W/12), W=0 full).
 * Header-only, int-only, no malloc. Owns no bytes (caller passes buffers).
 * Depends: fan24_gear.h (replay only).
 * ═══════════════════════════════════════════════════════════════════════════ */
#ifndef GEO_PLANET_H
#define GEO_PLANET_H

#include <stdint.h>
#include "fan24_gear.h"

#define PLANET_MAGIC      0x504C4E54u   /* "PLNT" */
#define PLANET_TOMB_MAGIC 0x544F4D42u   /* "TOMB" */
#define PLANET_TAIL_CAP   8u            /* error records max, then overflow flag */
#define PLANET_LINK_MASK  8u            /* voronoi radius: replay walks <= 8 events */
#define PLANET_LINK_CLOSE_AFTER_CLEAN 3u /* training wheels off: shut + drop
                                          * tail after 3 consecutive clean */

/* error record: collected ONLY on verify mismatch (idle = nothing) */
typedef struct {
    uint32_t w;          /* W tooth at detection */
    uint64_t expected;   /* baseline key */
    uint64_t observed;   /* recomputed key */
} PlanetErr;

/* tombstone plate: 32 bytes, written once on retire */
typedef struct {
    uint32_t magic;      /* PLANET_TOMB_MAGIC */
    uint32_t id;
    uint32_t birth_w;
    uint32_t death_w;
    uint32_t final_home;
    uint32_t reserved;
    uint64_t origin;     /* birth key (never moves) */
    uint64_t digest;     /* final key at retire */
} PlanetTomb;

typedef struct {
    uint32_t magic;      /* PLANET_MAGIC */
    uint32_t id;
    uint32_t home;       /* anchor in planet frame */
    uint32_t birth_w;    /* max scale point (W=0 full); W_now >= birth_W */
    uint32_t cur_w;      /* current W tooth */
    uint64_t origin;     /* birth key (audit across epochs) */
    uint64_t digest;     /* current baseline key */
    uint32_t violations; /* rejected expansions beyond birth */
    uint32_t retired;    /* 1 after planet_retire */
    uint32_t tail_n;     /* error records stored (0 while healthy) */
    uint32_t tail_overflow;
    uint32_t reanchors;    /* auto-reanchor epochs (tail-full adoptions) */
    uint32_t link_open;    /* entangle gate: 0 shut (default), 1 open.
                            * Opens ITSELF on mismatch (trouble = pointer
                            * lands, voronoi mask PLANET_LINK_MASK bounds
                            * the blast radius). Shuts itself after
                            * PLANET_LINK_CLOSE_AFTER_CLEAN clean. */
    uint32_t clean_streak; /* consecutive clean verifies */
    PlanetErr tail[PLANET_TAIL_CAP];
    PlanetTomb tomb;     /* valid after retire */
} Planet;

/* djb2-64 capture key (birth fingerprint of watched bytes, RDH shape) */
static inline uint64_t planet_digest(const int8_t *d, uint32_t n) {
    uint64_t h = 5381u;
    for (uint32_t i = 0; i < n; i++) h = h * 33u + (uint8_t)d[i];
    return h;
}

/* birth: born at current main W = its biggest. Always W=caller-supplied. */
static inline void planet_birth(Planet *p, uint32_t id, uint32_t w,
                                uint32_t home, const int8_t *d, uint32_t n) {
    if (!p) return;
    p->magic = PLANET_MAGIC;
    p->id = id;
    p->home = home;
    p->birth_w = w % FG_LOCAL;
    p->cur_w = p->birth_w;
    p->origin = d ? planet_digest(d, n) : 0u;
    p->digest = p->origin;
    p->violations = 0u;
    p->retired = 0u;
    p->tail_n = 0u;
    p->tail_overflow = 0u;
    p->reanchors = 0u;
    p->link_open = 0u;
    p->clean_streak = 0u;
    p->tomb.magic = 0u;
}

/* verify: 0 ok (tail untouched), 1 mismatch (collected + link OPENS
 * itself), 2 auto-reanchored (tail was full: baseline moved to current,
 * link open), -2 retired */
static inline int planet_verify(Planet *p, const int8_t *d, uint32_t n) {
    if (!p || p->magic != PLANET_MAGIC) return -1;
    if (p->retired) return -2;
    uint64_t obs = d ? planet_digest(d, n) : 0u;
    if (obs == p->digest) {
        /* clean: streak toward auto-close (training wheels off) */
        p->clean_streak++;
        if (p->clean_streak >= PLANET_LINK_CLOSE_AFTER_CLEAN) {
            p->link_open = 0u;
            p->tail_n = 0u;   /* drop tail, keep overflow scar for audit */
        }
        return 0;
    }
    /* trouble: pointer lands — gate opens itself, streak resets */
    p->link_open = 1u;
    p->clean_streak = 0u;
    if (p->tail_n < PLANET_TAIL_CAP) {
        p->tail[p->tail_n].w = p->cur_w;
        p->tail[p->tail_n].expected = p->digest;
        p->tail[p->tail_n].observed = obs;
        p->tail_n++;
        return 1;
    }
    /* tail full -> adopt current as new baseline (fresh epoch) */
    p->digest = obs;
    p->tail_n = 0u;
    p->tail_overflow = 1u;
    p->reanchors++;
    return 2;
}

/* shrink/re-widen within bounds: W_new >= birth_W. Beyond birth: reject. */
static inline int planet_shrink(Planet *p, uint32_t w_new) {
    if (!p || p->magic != PLANET_MAGIC || p->retired) return -1;
    w_new %= FG_LOCAL;
    if (w_new < p->birth_w) { p->violations++; return -1; }
    p->cur_w = w_new;
    return 0;
}

/* retire: write the one tombstone plate, sever by default. */
static inline void planet_retire(Planet *p, uint32_t death_w) {
    if (!p || p->magic != PLANET_MAGIC || p->retired) return;
    p->tomb.magic = PLANET_TOMB_MAGIC;
    p->tomb.id = p->id;
    p->tomb.birth_w = p->birth_w;
    p->tomb.death_w = death_w % FG_LOCAL;
    p->tomb.final_home = p->home;
    p->tomb.origin = p->origin;
    p->tomb.digest = p->digest;
    p->retired = 1u;
}

/* restore (deposit path): re-birth from a tombstone plate. Same soul
 * (id/home/digest), original max (birth_w) preserved. Strict continuity:
 *   -1 bad tomb/args, -2 body-changed (bytes differ from tomb digest:
 *      make a NEW planet instead), -3 scale violation (w_now < birth_w).
 * Fresh counters, tomb cleared (new life, old plate spent). */
static inline int planet_restore(Planet *p, const PlanetTomb *t,
                                 uint32_t w_now, const int8_t *d, uint32_t n) {
    if (!p || !t || t->magic != PLANET_TOMB_MAGIC) return -1;
    w_now %= FG_LOCAL;
    if (w_now < t->birth_w) return -3;
    uint64_t obs = d ? planet_digest(d, n) : 0u;
    if (obs != t->digest) return -2;
    p->magic = PLANET_MAGIC;
    p->id = t->id;
    p->home = t->final_home;
    p->birth_w = t->birth_w;
    p->cur_w = w_now;
    p->origin = t->origin;
    p->digest = obs;
    p->violations = 0u;
    p->retired = 0u;
    p->tail_n = 0u;
    p->tail_overflow = 0u;
    p->reanchors = 0u;
    p->link_open = 0u;      /* new life, gate shut until trouble */
    p->clean_streak = 0u;
    p->tomb.magic = 0u;
    return 0;
}

/* replay: walk main's FGLog tail from birth; 0 agrees with main_W_now,
 * 1 diverged, -1 no usable tail, -3 link shut (no trouble = no entry:
 * replay is the open gate's business only). Mask: walks at most
 * PLANET_LINK_MASK events from birth (voronoi radius — planet sees a
 * bounded window even when open). */
static inline int planet_replay(const Planet *p, const FGGearEv *ev,
                                uint32_t n_ev, uint32_t main_w_now) {
    if (!p || p->magic != PLANET_MAGIC || !ev) return -1;
    if (!p->link_open) return -3;
    if (n_ev > PLANET_LINK_MASK) n_ev = PLANET_LINK_MASK;
    uint32_t w = p->birth_w;
    for (uint32_t i = 0; i < n_ev; i++)
        w = (w + (uint32_t)ev[i].q * FG_RING + fg_crt(ev[i].dc, ev[i].dx)) % FG_LOCAL;
    return (w == (main_w_now % FG_LOCAL)) ? 0 : 1;
}

/* thaw (fail-closed keyed read): returns d iff key matches baseline AND
 * bytes recompute to it; NULL otherwise (retired, wrong key, corruption).
 * The read that refuses to lie — residual freeze/thaw shape. */
static inline const int8_t *planet_thaw(const Planet *p, uint64_t key,
                                        const int8_t *d, uint32_t n) {
    if (!p || p->magic != PLANET_MAGIC || p->retired) return 0;
    if (key != p->digest) return 0;
    if (!d || planet_digest(d, n) != p->digest) return 0;
    return d;
}

/* ═══════════════ 12-PENTAGON REGISTRY (face-spawn system) ═══════════════
 * One planet per pentagon face (faces 0..11, shared numbering with
 * geo_goldberg_frame.h): home = face*128 (face base in flat field).
 * Isolated sites -> independent frames; hex bulk stays on main field.
 * Additive only — single-planet API above untouched. */
#define PLANET_SYS_N 12u

typedef struct {
    Planet p[PLANET_SYS_N];
} PlanetSys;

/* birth all 12 over caller buffers d[i] (len n each), ids base_id+i */
static inline void planetsys_birth(PlanetSys *s, uint32_t base_id, uint32_t w,
                                   const int8_t **d, uint32_t n) {
    if (!s) return;
    for (uint32_t f = 0; f < PLANET_SYS_N; f++)
        planet_birth(&s->p[f], base_id + f, w, f * 128u, d ? d[f] : 0, n);
}

/* verify all: returns mismatches found (each collects into own tail) */
static inline uint32_t planetsys_verify(PlanetSys *s, const int8_t **d, uint32_t n) {
    uint32_t bad = 0;
    if (!s) return 0;
    for (uint32_t f = 0; f < PLANET_SYS_N; f++)
        if (planet_verify(&s->p[f], d ? d[f] : 0, n) > 0) bad++;
    return bad;
}

/* retire all at death_w (12 tombstones) */
static inline void planetsys_retire(PlanetSys *s, uint32_t death_w) {
    if (!s) return;
    for (uint32_t f = 0; f < PLANET_SYS_N; f++) planet_retire(&s->p[f], death_w);
}

#endif /* GEO_PLANET_H */
