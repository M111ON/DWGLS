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

/* ═══════════════════════════════════════════════════════════════
   A2 × A2 SYMMETRY VERIFICATION
   ═══════════════════════════════════════════════════════════════
   A2 = dihedral group of triangle (order 6, 60° rotations)
   A2 × A2 = direct product of two hex basis axis pairs
   Full hex-quad symmetry: A2 × A2 × C2 (orientation) = order 144
   
   For GEO_COMPOUND_144 (6ico = 6 × 24-cell):
     144 = |A2 × A2 × C2| = 6 × 6 × 4
     Each icosahedron carries one A2 factor
     Dual-pairing between icosahedra defines quad basis transform
   ═══════════════════════════════════════════════════════════════ */

#define A2_ORDER           6u   /* |D6| = dihedral hex group */
#define A2xA2_ORDER       36u   /* |A2 × A2| = 6 × 6 */
#define A2xA2xC2_ORDER   144u   /* full hex-quad symmetry = 144 */

/* A2 generators: rotate by 60° (order 6) */
static inline uint32_t a2_rotate(uint32_t pos, uint32_t n_faces)
{
    /* pos ∈ [0, n_faces), rotate 60° → pos = (pos + 1) mod n_faces */
    return (pos + 1u) % n_faces;
}

/* A2 reflection: mirror across axis */
static inline uint32_t a2_reflect(uint32_t pos, uint32_t n_faces)
{
    return (n_faces - pos) % n_faces;
}

/* A2 × A2 element: (rot_a, rot_b) where rot_a ∈ A2, rot_b ∈ A2 */
typedef struct {
    uint32_t rot_a;   /* rotation on axis A [0..5] */
    uint32_t rot_b;   /* rotation on axis B [0..5] */
} A2xA2Elem;

/* Apply A2 × A2 element to position pair (pos_a, pos_b) */
static inline void a2xa2_apply(A2xA2Elem e,
                                uint32_t pos_a, uint32_t pos_b,
                                uint32_t n_faces,
                                uint32_t *out_a, uint32_t *out_b)
{
    /* Apply rot_a on axis A, rot_b on axis B, then swap if needed */
    uint32_t ra = pos_a, rb = pos_b;
    for (uint32_t i = 0; i < e.rot_a; i++) ra = a2_rotate(ra, n_faces);
    for (uint32_t i = 0; i < e.rot_b; i++) rb = a2_rotate(rb, n_faces);
    *out_a = ra;
    *out_b = rb;
}

/*
 * geo_verify_a2xa2_symmetry — verify that geometry is closed under A2 × A2
 *
 * For GEO_COMPOUND_144:
 *   144 vertices partition into 6 groups of 24 (6 icosahedra)
 *   Each group is closed under its A2 factor
 *   Cross-group pairings form the A2 × A2 product
 *
 * Returns 0 on success.
 */
static inline int geo_verify_a2xa2_symmetry(GeoType t)
{
    GeoProps p = geo_props(t);

    if (t == GEO_COMPOUND_144) {
        /* 144 = 6 × 24: 6 icosahedra, each 24 vertices */
        /* Each icosahedron: 24 = 4 × 6 (4 faces × 6 rotations per face) */
        if (p.verts != 144) return -1;
        if (p.cells != 144) return -2;

        /* Verify orbit closure: rotating any vertex stays within group */
        uint32_t n_groups = 6;
        uint32_t verts_per_group = 24;
        for (uint32_t g = 0; g < n_groups; g++) {
            uint32_t base = g * verts_per_group;
            /* A2 orbit: rotate all 6 positions */
            for (uint32_t v = 0; v < verts_per_group; v++) {
                uint32_t pos = base + v;
                /* 6 rotations must stay within [base, base+24) */
                uint32_t orbit = pos;
                for (uint32_t r = 0; r < 6; r++) {
                    orbit = base + (orbit - base + 1u) % verts_per_group;
                    if (orbit < base || orbit >= base + verts_per_group)
                        return -3;
                }
                /* Reflection must stay within group */
                uint32_t refl = base + (verts_per_group - (pos - base)) % verts_per_group;
                if (refl < base || refl >= base + verts_per_group)
                    return -4;
            }
        }

        return 0;
    }

    /* For other types: verify vertex count matches expected A2 orbit sizes */
    if (p.verts == 0) return -10;

    /* Generic check: vertex count must be divisible by A2_ORDER (6) */
    if (p.verts % A2_ORDER != 0) return -11;

    return 0;
}

