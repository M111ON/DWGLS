/* ═══════════════════════════════════════════════════════════════════════════
 * geo_fs_voronoi.h — Voronoi Cell Cache for GeoFS
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * Adaptive access layer: decides what stays hot vs cold.
 *
 * "Structure (geometry) + Data (DW container) + Access (Voronoi)"
 *
 * Cell cache = fixed-size array of hot cells (LRU by geometric address).
 * When cache is full → collapse least-recently-used cell.
 * Subdivision = when a cell is accessed, split into smaller cells.
 * Merge = when idle, combine back into larger cell.
 *
 * All operations O(1). No malloc in hot path. Header-only.
 *
 * DEPENDS:
 *   geo_cube_addr.h  — (generation, face, slot) addressing
 *   geo_cell_addr.h  — O(1) flat_id ↔ (pipe_id, tick)
 *   geo_cube_in_dodeca.h — 8 cell types
 *   infra/gear_lock.h — GEAR_GEO_FULL = 20736
 *
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifndef GEO_FS_VORONOI_H
#define GEO_FS_VORONOI_H

#include <stdint.h>
#include <string.h>
#include "geo_cube_addr.h"
#include "geo_cell_addr.h"
#include "infra/gear_lock.h"

/* ═══════════════════════════════════════════════════════════════
   CONSTANTS
   ═══════════════════════════════════════════════════════════════ */

#define VORONOI_MAX_CELLS    64u    /* hot cells in cache */
#define VORONOI_ADDR_SPACE   20736u /* GEAR_GEO_FULL */
#define VORONOI_TICK_CYCLE   12u    /* KIS ticks per cycle */
#define VORONOI_MAX_DEPTH    8u     /* max subdivision depth */

/* ═══════════════════════════════════════════════════════════════
   CELL STATE
   ═══════════════════════════════════════════════════════════════ */

typedef enum {
    VORONOI_CELL_EMPTY    = 0,  /* slot unused */
    VORONOI_CELL_ACTIVE   = 1,  /* hot — recently accessed */
    VORONOI_CELL_COLD     = 2,  /* cold — waiting for collapse */
    VORONOI_CELL_FROZEN   = 3,  /* frozen — pinned, never collapse */
    VORONOI_CELL_SUBDIV   = 4,  /* subdivided — split into children */
} VoronoiCellState;

typedef struct {
    uint32_t  flat_id;       /* address in [0, 20736) */
    uint8_t   cell_type;     /* 3-bit parity (0-7) */
    uint8_t   depth;         /* subdivision depth (0 = top level) */
    uint8_t   state;         /* VoronoiCellState */
    uint8_t   entropy_tier;  /* 0-3 from adaptive_tier() */
    uint32_t  access_count;  /* total accesses */
    uint32_t  last_tick;     /* KIS tick of last access */
    uint32_t  parent_id;     /* parent cell flat_id (0xFFFFFFFF = root) */
    uint32_t  child_ids[4];  /* children if subdivided (0xFFFFFFFF = none) */
    uint32_t  data_offset;   /* byte offset into backing store */
    uint32_t  data_size;     /* byte size of this cell's data */
} VoronoiCell;

/* ═══════════════════════════════════════════════════════════════
   VORONOI CACHE
   ═══════════════════════════════════════════════════════════════ */

typedef struct {
    VoronoiCell cells[VORONOI_MAX_CELLS];
    uint32_t    count;        /* active cells in cache */
    uint32_t    tick;         /* current KIS tick (drives LRU) */
    uint32_t    hits;         /* cache hits */
    uint32_t    misses;       /* cache misses */
    uint32_t    collapses;    /* times a cell was collapsed */
    uint32_t    subdivisions; /* times a cell was subdivided */
} VoronoiCache;

/* ═══════════════════════════════════════════════════════════════
   INIT
   ═══════════════════════════════════════════════════════════════ */

static inline void voronoi_init(VoronoiCache *vc) {
    if (!vc) return;
    memset(vc, 0, sizeof(*vc));
    for (uint32_t i = 0; i < VORONOI_MAX_CELLS; i++) {
        vc->cells[i].state = VORONOI_CELL_EMPTY;
        vc->cells[i].flat_id = 0xFFFFFFFF;
        vc->cells[i].parent_id = 0xFFFFFFFF;
        for (int c = 0; c < 4; c++)
            vc->cells[i].child_ids[c] = 0xFFFFFFFF;
    }
}

/* ═══════════════════════════════════════════════════════════════
   LOOKUP — find cell by flat_id
   ═══════════════════════════════════════════════════════════════ */

