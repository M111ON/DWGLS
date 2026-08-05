/* ═══════════════════════════════════════════════════════════════════════════
 * geo_cell_prune.h — Geometric Pruning Engine
 * ═══════════════════════════════════════════════════════════════════════════
 *
 * PURPOSE:
 *   Identify which cell types have low weight mass → prunable.
 *   A cell type with < threshold% of total weight mass can be zeroed out
 *   to reduce model size with minimal quality impact.
 *
 * DEPENDS: geo_cell_classify.h
 *
 * USAGE:
 *   // 1. Analyze weight distribution by cell type
 *   GeoPruneReport rpt;
 *   geo_cell_prune_analyze(weights, n_weights, max_gen, &rpt);
 *
 *   // 2. Build prune mask (which indices to zero)
 *   uint8_t *mask = malloc(n_weights);  // 1=keep, 0=prune
 *   geo_cell_prune_mask(weights, n_weights, max_gen, threshold, mask);
 *
 *   // 3. Apply mask
 *   for (uint32_t i = 0; i < n_weights; i++)
 *       if (!mask[i]) weights[i] = 0.0f;
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifndef GEO_CELL_PRUNE_H
#define GEO_CELL_PRUNE_H

#include "geo_cell_classify.h"
#include <math.h>

/* ═══════════════════════════════════════════════════════════════
   PRUNE REPORT — per cell type statistics
   ═══════════════════════════════════════════════════════════════ */

typedef struct {
    /* Per cell type (0-7): count, mass (sum of |w|), mean |w| */
    uint32_t count[8];
    double   mass[8];       /* sum of |weight| */
    double   mean_abs[8];   /* mass / count */
    double   total_mass;    /* sum of all |w| */
    uint32_t total_count;   /* total weight count */

    /* Prunable types (below threshold) */
    uint32_t prunable_types;   /* bitmask of prunable cell types */
    uint32_t pruned_count;     /* total weights that would be pruned */
    double   pruned_mass;      /* total mass that would be pruned */
    double   prune_ratio;      /* pruned_count / total_count */
    double   mass_loss_ratio;  /* pruned_mass / total_mass */
} GeoPruneReport;

/* ═══════════════════════════════════════════════════════════════
   ANALYZE — scan weights, classify, compute per-type stats
   ═══════════════════════════════════════════════════════════════ */

static inline void geo_cell_prune_analyze(const float *weights,
                                           uint32_t n_weights,
                                           uint32_t max_gen,
                                           GeoPruneReport *rpt)
{
    memset(rpt, 0, sizeof(*rpt));

    for (uint32_t i = 0; i < n_weights; i++) {
        GeoCubeAddr addr = geo_flat_to_addr(i % 20736u);
        if (addr.generation > max_gen) continue;

        uint8_t ct = geo_cell_classify(addr);
        double abs_w = fabs((double)weights[i]);

        rpt->count[ct]++;
        rpt->mass[ct] += abs_w;
        rpt->total_count++;
        rpt->total_mass += abs_w;
    }

    /* Compute mean |w| per type */
    for (int ct = 0; ct < 8; ct++) {
        rpt->mean_abs[ct] = (rpt->count[ct] > 0)
            ? rpt->mass[ct] / rpt->count[ct]
            : 0.0;
    }
}

/* ═══════════════════════════════════════════════════════════════
   PRUNE MASK — build per-index mask (1=keep, 0=prune)
   
   threshold: cell types with count_pct < threshold% are pruned
              e.g. threshold=1.0 → prune types with < 1% of total count
   ═══════════════════════════════════════════════════════════════ */

static inline void geo_cell_prune_mask(const float *weights,
                                        uint32_t n_weights,
                                        uint32_t max_gen,
                                        float threshold_pct,
                                        uint8_t *mask,       /* out: 1=keep, 0=prune */
                                        GeoPruneReport *rpt)  /* out: report (or NULL) */
{
    GeoPruneReport local_rpt;
    if (!rpt) rpt = &local_rpt;

    /* Analyze first */
    geo_cell_prune_analyze(weights, n_weights, max_gen, rpt);

    /* Determine which types to prune */
    rpt->prunable_types = 0;
    rpt->pruned_count = 0;
    rpt->pruned_mass = 0.0;

    for (int ct = 0; ct < 8; ct++) {
        if (rpt->total_count == 0) continue;
        double pct = 100.0 * (double)rpt->count[ct] / (double)rpt->total_count;
        if (pct < (double)threshold_pct) {
            rpt->prunable_types |= (1u << ct);
        }
    }

    /* Build mask */
    for (uint32_t i = 0; i < n_weights; i++) {
        GeoCubeAddr addr = geo_flat_to_addr(i % 20736u);
        if (addr.generation > max_gen) {
            mask[i] = 1;  /* keep out-of-range */
            continue;
        }

        uint8_t ct = geo_cell_classify(addr);
        if (rpt->prunable_types & (1u << ct)) {
            mask[i] = 0;  /* prune */
            rpt->pruned_count++;
            rpt->pruned_mass += fabs((double)weights[i]);
        } else {
            mask[i] = 1;  /* keep */
        }
    }

    rpt->prune_ratio = (rpt->total_count > 0)
        ? (double)rpt->pruned_count / (double)rpt->total_count
        : 0.0;
    rpt->mass_loss_ratio = (rpt->total_mass > 0.0)
        ? rpt->pruned_mass / rpt->total_mass
        : 0.0;
}

/* ═══════════════════════════════════════════════════════════════
   PRINT REPORT
   ═══════════════════════════════════════════════════════════════ */

static inline void geo_cell_prune_print(const GeoPruneReport *rpt) {
    printf("===============================================================\n");
    printf("  Geometric Pruning Report\n");
    printf("---------------------------------------------------------------\n");
    printf("  Total weights:  %u\n", rpt->total_count);
    printf("  Total mass:     %.2f\n", rpt->total_mass);
    printf("---------------------------------------------------------------\n");
    printf("  %-6s  %8s  %12s  %10s  %s\n",
           "Type", "Count", "Mass", "Mean|w|", "Status");
    printf("  %-6s  %8s  %12s  %10s  %s\n",
           "────", "─────", "────", "───────", "──────");

    for (int ct = 0; ct < 8; ct++) {
        int prunable = (rpt->prunable_types >> ct) & 1;
        printf("  %-6s  %8u  %12.2f  %10.4f  %s\n",
               geo_cell_classify_name(ct),
               rpt->count[ct],
               rpt->mass[ct],
               rpt->mean_abs[ct],
               prunable ? "✂ PRUNE" : "✓ KEEP");
    }

    printf("---------------------------------------------------------------\n");
    printf("  Prunable types:  0x%02X (%d types)\n",
           rpt->prunable_types, __builtin_popcount(rpt->prunable_types));
    printf("  Weights pruned:  %u / %u (%.1f%%)\n",
           rpt->pruned_count, rpt->total_count, rpt->prune_ratio * 100.0);
    printf("  Mass pruned:     %.2f / %.2f (%.1f%%)\n",
           rpt->pruned_mass, rpt->total_mass, rpt->mass_loss_ratio * 100.0);
    printf("===============================================================\n");
}

#endif /* GEO_CELL_PRUNE_H */
