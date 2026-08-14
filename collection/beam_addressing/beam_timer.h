/*
 * beam_timer.h — Beam Timer: step+tick based on fibo_spine
 * ═══════════════════════════════════════════════════════════════════
 *
 * NEW TIMER — separate from frame_seek enc (1440)
 *
 * Base-12 structure (duodecimal):
 *   12⁴ = 20736  ← total field
 *   12³ = 1728   ← pipes (steps)
 *   12² = 144    ← pipe clusters
 *   12¹ = 12     ← ticks per pipe
 *
 * Naming convention:
 *   tick     (Fib1)  0..11       — time dimension
 *   cluster  (Fib2)  0..143      — spatial grouping (for capo)
 *   pipe     (Fib3)  0..1727     — spatial position
 *   slot     (Fib4)  0..20735    — linear index
 *
 * This header provides O(1) beam timer operations:
 *   slot_index ↔ (pipe, tick)
 *   beam_coord → timer (pipe, tick, slot)
 *   timer → beam_coord (roundtrip)
 *
 * All header-only, static inline, no malloc in hot path.
 * ═══════════════════════════════════════════════════════════════════
 */

#ifndef BEAM_TIMER_H
#define BEAM_TIMER_H

#include <stdint.h>

/* ══════════════════════════════════════════════════════════════
   CONSTANTS — spine-based timer
   ══════════════════════════════════════════════════════════════ */

#define BT_STEPS          1728u    /* pipes: 12 × 144 = 1728       */
#define BT_TICKS          12u      /* ticks per step: 0..11         */
#define BT_SLOTS          (BT_STEPS * BT_TICKS)  /* 20736          */
#define BT_JET_TICK       11u      /* Jet Bridge trigger tick       */
#define BT_BARRIER_TICK   0u       /* barrier sync tick             */

/* ══════════════════════════════════════════════════════════════
   TIMER STRUCT
   ══════════════════════════════════════════════════════════════ */

typedef struct {
    uint16_t pipe;        /* pipe: 0..1727 (Fib3) */
    uint8_t  tick;        /* 0..11 (Fib1) */
    uint32_t slot_index;  /* linear: 0..20735 (Fib4) */
} BeamTimer;

/* ══════════════════════════════════════════════════════════════
   CORE: slot_index ↔ (pipe, tick)
   ══════════════════════════════════════════════════════════════ */

/* (pipe, tick) → slot_index */
static inline uint32_t bt_slot_index(uint16_t pipe, uint8_t tick)
{
    return (uint32_t)pipe * BT_TICKS + tick;
}

/* slot_index → (pipe, tick) */
static inline BeamTimer bt_from_slot(uint32_t slot)
{
    BeamTimer t;
    t.pipe = (uint16_t)(slot / BT_TICKS);
    t.tick = (uint8_t)(slot % BT_TICKS);
    t.slot_index = slot;
    return t;
}

/* ══════════════════════════════════════════════════════════════
   TIMER NAVIGATION — advance / rewind
   ══════════════════════════════════════════════════════════════ */

/* next tick (wraps to next step) */
static inline BeamTimer bt_next(BeamTimer t)
{
    t.tick++;
    if (t.tick >= BT_TICKS) {
        t.tick = 0;
        t.pipe++;
        if (t.pipe >= BT_STEPS) t.pipe = 0;
    }
    t.slot_index = bt_slot_index(t.pipe, t.tick);
    return t;
}

/* prev tick (wraps to prev step) */
static inline BeamTimer bt_prev(BeamTimer t)
{
    if (t.tick == 0) {
        t.tick = BT_TICKS - 1;
        if (t.pipe == 0) t.pipe = BT_STEPS - 1;
        else             t.pipe--;
    } else {
        t.tick--;
    }
    t.slot_index = bt_slot_index(t.pipe, t.tick);
    return t;
}

/* advance N ticks */
static inline BeamTimer bt_advance(BeamTimer t, uint32_t n)
{
    uint32_t slot = (t.slot_index + n) % BT_SLOTS;
    return bt_from_slot(slot);
}

/* ══════════════════════════════════════════════════════════════
   BEAM COORD → TIMER
   ══════════════════════════════════════════════════════════════
 *
 * Map beam coordinate to step+tick timer.
 * Uses param_index as slot source.
 */

/* beam param_index → timer */
static inline BeamTimer bt_from_param(uint32_t param_index)
{
    uint32_t slot = param_index % BT_SLOTS;
    return bt_from_slot(slot);
}

