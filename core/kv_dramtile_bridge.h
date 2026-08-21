/*
 * kv_dramtile_bridge.h — Wire KV Remap blobs into DRamTile twin store
 * ════════════════════════════════════════════════════════════════════
 *
 * Production backing for park/resume: skeleton+delta blobs live in a
 * mmap'd twin file (disk at RAM speed, zero-copy reads via dt_get).
 * Same wire format as kv_geofs_bridge ("<agent>.skel"/"<agent>.delta").
 *
 * Unlike the GeoFS demo volume (1.3MB in-memory), a twin store scales
 * to GB — one file holds every parked agent.
 */
#ifndef KV_DRAMTILE_BRIDGE_H
#define KV_DRAMTILE_BRIDGE_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "kv_remap.h"
#include "dramtile_store.h"

/* ── Delta serialization (same layout as kv_geofs_bridge) ── */
#define KVD_DELTA_HDR   3u   /* type(1) + pct(2 LE) */

static inline size_t kv_dt_delta_serialize(const KVRemapCtx *ctx,
                                           uint8_t **out)
{
    uint8_t type = ctx->delta.type;
    size_t payload = 0;
    const uint8_t *blob = NULL;
    uint8_t *ser = NULL;

    if (type == DELTA_ENTROPY && ctx->delta.entropy_data) {
        blob = (const uint8_t *)ctx->delta.entropy_data;
        payload = ctx->delta.entropy_size;
    } else if (type == DELTA_GEO && ctx->delta.geo_data) {
        payload = 4 + (size_t)ctx->delta.n_ranges * sizeof(GeoRange)
                    + ctx->delta.geo_data_size;
        ser = (uint8_t *)malloc(payload);
        if (!ser) return 0;
        uint32_t nr = ctx->delta.n_ranges;
        memcpy(ser, &nr, 4);
        memcpy(ser + 4, ctx->delta.ranges, (size_t)nr * sizeof(GeoRange));
        memcpy(ser + 4 + (size_t)nr * sizeof(GeoRange),
               ctx->delta.geo_data, ctx->delta.geo_data_size);
        blob = ser;
    } else if (type != DELTA_NONE && type != DELTA_REBUILD) {
        return 0;
    }

    uint8_t *file = (uint8_t *)malloc(KVD_DELTA_HDR + payload);
    if (!file) { free(ser); return 0; }
    file[0] = type;
    file[1] = (uint8_t)(ctx->delta.change_pct & 0xFF);
    file[2] = (uint8_t)((ctx->delta.change_pct >> 8) & 0xFF);
    if (payload) memcpy(file + KVD_DELTA_HDR, blob, payload);
    free(ser);
    *out = file;
    return KVD_DELTA_HDR + payload;
}

/* ── Park: skeleton + delta blobs → twin store ──────────── */
static inline int kv_dt_park(DRamTileStore *store, KVRemapCtx *ctx,
                             const char *agent)
{
    if (!store || !ctx || !agent || !ctx->enabled || !ctx->skeleton_valid)
        return -1;

    char name[DT_NAME_MAX];

    snprintf(name, sizeof(name), "%s.skel", agent);
    if (!dt_put(store, name, (const uint8_t *)ctx->skeleton_data,
                ctx->skeleton_comp))
        return -2;

    snprintf(name, sizeof(name), "%s.delta", agent);
    uint8_t *dbuf = NULL;
    size_t dsz = kv_dt_delta_serialize(ctx, &dbuf);
    if (dsz == 0) return -3;
    uint8_t *p = dt_put(store, name, dbuf, dsz);
    free(dbuf);
    return p ? 0 : -4;
}

/* ── Resume: twin store → live KV (zero-copy blob reads) ── */
/* ctx must already be kv_remap_register()'d on the live buffers. */
static inline int kv_dt_resume(DRamTileStore *store, KVRemapCtx *ctx,
                               const char *agent)
{
    if (!store || !ctx || !agent || !ctx->enabled) return -1;

    char name[DT_NAME_MAX];

    /* ── skeleton (pointer into mmap — no copy) ── */
    snprintf(name, sizeof(name), "%s.skel", agent);
    const uint8_t *skel_blob = dt_get(store, name);
    if (!skel_blob) return -2;
    size_t skel_sz = dt_get_size(store, name);

    size_t raw_sz = 0;
    void *raw = kv_remap_decompress(skel_blob, skel_sz, &raw_sz);
    if (!raw || raw_sz != ctx->total_kv_bytes) { free(raw); return -3; }

    free(ctx->ref_skeleton);
    ctx->ref_skeleton = (uint8_t *)raw;
    ctx->ref_size = raw_sz;
    free(ctx->skeleton_data);
    ctx->skeleton_data = NULL;
    ctx->skeleton_comp = 0;
    ctx->skeleton_orig = raw_sz;
    ctx->skeleton_valid = 1;

    /* ── delta ── */
    snprintf(name, sizeof(name), "%s.delta", agent);
    const uint8_t *dbuf = dt_get(store, name);
    if (!dbuf) return -4;
    size_t dsz = dt_get_size(store, name);
    if (dsz < KVD_DELTA_HDR) return -5;

    memset(&ctx->delta, 0, sizeof(ctx->delta));
    ctx->delta.type = dbuf[0];
    ctx->delta.change_pct = (uint16_t)(dbuf[1] | (dbuf[2] << 8));

    const uint8_t *pl = dbuf + KVD_DELTA_HDR;
    size_t pl_sz = dsz - KVD_DELTA_HDR;
    int rc = 0;

    if (ctx->delta.type == DELTA_ENTROPY) {
        ctx->delta.entropy_data = malloc(pl_sz ? pl_sz : 1);
        if (!ctx->delta.entropy_data) { rc = -6; goto out; }
        memcpy(ctx->delta.entropy_data, pl, pl_sz);
        ctx->delta.entropy_size = pl_sz;
        ctx->delta.delta_size = pl_sz;
    } else if (ctx->delta.type == DELTA_GEO) {
        if (pl_sz < 4) { rc = -7; goto out; }
        uint32_t nr;
        memcpy(&nr, pl, 4);
        if ((size_t)4 + (size_t)nr * sizeof(GeoRange) > pl_sz) {
            rc = -7; goto out;
        }
        ctx->delta.n_ranges = nr;
        memcpy(ctx->delta.ranges, pl + 4, (size_t)nr * sizeof(GeoRange));
        size_t hdr = 4 + (size_t)nr * sizeof(GeoRange);
        size_t gd = pl_sz - hdr;
        ctx->delta.geo_data = malloc(gd ? gd : 1);
        if (!ctx->delta.geo_data) { rc = -6; goto out; }
        memcpy(ctx->delta.geo_data, pl + hdr, gd);
        ctx->delta.geo_data_size = gd;
        ctx->delta.delta_size = pl_sz;
    } else if (ctx->delta.type != DELTA_NONE &&
               ctx->delta.type != DELTA_REBUILD) {
        rc = -7;
    }

out:
    if (rc) return rc;
    return kv_remap_restore(ctx);
}

#endif /* KV_DRAMTILE_BRIDGE_H */
