/* geo_hyper_jump.h — hyper_jump: geo_jump concept lifted to 4D tes-compound towers.
 *
 * 48-tes x 3 towers = 144-tess  (gidpith tower, mirrors stride-3)
 * 36-tes x 4 towers = 144-tess  (spic tower, mirrors stride-4)
 *
 * Metatron identity baked in: 48 visible + 16 residual = 64 full.
 * Entry/exit gates: pole-to-pole (layer 1 <-> layer 24 antipodal pair).
 *
 * Header-only, integer-only, stdint only. No build step.
 */
#ifndef GEO_HYPER_JUMP_H
#define GEO_HYPER_JUMP_H

#include <stdint.h>

#define HJ_TES48      48u   /* tes units per gidpith tower (Klitzing: 48xtes/gidpith) */
#define HJ_TES36      36u   /* tes units per spic tower (Klitzing: 36xtes/spic) */
#define HJ_TOWERS3    3u
#define HJ_TOWERS4    4u
#define HJ_TOTAL      144u  /* 48*3 == 36*4 == 144-tess */

#define HJ_METATRON_VIS    48u   /* visible units per block */
#define HJ_METATRON_RESID  16u   /* invisible residual (one metatron floor) */
#define HJ_METATRON_FULL   64u   /* 48 + 16 */

_Static_assert(HJ_TES48 * HJ_TOWERS3 == HJ_TOTAL, "48x3 must be 144");
_Static_assert(HJ_TES36 * HJ_TOWERS4 == HJ_TOTAL, "36x4 must be 144");
_Static_assert(HJ_METATRON_VIS + HJ_METATRON_RESID == HJ_METATRON_FULL, "48+16 must be 64");

/* tower / local split */
static inline uint32_t hj3_tower(uint32_t pos) { return (pos % HJ_TOTAL) / HJ_TES48; }
static inline uint32_t hj3_local(uint32_t pos) { return (pos % HJ_TOTAL) % HJ_TES48; }
static inline uint32_t hj4_tower(uint32_t pos) { return (pos % HJ_TOTAL) / HJ_TES36; }
static inline uint32_t hj4_local(uint32_t pos) { return (pos % HJ_TOTAL) % HJ_TES36; }

/* antipode within a span (involution): pos -> span-1-pos */
static inline uint32_t hj_antipode(uint32_t pos, uint32_t span) {
    return (span - 1u) - (pos % span);
}

/* jump: next tower (entry/exit gate) + mirror local (same shape as geo_jump JUMP_INVERT) */
static inline uint32_t hj3_jump(uint32_t pos) {
    uint32_t t = hj3_tower(pos);
    uint32_t next = (t + 1u) % HJ_TOWERS3;
    return next * HJ_TES48 + (HJ_TES48 - 1u - hj3_local(pos));
}

static inline uint32_t hj4_jump(uint32_t pos) {
    uint32_t t = hj4_tower(pos);
    uint32_t next = (t + 1u) % HJ_TOWERS4;
    return next * HJ_TES36 + (HJ_TES36 - 1u - hj4_local(pos));
}

/* span-parameterized jump: same tower+mirror rule at any scale.
 * Home (s=1.0) is span=48/towers=3; expand x2 is span=96/towers=3.
 * total = span * towers; all arithmetic mod total. */
static inline uint32_t hj_total(uint32_t span, uint32_t towers) {
    return span * towers;
}
static inline uint32_t hj_tower_span(uint32_t pos, uint32_t span, uint32_t towers) {
    return (pos % hj_total(span, towers)) / span;
}
static inline uint32_t hj_local_span(uint32_t pos, uint32_t span, uint32_t towers) {
    return (pos % hj_total(span, towers)) % span;
}
static inline uint32_t hj_jump_span(uint32_t pos, uint32_t span, uint32_t towers) {
    uint32_t t = hj_tower_span(pos, span, towers);
    uint32_t next = (t + 1u) % towers;
    return next * span + (span - 1u - hj_local_span(pos, span, towers));
}

#endif /* GEO_HYPER_JUMP_H */
