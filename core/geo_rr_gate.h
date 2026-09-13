/*
 * geo_rr_gate.h — Minimal Rotation-Reversal Gate
 * ═══════════════════════════════════════════════════════════════════════════════
 *
 * Detects rotation reversal in a Hilbert walk sequence.
 *
 * Context: L-block rotation at position d is determined by the Hilbert curve
 * direction entering d from d-1. Rotations cycle {0,1,2,3} (right/down/left/up)
 * as the curve winds through the grid. A reversal occurs when the rotation
 * sequence breaks its expected cycle — the curve changes winding direction.
 *
 * Gate pattern (follows geo_phase_rail.h convention):
 *   rr_gate(rot_prev, rot_curr, rot_next) → RRState
 *
 *   RR_FORWARD  = rotation continues in same direction
 *   RR_REVERSED = rotation changed direction (reversal detected)
 *   RR_HOLD     = rotation unchanged (stall or boundary)
 *
 * Sacred constants:
 *   RR_ROTS = 4 (rotation count, Hilbert is 4-directional)
 *
 * Design:
 *   No malloc. No float. Stateless O(1). Header-only. C99.
 *   Depends: <stdint.h> only
 * ═══════════════════════════════════════════════════════════════════════════════
 */
#ifndef GEO_RR_GATE_H
#define GEO_RR_GATE_H

#include <stdint.h>

/* ═══════════════════════════════════════════════════════════════════════════════
   CONSTANTS
   ═══════════════════════════════════════════════════════════════════════════════ */

#define RR_ROTS 4u   /* Hilbert curve: 4 directions = 4 rotations */

/* ═══════════════════════════════════════════════════════════════════════════════
   STATE ENUM
   ═══════════════════════════════════════════════════════════════════════════════ */

typedef enum {
    RR_HOLD     = 0,   /* rotation unchanged (stall, boundary, or same dir) */
    RR_FORWARD  = 1,   /* rotation continues in same direction             */
    RR_REVERSED = 2    /* rotation changed direction (reversal detected)    */
} RRState;

/* ═══════════════════════════════════════════════════════════════════════════════
   SIGNED ROTATION DISTANCE (mod 4, centered)
   ═══════════════════════════════════════════════════════════════════════════════
   Returns signed distance from a to b in [-2, +2]:
     +1 = one step forward (clockwise)
     -1 = one step backward (counter-clockwise)
      0 = same rotation
     ±2 = diametrically opposite (180° on 4-rot circle)
   ═══════════════════════════════════════════════════════════════════════════════ */

static inline int8_t rr_rot_dist(uint8_t a, uint8_t b) {
    int8_t d = (int8_t)((b - a + RR_ROTS) % RR_ROTS);
    if (d > 2) d -= RR_ROTS;   /* center: 3 → -1, 2 stays ±2 */
    return d;
}

/* ═══════════════════════════════════════════════════════════════════════════════
   CORE GATE: 3-rotation window → state
   ═══════════════════════════════════════════════════════════════════════════════
   rr_gate(prev, curr, next):
     Computes signed step from prev→curr (step1) and curr→next (step2).
     If step1 == 0 and step2 == 0 → HOLD (no rotation happening)
     If step1 == 0 and step2 != 0 → FORWARD (starting to move)
     If step1 != 0 and step2 == 0 → HOLD (stopped)
     If step1 != 0 and step2 == step1 → FORWARD (same direction)
     If step1 != 0 and step2 == -step1 → REVERSED (direction changed)
     If step1 != 0 and step2 != step1 and step2 != -step1 → REVERSED (any change)
   ═══════════════════════════════════════════════════════════════════════════════ */

