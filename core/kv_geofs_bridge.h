/*
 * kv_geofs_bridge.h — Wire KV Remap (skeleton+delta) into GeoFS
 * ══════════════════════════════════════════════════════════════
 *
 * Park/resume an agent's KV cache through a GeosVolume:
 *   snapshot : skeleton blob + current delta blob → geofs files
 *   resume   : geofs files → ref_skeleton + delta → kv_remap_restore
 *
 * Files per agent:
 *   "<agent>.skel"  compressed skeleton (Diamond Shell / RLE blob)
 *   "<agent>.delta" [type:1][pct:2][payload]
 *     ENTROPY payload: compressed XOR diff blob
 *     GEO     payload: [n_ranges:4][ranges*n][xor bytes]
 *     NONE/REBUILD: empty payload
 *
 * Lossless contract: resume(snapshot(live)) == live, byte-for-byte.
 */
#ifndef KV_GEOFS_BRIDGE_H
#define KV_GEOFS_BRIDGE_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "kv_remap.h"
#include "kv_remap_rail.h"
#include "geofs_core.h"

/* ── Delta file layout ──────────────────────────────────── */
#define KVG_DELTA_HDR   3u   /* type(1) + pct(2 LE) */

static inline int kv_geofs_drop(GeosVolume *v, const char *agent);

/* ── Snapshot: write skeleton + delta blobs into geofs ──── */
static inline int kv_geofs_snapshot(GeosVolume *v, KVRemapCtx *ctx,
                                    const char *agent)
{
    if (!v || !ctx || !agent || !ctx->enabled || !ctx->skeleton_valid)
        return -1;

    char name[GEOS_MAX_NAME];

    /* ── skeleton file (overwrite-safe) ── */
    snprintf(name, sizeof(name), "%s.skel", agent);
    if (geos_find(v, name)) geos_delete(v, name);
    if (!geos_create(v, name, (uint32_t)ctx->skeleton_comp,
                     (const uint8_t *)ctx->skeleton_data))
        return -2;
    /* geos_create only registers the inode — data lands via geos_write */
    if (ctx->skeleton_comp > 0 &&
        geos_write(v, name, (const uint8_t *)ctx->skeleton_data,
                   (uint32_t)ctx->skeleton_comp) != (int)ctx->skeleton_comp)
        return -2;

    /* ── delta file ── */
    snprintf(name, sizeof(name), "%s.delta", agent);
    if (geos_find(v, name)) geos_delete(v, name);

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
        if (!ser) return -3;
        uint32_t nr = ctx->delta.n_ranges;
        memcpy(ser, &nr, 4);
        memcpy(ser + 4, ctx->delta.ranges,
               (size_t)nr * sizeof(GeoRange));
        memcpy(ser + 4 + (size_t)nr * sizeof(GeoRange),
               ctx->delta.geo_data, ctx->delta.geo_data_size);
        blob = ser;
    } else if (type != DELTA_NONE && type != DELTA_REBUILD) {
        return -4;
    }

    uint8_t *file = (uint8_t *)malloc(KVG_DELTA_HDR + payload);
    if (!file) { free(ser); return -3; }
    file[0] = type;
    file[1] = (uint8_t)(ctx->delta.change_pct & 0xFF);
    file[2] = (uint8_t)((ctx->delta.change_pct >> 8) & 0xFF);
    if (payload) memcpy(file + KVG_DELTA_HDR, blob, payload);
    free(ser);

    GeosInode *in = geos_create(v, name, KVG_DELTA_HDR + payload, file);
    if (!in) { free(file); return -5; }
    geos_write(v, name, file, KVG_DELTA_HDR + (uint32_t)payload);
    free(file);
    return 0;
}