static inline VoronoiCell* voronoi_lookup(VoronoiCache *vc, uint32_t flat_id) {
    if (!vc) return NULL;
    flat_id %= VORONOI_ADDR_SPACE;

    for (uint32_t i = 0; i < VORONOI_MAX_CELLS; i++) {
        if (vc->cells[i].state != VORONOI_CELL_EMPTY &&
            vc->cells[i].flat_id == flat_id) {
            vc->hits++;
            vc->cells[i].access_count++;
            vc->cells[i].last_tick = vc->tick;
            return &vc->cells[i];
        }
    }
    vc->misses++;
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════
   EVICT — find LRU cell (oldest last_tick, not frozen)
   ═══════════════════════════════════════════════════════════════ */

static inline int voronoi_evict_lru(VoronoiCache *vc) {
    int oldest = -1;
    uint32_t oldest_tick = 0xFFFFFFFF;

    for (uint32_t i = 0; i < VORONOI_MAX_CELLS; i++) {
        VoronoiCell *c = &vc->cells[i];
        if (c->state == VORONOI_CELL_EMPTY) {
            return (int)i;  /* empty slot, use it directly */
        }
        if (c->state == VORONOI_CELL_FROZEN) continue;
        if (c->last_tick < oldest_tick) {
            oldest_tick = c->last_tick;
            oldest = (int)i;
        }
    }
    return oldest;
}

/* ═══════════════════════════════════════════════════════════════
   INSERT — add cell to cache (may evict LRU)
   ═══════════════════════════════════════════════════════════════ */

static inline VoronoiCell* voronoi_insert(VoronoiCache *vc,
                                            uint32_t flat_id,
                                            uint8_t entropy_tier,
                                            uint32_t data_offset,
                                            uint32_t data_size) {
    if (!vc) return NULL;
    flat_id %= VORONOI_ADDR_SPACE;

    /* Check if already present */
    VoronoiCell *existing = voronoi_lookup(vc, flat_id);
    if (existing) return existing;

    /* Find slot */
    int slot = voronoi_evict_lru(vc);
    if (slot < 0) return NULL;  /* all frozen, can't evict */

    vc->count++;

    VoronoiCell *c = &vc->cells[slot];
    GeoCellAddr addr = geo_cell_addr_from_offset(flat_id);

    c->flat_id     = flat_id;
    c->cell_type   = addr.cell_type;
    c->depth       = 0;
    c->state       = VORONOI_CELL_ACTIVE;
    c->entropy_tier = entropy_tier;
    c->access_count = 1;
    c->last_tick   = vc->tick;
    c->parent_id   = 0xFFFFFFFF;
    for (int i = 0; i < 4; i++) c->child_ids[i] = 0xFFFFFFFF;
    c->data_offset = data_offset;
    c->data_size   = data_size;

    return c;
}

/* ═══════════════════════════════════════════════════════════════
   SUBDIVIDE — split a cell into 4 children (Voronoi subdivision)
   ═══════════════════════════════════════════════════════════════
   Each child gets 1/4 of parent's data range.
   Parent becomes VORONOI_CELL_SUBDIV.
   ═══════════════════════════════════════════════════════════════ */

static inline int voronoi_subdivide(VoronoiCache *vc, uint32_t parent_flat_id) {
    if (!vc) return -1;
    if (vc->count + 4 > VORONOI_MAX_CELLS) return -2;  /* not enough room */

    VoronoiCell *parent = voronoi_lookup(vc, parent_flat_id);
    if (!parent) return -3;
    if (parent->depth >= VORONOI_MAX_DEPTH) return -4;  /* max depth */
    if (parent->state == VORONOI_CELL_FROZEN) return -5;

    uint32_t quarter = parent->data_size / 4;
    if (quarter == 0) return -6;  /* too small to subdivide */

    parent->state = VORONOI_CELL_SUBDIV;

    /* Create 4 children at adjacent addresses */
    for (int i = 0; i < 4; i++) {
        uint32_t child_flat = (parent->flat_id + i + 1) % VORONOI_ADDR_SPACE;
        uint32_t child_offset = parent->data_offset + i * quarter;
        uint32_t child_size = (i == 3) ? parent->data_size - 3 * quarter : quarter;

        VoronoiCell *child = voronoi_insert(vc, child_flat,
                                             parent->entropy_tier,
                                             child_offset, child_size);
        if (child) {
            child->depth = parent->depth + 1;
            child->parent_id = parent->flat_id;
            parent->child_ids[i] = child_flat;
        }
    }

    vc->subdivisions++;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
   COLLAPSE — merge children back into parent
   ═══════════════════════════════════════════════════════════════ */

static inline int voronoi_collapse(VoronoiCache *vc, uint32_t parent_flat_id) {
    if (!vc) return -1;

    VoronoiCell *parent = voronoi_lookup(vc, parent_flat_id);
    if (!parent) return -2;
    if (parent->state != VORONOI_CELL_SUBDIV) return -3;

    /* Remove children from cache */
    for (int i = 0; i < 4; i++) {
        uint32_t cid = parent->child_ids[i];
        if (cid == 0xFFFFFFFF) continue;
        for (uint32_t j = 0; j < VORONOI_MAX_CELLS; j++) {
            if (vc->cells[j].flat_id == cid &&
                vc->cells[j].state != VORONOI_CELL_EMPTY) {
                vc->cells[j].state = VORONOI_CELL_EMPTY;
                vc->cells[j].flat_id = 0xFFFFFFFF;
                vc->count--;
                break;
            }
        }
        parent->child_ids[i] = 0xFFFFFFFF;
    }

    parent->state = VORONOI_CELL_ACTIVE;
    vc->collapses++;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
   TICK — advance time, collapse cold cells
   ═══════════════════════════════════════════════════════════════
   Called once per KIS tick cycle.
   Cells not accessed for VORONOI_TICK_CYCLE ticks → collapse.
   ═══════════════════════════════════════════════════════════════ */

static inline uint32_t voronoi_tick(VoronoiCache *vc) {
    if (!vc) return 0;
    vc->tick++;
    uint32_t collapsed = 0;

    for (uint32_t i = 0; i < VORONOI_MAX_CELLS; i++) {
        VoronoiCell *c = &vc->cells[i];
        if (c->state == VORONOI_CELL_EMPTY) continue;
        if (c->state == VORONOI_CELL_FROZEN) continue;

        uint32_t age = vc->tick - c->last_tick;

        if (c->state == VORONOI_CELL_SUBDIV && age > VORONOI_TICK_CYCLE) {
            voronoi_collapse(vc, c->flat_id);
            collapsed++;
        }
        else if (c->state == VORONOI_CELL_ACTIVE && age > VORONOI_TICK_CYCLE) {
            c->state = VORONOI_CELL_COLD;
        }
    }
    return collapsed;
}

/* ═══════════════════════════════════════════════════════════════
   FREEZE / UNFREEZE — pin a cell so it never gets evicted
   ═══════════════════════════════════════════════════════════════ */

static inline int voronoi_freeze(VoronoiCache *vc, uint32_t flat_id) {
    if (!vc) return -1;
    VoronoiCell *c = voronoi_lookup(vc, flat_id);
    if (!c) return -2;
    c->state = VORONOI_CELL_FROZEN;
    return 0;
}

static inline int voronoi_unfreeze(VoronoiCache *vc, uint32_t flat_id) {
    if (!vc) return -1;
    VoronoiCell *c = voronoi_lookup(vc, flat_id);
    if (!c) return -2;
    c->state = VORONOI_CELL_ACTIVE;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════
   STATISTICS
   ═══════════════════════════════════════════════════════════════ */

static inline void voronoi_stats(const VoronoiCache *vc) {
    if (!vc) return;

    uint32_t active = 0, cold = 0, frozen = 0, subdiv = 0;
    for (uint32_t i = 0; i < VORONOI_MAX_CELLS; i++) {
        switch (vc->cells[i].state) {
            case VORONOI_CELL_ACTIVE:  active++;  break;
            case VORONOI_CELL_COLD:    cold++;    break;
            case VORONOI_CELL_FROZEN:  frozen++;  break;
            case VORONOI_CELL_SUBDIV:  subdiv++;  break;
            default: break;
        }
    }

    uint32_t total = vc->hits + vc->misses;
    double hit_rate = total ? (double)vc->hits / total * 100.0 : 0.0;

    printf("===============================================================\n");
    printf("  Voronoi Cell Cache\n");
    printf("---------------------------------------------------------------\n");
    printf("  Cache size:       %u / %u\n", vc->count, VORONOI_MAX_CELLS);
    printf("  Active:           %u\n", active);
    printf("  Cold:             %u\n", cold);
    printf("  Frozen:           %u\n", frozen);
    printf("  Subdivided:       %u\n", subdiv);
    printf("  Hits:             %u\n", vc->hits);
    printf("  Misses:           %u\n", vc->misses);
    printf("  Hit rate:         %.1f%%\n", hit_rate);
    printf("  Collapses:        %u\n", vc->collapses);
    printf("  Subdivisions:     %u\n", vc->subdivisions);
    printf("  Current tick:     %u\n", vc->tick);
    printf("===============================================================\n");
}

/* ═══════════════════════════════════════════════════════════════
   VERIFY — check internal consistency
   ═══════════════════════════════════════════════════════════════ */

static inline int voronoi_verify(const VoronoiCache *vc) {
    if (!vc) return -1;

    uint32_t count_check = 0;
    for (uint32_t i = 0; i < VORONOI_MAX_CELLS; i++) {
        const VoronoiCell *c = &vc->cells[i];
        if (c->state != VORONOI_CELL_EMPTY) {
            if (c->flat_id >= VORONOI_ADDR_SPACE) return -2;
            if (c->depth > VORONOI_MAX_DEPTH) return -3;
            count_check++;

            /* Check parent consistency */
            if (c->parent_id != 0xFFFFFFFF) {
                int parent_found = 0;
                for (uint32_t j = 0; j < VORONOI_MAX_CELLS; j++) {
                    if (vc->cells[j].flat_id == c->parent_id) {
                        parent_found = 1;
                        break;
                    }
                }
                if (!parent_found) return -4;
            }
        }
    }
    if (count_check != vc->count) return -5;

    return 0;
}

#endif /* GEO_FS_VORONOI_H */