static inline RRState rr_gate(uint8_t prev, uint8_t curr, uint8_t next) {
    int8_t step1 = rr_rot_dist(prev, curr);
    int8_t step2 = rr_rot_dist(curr, next);

    if (step1 == 0 && step2 == 0) return RR_HOLD;       /* no movement */
    if (step1 == 0)               return RR_FORWARD;     /* starting */
    if (step2 == 0)               return RR_HOLD;         /* stopping */
    if (step1 == step2)           return RR_FORWARD;     /* same direction */
    return RR_REVERSED;                                   /* any change = reversal */
}

/* ═══════════════════════════════════════════════════════════════════════════════
   2-ROTATION GATE (simpler, for pairwise comparison)
   ═══════════════════════════════════════════════════════════════════════════════
   rr_pair_gate(a, b):
     0 → HOLD (same rotation)
     +1 → FORWARD (b is one step clockwise from a)
     -1 → FORWARD (b is one step counter-clockwise from a — still "forward" in
                    the sense that movement occurred)
     ±2 → REVERSED (diametrically opposite — likely reversal)

   For directional detection, use rr_rot_dist directly.
   ═══════════════════════════════════════════════════════════════════════════════ */

static inline RRState rr_pair_gate(uint8_t a, uint8_t b) {
    int8_t d = rr_rot_dist(a, b);
    if (d == 0)  return RR_HOLD;
    if (d == 2 || d == -2) return RR_REVERSED;
    return RR_FORWARD;
}

/* ═══════════════════════════════════════════════════════════════════════════════
   SEQUENCE GATE: walk an entire rotation sequence
   ═══════════════════════════════════════════════════════════════════════════════
   Counts reversals in a rotation sequence.
   rots: rotation values (0..3)
   n: sequence length
   Returns: number of reversal points (index where rr_gate → REVERSED)
   ═══════════════════════════════════════════════════════════════════════════════ */

static inline uint32_t rr_count_reversals(const uint8_t *rots, uint32_t n) {
    if (n < 3) return 0;
    uint32_t count = 0;
    for (uint32_t i = 1; i + 1 < n; i++) {
        if (rr_gate(rots[i-1], rots[i], rots[i+1]) == RR_REVERSED)
            count++;
    }
    return count;
}

/* ═══════════════════════════════════════════════════════════════════════════════
   VERIFICATION
   ═══════════════════════════════════════════════════════════════════════════════ */

/*
 * rr_verify_constants — sacred constant check.
 * Returns 0 on success.
 */
static inline int rr_verify_constants(void) {
    if (RR_ROTS != 4u) return -1;
    return 0;
}

/*
 * rr_verify_rot_dist — rot_dist bijection over all rotation pairs.
 * Returns 0 on success.
 */
static inline int rr_verify_rot_dist(void) {
    for (uint8_t a = 0; a < RR_ROTS; a++) {
        for (uint8_t b = 0; b < RR_ROTS; b++) {
            int8_t d = rr_rot_dist(a, b);
            /* roundtrip: a + d ≡ b (mod 4) */
            uint8_t reconstructed = (uint8_t)((a + d + RR_ROTS) % RR_ROTS);
            if (reconstructed != b) return -1;
            /* bounds: d ∈ [-2, +2] */
            if (d < -2 || d > 2) return -2;
        }
    }
    return 0;
}

/*
 * rr_verify_gate — gate consistency over all rotation triples.
 * Returns 0 on success.
 */
static inline int rr_verify_gate(void) {
    for (uint8_t a = 0; a < RR_ROTS; a++) {
        for (uint8_t b = 0; b < RR_ROTS; b++) {
            for (uint8_t c = 0; c < RR_ROTS; c++) {
                RRState s = rr_gate(a, b, c);
                if (s != RR_HOLD && s != RR_FORWARD && s != RR_REVERSED)
                    return -1;

                /* Symmetry: if a==b==c → must be HOLD */
                if (a == b && b == c && s != RR_HOLD) return -2;

                /* If a==b and b!=c → must be FORWARD (starting) */
                if (a == b && b != c && s != RR_FORWARD) return -3;
            }
        }
    }
    return 0;
}

#endif /* GEO_RR_GATE_H */
