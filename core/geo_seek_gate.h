/*
 * geo_seek_gate.h — Unified Seek Router (Logic Gate)
 *
 * Selects the right seeker based on access pattern:
 *   Teleport  — coordinate jump (O(1), Cayley transform)
 *   Chord     — parallel access (3-4 addresses simultaneously)
 *   Tantrix   — directed routing (fabric path)
 *   RDH       — hash lookup (bond_key)
 *   Frame     — sequential walk (stride-37)
 *
 * BUILD: gcc -O2 -I. -o test_seek_gate test_seek_gate.c -lm
 */

#ifndef GEO_SEEK_GATE_H
#define GEO_SEEK_GATE_H

#include <stdint.h>
#include <stdbool.h>

/* ═══════════════════════════════════════════════════════
   SEEK MODES
   ═══════════════════════════════════════════════════════ */
typedef enum {
    SEEK_TELEPORT  = 0,  /* O(1) coordinate jump (Cayley)      */
    SEEK_CHORD     = 1,  /* O(1) parallel access (3-4 addrs)   */
    SEEK_TANTRIX   = 2,  /* O(1) directed routing (fabric)     */
    SEEK_RDH       = 3,  /* O(1) hash lookup (bond_key)        */
    SEEK_FRAME     = 4,  /* O(n) sequential walk (stride-37)   */
    SEEK_COUNT     = 5
} SeekMode;

/* ═══════════════════════════════════════════════════════
   ACCESS PATTERN (input to logic gate)
   ═══════════════════════════════════════════════════════ */
typedef struct {
    uint32_t coordinate;      /* target coordinate (if known)     */
    uint32_t bond_key;        /* bond_key for lookup (if known)   */
    uint8_t  quality;         /* chord quality (for parallel)     */
    uint8_t  incoming_gate;   /* Tantrix gate (for routing)       */
    bool     has_coordinate;  /* true if coordinate is valid      */
    bool     has_bond_key;    /* true if bond_key is valid        */
    bool     needs_parallel;  /* true if need 3-4 addresses       */
    bool     needs_routing;   /* true if need directed path       */
} AccessPattern;

/* ═══════════════════════════════════════════════════════
   LOGIC GATE: select seeker
   ═══════════════════════════════════════════════════════ */
static inline SeekMode seek_gate(const AccessPattern *ap) {
    /* Priority: Chord > Tantrix > RDH > Teleport > Frame */
    if (ap->needs_parallel)
        return SEEK_CHORD;

    if (ap->needs_routing)
        return SEEK_TANTRIX;

    if (ap->has_bond_key)
        return SEEK_RDH;

    if (ap->has_coordinate)
        return SEEK_TELEPORT;

    return SEEK_FRAME;
}

/* ═══════════════════════════════════════════════════════
   SEEK RESULT (output from seeker)
   ═══════════════════════════════════════════════════════ */
#define SEEK_MAX_ADDRS 4u

typedef struct {
    uint32_t addrs[SEEK_MAX_ADDRS];  /* resolved addresses           */
    uint8_t  count;                   /* actual address count (1-4)   */
    SeekMode mode;                    /* which seeker was used        */
    uint8_t  _pad[3];
} SeekResult;

/* ═══════════════════════════════════════════════════════
   UNIFIED SEEK INTERFACE
   ═══════════════════════════════════════════════════════ */
/*
 * geo_seek() — unified seek that routes to the right seeker
 *
 * ap: access pattern (input)
 * result: seek result (output)
 *
 * Returns 0 on success, -1 on error.
 *
 * Implementation dispatches to the appropriate seeker based on
 * the logic gate decision.
 */
