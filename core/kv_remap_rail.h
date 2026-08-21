#ifndef KV_REMAP_RAIL_H
#define KV_REMAP_RAIL_H

/*
 * KV Remap Rail — Layer-based segment chunking
 * ═══════════════════════════════════════════════════
 *
 * Layer-based: each lane scans a group of layers (not flat byte ranges).
 * Scan walks within each layer: K then V, byte-by-byte.
 * Patch writes skeleton back per-layer (decompress → copy per layer).
 *
 * Rail states:
 *   PARK   — no work, waiting for idle
 *   SCAN   — comparing skeleton vs live KV (per-layer)
 *   PATCH  — writing skeleton back to live KV (per-layer)
 *   REBUILD — flush + new skeleton
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "kv_remap.h"

/* ── Rail states ────────────────────────────────────────── */
#define RAIL_PARK    0
#define RAIL_SCAN    1
#define RAIL_PATCH   2
#define RAIL_REBUILD 3
#define RAIL_FREEZE  4

/* ── Rail config ────────────────────────────────────────── */
#define RAIL_SOFT_THRESH     15     /* below = skeleton fine, no action */
#define RAIL_HARD_THRESH     85     /* above = rebuild */
#define RAIL_PATCH_THRESH    40     /* between soft/hard = patch */
#define RAIL_LAYERS_PER_LANE 2      /* layers per lane (adjustable) */

/* ── Per-lane state (layer-based) ───────────────────────── */
typedef struct {
    int       layer_start;   /* first layer index for this lane */
    int       layer_end;     /* one-past-last layer index */
    int       cur_layer;     /* currently scanning layer */
    int       cur_phase;     /* 0 = scanning K, 1 = scanning V */
    size_t    cur_off;       /* byte offset within current K or V */
    uint64_t  diff_count;    /* bytes changed in this lane */
    uint64_t  checked;       /* bytes checked so far */
    uint64_t  total;         /* total bytes in this lane (all layers) */
    int       complete;      /* lane finished */
} RailLane;

/* ── Rail context ───────────────────────────────────────── */
typedef struct {
    KVRemapCtx *remap;       /* backlink to remap context */

    int       state;
    int       lane;
    RailLane  lanes[3];

    uint64_t  total_diff;
    uint64_t  total_checked;
    int       change_pct;

    /* Patch: per-layer decompression */
    int       patch_layer;   /* current layer being patched */
    int       patch_phase;   /* 0 = writing K, 1 = writing V */
    size_t    patch_off;     /* offset within K or V */

    int       freeze_state;
    int       freeze_lane;

    double    scan_start_ms;
    double    scan_elapsed_ms;

    int       enabled;
} KVRemapRail;


/* =============================================================
 * Clock helper (ms)
 * If POGLS_RAIL_USE_POGTIME is defined, use PoglsTime.
 * Otherwise use standard struct timespec.
 * ============================================================= */

static inline double rail_clock_ms(void) {
#ifdef POGLS_RAIL_USE_POGTIME
    PoglsTime ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#endif
}


/* =============================================================
 * Init
 * ============================================================= */

static inline void kv_remap_rail_init(KVRemapRail *rail, KVRemapCtx *remap) {
    memset(rail, 0, sizeof(*rail));
    rail->remap = remap;
    rail->state = RAIL_PARK;
    rail->enabled = 1;
    fprintf(stderr, "[rail] init: layers_per_lane=%d, thresholds=%d/%d/%d%%\n",
        RAIL_LAYERS_PER_LANE, RAIL_SOFT_THRESH, RAIL_PATCH_THRESH, RAIL_HARD_THRESH);
}


/* =============================================================
 * Start scan — assign layers to 3 lanes
 * ============================================================= */