/* ── Resume: rebuild live KV from geofs blobs ───────────── */
/* ctx must already be kv_remap_register()'d on the live buffers. */
static inline int kv_geofs_resume(GeosVolume *v, KVRemapCtx *ctx,
                                  const char *agent)
{
    if (!v || !ctx || !agent || !ctx->enabled) return -1;

    char name[GEOS_MAX_NAME];

    /* ── skeleton → ref_skeleton ── */
    snprintf(name, sizeof(name), "%s.skel", agent);
    GeosInode *sk = geos_find(v, name);
    if (!sk) return -2;
    uint8_t *skel_blob = (uint8_t *)malloc(sk->size_bytes);
    if (!skel_blob) return -3;
    if (geos_read(v, name, skel_blob, sk->size_bytes) != (int)sk->size_bytes) {
        free(skel_blob); return -4;
    }

    size_t raw_sz = 0;
    void *raw = kv_remap_decompress(skel_blob, sk->size_bytes, &raw_sz);
    free(skel_blob);
    if (!raw || raw_sz != ctx->total_kv_bytes) { free(raw); return -5; }

    free(ctx->ref_skeleton);
    ctx->ref_skeleton = (uint8_t *)raw;
    ctx->ref_size = raw_sz;
    free(ctx->skeleton_data);
    ctx->skeleton_data = NULL;
    ctx->skeleton_comp = 0;
    ctx->skeleton_orig = raw_sz;
    ctx->skeleton_valid = 1;

    /* ── delta file → ctx->delta ── */
    snprintf(name, sizeof(name), "%s.delta", agent);
    GeosInode *dl = geos_find(v, name);
    if (!dl) return -6;

    uint8_t *dbuf = (uint8_t *)malloc(dl->size_bytes);
    if (!dbuf) return -3;
    if (geos_read(v, name, dbuf, dl->size_bytes) != (int)dl->size_bytes) {
        free(dbuf); return -4;
    }

    memset(&ctx->delta, 0, sizeof(ctx->delta));
    ctx->delta.type = dbuf[0];
    ctx->delta.change_pct = (uint16_t)(dbuf[1] | (dbuf[2] << 8));

    int rc = 0;
    const uint8_t *pl = dbuf + KVG_DELTA_HDR;
    size_t pl_sz = dl->size_bytes - KVG_DELTA_HDR;

    if (ctx->delta.type == DELTA_ENTROPY) {
        ctx->delta.entropy_data = malloc(pl_sz ? pl_sz : 1);
        if (!ctx->delta.entropy_data) { rc = -3; goto out; }
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
        if (!ctx->delta.geo_data) { rc = -3; goto out; }
        memcpy(ctx->delta.geo_data, pl + hdr, gd);
        ctx->delta.geo_data_size = gd;
        ctx->delta.delta_size = pl_sz;
    } else if (ctx->delta.type != DELTA_NONE &&
               ctx->delta.type != DELTA_REBUILD) {
        rc = -7;
    }

out:
    free(dbuf);
    if (rc) return rc;
    return kv_remap_restore(ctx);
}

/* ── Drop both agent files ──────────────────────────────── */
static inline int kv_geofs_drop(GeosVolume *v, const char *agent)
{
    if (!v || !agent) return -1;
    char name[GEOS_MAX_NAME];
    int rc = 0;
    snprintf(name, sizeof(name), "%s.skel", agent);
    if (geos_delete(v, name) != 0) rc = -1;
    snprintf(name, sizeof(name), "%s.delta", agent);
    if (geos_delete(v, name) != 0) rc = -1;
    return rc;
}

/* ═══════════════════════════════════════════════════════════
   RAIL CHECKPOINT — persist mid-scan rail state to GeoFS
   ═══════════════════════════════════════════════════════════
   Fresh-process restore: save → destroy everything → load →
   continue stepping → same decision as uninterrupted scan.
   Layout (all fixed-width, no pointers):
     [state:4][lane:4][freeze_state:4][freeze_lane:4]
     [total_diff:8][total_checked:8][change_pct:4]
     [patch_layer:4][patch_phase:4][patch_off:8]
     lane×3: [layer_start:4][layer_end:4][cur_layer:4][cur_phase:4]
             [cur_off:8][diff_count:8][checked:8][total:8][complete:4]
   ═══════════════════════════════════════════════════════════ */

#define KVG_RAIL_STATE_SIZE  (4*4 + 8 + 8 + 4 + 4 + 4 + 8 + \
                              3 * (4*4 + 8*4 + 4))

