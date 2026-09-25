/*
 * frustum_route.h — Frustum Route Producer
 * ════════════════════════════════════════════════════════════════
 *
 * Pure geometric route producer — computes adaptive routes from seeker
 * position + view + Voronoi mask + frustum depth. NO tensor bytes touched,
 * NO buckets created, NO persistent graph, NO SID modification.
 *
 * Rules:
 *   - Repeated identical inputs → identical output reference (idempotent)
 *   - Multiple seekers: merge masks via OR before routing
 *   - Points outside mask → no route produced
 *   - No heap allocation, no float, no malloc
 *
 * Depends: frustum_trit.h, frustum_slot64.h, geo_voronoi_mask.h,
 *          geo_box_axes.h, geo_tess_container.h
 */

#ifndef FRUSTUM_ROUTE_H
#define FRUSTUM_ROUTE_H

#include <stdint.h>
#include <string.h>
#include "frustum_trit.h"
#include "frustum_slot64.h"
#include "geo_voronoi_mask.h"
#include "geo_box_axes.h"
#include "geo_tess_container.h"

/* ════════════════════════════════════════════════════════════════
   CONSTANTS
   ════════════════════════════════════════════════════════════════ */

#define FR_MAX_DEPTH        4u       /* frustum depth levels (matches LEVEL_COUNT) */
#define FR_MAX_BRANCHES     6u       /* max branch paths per level (FACE_COUNT) */
#define FR_MAX_SEEKERS      8u       /* max seekers for mask merge */
#define FR_ROUTE_EVENT_SZ   32       /* FrustumRouteEvent size in bytes */

#define FR_CAPO_KEY_MASK    0xFFFFu  /* 16-bit capo key */
#define FR_TENSOR_ID_MASK   0xFFFFu  /* 16-bit tensor id */

/* ════════════════════════════════════════════════════════════════
   OUTPUT: FrustumRouteEvent (32 bytes, packed)
   ════════════════════════════════════════════════════════════════ */

#pragma pack(push, 1)

typedef struct {
    uint8_t  level;         /* 0..3 — frustum depth level */
    uint8_t  branch_path;   /* 0..5 — branch within level (face index) */
    uint8_t  view;          /* view id (0..7 octant, or custom) */
    uint8_t  node_id;       /* frustum node (0..53 GEAR_MESH) */
    uint16_t capo_key;      /* capo chunk key (0..65535) */
    uint16_t tensor_id;     /* tensor identifier */
    uint32_t span_offset;   /* byte offset within tensor */
    uint32_t span_size;     /* byte span size */
    uint64_t route_ref;     /* stable reference for idempotency */
    uint64_t reserved;      /* reserved for future use — pads to 32B */
} FrustumRouteEvent;

#pragma pack(pop)

typedef char _fr_event_sz[(sizeof(FrustumRouteEvent) == FR_ROUTE_EVENT_SZ) ? 1 : -1];



/* ════════════════════════════════════════════════════════════════
   SEEKER INPUT
   ════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t position;      /* flat position in 20736 field */
    uint8_t  view_id;       /* view / octant id (0..7 or custom) */
    uint32_t voronoi_mask;  /* 24-bit mask: 1 bit per Voronoi cell (VM_CELLS=24) */
    uint8_t  frustum_depth; /* 1..FR_MAX_DEPTH */
} FrustumSeeker;

/* Multiple seekers merged into single mask */
typedef struct {
    FrustumSeeker seekers[FR_MAX_SEEKERS];
    uint8_t count;
    uint32_t merged_mask;   /* OR of all seeker voronoi_masks */
} FrustumSeekerGroup;

/* ════════════════════════════════════════════════════════════════
   ROUTE CACHE (idempotency: same input → same reference)
   ════════════════════════════════════════════════════════════════ */

#define FR_CACHE_SLOTS 64u  /* power of 2 for fast modulo */

typedef struct {
    uint64_t key;                    /* hash of input parameters */
    FrustumRouteEvent event;         /* cached route */
    uint8_t  valid;
} FrRouteCacheEntry;

typedef struct {
    FrRouteCacheEntry entries[FR_CACHE_SLOTS];
} FrRouteCache;

static inline void fr_cache_init(FrRouteCache *cache) {
    if (cache) memset(cache, 0, sizeof(*cache));
}

