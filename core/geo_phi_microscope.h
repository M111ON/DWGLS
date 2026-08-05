/* ═══════════════════════════════════════════════════════════════════════════
 * geo_phi_microscope.h — φ-Zoom Weight Observation Tool
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * OBSERVATION, not compression.
 * Look at NN weights at different φ-zoom levels and see geometric structure.
 *   Random weights  → flat cell-type distribution (high entropy)
 *   Real NN weights  → structured cell-type distribution (lower entropy)
 *
 * How it works:
 *   1. Map weight indices → GeoCubeAddr via flat→addr reverse lookup
 *   2. At a given generation, classify each weight's cell type (0-7)
 *   3. Count per-type, compute Shannon entropy, mean |weight|
 *   4. Compare across generations to find where structure appears
 *
 * Depends: geo_cube_addr.h (which depends on geo_cube_in_dodeca.h)
 *
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifndef GEO_PHI_MICROSCOPE_H
#define GEO_PHI_MICROSCOPE_H

#include "geo_cube_addr.h"
#include <stdlib.h>  /* for abs() */
#include <string.h>  /* for memset */
#include <float.h>   /* for DBL_MAX */

/* ═══════════════════════════════════════════════════════════════
   CONSTANTS
   ═══════════════════════════════════════════════════════════════ */

#define PHI_MICRO_CELL_TYPES 8u   /* III, IID, IDI, IDD, DII, DID, DDI, DDD */
#define PHI_MICRO_GEN_MAX    12u  /* practical zoom ceiling */

/* ═══════════════════════════════════════════════════════════════
   RESULT STRUCT
   ═══════════════════════════════════════════════════════════════ */

typedef struct {
    uint8_t  gen;                           /* generation observed */
    uint32_t shell_size;                    /* total_slots(gen) — how many slots */
    uint32_t type_counts[PHI_MICRO_CELL_TYPES]; /* weight count per cell type */
    double   type_magnitudes[PHI_MICRO_CELL_TYPES]; /* sum of |w| per type */
    double   entropy;                       /* Shannon entropy (bits) of distribution */
    double   magnitude_entropy;             /* Shannon entropy of magnitude distribution */
    double   mean_magnitude;                /* mean |weight| */
    double   max_magnitude;                 /* max |weight| */
    double   min_magnitude;                 /* min |weight| */
    uint32_t n_observed;                    /* number of weights actually mapped */
} PhiMicroscopeResult;

/* ═══════════════════════════════════════════════════════════════
   SHANNON ENTROPY
   ═══════════════════════════════════════════════════════════════
   H = -Σ p_i × log2(p_i)   in bits
   Max entropy for 8 types = log2(8) = 3.0 bits (uniform)
   ═══════════════════════════════════════════════════════════════ */

static inline double phi_entropy(const uint32_t *type_counts, uint32_t total) {
    if (total == 0) return 0.0;
    double H = 0.0;
    double inv_total = 1.0 / (double)total;
    for (uint32_t i = 0; i < PHI_MICRO_CELL_TYPES; i++) {
        if (type_counts[i] > 0) {
            double p = (double)type_counts[i] * inv_total;
            H -= p * log2(p);
        }
    }
    return H;
}

/* Same entropy but for double array (magnitude distribution) */
static inline double phi_entropy_dbl(const double *values, uint32_t total) {
    if (total == 0) return 0.0;
    double H = 0.0;
    double inv_total = 1.0 / (double)total;
    for (uint32_t i = 0; i < PHI_MICRO_CELL_TYPES; i++) {
        if (values[i] > 0.0) {
            double p = values[i] * inv_total;
            H -= p * log2(p);
        }
    }
    return H;
}

/* ═══════════════════════════════════════════════════════════════
   OBSERVE ONE GENERATION
   ═══════════════════════════════════════════════════════════════
   Map weight[i] → geo_flat_to_addr(i), then classify by
   the address's cell_type. Aggregate counts, entropy, magnitude.
   
   Weights that map to addresses beyond total_slots(gen) wrap via %.
   ═══════════════════════════════════════════════════════════════ */

