/*
 * geofs_multivol.h — Option A: multi-volume GeoFS (2026-08-21)
 * ═══════════════════════════════════════════════════════════
 * One GeosVolume = one 20736-address field = ~1.3 MB of blocks.
 * Real models need more, so the outer dimension is a volume index:

 *     file = (vol_idx, seed, axis)      — still MAP: no block list
 *     address(b) = hw_at(seed, axis, b) within its own volume

 * A file lives ENTIRELY inside one volume (no striping): placement
 * tries existing volumes via geos_hyper_find_seed and opens a fresh
 * volume only when none fits. The name→volume directory is a linear
 * scan (same style as geos_find — no hash).
 *
 * Hot-path access resolves once (geos_mv_resolve → GeosMvInode) and
 * then reuses the inode-resolved core APIs directly.
 *
 * Header-only, static inline. Volumes are heap-allocated lazily
 * (~1.3 MB each). Serialize/deserialize stays per-volume (.geofs).
 */
#ifndef GEOFS_MULTIVOL_H
#define GEOFS_MULTIVOL_H

#include <stdlib.h>
#include <string.h>
#include "geofs_core.h"

typedef struct {
    GeosVolume *vol;     /* owning volume (NULL until resolved) */
    GeosInode  *in;      /* inode inside vol->inodes            */
} GeosMvInode;

typedef struct {
    char     name[GEOS_MAX_NAME];
    uint32_t vol;        /* index into GeosMV.vol[] */
} GeosMvDirEntry;

typedef struct {
    GeosVolume    **vol;      /* lazily created volumes          */
    uint32_t       n_used;    /* volumes in use                  */
    uint32_t       n_cap;     /* allocated slots in vol[]        */
    GeosMvDirEntry *dir;      /* name → volume index             */
    uint32_t       dir_n;
    uint32_t       dir_cap;
} GeosMV;

static inline int geos_mv_init(GeosMV *m, uint32_t vol_hint) {
    if (!m || vol_hint == 0u) return -1;
    memset(m, 0, sizeof(*m));
    m->n_cap = vol_hint;
    m->vol = (GeosVolume **)calloc(m->n_cap, sizeof(GeosVolume *));
    m->dir_cap = vol_hint * 16u;
    m->dir = (GeosMvDirEntry *)calloc(m->dir_cap, sizeof(GeosMvDirEntry));
    if (!m->vol || !m->dir) return -1;
    return 0;
}

static inline void geos_mv_free(GeosMV *m) {
    if (!m) return;
    for (uint32_t i = 0; i < m->n_used; i++) {
        geos_volume_free(m->vol[i]);
        free(m->vol[i]);
    }
    free(m->vol);
    free(m->dir);
    memset(m, 0, sizeof(*m));
}

/* get volume i, creating it on first touch */
static inline GeosVolume* geos_mv_volume(GeosMV *m, uint32_t i) {
    if (!m || i >= m->n_cap) return NULL;
    if (!m->vol[i]) {
        m->vol[i] = (GeosVolume *)calloc(1, sizeof(GeosVolume));
        if (!m->vol[i]) return NULL;
        geos_volume_init(m->vol[i]);
    }
    if (i >= m->n_used) m->n_used = i + 1u;
    return m->vol[i];
}

/* resolve name → (volume, inode); NULL when absent or not hyper */
static inline int geos_mv_resolve(GeosMV *m, const char *name,
                                  GeosMvInode *out) {
    if (!m || !name || !out) return -1;
    for (uint32_t i = 0; i < m->dir_n; i++) {
        if (strcmp(m->dir[i].name, name) == 0) {
            GeosVolume *v = m->vol[m->dir[i].vol];
            if (!v) return -1;
            out->vol = v;
            out->in  = geos_find(v, name);
            return (out->in && (out->in->flags & GEOS_FLAG_HYPER)) ? 0 : -1;
        }
    }
    return -1;
}