/* FNV-1a hash for cache key */
static inline uint64_t fr_hash_input(uint32_t pos, uint8_t view, uint32_t mask, uint8_t depth) {
    uint64_t h = 14695981039346656037ull;  /* FNV offset basis */
    h ^= pos;       h *= 1099511628211ull;
    h ^= view;      h *= 1099511628211ull;
    h ^= mask;      h *= 1099511628211ull;
    h ^= depth;     h *= 1099511628211ull;
    return h;
}

/* ════════════════════════════════════════════════════════════════
   MASK MERGE (multiple seekers)
   ════════════════════════════════════════════════════════════════ */

static inline void fr_seeker_group_init(FrustumSeekerGroup *group) {
    if (group) {
        memset(group, 0, sizeof(*group));
    }
}

static inline int fr_seeker_group_add(FrustumSeekerGroup *group,
                                       uint32_t position, uint8_t view_id,
                                       uint32_t voronoi_mask, uint8_t frustum_depth) {
    if (!group || group->count >= FR_MAX_SEEKERS) return -1;
    FrustumSeeker *s = &group->seekers[group->count];
    s->position = position;
    s->view_id = view_id;
    s->voronoi_mask = voronoi_mask & 0xFFFFFFu;  /* 24 bits */
    s->frustum_depth = (frustum_depth > FR_MAX_DEPTH) ? FR_MAX_DEPTH : frustum_depth;
    group->merged_mask |= s->voronoi_mask;
    group->count++;
    return 0;
}

/* ════════════════════════════════════════════════════════════════
   ROUTE PRODUCTION
   ════════════════════════════════════════════════════════════════ */

/*
 * Produce route event from single seeker.
 * Returns 1 if route produced, 0 if seeker position outside mask.
 * Event is written to 'out'. Uses cache for idempotency.
 */
static inline int fr_route_produce(const FrustumSeeker *seeker,
                                    FrRouteCache *cache,
                                     FrustumRouteEvent *out) {
    if (!seeker || !out) return 0;

    /* Check if position is within seeker's voronoi mask */
    uint32_t cell = vm_cell_of(seeker->position);
    if (!(seeker->voronoi_mask & (1u << cell))) {
        return 0;  /* Outside mask — no route */
    }

    /* Compute cache key */
    uint64_t key = fr_hash_input(seeker->position, seeker->view_id,
                                  seeker->voronoi_mask, seeker->frustum_depth);

    /* Check cache for idempotent repeat */
    if (cache) {
        uint32_t idx = (uint32_t)(key % FR_CACHE_SLOTS);
        if (cache->entries[idx].valid && cache->entries[idx].key == key) {
            *out = cache->entries[idx].event;
            return 1;
        }
    }

    /* Build route event from geometry */
    FrustumRouteEvent evt = {0};

    /* Decompose position to frustum coordinates */
    TritAddr trit;
    trit_decompose((uint8_t)(seeker->position % TRIT_MOD), &trit);
    trit.slope = trit_slope(0x9E3779B97F4A7C15ull, seeker->position);

    evt.level = (seeker->frustum_depth > 0 && seeker->frustum_depth <= FR_MAX_DEPTH)
                    ? (seeker->frustum_depth - 1)
                    : trit.level;
    evt.branch_path = trit.face;          /* 0..5 */
    evt.view = seeker->view_id;
    evt.node_id = (uint8_t)(trit.coset * FACE_COUNT + trit.face);  /* 0..53 */
    evt.capo_key = (uint16_t)((seeker->position / TESS_AXIS_STRIDE) & FR_CAPO_KEY_MASK);
    evt.tensor_id = (uint16_t)((seeker->position / 144) & FR_TENSOR_ID_MASK);

    /* Span derived from frustum depth: 144 * 2^depth slots per level */
    uint32_t base_span = 144u << evt.level;
    evt.span_offset = (seeker->position % base_span) * TESS_CELL_F16;  /* assume F16 cell */
    evt.span_size = base_span * TESS_CELL_F16;

    /* Stable reference = hash of all fields except route_ref itself */
    uint64_t ref = 14695981039346656037ull;
    ref ^= (uint64_t)evt.level;       ref *= 1099511628211ull;
    ref ^= (uint64_t)evt.branch_path; ref *= 1099511628211ull;
    ref ^= (uint64_t)evt.view;        ref *= 1099511628211ull;
    ref ^= (uint64_t)evt.node_id;     ref *= 1099511628211ull;
    ref ^= (uint64_t)evt.capo_key;    ref *= 1099511628211ull;
    ref ^= (uint64_t)evt.tensor_id;   ref *= 1099511628211ull;
    ref ^= evt.span_offset;           ref *= 1099511628211ull;
    ref ^= evt.span_size;             ref *= 1099511628211ull;
    evt.route_ref = ref;

    *out = evt;

    /* Store in cache */
    if (cache) {
        uint32_t idx = (uint32_t)(key % FR_CACHE_SLOTS);
        cache->entries[idx].key = key;
        cache->entries[idx].event = evt;
        cache->entries[idx].valid = 1;
    }

    return 1;
}