static inline PhiMicroscopeResult phi_observe_generation(
    const float *weights, uint32_t n_weights, uint32_t generation)
{
    PhiMicroscopeResult r;
    r.gen = (uint8_t)generation;
    r.shell_size = total_slots(generation);
    r.n_observed = n_weights;
    memset(r.type_counts, 0, sizeof(r.type_counts));
    memset(r.type_magnitudes, 0, sizeof(r.type_magnitudes));
    r.entropy = 0.0;
    r.magnitude_entropy = 0.0;
    r.mean_magnitude = 0.0;
    r.max_magnitude = 0.0;
    r.min_magnitude = FLT_MAX;

    if (n_weights == 0 || generation > CUBE_ADDR_GEN_MAX) return r;

    double mag_sum = 0.0;

    for (uint32_t i = 0; i < n_weights; i++) {
        /* Map flat weight index → GeoCubeAddr at target generation.
         * We use geo_flat_to_addr which finds the natural gen for flat=i,
         * but we want ALL weights viewed at a SINGLE generation.
         * So we compute face and slot within that generation directly. */
        uint32_t shell = r.shell_size;
        uint32_t pos = i % shell;                 /* wrap into shell */
        uint8_t face = (uint8_t)((pos / slots_per_face(generation)) % CUBE_ADDR_FACES);
        uint16_t slot = (uint16_t)(pos % slots_per_face(generation));

        GeoCubeAddr addr = geo_cube_addr(generation, face, slot);
        r.type_counts[addr.cell_type]++;

        double mag = fabs((double)weights[i]);
        r.type_magnitudes[addr.cell_type] += mag;
        mag_sum += mag;
        if (mag > r.max_magnitude) r.max_magnitude = mag;
        if (mag < r.min_magnitude) r.min_magnitude = mag;
    }

    r.mean_magnitude = mag_sum / (double)n_weights;
    r.entropy = phi_entropy(r.type_counts, n_weights);
    r.magnitude_entropy = phi_entropy_dbl(r.type_magnitudes, (uint32_t)(mag_sum + 1e-15));

    return r;
}

/* ═══════════════════════════════════════════════════════════════
   PRINT RESULT ROW
   ═══════════════════════════════════════════════════════════════ */

static inline void phi_print_result(const PhiMicroscopeResult *r) {
    printf("  Gen %2u | shell=%4u | H_cnt=%.2f H_mag=%.2f | |w|_mean=%8.5f | max=%8.5f",
           r->gen, r->shell_size, r->entropy, r->magnitude_entropy, r->mean_magnitude, r->max_magnitude);
    printf(" | ");
    for (uint32_t i = 0; i < PHI_MICRO_CELL_TYPES; i++) {
        printf("%s:%.0f ", cell_type_name((uint8_t)i), r->type_magnitudes[i]);
    }
    printf("\n");
}

/* ═══════════════════════════════════════════════════════════════
   SCAN MULTIPLE GENERATIONS
   ═══════════════════════════════════════════════════════════════
   Run phi_observe_generation for gen_min..gen_max, print table.
   ═══════════════════════════════════════════════════════════════ */

