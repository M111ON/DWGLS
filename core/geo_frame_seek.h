/* geo_frame_seek.h - Deterministic Frame Seek on Fibo 1440 Timeline */

#ifndef GEO_FRAME_SEEK_H
#define GEO_FRAME_SEEK_H

#include <stdint.h>

#define FRAME_CYCLE       1440u
#define FRAME_STRIDE        37u
#define FRAME_FACE_SZ      120u
#define FRAME_EDGES         12u
#define FRAME_H_ACTIVE       9u
#define FRAME_P_STEPS        4u
#define FRAME_ICO_NODES    162u
#define FRAME_PEANO_GRID    81u

typedef struct {
    uint8_t group;
    uint8_t edge;
    uint8_t is_skip;
} FrameHilbert;

typedef struct {
    uint8_t step;
    uint8_t sub;
    uint8_t hilbert_group;
} FramePeano;

typedef struct {
    FrameHilbert h;
    FramePeano   p;
    uint16_t     enc;
    uint8_t      ico_idx;
    uint8_t      face;
    uint8_t      slot;
    uint8_t      phase;
} DualFrame;

static inline DualFrame frame_at(uint16_t enc)
{
    DualFrame f;
    f.enc  = enc;
    f.face = (uint8_t)(enc / FRAME_FACE_SZ);
    f.slot = (uint8_t)(enc % FRAME_FACE_SZ);
    f.h.group   = (uint8_t)(f.face % 3);
    f.h.edge    = (uint8_t)(enc % 3);
    f.h.is_skip = (uint8_t)((enc % FRAME_EDGES) >= FRAME_H_ACTIVE);
    f.p.step          = (uint8_t)((enc / 3) % FRAME_P_STEPS);
    f.p.sub           = (uint8_t)(enc % 3);
    f.p.hilbert_group = (uint8_t)((enc / FRAME_EDGES) % 3);
    f.ico_idx = (uint8_t)(enc % FRAME_ICO_NODES);
    f.phase   = (uint8_t)((enc / FRAME_EDGES) % 12);
    return f;
}

static inline uint16_t frame_enc(uint32_t t)
{
    return (uint16_t)((t * FRAME_STRIDE) % FRAME_CYCLE);
}

static inline uint16_t frame_next(uint16_t enc)
{
    return (uint16_t)((enc + FRAME_STRIDE) % FRAME_CYCLE);
}

static inline uint16_t frame_prev(uint16_t enc)
{
    return (uint16_t)((enc + FRAME_CYCLE - FRAME_STRIDE) % FRAME_CYCLE);
}

static inline DualFrame frame_seek(uint32_t t)
{
    return frame_at(frame_enc(t));
}

static inline uint16_t frame_cpair(uint16_t enc)
{
    return (uint16_t)((enc + FRAME_CYCLE / 2u) % FRAME_CYCLE);
}

#define FRAME_MAX        120u
#define FRAME_MAX_SPAN    60u

static const uint8_t FIB_SPANS[4] = { 0, 1, 2, 3 };

typedef struct {
    uint8_t  home_frame;
    uint8_t  span;
    uint8_t  frame_lo;
    uint8_t  frame_hi;
} FrameRange;

static inline FrameRange frame_range(uint16_t enc, uint8_t entropy_class)
{
    FrameRange fr;
    fr.home_frame = (uint8_t)((enc / FRAME_EDGES) % FRAME_MAX);
    fr.span = FIB_SPANS[entropy_class & 3];
    int lo = (int)fr.home_frame - fr.span;
    int hi = (int)fr.home_frame + fr.span;
    if (lo < 0) lo += FRAME_MAX;
    if (hi >= (int)FRAME_MAX) hi -= FRAME_MAX;
    fr.frame_lo = (uint8_t)((lo + FRAME_MAX) % FRAME_MAX);
    fr.frame_hi = (uint8_t)((hi + FRAME_MAX) % FRAME_MAX);
    return fr;
}

