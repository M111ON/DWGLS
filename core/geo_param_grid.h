/* ═══════════════════════════════════════════════════════════════════════════
 * geo_param_grid.h — Parameterized Geometry Grid (MAP NOT COMPRESS)
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * One family: dodeca root → compound → goldberg → pentakis.
 * Select geometry via GeoType enum — all shapes derive from the same parent.
 *
 * WORKING CODEC (lossless roundtrip):
 *   Encode:  sort weights → codebook (distinct values) →
 *            each weight → index into codebook → (codebook + idx-stream)
 *   Decode:  read idx-stream → look up value → reconstruct in original order
 *
 * Geometry provides: mask bit per vertex (which slots used) + addressing.
 * Compression comes from codebook collapse (repetition); geometry = mask.
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifndef GEO_PARAM_GRID_H
#define GEO_PARAM_GRID_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ═══════════════════════════════════════════════════════════════
   GEOMETRY FAMILY
   ═══════════════════════════════════════════════════════════════ */

typedef enum {
    GEO_AUTO             = 0,   /* auto-select smallest that fits */
    GEO_DODEC_BASE       = 12,  /* dodecahedron: 12 faces */
    GEO_ICO_BASE         = 20,  /* icosahedron: 20 faces */
    GEO_COMPOUND_24      = 24,  /* inverted dodeca compound */
    GEO_DODEC_EDGES      = 30,  /* 30 edges */
    GEO_COMPOUND_60      = 60,  /* pentakis dodeca (60 faces) */
    GEO_PENTAKIS_72      = 72,  /* 12 base + 60 pyramids */
    GEO_GOLDBERG_92      = 92,
    GEO_COMP_SPIKE_120   = 120,
    GEO_GOLDBERG_132     = 132,
    GEO_COMPOUND_144     = 144, /* 6 × 24 = 144 */
    GEO_GOLDBERG_192     = 192,
} GeoType;

/* ═══════════════════════════════════════════════════════════════
   PROPS TABLE
   ═══════════════════════════════════════════════════════════════ */

typedef struct {
    GeoType  type;
    uint32_t verts;
    uint32_t edges;
    uint32_t faces;
    uint32_t cells;
    uint32_t slot_cap;
} GeoProps;

static inline GeoProps geo_props(GeoType t)
{
    GeoProps p = {0};
    p.type = t; p.slot_cap = 256;
    switch (t) {
    case GEO_DODEC_BASE:      p.verts=20; p.edges=30; p.faces=12; p.cells=1; break;
    case GEO_ICO_BASE:        p.verts=20; p.edges=30; p.faces=20; p.cells=1; break;
    case GEO_COMPOUND_24:     p.verts=24; p.edges=48; p.faces=24; p.cells=6; break;
    case GEO_DODEC_EDGES:     p.verts=30; p.edges=60; p.faces=32; p.cells=1; break;
    case GEO_COMPOUND_60:     p.verts=60; p.edges=90; p.faces=32; p.cells=1; break;
    case GEO_PENTAKIS_72:     p.verts=72; p.edges=90; p.faces=32; p.cells=1; break;
    case GEO_GOLDBERG_92:     p.verts=92; p.edges=270;p.faces=92; p.cells=1; break;
    case GEO_COMP_SPIKE_120:  p.verts=120;p.edges=180;p.faces=62; p.cells=1; break;
    case GEO_GOLDBERG_132:    p.verts=132;p.edges=270;p.faces=92; p.cells=1; break;
    case GEO_COMPOUND_144:    p.verts=144; p.edges=576; p.faces=576; p.cells=144; break;
    case GEO_GOLDBERG_192:    p.verts=192;p.edges=270;p.faces=92; p.cells=1; break;
    default:                  p.verts=20; p.edges=30; p.faces=12; p.cells=1; break;
    }
    return p;
}

/* ═══════════════════════════════════════════════════════════════
   CODEC
   ═══════════════════════════════════════════════════════════════ */

#define GEO_MAX_DISTINCT  (1u<<24)  /* 16M — codebook must hold ALL distinct */

typedef struct {
    /* inputs */
    const float *weights;
    uint32_t     n_weights;
    GeoType      type;
    GeoProps     props;

    /* codebook */
    float      *uniq;
    uint32_t    n_uniq;

    /* idx stream */
    uint32_t   *idx;
    uint32_t    idx_bits;

    /* geometry */
    uint32_t    n_used_verts;

    /* sizes */
    uint64_t    codebook_len;
    uint64_t    idx_len;
    uint64_t    mask_len;
    uint64_t    total_len;
    float       ratio;
} GeoCodec;

static int _cmpf(const void *a, const void *b)
{
    float da = *(const float*)a, db = *(const float*)b;
    return (da > db) - (da < db);
}

