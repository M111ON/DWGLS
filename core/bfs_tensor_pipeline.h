/* bfs_tensor_pipeline.c — BreathingFS tensor pipeline consumer
 *
 * Connects BreathingFS (seeker movement + codec) to tensor storage (.tesspack).
 * Seeker position = placement decision. Route events point to tensor spans.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "breathing_fs.h"
#include "frustum_route.h"
#include "geo_tess_container.h"

#ifndef BFS_TENSOR_PIPELINE_H
#define BFS_TENSOR_PIPELINE_H

/* Pipeline context: ties together BFS seeker, frustum route producer, and pack reader */
typedef struct {
    BreathingFS *fs;
    FrRouteCache route_cache;
    TESS_PackIndex pack;
    int pack_open;
    uint8_t view_id;  /* view ID for frustum routing */
} BFSTensorPipeline;

/* Initialize pipeline with seeker config and optional pack path */
static inline int bfs_tensor_pipeline_init(BFSTensorPipeline *pipe,
                                           BreathingFS *fs,
                                           uint32_t seeker_pos, uint8_t view_id,
                                           uint32_t voronoi_mask, uint8_t frustum_depth,
                                           const char *pack_path) {
    if (!pipe || !fs) return -1;
    memset(pipe, 0, sizeof(*pipe));
    pipe->fs = fs;
    pipe->view_id = view_id;

    fr_cache_init(&pipe->route_cache);

    /* Initialize seeker in BFS */
    seeker_init(&fs->seeker);
    fs->seeker.current_pos = seeker_pos;
    fs->seeker.scale = 1.0;
    /* window is set by seeker_init to BFS_SEEKER_K (5184) */

    /* Open tesspack if provided */
    pipe->pack_open = 0;
    if (pack_path && pack_path[0]) {
        if (tess_pack_open(&pipe->pack, pack_path) == 0) {
            pipe->pack_open = 1;
        }
    }
    return 0;
}

/* Cleanup pipeline */
static inline void bfs_tensor_pipeline_close(BFSTensorPipeline *pipe) {
    if (!pipe) return;
    if (pipe->pack_open) {
        tess_pack_close(&pipe->pack);
        pipe->pack_open = 0;
    }
}

/* Produce route event for current seeker position */
static inline int bfs_tensor_route_produce(BFSTensorPipeline *pipe, FrustumRouteEvent *out) {
    if (!pipe || !out) return -1;

    FrustumSeeker seeker = {
        .position = pipe->fs->seeker.current_pos,
        .view_id = pipe->view_id,
        .voronoi_mask = 0xFFFFFFu,  /* full mask default */
        .frustum_depth = 4
    };

    return fr_route_produce(&seeker, &pipe->route_cache, out);
}

/* Find tensor data by name (ONION first, then capo) */
static inline int bfs_tensor_find(const BFSTensorPipeline *pipe,
                                   const char *tensor_name,
                                   const uint8_t **data_out,
                                   uint32_t *data_sz) {
    if (!pipe || !pipe->pack_open || !tensor_name) return -1;

    /* Try ONION (raw contiguous f16/f32) first */
    int rc = tess_pack_find_onion(&pipe->pack, tensor_name, data_out, data_sz);
    if (rc == 0) return 0;  /* found */

    /* Fall back to first capo (capo_id == 0) */
    for (uint32_t i = 0; i < pipe->pack.n_entries; i++) {
        if (pipe->pack.entries[i].capo_id == 0 &&
            strcmp(pipe->pack.entries[i].name, tensor_name) == 0) {
            uint64_t off = pipe->pack.entries[i].offset;
            uint32_t sz = pipe->pack.entries[i].size;
            if (off + sz > pipe->pack.file_sz) return -2;
            *data_out = pipe->pack.base + off;
            *data_sz = sz;
            return 0;
        }
    }
    return -3;  /* not found */
}

/* Load tensor span from route event into destination buffer */
static inline int bfs_tensor_load_span(const BFSTensorPipeline *pipe,
                                        const FrustumRouteEvent *evt,
                                        void *dst, size_t dst_cap) {
    if (!pipe || !evt || !dst) return -1;

    char tensor_name[64];
    snprintf(tensor_name, sizeof(tensor_name), "tensor_%u", evt->tensor_id);

    const uint8_t *src = NULL;
    uint32_t src_sz = 0;
    int rc = bfs_tensor_find(pipe, tensor_name, &src, &src_sz);
    if (rc != 0) return rc;

    if (evt->span_offset + evt->span_size > src_sz) return -2;
    if (evt->span_size > dst_cap) return -3;

    memcpy(dst, src + evt->span_offset, evt->span_size);
    return (int)evt->span_size;
}

/* Verify route idempotency (same inputs -> same route_ref) */
static inline int bfs_tensor_verify_idempotent(BFSTensorPipeline *pipe, int iterations) {
    if (!pipe) return -1;

    FrustumRouteEvent evt1 = {0}, evt2 = {0};
    FrustumSeeker seeker = {
        .position = pipe->fs->seeker.current_pos,
        .view_id = pipe->view_id,
        .voronoi_mask = 0xFFFFFFu,
        .frustum_depth = 4
    };

    for (int i = 0; i < iterations; i++) {
        if (fr_route_produce(&seeker, &pipe->route_cache, &evt1) != 1) return 0;
        if (fr_route_produce(&seeker, &pipe->route_cache, &evt2) != 1) return 0;
        if (evt1.route_ref != evt2.route_ref) return 0;
    }
    return 1;  /* all iterations produced identical route_ref */
}

#endif /* BFS_TENSOR_PIPELINE_H */