static inline void kv_remap_rail_start_scan(KVRemapRail *rail) {
    if (!rail->enabled || rail->state != RAIL_PARK) return;
    if (!rail->remap->skeleton_valid || !rail->remap->ref_skeleton) return;
    if (rail->remap->n_layers <= 0) return;

    rail->state = RAIL_SCAN;
    rail->lane = 0;
    rail->total_diff = 0;
    rail->total_checked = 0;
    rail->change_pct = -1;
    rail->scan_start_ms = rail_clock_ms();

    int n_layers = rail->remap->n_layers;
    int lpl = RAIL_LAYERS_PER_LANE;

    for (int i = 0; i < 3; i++) {
        rail->lanes[i].layer_start = i * lpl;
        rail->lanes[i].layer_end   = (i + 1) * lpl;
        if (rail->lanes[i].layer_start >= n_layers) {
            rail->lanes[i].layer_start = n_layers;
            rail->lanes[i].layer_end   = n_layers;
        } else if (rail->lanes[i].layer_end > n_layers) {
            rail->lanes[i].layer_end = n_layers;
        }
        rail->lanes[i].cur_layer = rail->lanes[i].layer_start;
        rail->lanes[i].cur_phase = 0;
        rail->lanes[i].cur_off   = 0;
        rail->lanes[i].diff_count = 0;
        rail->lanes[i].checked   = 0;
        rail->lanes[i].total     = 0;
        rail->lanes[i].complete  = 0;

        /* Sum bytes for this lane's layers */
        for (int l = rail->lanes[i].layer_start; l < rail->lanes[i].layer_end; l++) {
            rail->lanes[i].total += rail->remap->layers[l].k_size
                                  + rail->remap->layers[l].v_size;
        }
    }

    fprintf(stderr, "[rail] scan started: %d layers across 3 lanes\n", n_layers);
    for (int i = 0; i < 3; i++) {
        fprintf(stderr, "  lane%d: layers[%d..%d) %zu bytes\n",
            i, rail->lanes[i].layer_start, rail->lanes[i].layer_end,
            rail->lanes[i].total);
    }
}


/* =============================================================
 * Step: one idle-friendly scan/patch chunk
 * Returns: 0 = still working, 1 = phase complete, -1 = error
 * ============================================================= */