static inline void phi_microscope(const float *weights, uint32_t n_weights,
                                   uint8_t gen_min, uint8_t gen_max)
{
    printf("=================================================================\n");
    printf("  φ-Microscope — Weight Observation at Multiple Zoom Levels\n");
    printf("-----------------------------------------------------------------\n");
    printf("  Weights: %u  |  Generations: %u → %u  |  Max H per gen = 2.000 bits (4 types/gen)\n",
           n_weights, gen_min, gen_max);
    printf("=================================================================\n");
    printf("  Gen | Shell  | Entropy     | |w|_mean  | max     | type counts\n");
    printf("  ----|--------|-------------|-----------|---------|-------------\n");

    uint8_t lo = gen_min;
    uint8_t hi = gen_max;
    if (hi > PHI_MICRO_GEN_MAX) hi = PHI_MICRO_GEN_MAX;
    if (lo > hi) lo = hi;

    for (uint8_t g = lo; g <= hi; g++) {
        PhiMicroscopeResult r = phi_observe_generation(weights, n_weights, g);
        phi_print_result(&r);
    }

    printf("=================================================================\n");
    printf("  Interpretation:\n");
    printf("    Random weights → H_mag ≈ 2.0 bits (uniform magnitude across 4 active types)\n");
    printf("    Structured weights → H_mag < 2.0 bits (geometric clustering by magnitude)\n");
    printf("    Note: gen even→types III/IID/IDI/IDD, gen odd→DII/DID/DDI/DDD\n");
    printf("    Best separation gen = where Δ_mag peaks\n");
    printf("=================================================================\n");
}

/* ═══════════════════════════════════════════════════════════════
   DEMO — SYNTHETIC DATA
   ═══════════════════════════════════════════════════════════════
   Generate random + structured weight arrays, compare patterns.
   Uses a simple LCG PRNG for reproducibility (no deps).
   ═══════════════════════════════════════════════════════════════ */

static inline void phi_microscope_demo(void) {
    const uint32_t N = 1000;
    float random_weights[1000];
    float structured_weights[1000];

    /* Simple LCG PRNG: seed = 42 */
    uint32_t seed = 42;
    #define LCG_NEXT(s) ((s) = (s) * 1103515245u + 12345u)

    /* Generate random weights: uniform [-1, 1] */
    for (uint32_t i = 0; i < N; i++) {
        LCG_NEXT(seed);
        random_weights[i] = ((float)(seed & 0xFFFF) / 32768.0f) - 1.0f;
    }

    /* Generate structured weights: cluster 60% at type 0, 20% at type 7, rest spread.
     * This simulates NN weights that "prefer" certain cell types. */
    seed = 12345;
    for (uint32_t i = 0; i < N; i++) {
        LCG_NEXT(seed);
        float base = ((float)(seed & 0xFFFF) / 32768.0f) - 1.0f;
        /* Modulate magnitude by cell-type preference */
        uint32_t pos = i % (uint32_t)total_slots(6);  /* gen 6 shell */
        uint8_t face = (uint8_t)((pos / slots_per_face(6)) % CUBE_ADDR_FACES);
        uint16_t slot = (uint16_t)(pos % slots_per_face(6));
        GeoCubeAddr addr = geo_cube_addr(6, face, slot);

        double bias = 1.0;
        if (addr.cell_type == 0) bias = 3.0;   /* III cells get big weights */
        else if (addr.cell_type == 7) bias = 0.1; /* DDD cells get small weights */
        else bias = 0.5 + 0.5 * (addr.cell_type / 7.0);

        structured_weights[i] = base * (float)bias;
    }

    printf("\n");
    printf("╔═══════════════════════════════════════════════════════════════╗\n");
    printf("║       φ-MICROSCOPE DEMO — Observation, Not Compression       ║\n");
    printf("╚═══════════════════════════════════════════════════════════════╝\n\n");

    printf("─── Random Weights (uniform [-1, 1]) ──────────────────────────\n");
    phi_microscope(random_weights, N, 0, 8);

    printf("\n─── Structured Weights (biased by cell type) ──────────────────\n");
    phi_microscope(structured_weights, N, 0, 8);

    printf("\n─── Comparison ───────────────────────────────────────────────\n");
    printf("  If structured weights show LOWER entropy than random,\n");
    printf("  the φ-addressing reveals geometric structure in the data.\n");
    printf("  Real NN weights will show structure at specific generations.\n");
    printf("═══════════════════════════════════════════════════════════════\n\n");

    #undef LCG_NEXT
}

#endif /* GEO_PHI_MICROSCOPE_H */