static inline int geo_seek(const AccessPattern *ap, SeekResult *result) {
    if (!ap || !result) return -1;

    SeekMode mode = seek_gate(ap);
    result->mode = mode;
    result->count = 0;

    switch (mode) {
        case SEEK_TELEPORT: {
            /* Teleport: O(1) coordinate jump */
            result->addrs[0] = ap->coordinate;
            result->count = 1;
            break;
        }

        case SEEK_CHORD: {
            /* Chord: 3-4 parallel addresses */
            /* Requires geo_chord.h integration */
            /* For now: placeholder — compute 3 addresses from root */
            uint32_t root = ap->coordinate;
            uint8_t q = ap->quality;
            /* MAJOR intervals: 0, 4, 7 */
            static const uint8_t intervals[][4] = {
                {0, 4, 7, 0},  /* MAJOR */
                {0, 3, 7, 0},  /* MINOR */
                {0, 4, 7, 10}, /* DOM7  */
                {0, 4, 7, 11}, /* MAJ7  */
            };
            uint8_t n = (q < 2) ? 3 : 4;
            for (uint8_t i = 0; i < n; i++) {
                result->addrs[i] = (root + intervals[q][i]) % 20736;
            }
            result->count = n;
            break;
        }

        case SEEK_TANTRIX: {
            /* Tantrix: directed routing */
            /* Requires lc_tantrix.h integration */
            /* For now: placeholder — route through fabric */
            result->addrs[0] = ap->coordinate;
            result->count = 1;
            break;
        }

        case SEEK_RDH: {
            /* RDH: hash lookup by bond_key */
            /* Requires residual_space.h integration */
            /* For now: placeholder — return bond_key as address */
            result->addrs[0] = ap->bond_key % 20736;
            result->count = 1;
            break;
        }

        case SEEK_FRAME: {
            /* Frame: sequential walk (stride-37) */
            /* Requires geo_frame_seek.h integration */
            /* For now: placeholder — return frame_enc */
            result->addrs[0] = (ap->coordinate * 37) % 1440;
            result->count = 1;
            break;
        }

        default:
            return -1;
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════
   HELPER: create access patterns
   ═══════════════════════════════════════════════════════ */
static inline AccessPattern ap_teleport(uint32_t coord) {
    AccessPattern ap = {0};
    ap.coordinate = coord;
    ap.has_coordinate = true;
    return ap;
}

static inline AccessPattern ap_chord(uint32_t root, uint8_t quality) {
    AccessPattern ap = {0};
    ap.coordinate = root;
    ap.has_coordinate = true;
    ap.needs_parallel = true;
    ap.quality = quality;
    return ap;
}

static inline AccessPattern ap_tantrix(uint32_t coord, uint8_t gate) {
    AccessPattern ap = {0};
    ap.coordinate = coord;
    ap.has_coordinate = true;
    ap.needs_routing = true;
    ap.incoming_gate = gate;
    return ap;
}

static inline AccessPattern ap_rdh(uint32_t bond_key) {
    AccessPattern ap = {0};
    ap.bond_key = bond_key;
    ap.has_bond_key = true;
    return ap;
}

static inline AccessPattern ap_frame(uint32_t start) {
    AccessPattern ap = {0};
    ap.coordinate = start;
    ap.has_coordinate = true;
    return ap;
}

/* ═══════════════════════════════════════════════════════
   STATS
   ═══════════════════════════════════════════════════════ */
typedef struct {
    uint32_t seek_count;                   /* total seeks           */
    uint32_t mode_count[SEEK_COUNT];       /* per-mode seeks       */
    uint32_t total_addrs;                  /* total addresses resolved */
} SeekStats;

static inline void seek_stats_reset(SeekStats *s) {
    if (!s) return;
    s->seek_count = 0;
    s->total_addrs = 0;
    for (int i = 0; i < SEEK_COUNT; i++) s->mode_count[i] = 0;
}

static inline void seek_stats_record(SeekStats *s, const SeekResult *r) {
    if (!s || !r) return;
    s->seek_count++;
    s->mode_count[r->mode]++;
    s->total_addrs += r->count;
}

#endif /* GEO_SEEK_GATE_H */