static inline int kv_remap_rail_step(KVRemapRail *rail) {
    if (!rail->enabled) return -1;

    if (rail->state == RAIL_PARK || rail->state == RAIL_FREEZE)
        return 0;

    /* ── SCAN: walk layers within current lane ── */
    if (rail->state == RAIL_SCAN) {
        KVRemapCtx *ctx = rail->remap;
        RailLane *cl = &rail->lanes[rail->lane];

        if (cl->complete) {
            /* Advance to next lane */
            rail->lane++;
            if (rail->lane >= 3) {
                /* All lanes done — classify */
                rail->total_checked = 0;
                rail->total_diff = 0;
                for (int i = 0; i < 3; i++) {
                    rail->total_checked += rail->lanes[i].checked;
                    rail->total_diff += rail->lanes[i].diff_count;
                }
                if (rail->total_checked > 0)
                    rail->change_pct = (int)(rail->total_diff * 100 / rail->total_checked);
                else
                    rail->change_pct = 0;

                rail->scan_elapsed_ms = rail_clock_ms() - rail->scan_start_ms;
                fprintf(stderr, "[rail] scan complete: %llu/%llu = %d%% (%.1f ms)\n",
                    (unsigned long long)rail->total_diff,
                    (unsigned long long)rail->total_checked,
                    rail->change_pct, rail->scan_elapsed_ms);

                if (rail->change_pct >= RAIL_HARD_THRESH) {
                    rail->state = RAIL_REBUILD;
                    fprintf(stderr, "[rail] decision: REBUILD (%d%% >= %d%%)\n",
                        rail->change_pct, RAIL_HARD_THRESH);
                } else if (rail->change_pct > RAIL_SOFT_THRESH) {
                    rail->state = RAIL_PATCH;
                    rail->patch_layer = 0;
                    rail->patch_phase = 0;
                    rail->patch_off = 0;
                    fprintf(stderr, "[rail] decision: PATCH (%d%% > %d%%)\n",
                        rail->change_pct, RAIL_SOFT_THRESH);
                } else {
                    rail->state = RAIL_PARK;
                    fprintf(stderr, "[rail] decision: PARK (skeleton valid, %d%%)\n",
                        rail->change_pct);
                }
                return 1;
            }
            return 0;
        }

        /* Scan current layer in this lane */
        if (cl->cur_layer >= cl->layer_end) {
            cl->complete = 1;
            return 0;
        }

        KVRemapLayer *lyr = &ctx->layers[cl->cur_layer];
        size_t region_size = (cl->cur_phase == 0) ? lyr->k_size : lyr->v_size;
        const uint8_t *live = (cl->cur_phase == 0)
            ? (const uint8_t *)lyr->k_data
            : (const uint8_t *)lyr->v_data;

        /* Pre-compute skeleton offset for this layer's K or V */
        size_t base_sk_off = 0;
        for (int l = 0; l < cl->cur_layer; l++)
            base_sk_off += ctx->layers[l].k_size + ctx->layers[l].v_size;
        if (cl->cur_phase == 1) base_sk_off += lyr->k_size;

        /* Process multiple chunks per step using memcmp (much faster than byte-by-byte) */
        #define RAIL_SCAN_CHUNK  4096
        #define RAIL_SCAN_BATCH  64   /* 64 chunks = 256KB per step */
        int remaining = RAIL_SCAN_BATCH;

        while (remaining > 0 && cl->cur_off < region_size) {
            size_t end = cl->cur_off + RAIL_SCAN_CHUNK;
            if (end > region_size) end = region_size;
            size_t chunk_sz = end - cl->cur_off;

            if (memcmp(ctx->ref_skeleton + base_sk_off + cl->cur_off,
                       live + cl->cur_off, chunk_sz) != 0)
                cl->diff_count += chunk_sz;
            cl->checked += chunk_sz;

            cl->cur_off = end;
            remaining--;

            if (cl->cur_off >= region_size) {
                cl->cur_phase++;
                cl->cur_off = 0;
                if (cl->cur_phase > 1) {
                    cl->cur_phase = 0;
                    cl->cur_layer++;
                    if (cl->cur_layer >= cl->layer_end) {
                        cl->complete = 1;
                        break;
                    }
                    /* Recompute base_sk_off for next layer */
                    base_sk_off = 0;
                    for (int l = 0; l < cl->cur_layer; l++)
                        base_sk_off += ctx->layers[l].k_size + ctx->layers[l].v_size;
                } else {
                    /* Switch from K to V — add K size to sk_off */
                    base_sk_off += lyr->k_size;
                }
                /* Update layer/region for next iteration */
                lyr = &ctx->layers[cl->cur_layer];
                region_size = (cl->cur_phase == 0) ? lyr->k_size : lyr->v_size;
                live = (cl->cur_phase == 0)
                    ? (const uint8_t *)lyr->k_data
                    : (const uint8_t *)lyr->v_data;
            }
        }
        #undef RAIL_SCAN_CHUNK
        #undef RAIL_SCAN_BATCH

        return 0;
    }

    /* ── PATCH: decompress skeleton, write back per-layer ── */
    if (rail->state == RAIL_PATCH) {
        KVRemapCtx *ctx = rail->remap;

        /* Decompress skeleton (full, for simplicity) */
        size_t dec_size = 0;
        void *dec = kv_remap_decompress(ctx->skeleton_data, ctx->skeleton_comp, &dec_size);
        if (!dec) {
            rail->state = RAIL_PARK;
            fprintf(stderr, "[rail] patch failed (decompress error)\n");
            return -1;
        }

        /* Walk layers until we've done one chunk or all layers */
        #define RAIL_PATCH_CHUNK 4096
        size_t written = 0;
        size_t sk_off = 0;

        for (int l = 0; l < rail->patch_layer; l++)
            sk_off += ctx->layers[l].k_size + ctx->layers[l].v_size;

        for (int l = rail->patch_layer; l < ctx->n_layers && written < RAIL_PATCH_CHUNK; l++) {
            KVRemapLayer *lyr = &ctx->layers[l];
            size_t k_off = (rail->patch_phase == 0 && l == rail->patch_layer) ? rail->patch_off : 0;

            /* Write K portion */
            if (rail->patch_phase == 0 && k_off < lyr->k_size) {
                size_t can = lyr->k_size - k_off;
                if (can > RAIL_PATCH_CHUNK - written) can = RAIL_PATCH_CHUNK - written;
                memcpy((uint8_t *)lyr->k_data + k_off, (uint8_t *)dec + sk_off + k_off, can);
                written += can;
                rail->patch_off = k_off + can;
                if (rail->patch_off >= lyr->k_size) {
                    rail->patch_phase = 1;
                    rail->patch_off = 0;
                }
            }

            /* Write V portion */
            if (rail->patch_phase == 1 || (l > rail->patch_layer)) {
                size_t v_off = (l == rail->patch_layer && rail->patch_phase == 1) ? rail->patch_off : 0;
                if (v_off < lyr->v_size) {
                    size_t can = lyr->v_size - v_off;
                    if (can > RAIL_PATCH_CHUNK - written) can = RAIL_PATCH_CHUNK - written;
                    memcpy((uint8_t *)lyr->v_data + v_off,
                           (uint8_t *)dec + sk_off + lyr->k_size + v_off, can);
                    written += can;
                    rail->patch_off = v_off + can;
                    if (rail->patch_off >= lyr->v_size) {
                        rail->patch_layer = l + 1;
                        rail->patch_phase = 0;
                        rail->patch_off = 0;
                    }
                }
            }

            sk_off += lyr->k_size + lyr->v_size;
        }

        free(dec);

        if (rail->patch_layer >= ctx->n_layers) {
            rail->state = RAIL_PARK;
            fprintf(stderr, "[rail] patch complete (%d layers)\n", ctx->n_layers);
            return 1;
        }

        return 0;
        #undef RAIL_PATCH_CHUNK
    }

    /* ── REBUILD ── */
    if (rail->state == RAIL_REBUILD) {
        kv_remap_rebuild(rail->remap);
        rail->state = RAIL_PARK;
        fprintf(stderr, "[rail] rebuild complete\n");
        return 1;
    }

    return -1;
}