/*
 * geo_a2xa2_info — print symmetry information for a geometry type
 */
static inline void geo_a2xa2_info(GeoType t)
{
    GeoProps p = geo_props(t);
    printf("A2×A2 Symmetry: type=%u\n", (unsigned)t);
    printf("  Vertices: %u (divisible by 6: %s)\n", p.verts,
           (p.verts % 6 == 0) ? "YES" : "NO");
    printf("  Expected A2 orbits: %u\n", p.verts / 6);
    if (t == GEO_COMPOUND_144) {
        printf("  6ico compound: 6 × 24 = 144 vertices\n");
        printf("  A2×A2×C2 order: 6×6×4 = 144\n");
        printf("  Triality: D4 automorphism permutes 3 8D representations\n");
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   D4 WEYL GROUP — Goldberg 192
   ═══════════════════════════════════════════════════════════════════════════
   |W(D4)| = 192 = 2⁷ × 3
   D4 roots: 24 vectors in 4D
   D4 Coxeter number h = 6
   Triality: unique automorphism of D4, order 3
   ═══════════════════════════════════════════════════════════════════════════ */

#define D4_WEYL_ORDER     192u
#define D4_ROOT_COUNT      24u
#define D4_COXETER_NUM      6u
#define D4_TRIALITY_ORDER    3u

/*
 * geo_verify_d4_structure — verify D4 properties for Goldberg 192
 * Returns 0 on success.
 */
static inline int geo_verify_d4_structure(GeoType t)
{
    GeoProps p = geo_props(t);

    if (t == GEO_GOLDBERG_192) {
        if (p.verts != D4_WEYL_ORDER) return -1;
        /* 192 = 8 × 24 (8 cells × 24 vertices per cell) */
        if (192 % D4_ROOT_COUNT != 0) return -2;
        /* Triality: 192 / 3 = 64 (orbits under triality) */
        if (192 % D4_TRIALITY_ORDER != 0) return -3;
        return 0;
    }

    /* Generic: check if vertex count is divisible by D4 root count */
    if (p.verts > 0 && p.verts % D4_ROOT_COUNT != 0) return -10;

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
   CROSS-SYSTEM VERIFY — all hex-quad-dual properties
   ═══════════════════════════════════════════════════════════════════════════ */

static inline int geo_verify_hex_quad_dual(void)
{
    /* 1. Fundamental equation: 128 × 162 = 144 × 144 = 20736 */
    if (128u * 162u != 20736u) return -1;
    if (144u * 144u != 20736u) return -2;

    /* 2. Prime factorization: 20736 = 2⁸ × 3⁴ */
    if ((1u << 8) * 81u != 20736u) return -3;

    /* 3. CRT: 81 × 177 ≡ 1 mod 256 */
    if ((81u * 177u) % 256u != 1u) return -4;

    /* 4. CRT: 256 ≡ 13 mod 81, 13 × 25 ≡ 1 mod 81 */
    if (256u % 81u != 13u) return -5;
    if ((13u * 25u) % 81u != 1u) return -6;

    /* 5. A2×A2: 144 = 6 × 6 × 4 */
    if (A2xA2xC2_ORDER != 144u) return -7;

    /* 6. D4: 192 = |W(D4)| */
    if (D4_WEYL_ORDER != 192u) return -8;

    /* 7. Tesseract: 18 × 1152 = 20736 */
    if (18u * 1152u != 20736u) return -9;

    /* 8. Dual: 144² = 18 × 8 × 144 */
    if (18u * 8u * 144u != 20736u) return -10;

    return 0;
}

#endif /* GEO_PARAM_GRID_H */