static inline int geo_codec_init(GeoCodec *gc, GeoType t, const float *w, uint32_t n)
{
    if (!gc || !w || n == 0) return -1;
    memset(gc, 0, sizeof(*gc));
    gc->weights   = w;
    gc->n_weights = n;
    gc->type      = t;
    gc->props     = geo_props(t);

    /* sorted copy */
    float *sorted = (float*)malloc(n * sizeof(float));
    if (!sorted) return -1;
    memcpy(sorted, w, n * sizeof(float));
    qsort(sorted, n, sizeof(float), _cmpf);

    /* count distinct */
    uint32_t nd = 1;
    for (uint32_t i = 1; i < n; i++)
        if (sorted[i] != sorted[i-1]) nd++;
    if (nd > GEO_MAX_DISTINCT) nd = GEO_MAX_DISTINCT;
    gc->n_uniq = nd;

    /* codebook */
    gc->uniq = (float*)malloc(nd * sizeof(float));
    if (!gc->uniq) { free(sorted); return -1; }
    uint32_t u = 0;
    gc->uniq[u] = sorted[0];
    for (uint32_t i = 1; i < n; i++) {
        if (sorted[i] != sorted[i-1]) {
            if (++u >= GEO_MAX_DISTINCT) break;  /* keep within allocation */
            gc->uniq[u] = sorted[i];
        }
    }
    free(sorted);

    /* bits per index */
    uint32_t ib = 1;
    while ((1u<<ib) < nd) ib++;
    gc->idx_bits = ib;

    /* idx stream: binary search per weight */
    gc->idx = (uint32_t*)malloc(n * sizeof(uint32_t));
    if (!gc->idx) { free(gc->uniq); gc->uniq=NULL; return -1; }
    for (uint32_t i = 0; i < n; i++) {
        float target = w[i];
        uint32_t lo = 0, hi = nd;
        while (lo < hi) {
            uint32_t mid = (lo+hi)>>1;
            if (gc->uniq[mid] < target) lo = mid+1; else hi = mid;
        }
        gc->idx[i] = (lo < nd) ? lo : 0;
    }

    /* geometry: used verts = distinct clamped to capacity */
    uint64_t capacity = (uint64_t)gc->props.verts * gc->props.slot_cap;
    gc->n_used_verts = (uint64_t)nd < capacity ? nd : (uint32_t)capacity;

    /* lengths */
    gc->codebook_len = (uint64_t)nd * 4;
    gc->idx_len      = ((uint64_t)n * gc->idx_bits + 7) / 8;
    gc->mask_len     = (gc->props.verts + 7) / 8;
    gc->total_len    = gc->codebook_len + gc->idx_len + gc->mask_len + 16;

    uint64_t raw = (uint64_t)n * 4;
    gc->ratio = raw ? (float)((double)raw / (double)gc->total_len) : 0.0f;
    return 0;
}

static inline void geo_codec_free(GeoCodec *gc)
{
    free(gc->uniq);
    free(gc->idx);
    memset(gc, 0, sizeof(*gc));
}

static inline int geo_codec_decode(GeoCodec *gc, float *out, uint32_t out_n)
{
    if (!gc || !out || out_n < gc->n_weights) return -1;
    for (uint32_t i = 0; i < gc->n_weights; i++) {
        uint32_t c = gc->idx[i];
        out[i] = (c < gc->n_uniq) ? gc->uniq[c] : gc->uniq[0];
    }
    return 0;
}

static inline int geo_codec_verify(GeoCodec *gc)
{
    float *recon = (float*)malloc(gc->n_weights * sizeof(float));
    if (!recon) return -1;
    memset(recon, 0, gc->n_weights * sizeof(float));
    if (geo_codec_decode(gc, recon, gc->n_weights) != 0) { free(recon); return -1; }
    uint32_t mm = 0;
    int shown = 0;
    for (uint32_t i = 0; i < gc->n_weights; i++) {
        float a = gc->weights[i], b = recon[i];
        if (a != b && !(a!=a && b!=b)) {
            mm++;
            if (!shown && mm <= 5) {
                printf("    [mismatch] i=%u w=%.9g idx=%u uniq=%d => recon=%.9g\n",
                       i, a, gc->idx[i], (int)gc->n_uniq, b);
                shown = 1;
            }
        }
    }
    free(recon);
    return mm ? -1 : 0;
}

static inline void geo_codec_stats(const GeoCodec *gc)
{
    GeoProps p = gc->props;
    printf("===============================================================\n");
    printf("  Geo Parametric Grid  (%u)\n", (unsigned)gc->type);
    printf("---------------------------------------------------------------\n");
    printf("  Geometry:    %u verts, %u edges, %u faces\n", p.verts, p.edges, p.faces);
    printf("  Weights:     %u\n", gc->n_weights);
    printf("  Distinct:    %u\n", gc->n_uniq);
    printf("  idx bits/val:%u\n", gc->idx_bits);
    printf("  Raw bytes:   %I64u\n", (uint64_t)gc->n_weights*4);
    printf("  Codebook:    %I64u B\n", gc->codebook_len);
    printf("  idx stream:  %I64u B\n", gc->idx_len);
    printf("  mask:        %I64u B\n", gc->mask_len);
    printf("  Total out:   %I64u B\n", gc->total_len);
    printf("  Ratio:       %.3fx\n", gc->ratio);
    printf("===============================================================\n");
}

#endif /* GEO_PARAM_GRID_H */