/* =============================================================
 * Freeze / Resume
 * ============================================================= */

static inline void kv_remap_rail_freeze(KVRemapRail *rail) {
    if (rail->state == RAIL_PARK) return;
    rail->freeze_state = rail->state;
    rail->freeze_lane = rail->lane;
    rail->state = RAIL_FREEZE;
}

static inline void kv_remap_rail_resume(KVRemapRail *rail) {
    if (rail->state != RAIL_FREEZE) return;
    rail->state = rail->freeze_state;
    rail->lane = rail->freeze_lane;
}


/* =============================================================
 * Print status
 * ============================================================= */

static inline void kv_remap_rail_print_status(const KVRemapRail *rail) {
    static const char *state_names[] = {"PARK", "SCAN", "PATCH", "REBUILD", "FREEZE"};
    const char *s = (rail->state >= 0 && rail->state <= RAIL_FREEZE) ?
        state_names[rail->state] : "?";
    fprintf(stderr, "[rail] status: state=%s lane=%d\n", s, rail->lane);
    if (rail->state == RAIL_SCAN) {
        for (int i = 0; i < 3; i++) {
            fprintf(stderr, "  lane%d: layers[%d..%d) cur=%d/%d diff=%llu checked=%llu %s\n",
                i, rail->lanes[i].layer_start, rail->lanes[i].layer_end,
                rail->lanes[i].cur_layer, rail->lanes[i].cur_phase,
                (unsigned long long)rail->lanes[i].diff_count,
                (unsigned long long)rail->lanes[i].checked,
                rail->lanes[i].complete ? "[done]" : "");
        }
    }
    if (rail->state == RAIL_PATCH) {
        fprintf(stderr, "  patch: layer=%d phase=%d off=%zu\n",
            rail->patch_layer, rail->patch_phase, rail->patch_off);
    }
    fprintf(stderr, "  last scan: %d%% (%.1f ms)\n",
        rail->change_pct, rail->scan_elapsed_ms);
}


/* =============================================================
 * Destroy
 * ============================================================= */

static inline void kv_remap_rail_destroy(KVRemapRail *rail) {
    fprintf(stderr, "[rail] destroyed (final state=%d, last_pct=%d%%)\n",
        rail->state, rail->change_pct);
    memset(rail, 0, sizeof(*rail));
}


#endif /* KV_REMAP_RAIL_H */