/* place a whole file into ONE volume — tries volumes in order with
 * geos_hyper_find_seed, appends a fresh volume when none fits.
 * Returns 0 on success. */
static inline int geos_mv_place(GeosMV *m, const char *name,
                                uint32_t size_bytes, const uint8_t *data,
                                uint32_t axis) {
    if (!m || !name || !*name) return -1;
    GeosMvInode hit;
    if (geos_mv_resolve(m, name, &hit) == 0) return -2;   /* exists */

    uint32_t n_blocks = (size_bytes + GEOS_BLOCK_SZ - 1) / GEOS_BLOCK_SZ;

    for (uint32_t i = 0; i < m->n_used; i++) {
        GeosVolume *v = m->vol[i];
        if ((uint32_t)n_blocks > v->total_blocks_free) continue;
        uint32_t seed = geos_hyper_find_seed(v, GEOS_VOL_DATA_START,
                                             n_blocks, hw_stride(axis % HW_AXES));
        if (seed == 0xFFFFFFFFu) continue;
        if (!geos_hyper_place(v, name, size_bytes, data, seed, axis)) continue;
        if (m->dir_n == m->dir_cap) return -3;           /* dir full */
        snprintf(m->dir[m->dir_n].name, GEOS_MAX_NAME, "%s", name);
        m->dir[m->dir_n].vol = i;
        m->dir_n++;
        return 0;
    }

    /* no existing volume fits → open a new one */
    if (m->n_used == m->n_cap) {
        uint32_t nc = m->n_cap * 2u;
        GeosVolume **nv = (GeosVolume **)realloc(m->vol, nc * sizeof(*nv));
        if (!nv) return -4;
        memset(nv + m->n_cap, 0, (nc - m->n_cap) * sizeof(*nv));
        m->vol = nv; m->n_cap = nc;
    }
    GeosVolume *v = geos_mv_volume(m, m->n_used);
    if (!v) return -4;
    uint32_t seed = geos_hyper_find_seed(v, GEOS_VOL_DATA_START,
                                         n_blocks, hw_stride(axis % HW_AXES));
    if (seed == 0xFFFFFFFFu) return -5;   /* file larger than a volume */
    if (!geos_hyper_place(v, name, size_bytes, data, seed, axis)) return -5;
    if (m->dir_n == m->dir_cap) return -3;
    snprintf(m->dir[m->dir_n].name, GEOS_MAX_NAME, "%s", name);
    m->dir[m->dir_n].vol = m->n_used - 1u;
    m->dir_n++;
    return 0;
}

/* read through the directory (resolve + inode-resolved read) */
static inline int geos_mv_read(GeosMV *m, const char *name,
                               uint8_t *buf, uint32_t buf_size) {
    GeosMvInode mi;
    if (geos_mv_resolve(m, name, &mi) != 0) return -1;
    return geos_hyper_read_inode(mi.vol, mi.in, buf, buf_size);
}

/* zero-copy project through the directory */
static inline const uint8_t* geos_mv_project_block(GeosMV *m, const char *name,
                                                   uint32_t b) {
    GeosMvInode mi;
    if (geos_mv_resolve(m, name, &mi) != 0) return NULL;
    return geos_hyper_project_block_inode(mi.vol, mi.in, b);
}

/* delete: free blocks in the owning volume + drop the dir entry */
static inline int geos_mv_delete(GeosMV *m, const char *name) {
    if (!m || !name) return -1;
    for (uint32_t i = 0; i < m->dir_n; i++) {
        if (strcmp(m->dir[i].name, name) == 0) {
            int rc = geos_delete(m->vol[m->dir[i].vol], name);
            m->dir[i] = m->dir[m->dir_n - 1u];   /* swap-remove */
            m->dir_n--;
            return rc;
        }
    }
    return -1;
}

#endif /* GEOFS_MULTIVOL_H */