/*
 * Produce routes from seeker group (merged mask).
 * Returns number of routes produced (0..FR_MAX_SEEKERS).
 * Output array 'events' must hold at least FR_MAX_SEEKERS entries.
 */
static inline uint8_t fr_route_produce_group(const FrustumSeekerGroup *group,
                                              FrRouteCache *cache,
                                               FrustumRouteEvent *events) {
    if (!group || !events || group->count == 0) return 0;

    uint8_t produced = 0;
    for (uint8_t i = 0; i < group->count; i++) {
        /* Use merged mask for all seekers in group */
        FrustumSeeker merged_seeker = group->seekers[i];
        merged_seeker.voronoi_mask = group->merged_mask;

        FrustumRouteEvent evt;
        if (fr_route_produce(&merged_seeker, cache, &evt)) {
            events[produced++] = evt;
        }
    }
    return produced;
}

/* ════════════════════════════════════════════════════════════════
   MASK QUERY HELPERS
   ════════════════════════════════════════════════════════════════ */

/* Check if a flat position is within any seeker's mask in the group */
static inline int fr_group_contains(const FrustumSeekerGroup *group, uint32_t flat) {
    if (!group) return 0;
    uint32_t cell = vm_cell_of(flat);
    return (group->merged_mask & (1u << cell)) != 0;
}

/* Get voronoi cell for a position */
static inline uint8_t fr_cell_of(uint32_t flat) {
    return (uint8_t)vm_cell_of(flat);
}

/* ════════════════════════════════════════════════════════════════
   VERIFICATION
   ════════════════════════════════════════════════════════════════ */

static inline int frustum_route_verify(void) {
    FrRouteCache cache;
    fr_cache_init(&cache);

    /* Test 1: Basic route production */
    FrustumSeeker s = {
        .position = 1000,
        .view_id = 3,
        .voronoi_mask = (1u << vm_cell_of(1000)) | (1u << 5),
        .frustum_depth = 2
    };
    FrustumRouteEvent evt;
    int r = fr_route_produce(&s, &cache, &evt);
    if (!r) return -1;  /* should produce route */

    /* Test 2: Idempotency — same input → same reference */
    FrustumRouteEvent evt2;
    r = fr_route_produce(&s, &cache, &evt2);
    if (!r || evt.route_ref != evt2.route_ref) return -2;

    /* Test 3: Outside mask → no route */
    FrustumSeeker s_out = {
        .position = 50000,
        .view_id = 3,
        .voronoi_mask = 1u << 0,  /* only cell 0 */
        .frustum_depth = 2
    };
    r = fr_route_produce(&s_out, &cache, &evt);
    if (r) return -3;  /* should NOT produce route */

    /* Test 4: Seeker group mask merge */
    FrustumSeekerGroup group;
    fr_seeker_group_init(&group);
    fr_seeker_group_add(&group, 1000, 1, 1u << vm_cell_of(1000), 1);
    fr_seeker_group_add(&group, 2000, 2, 1u << vm_cell_of(2000), 2);
    FrustumRouteEvent events[FR_MAX_SEEKERS];
    uint8_t n = fr_route_produce_group(&group, &cache, events);
    if (n != 2) return -4;  /* both should produce (merged mask covers both cells) */

    /* Test 5: Event size check */
    if (sizeof(FrustumRouteEvent) != FR_ROUTE_EVENT_SZ) return -5;

    /* Test 6: Cache invalidation on different input */
    FrustumSeeker s3 = s;
    s3.position = 1001;  /* different position */
    FrustumRouteEvent evt3;
    r = fr_route_produce(&s3, &cache, &evt3);
    if (!r || evt3.route_ref == evt.route_ref) return -6;  /* different ref */

    return 0;
}

#endif /* FRUSTUM_ROUTE_H */