static inline int kv_geofs_rail_save(GeosVolume *v, const char *agent,
                                     const KVRemapRail *rail)
{
    if (!v || !agent || !rail) return -1;

    uint8_t buf[KVG_RAIL_STATE_SIZE];
    size_t o = 0;
    #define KVG_PUT32(x) do { uint32_t _t=(uint32_t)(x); \
        memcpy(buf+o,&_t,4); o+=4; } while(0)
    #define KVG_PUT64(x) do { uint64_t _t=(uint64_t)(x); \
        memcpy(buf+o,&_t,8); o+=8; } while(0)

    KVG_PUT32(rail->state); KVG_PUT32(rail->lane);
    KVG_PUT32(rail->freeze_state); KVG_PUT32(rail->freeze_lane);
    KVG_PUT64(rail->total_diff); KVG_PUT64(rail->total_checked);
    KVG_PUT32(rail->change_pct);
    KVG_PUT32(rail->patch_layer); KVG_PUT32(rail->patch_phase);
    KVG_PUT64(rail->patch_off);
    for (int i = 0; i < 3; i++) {
        const RailLane *ln = &rail->lanes[i];
        KVG_PUT32(ln->layer_start); KVG_PUT32(ln->layer_end);
        KVG_PUT32(ln->cur_layer);   KVG_PUT32(ln->cur_phase);
        KVG_PUT64(ln->cur_off);     KVG_PUT64(ln->diff_count);
        KVG_PUT64(ln->checked);     KVG_PUT64(ln->total);
        KVG_PUT32(ln->complete);
    }
    #undef KVG_PUT32
    #undef KVG_PUT64

    char name[GEOS_MAX_NAME];
    snprintf(name, sizeof(name), "%s.rail", agent);
    if (geos_find(v, name)) geos_delete(v, name);
    if (!geos_create(v, name, (uint32_t)o, buf)) return -2;
    geos_write(v, name, buf, (uint32_t)o);
    return 0;
}

static inline int kv_geofs_rail_load(GeosVolume *v, const char *agent,
                                     KVRemapRail *rail)
{
    if (!v || !agent || !rail || !rail->remap) return -1;

    char name[GEOS_MAX_NAME];
    snprintf(name, sizeof(name), "%s.rail", agent);
    GeosInode *in = geos_find(v, name);
    if (!in) return -2;
    if (in->size_bytes != KVG_RAIL_STATE_SIZE) return -3;

    uint8_t buf[KVG_RAIL_STATE_SIZE];
    if (geos_read(v, name, buf, sizeof(buf)) != (int)sizeof(buf)) return -4;

    size_t o = 0;
    #define KVG_GET32(dst) do { uint32_t _t; memcpy(&_t,buf+o,4); \
        o+=4; (dst) = (_t); } while(0)
    #define KVG_GET64(dst) do { uint64_t _t; memcpy(&_t,buf+o,8); \
        o+=8; (dst) = (_t); } while(0)

    KVRemapCtx *back = rail->remap;
    memset(rail, 0, sizeof(*rail));
    rail->remap = back;
    rail->enabled = 1;

    KVG_GET32(rail->state); KVG_GET32(rail->lane);
    KVG_GET32(rail->freeze_state); KVG_GET32(rail->freeze_lane);
    KVG_GET64(rail->total_diff); KVG_GET64(rail->total_checked);
    KVG_GET32(rail->change_pct);
    KVG_GET32(rail->patch_layer); KVG_GET32(rail->patch_phase);
    KVG_GET64(rail->patch_off);
    for (int i = 0; i < 3; i++) {
        RailLane *ln = &rail->lanes[i];
        KVG_GET32(ln->layer_start); KVG_GET32(ln->layer_end);
        KVG_GET32(ln->cur_layer);   KVG_GET32(ln->cur_phase);
        KVG_GET64(ln->cur_off);     KVG_GET64(ln->diff_count);
        KVG_GET64(ln->checked);     KVG_GET64(ln->total);
        KVG_GET32(ln->complete);
    }
    #undef KVG_GET32
    #undef KVG_GET64
    return 0;
}

/* ── Rail cycle + auto-snapshot on decision ─────────────── */
/* One idle-friendly step. When a scan completes with a delta-worthy
   change (soft < pct < hard → PATCH decision), stores the delta and
   snapshots to GeoFS before the patch phase runs.
   Returns kv_remap_rail_step's value, or -2 on snapshot failure.  */
static inline int kv_geofs_rail_cycle(GeosVolume *v, const char *agent,
                                      KVRemapRail *rail)
{
    int r = kv_remap_rail_step(rail);
    if (r != 1) return r;
    /* scan-complete lands in PATCH (band) / REBUILD / PARK */
    if (rail->state == RAIL_PATCH &&
        rail->change_pct > RAIL_SOFT_THRESH &&
        rail->change_pct < RAIL_HARD_THRESH) {
        KVRemapCtx *ctx = rail->remap;
        if (kv_remap_store_delta(ctx, rail->change_pct) != 0) return -2;
        if (kv_geofs_snapshot(v, ctx, agent) != 0) return -2;
    }
    return r;
}

#endif /* KV_GEOFS_BRIDGE_H */