/* beam param_index → step */
static inline uint16_t bt_param_to_pipe(uint32_t param_index)
{
    return (uint16_t)((param_index / BT_TICKS) % BT_STEPS);
}

/* beam param_index → tick */
static inline uint8_t bt_param_to_tick(uint32_t param_index)
{
    return (uint8_t)(param_index % BT_TICKS);
}

/* ══════════════════════════════════════════════════════════════
   TIMER → BEAM COORD (roundtrip)
   ══════════════════════════════════════════════════════════════ */

/* timer → param_index */
static inline uint32_t bt_to_param(BeamTimer t)
{
    return t.slot_index;
}

/* ══════════════════════════════════════════════════════════════
   BASE-12 DECOMPOSITION — duodecimal coordinates
   ══════════════════════════════════════════════════════════════
 *
 * 12⁴ = 20736  →  (d4, d3, d2, d1)  where d1=LSB
 *   d1 = tick     (12⁰ = 1)  — time dimension
 *   d2 = cluster  (12¹ = 12) — spatial grouping
 *   d3 = pipe     (12² = 144) — spatial position
 *   d4 = field    (12³ = 1728) — top-level partition
 */

typedef struct {
    uint8_t  d1;    /* tick:    0..11   (12⁰) */
    uint8_t  d2;    /* cluster: 0..11   (12¹) */
    uint8_t  d3;    /* pipe:    0..11   (12²) */
    uint8_t  d4;    /* field:   0..11   (12³) */
} Base12;

/* slot → base-12 */
static inline Base12 bt_to_base12(uint32_t slot)
{
    Base12 b;
    slot = slot % BT_SLOTS;
    b.d1 = (uint8_t)(slot % 12);            slot /= 12;
    b.d2 = (uint8_t)(slot % 12);            slot /= 12;
    b.d3 = (uint8_t)(slot % 12);            slot /= 12;
    b.d4 = (uint8_t)(slot % 12);
    return b;
}

/* base-12 → slot */
static inline uint32_t bt_from_base12(Base12 b)
{
    return (uint32_t)b.d1
         + (uint32_t)b.d2 * 12
         + (uint32_t)b.d3 * 144
         + (uint32_t)b.d4 * 1728;
}