static inline FrameRange frame_range_adaptive(uint16_t enc, uint8_t entropy_score)
{
    FrameRange fr;
    fr.home_frame = (uint8_t)((enc / FRAME_EDGES) % FRAME_MAX);
    static const uint8_t FIB[] = { 0,1,1,2,3,5,8,13,21,34,55,89 };
    static const int N_FIB = 12;
    int fib_idx = (int)entropy_score * (N_FIB - 1) / 255;
    if (fib_idx >= N_FIB) fib_idx = N_FIB - 1;
    fr.span = FIB[fib_idx];
    if (fr.span >= FRAME_MAX_SPAN) fr.span = FRAME_MAX_SPAN - 1;
    int lo = (int)fr.home_frame - fr.span;
    int hi = (int)fr.home_frame + fr.span;
    if (lo < 0) lo += FRAME_MAX;
    if (hi >= (int)FRAME_MAX) hi -= FRAME_MAX;
    fr.frame_lo = (uint8_t)((lo + FRAME_MAX) % FRAME_MAX);
    fr.frame_hi = (uint8_t)((hi + FRAME_MAX) % FRAME_MAX);
    return fr;
}

static inline int frame_in_range(uint16_t enc, uint16_t home_enc, uint8_t entropy_class)
{
    FrameRange fr = frame_range(home_enc, entropy_class);
    uint8_t test_frame = (uint8_t)((enc / FRAME_EDGES) % FRAME_MAX);
    if (fr.span == 0) return test_frame == fr.home_frame;
    int diff = (int)test_frame - (int)fr.home_frame;
    int half = (int)(FRAME_MAX / 2);
    if (diff > half) diff -= (int)FRAME_MAX;
    if (diff < -half) diff += (int)FRAME_MAX;
    return diff >= -(int)fr.span && diff <= (int)fr.span;
}

/* Self-verification: stride-37 walk, 1440-cycle, all invariants.
 * Restored from collection/geo_frame_seek.h — required by fgls_cli.c
 * (frmd_encode guard + `fgls frame-seek-verify` command). */
static inline int geo_frame_seek_verify(void)
{
    /* [T1] stride-37 full cycle on 1440 */
    uint16_t visited[1440] = {0};
    uint16_t e = 0;
    for (uint32_t i = 0; i < FRAME_CYCLE; i++) {
        if (visited[e]) return -1;   /* duplicate */
        visited[e] = 1;
        e = frame_next(e);
    }
    if (e != 0) return -2;           /* must return to start */

    /* [T2] frame_enc / frame_at roundtrip */
    for (uint32_t t = 0; t < 1440u; t++) {
        DualFrame f = frame_seek(t);
        if (f.enc != frame_enc(t))   return -3;
        if (f.face > 11)             return -4;
        if (f.h.group > 2)           return -5;
        if (f.h.edge  > 2)           return -6;
        if (f.p.step  >= FRAME_P_STEPS) return -7;
        if (f.p.sub   > 2)           return -8;
        if (f.ico_idx >= FRAME_ICO_NODES) return -9;
    }

    /* [T3] cpair self-inverse */
    for (uint16_t enc = 0; enc < FRAME_CYCLE; enc++) {
        if (frame_cpair(frame_cpair(enc)) != enc) return -10;
    }

    /* [T4] prev(next(enc)) == enc */
    for (uint16_t enc = 0; enc < FRAME_CYCLE; enc++) {
        if (frame_prev(frame_next(enc)) != enc) return -11;
    }

    /* [T5] 9+3=12 edges: is_skip count over 12 consecutive */
    uint32_t skip_count = 0, active_count = 0;
    for (uint16_t enc = 0; enc < FRAME_EDGES; enc++) {
        DualFrame f = frame_at(enc);
        if (f.h.is_skip) skip_count++;
        else             active_count++;
    }
    if (active_count != FRAME_H_ACTIVE) return -12;  /* must be 9 */
    if (skip_count   != 3u)             return -13;  /* must be 3 */

    /* [T6] phase cycles 0..11 over 144 frames */
    for (uint16_t enc = 0; enc < 144u; enc++) {
        DualFrame f = frame_at(enc);
        if (f.phase >= 12u) return -14;
    }

    return 0;
}

#endif