/* verify base-12 roundtrip */
static inline int bt_base12_verify(void)
{
    for (uint32_t s = 0; s < BT_SLOTS; s++) {
        Base12 b = bt_to_base12(s);
        if (b.d1 > 11 || b.d2 > 11 || b.d3 > 11 || b.d4 > 11) return -1;
        uint32_t back = bt_from_base12(b);
        if (back != s) return -2;
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════════
   TIMER PROPERTIES
   ══════════════════════════════════════════════════════════════ */

/* is barrier tick? (tick 0) */
static inline int bt_is_barrier(BeamTimer t)
{
    return t.tick == BT_BARRIER_TICK;
}

/* is jet bridge tick? (tick 11) */
static inline int bt_is_bridge(BeamTimer t)
{
    return t.tick == BT_JET_TICK;
}

/* is pipe room? (tick 2..10) */
static inline int bt_is_pipe_room(BeamTimer t)
{
    return t.tick >= 2 && t.tick <= 10;
}

/* ══════════════════════════════════════════════════════════════
   TIMER SCATTER — distribute params across steps
   ══════════════════════════════════════════════════════════════
 *
 * Different scatter strategies for different use cases.
 */

/* scatter 1: param_index → step (direct modulo) */
static inline uint16_t bt_scatter_mod(uint32_t param_index)
{
    return (uint16_t)(param_index % BT_STEPS);
}

/* scatter 2: param_index → step (stride prime for better distribution) */
static inline uint16_t bt_scatter_stride(uint32_t param_index)
{
    /* stride 37 (same as frame_seek), gcd(37,1728)=1 for full cycle */
    return (uint16_t)((param_index * 37u) % BT_STEPS);
}

/* scatter 3: param_index → step (fibonacci scatter) */
static inline uint16_t bt_scatter_fibo(uint32_t param_index)
{
    /* fibo-like: (idx * 144) % 1728 — aligns with 12×144 structure */
    return (uint16_t)((param_index * 144u) % BT_STEPS);
}

/* ══════════════════════════════════════════════════════════════
   TIMER DISTRIBUTION — stats
   ══════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t pipe_counts[BT_STEPS];  /* per-pipe counts */
    uint32_t tick_counts[BT_TICKS];  /* per-tick counts */
    uint32_t total;
    uint16_t min_pipe;
    uint16_t max_pipe;
    uint8_t  min_tick;
    uint8_t  max_tick;
} BTStats;

static inline BTStats bt_compute_stats(const uint32_t *param_indices,
                                        uint32_t count)
{
    BTStats s = {0};
    s.total = count;
    s.min_pipe = BT_STEPS;
    s.max_pipe = 0;
    s.min_tick = BT_TICKS;
    s.max_tick = 0;
    
    for (uint32_t i = 0; i < count; i++) {
        uint16_t pipe = bt_param_to_pipe(param_indices[i]);
        uint8_t tick = bt_param_to_tick(param_indices[i]);
        s.pipe_counts[pipe]++;
        s.tick_counts[tick]++;
        if (pipe < s.min_pipe) s.min_pipe = pipe;
        if (pipe > s.max_pipe) s.max_pipe = pipe;
        if (tick < s.min_tick) s.min_tick = tick;
        if (tick > s.max_tick) s.max_tick = tick;
    }
    
    return s;
}

/* ══════════════════════════════════════════════════════════════
   VERIFY — call once at init, returns 0 on pass
   ══════════════════════════════════════════════════════════════ */

static inline int beam_timer_verify(void)
{
    /* T1: slot_index roundtrip */
    for (uint32_t s = 0; s < BT_SLOTS; s++) {
        BeamTimer t = bt_from_slot(s);
        uint32_t back = bt_slot_index(t.pipe, t.tick);
        if (back != s) return -1;
    }
    
    /* T2: step/tick range */
    for (uint32_t s = 0; s < BT_SLOTS; s++) {
        BeamTimer t = bt_from_slot(s);
        if (t.pipe >= BT_STEPS) return -2;
        if (t.tick >= BT_TICKS) return -3;
    }
    
    /* T3: next/prev roundtrip */
    for (uint32_t s = 0; s < BT_SLOTS; s++) {
        BeamTimer t = bt_from_slot(s);
        BeamTimer n = bt_next(t);
        BeamTimer p = bt_prev(n);
        if (p.pipe != t.pipe || p.tick != t.tick) return -4;
    }
    
    /* T4: advance roundtrip */
    for (uint32_t s = 0; s < 100; s++) {
        BeamTimer t = bt_from_slot(s);
        BeamTimer a = bt_advance(t, 37);
        BeamTimer b = bt_advance(a, BT_SLOTS - 37);
        if (b.pipe != t.pipe || b.tick != t.tick) return -5;
    }
    
    /* T5: barrier detection */
    for (uint16_t step = 0; step < BT_STEPS; step++) {
        BeamTimer t = bt_from_slot(bt_slot_index(step, 0));
        if (!bt_is_barrier(t)) return -6;
        if (bt_is_bridge(t)) return -7;
    }
    
    /* T6: bridge detection */
    for (uint16_t step = 0; step < BT_STEPS; step++) {
        BeamTimer t = bt_from_slot(bt_slot_index(step, BT_JET_TICK));
        if (!bt_is_bridge(t)) return -8;
        if (bt_is_barrier(t)) return -9;
    }
    
    /* T7: param_index → timer roundtrip */
    for (uint32_t i = 0; i < 1000; i++) {
        BeamTimer t = bt_from_param(i);
        uint32_t back = bt_to_param(t);
        if (back != (i % BT_SLOTS)) return -10;
    }
    
    /* T8: total slots = 20736 */
    if (BT_SLOTS != 20736) return -11;
    
    /* T9: steps = 1728 */
    if (BT_STEPS != 1728) return -12;
    
    /* T10: ticks = 12 */
    if (BT_TICKS != 12) return -13;
    
    /* T11: base-12 roundtrip (20736 slots) */
    if (bt_base12_verify() != 0) return -14;
    
    /* T12: base-12 example — slot 20735 = (11,11,11,11) */
    {
        Base12 b = bt_to_base12(20735);
        if (b.d1 != 11 || b.d2 != 11 || b.d3 != 11 || b.d4 != 11) return -15;
        uint32_t back = bt_from_base12(b);
        if (back != 20735) return -16;
    }
    
    /* T13: base-12 example — slot 0 = (0,0,0,0) */
    {
        Base12 b = bt_to_base12(0);
        if (b.d1 != 0 || b.d2 != 0 || b.d3 != 0 || b.d4 != 0) return -17;
    }
    
    return 0; /* ALL PASS */
}

#endif /* BEAM_TIMER_H */
